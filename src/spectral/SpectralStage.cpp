// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpectralStage.h"
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <thread>
#include <vector>

namespace livecut::spectral
{
namespace
{
constexpr std::size_t chunkFrames = 64;
Availability availability (SpectralState s)
{
    switch (s)
    {
    case SpectralState::waiting:
        return Availability::waiting;
    case SpectralState::priming:
        return Availability::priming;
    case SpectralState::ready:
        return Availability::ready;
    }
    throw std::logic_error ("Unknown spectral availability");
}
bool finiteRange (double v, double low, double high) noexcept
{
    return std::isfinite (v) && v >= low && v <= high;
}
bool validPacket (const Packet &p) noexcept
{
    if (p.before.count > p.before.items.size () || p.after.count > p.after.items.size () || p.epoch == 0 ||
        !finiteRange (p.spectral.smearMs, 0, 2000) || !finiteRange (p.spectral.localMix, 0, 1) || !finiteRange (p.output.globalMix, 0, 1) ||
        !std::isfinite (p.output.trimDb) || !finiteRange (p.output.quarterNotesPerSample, 0, 1))
        return false;
    for (std::size_t i = 0; i < p.before.count; ++i)
    {
        const auto &e = p.before.items[i];
        switch (e.kind)
        {
        case EventKind::discontinuity:
        case EventKind::phraseCompleted:
        case EventKind::barStarted:
            break;
        case EventKind::phraseStarted:
            if (!std::isfinite (e.phraseQuarterNotes) || e.phraseQuarterNotes <= 0)
                return false;
            break;
        default:
            return false;
        }
    }
    // endRenderContext emits completion only, at exclusive sourceFrame+1.
    return p.after.count == 0 || p.after.items[0].kind == EventKind::phraseCompleted;
}
template <class Item> struct Ring
{
    explicit Ring (std::size_t capacity) : items (capacity) {}
    Item *producer () noexcept
    {
        const auto head = written.load (std::memory_order_relaxed);
        return head - read.load (std::memory_order_acquire) < items.size () ? &items[static_cast<std::size_t> (head % items.size ())]
                                                                            : nullptr;
    }
    void publish () noexcept
    {
        written.fetch_add (1, std::memory_order_release);
    }
    const Item *consumer () const noexcept
    {
        const auto tail = read.load (std::memory_order_relaxed);
        return tail < written.load (std::memory_order_acquire) ? &items[static_cast<std::size_t> (tail % items.size ())] : nullptr;
    }
    void consume () noexcept
    {
        read.fetch_add (1, std::memory_order_release);
    }
    std::vector<Item> items;
    alignas (64) std::atomic<std::uint64_t> written{0};
    alignas (64) std::atomic<std::uint64_t> read{0};
};
} // namespace
struct StopHandle::State
{
    std::atomic<bool> stopping{false};
    std::atomic<std::uint64_t> progress{0};
};
StopHandle::StopHandle (std::shared_ptr<State> s) : state (std::move (s)) {}
void StopHandle::requestStop () const noexcept
{
    if (!state)
        return;
    state->stopping.store (true, std::memory_order_release);
    state->progress.fetch_add (1, std::memory_order_release);
    state->progress.notify_all ();
}
bool StopHandle::requested () const noexcept
{
    return state && state->stopping.load (std::memory_order_acquire);
}
struct Stage::Impl
{
    struct InputFrame
    {
        Frame processed;
        std::uint64_t epoch = 1;
        Wola::Controls controls;
    };
    struct WetFrame
    {
        Frame wet;
        std::int64_t source = -1;
        std::uint64_t epoch = 0;
        Availability state = Availability::waiting;
    };
    struct InputChunk
    {
        std::int64_t first = 0;
        std::array<InputFrame, chunkFrames> frames;
    };
    struct OutputChunk
    {
        std::int64_t first = 0;
        std::array<WetFrame, chunkFrames> frames;
    };
    explicit Impl (Preparation p)
        : prepared (p), config (Configuration::prepare (p.sampleRate, 4096)), guard (config.size),
          kernel (p.enabled ? std::make_unique<Wola> (p.sampleRate, 4096) : nullptr),
          input (!p.enabled                                          ? 0
                 : p.testHooks && p.testHooks->inputCapacityOverride ? p.testHooks->inputCapacityOverride
                                                                     : static_cast<std::size_t> (guard) / chunkFrames + 8),
          output (p.enabled ? static_cast<std::size_t> (guard) / chunkFrames + 4 : 0),
          metadata (p.enabled ? static_cast<std::size_t> (config.size + guard) : 0),
          workerEpochs (p.enabled ? static_cast<std::size_t> (config.size) : 0)
    {
        if (p.testHooks && p.testHooks->inputCapacityOverride > 8192)
            throw std::invalid_argument ("Input capacity exceeds preparation bound");
        if (p.enabled)
            thread = std::thread ([this] { run (); });
    }
    ~Impl ()
    {
        stop ();
        if (thread.joinable ())
            thread.join ();
    }
    Fault fault () const noexcept
    {
        const auto f = failure.load (std::memory_order_acquire);
        return f == Fault::none && cancellation.state->stopping.load (std::memory_order_acquire) ? Fault::stopped : f;
    }
    bool healthy () const noexcept
    {
        return fault () == Fault::none;
    }
    void fail (Fault f) noexcept
    {
        auto expected = Fault::none;
        failure.compare_exchange_strong (expected, f, std::memory_order_acq_rel);
        // No notify on producer fault: real-time fault paths only publish atomics.
        // Worker exit notifies offline waiters; offline producer faults return directly.
    }
    void advanceProgress () noexcept
    {
        cancellation.state->progress.fetch_add (1, std::memory_order_release);
        cancellation.state->progress.notify_all ();
    }
    void stop () noexcept
    {
        cancellation.requestStop ();
    }
    void waitProgress (std::uint64_t observed) noexcept
    {
        // Only explicit offline producer paths reach this function.
        waits.fetch_add (1, std::memory_order_relaxed);
        if (prepared.testHooks) prepared.testHooks->offlineWaitsObserved.fetch_add (1, std::memory_order_release);
        cancellation.state->progress.wait (observed, std::memory_order_acquire);
    }
    Result gated () const noexcept
    {
        Result r;
        r.fault = fault ();
        r.recoveryRequired = r.fault != Fault::none;
        return r;
    }
    void run () noexcept
    {
        Wola::Controls active;
        std::int64_t expected = 0;
        std::uint64_t epoch = 1;
        try
        {
            while (healthy ())
            {
                auto *hooks = prepared.testHooks;
                if (hooks && hooks->pause.load (std::memory_order_acquire))
                {
                    std::this_thread::sleep_for (std::chrono::microseconds (50));
                    continue;
                }
                const auto *source = input.consumer ();
                auto *destination = output.producer ();
                if (!source || !destination)
                {
                    std::this_thread::sleep_for (std::chrono::microseconds (50));
                    continue;
                }
                if (source->first != expected)
                {
                    fail (Fault::workerFailure);
                    break;
                }
                if (hooks)
                {
                    auto at = hooks->delayAtInputFrame.load (std::memory_order_acquire);
                    if (at == expected && hooks->delayAtInputFrame.compare_exchange_strong (at, -1))
                    {
                        const auto until = std::chrono::steady_clock::now () + std::chrono::microseconds (hooks->delayMicroseconds.load ());
                        while (healthy () && std::chrono::steady_clock::now () < until)
                            std::this_thread::sleep_for (std::chrono::microseconds (50));
                    }
                }
                destination->first = expected;
                if (hooks && hooks->corruptOutput.exchange (false))
                    ++destination->first;
                bool finished = true;
                for (std::size_t i = 0; i < chunkFrames; ++i)
                {
                    if (!healthy ())
                    {
                        finished = false;
                        break;
                    }
                    const auto &frame = source->frames[i];
                    // Close the admitted old suffix under its own controls.
                    // A request on the first new-epoch frame applies only after
                    // beginEpoch has completed that prior epoch's closure.
                    if (frame.epoch != epoch)
                    {
                        if (!kernel->beginEpoch (frame.epoch))
                            throw std::logic_error ("Unordered spectral epoch");
                        epoch = frame.epoch;
                    }
                    if (frame.controls.freeze != active.freeze || frame.controls.smearMs != active.smearMs)
                    {
                        kernel->setControls (frame.controls);
                        active = frame.controls;
                    }
                    const auto absolute = expected + static_cast<std::int64_t> (i);
                    const auto slot = static_cast<std::size_t> (absolute % config.size);
                    const auto outputEpoch = workerEpochs[slot];
                    workerEpochs[slot] = epoch;
                    auto &result = destination->frames[i];
                    result.wet = kernel->process (frame.processed);
                    result.source = absolute - config.size;
                    result.epoch = outputEpoch;
                    const auto &diagnostics = kernel->diagnostics ();
                    result.state = availability (diagnostics.outputState);
                    if (diagnostics.outputEpoch != 0 && diagnostics.outputEpoch != outputEpoch)
                        throw std::logic_error ("OLA ownership differs from delayed source epoch");
                    if (!std::isfinite (result.wet.left) || !std::isfinite (result.wet.right))
                        throw std::runtime_error ("Non-finite spectral output");
                }
                if (!finished)
                    break;
                expected += static_cast<std::int64_t> (chunkFrames);
                output.publish ();
                input.consume ();
                processedFrames.store (static_cast<std::uint64_t> (expected), std::memory_order_release);
                advanceProgress ();
            }
        }
        catch (...)
        {
            fail (Fault::workerFailure);
        }
        advanceProgress ();
    }
    Preparation prepared;
    Configuration config;
    int guard;
    std::unique_ptr<Wola> kernel; // exclusively worker-owned after construction
    Ring<InputChunk> input;
    Ring<OutputChunk> output;
    std::vector<Packet> metadata;            // audio-owned, fixed Ds slots
    std::vector<std::uint64_t> workerEpochs; // worker-owned, fixed N slots
    std::atomic<Fault> failure{Fault::none};
    StopHandle cancellation{std::make_shared<StopHandle::State> ()};
    std::atomic<std::uint64_t> processedFrames{0}, waits{0};
    std::thread thread;
    InputChunk *writing = nullptr;
    const OutputChunk *reading = nullptr;
    std::size_t writeOffset = 0, readOffset = 0;
    std::int64_t nextFrame = 0;
    std::uint64_t publishedFrames = 0, lastEpoch = 0;
    Execution execution = Execution::realtime;
    bool callbackOpen = false;
};
static_assert (std::atomic<std::uint64_t>::is_always_lock_free);
static_assert (std::atomic<bool>::is_always_lock_free);
static_assert (std::atomic<Fault>::is_always_lock_free);
Stage::Stage (Preparation p) : impl (std::make_unique<Impl> (p)) {}
Stage::~Stage () = default;
void Stage::requestStop () noexcept
{
    impl->stop ();
}
StopHandle Stage::stopHandle () const
{
    return impl->cancellation;
}
Fault Stage::fault () const noexcept
{
    return impl->fault ();
}
int Stage::latency () const noexcept
{
    return impl->prepared.enabled ? impl->config.size + impl->guard : 0;
}
int Stage::guardFrames () const noexcept
{
    return impl->prepared.enabled ? impl->guard : 0;
}
const Configuration &Stage::configuration () const noexcept
{
    return impl->config;
}
std::uint64_t Stage::offlineWaits () const noexcept
{
    return impl->waits.load (std::memory_order_relaxed);
}
void Stage::beginCallback (Execution e) noexcept
{
    auto &state = *impl;
    if (state.callbackOpen || (e != Execution::realtime && e != Execution::offline))
    {
        state.fail (Fault::callbackContract);
        return;
    }
    state.execution = e;
    state.callbackOpen = true;
}
Result Stage::process (const Packet &p) noexcept
{
    auto &state = *impl;
    if (!state.callbackOpen)
    {
        state.fail (Fault::callbackContract);
        return state.gated ();
    }
    if (!state.healthy ())
        return state.gated ();
    if (!validPacket (p) || p.sourceFrame != state.nextFrame || p.epoch < state.lastEpoch ||
        state.nextFrame >= std::numeric_limits<std::int64_t>::max () - latency ())
    {
        state.fail (Fault::packetContract);
        return state.gated ();
    }
    state.lastEpoch = p.epoch;
    if (!state.prepared.enabled)
    {
        ++state.nextFrame;
        Result r;
        r.packet = p;
        r.spectralWet = p.processed;
        r.availability = Availability::disabled;
        r.valid = true;
        return r;
    }
    if (!state.writing)
        for (;;)
        {
            const auto observed = state.cancellation.state->progress.load (std::memory_order_acquire);
            state.writing = state.input.producer ();
            if (state.writing)
                break;
            if (state.execution == Execution::realtime)
            {
                state.fail (Fault::inputAdmission);
                return state.gated ();
            }
            if (!state.healthy ())
                return state.gated ();
            state.waitProgress (observed);
        }
    if (state.writeOffset == 0)
        state.writing->first = state.nextFrame;
    state.writing->frames[state.writeOffset++] = {p.processed, p.epoch, {p.spectral.freeze, p.spectral.smearMs}};
    if (state.writeOffset == chunkFrames)
    {
        state.input.publish ();
        state.writing = nullptr;
        state.writeOffset = 0;
        state.publishedFrames = static_cast<std::uint64_t> (state.nextFrame + 1);
    }
    const auto slot = static_cast<std::size_t> (state.nextFrame % latency ());
    Result result;
    if (state.nextFrame >= latency ())
    {
        result.packet = state.metadata[slot];
        result.valid = true;
    }
    state.metadata[slot] = p;
    if (state.nextFrame >= state.guard)
    {
        const auto expected = state.nextFrame - state.guard;
        if (!state.reading)
        {
            for (;;)
            {
                const auto observed = state.cancellation.state->progress.load (std::memory_order_acquire);
                state.reading = state.output.consumer ();
                if (state.reading)
                    break;
                if (state.execution == Execution::realtime)
                {
                    state.fail (Fault::deadline);
                    return state.gated ();
                }
                if (!state.healthy ())
                    return state.gated ();
                state.waitProgress (observed);
            }
            if (state.reading->first != expected)
            {
                state.fail (Fault::staleResult);
                return state.gated ();
            }
        }
        if (state.reading->first + static_cast<std::int64_t> (state.readOffset) != expected)
        {
            state.fail (Fault::staleResult);
            return state.gated ();
        }
        const auto &wet = state.reading->frames[state.readOffset++];
        if (result.valid)
        {
            if (wet.source != result.packet.sourceFrame || wet.epoch != result.packet.epoch ||
                result.packet.sourceFrame != state.nextFrame - latency ())
            {
                state.fail (Fault::staleResult);
                return state.gated ();
            }
            result.spectralWet = wet.wet;
            result.availability = wet.state;
        }
        if (state.readOffset == chunkFrames)
        {
            state.output.consume ();
            state.reading = nullptr;
            state.readOffset = 0;
        }
    }
    ++state.nextFrame;
    return state.healthy () ? result : state.gated ();
}
void Stage::endCallback () noexcept
{
    auto &state = *impl;
    if (!state.callbackOpen)
    {
        state.fail (Fault::callbackContract);
        return;
    }
    if (state.prepared.enabled && state.execution == Execution::offline)
        while (state.healthy ())
        {
            const auto observed = state.cancellation.state->progress.load (std::memory_order_acquire);
            if (state.processedFrames.load (std::memory_order_acquire) >= state.publishedFrames)
                break;
            state.waitProgress (observed);
        }
    state.callbackOpen = false;
}
std::size_t Stage::explicitStorageBytes () const noexcept
{
    const auto &s = *impl;
    return sizeof (*this) + sizeof (Impl) + sizeof (StopHandle::State) + s.input.items.capacity () * sizeof (Impl::InputChunk) +
           s.output.items.capacity () * sizeof (Impl::OutputChunk) + s.metadata.capacity () * sizeof (Packet) +
           s.workerEpochs.capacity () * sizeof (std::uint64_t) + (s.kernel ? s.kernel->explicitStorageBytes () : 0);
}
Frame Result::localWet () const noexcept
{
    if (!valid)
        return {};
    const auto mix = packet.spectral.localMix;
    if (availability == Availability::disabled || mix == 0)
        return packet.processed;
    if (mix == 1)
        return spectralWet;
    return {packet.processed.left + mix * (spectralWet.left - packet.processed.left),
            packet.processed.right + mix * (spectralWet.right - packet.processed.right)};
}
bool Result::holdEligible () const noexcept
{
    return valid && (availability == Availability::disabled || packet.spectral.localMix == 0 || availability == Availability::ready);
}
bool Result::captureHasWarmup () const noexcept
{
    return valid && packet.output.running && !packet.output.bypass && packet.spectral.localMix > 0 &&
           (availability == Availability::waiting || availability == Availability::priming);
}
} // namespace livecut::spectral
