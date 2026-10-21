// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "SpectralKernel.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace livecut::spectral
{
enum class Execution
{
    realtime,
    offline
};
enum class Availability
{
    disabled,
    waiting,
    priming,
    ready
};
enum class Fault
{
    none,
    packetContract,
    inputAdmission,
    deadline,
    staleResult,
    workerFailure,
    callbackContract,
    stopped
};
enum class EventKind
{
    discontinuity,
    phraseCompleted,
    barStarted,
    phraseStarted
};
struct Event
{
    EventKind kind = EventKind::discontinuity;
    bool fullStart = false;
    std::int64_t bar = 0;
    double phraseQuarterNotes = 0;
};
template <std::size_t Capacity> struct Events
{
    std::array<Event, Capacity> items{};
    std::size_t count = 0;
    // Overflow is retained for Stage validation; never drop/collapse an event.
    bool append (Event e) noexcept
    {
        if (count >= Capacity)
        {
            count = Capacity + 1;
            return false;
        }
        items[count++] = e;
        return true;
    }
};
struct Controls
{
    bool freeze = false;
    double smearMs = 0; // 0..2000, hop-rate magnitude EMA time constant
    float localMix = 1; // linear pre-spectral processed <-> spectral
};
struct OutputContext
{
    double quarterNotesPerSample = 0;
    float globalMix = 1, trimDb = 0;
    bool running = false, bypass = false, hold = false, resetHold = false;
};
// Values captured from the original rendered sample, never a Cutter/timeline pointer.
struct DisplaySnapshot
{
    std::uint64_t transformEligible = 0, attackApplied = 0, reverseApplied = 0, transformIneligible = 0;
    double blockStart = 0, blockEnd = 0, position = 0, fractureTapeSpeed = 0;
    int phraseBars = 0, unitsPerBar = 8, transformReason = 0, cutterError = 0;
    int fractureFamily = 0, fractureState = 0, fractureVoices = 0;
    int resonatorState = 0, effectRoute = 0, requestedEffectRoute = 0;
    bool cutting = false, timingSupported = true, resonatorRetuning = false;
    bool resonatorPending = false, effectRouteTransition = false;
};
// All values own their storage. Seal only after optional endRenderContext.
// before order is precisely observed, NOT reconstructed from event kinds.
struct Packet
{
    std::int64_t sourceFrame = 0;
    std::uint64_t epoch = 1;
    Frame processed, raw;
    Controls spectral;
    OutputContext output;
    DisplaySnapshot display;
    Events<4> before;
    Events<1> after; // exclusive sourceFrame+1, even with no next source sample
};
struct Result
{
    Packet packet;
    Frame spectralWet;
    Availability availability = Availability::waiting;
    Fault fault = Fault::none;
    bool valid = false, recoveryRequired = false;
    Frame localWet () const noexcept;
    bool holdEligible () const noexcept;
    bool captureHasWarmup () const noexcept;
};
struct TestHooks
{
    std::atomic<bool> pause{false}, corruptOutput{false};
    std::atomic<std::int64_t> delayAtInputFrame{-1};
    std::atomic<int> delayMicroseconds{0};
    std::size_t inputCapacityOverride = 0;
    std::atomic<std::uint64_t> offlineWaitsObserved { 0 };
};
struct Preparation
{
    double sampleRate = 48000;
    bool enabled = true;
    TestHooks *testHooks = nullptr; // test seam, lifetime must extend through join
};
class StopHandle
{
  public:
    StopHandle () = default;
    void requestStop () const noexcept; // lifecycle/off audio: cancellation + waiter notification
    bool requested () const noexcept;

  private:
    struct State;
    std::shared_ptr<State> state;
    explicit StopHandle (std::shared_ptr<State>);
    friend class Stage;
};
class Stage
{
  public:
    explicit Stage (Preparation);
    ~Stage (); // callback exclusion first; requestStop/join off audio and callback lock
    Stage (const Stage &) = delete;
    Stage &operator= (const Stage &) = delete;
    void beginCallback (Execution) noexcept;
    Result process (const Packet &) noexcept;
    void endCallback () noexcept;   // only explicit offline mode may wait
    void requestStop () noexcept;   // lifecycle/off audio: safe cancellation; does not destroy/join
    StopHandle stopHandle () const; // copy off audio before publishing the prepared stream
    Fault fault () const noexcept;
    int latency () const noexcept; // Ds only; Cutter L0 remains separate
    int guardFrames () const noexcept;
    const Configuration &configuration () const noexcept;
    std::uint64_t offlineWaits () const noexcept;
    std::size_t explicitStorageBytes () const noexcept; // immutable capacities
  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
static_assert (std::is_trivially_copyable_v<Packet>);
} // namespace livecut::spectral
