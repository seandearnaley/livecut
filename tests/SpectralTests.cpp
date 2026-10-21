// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/AllocationCounter.h"
#include "spectral/SpectralStage.h"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <mutex>
#include <numbers>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace livecut::spectral;
namespace
{
void require (bool condition, const char *message)
{
    INFO (message);
    REQUIRE (condition);
}
Packet packet (std::int64_t n, std::uint64_t epoch = 1)
{
    Packet p;
    p.sourceFrame = n;
    p.epoch = epoch;
    p.output.running = true;
    p.output.quarterNotesPerSample = 1. / 24000;
    return p;
}
void good (const Result &r)
{
    require (r.fault == Fault::none && !r.recoveryRequired, "unexpected stage fault");
}
void identityAndEvents ()
{
    Stage s ({48000, true});
    require (s.latency () == 8192 && s.guardFrames () == 4096, "reference4096 guard4H delay");
    const auto impulse = 12003, exclusiveEnd = 13001;
    int completions = 0;
    double maxError = 0;
    bool seen = false;
    s.beginCallback (Execution::offline);
    for (int n = 0; n < exclusiveEnd + s.latency (); ++n)
    {
        auto p = packet (n);
        p.processed = {n == impulse ? 1.f : 0.f, n == impulse ? -.5f : 0.f};
        p.raw = {static_cast<float> (n) / 100000.f, -static_cast<float> (n) / 100000.f};
        p.output.globalMix = (n % 19) / 19.f;
        p.output.hold = (n % 7) == 0;
        if (n == exclusiveEnd - 1)
        {
            p.before.append ({EventKind::discontinuity});
            p.before.append ({EventKind::phraseCompleted});
            p.before.append ({EventKind::barStarted, false, 43});
            p.before.append ({EventKind::phraseStarted, true, 0, 16});
            p.after.append ({EventKind::phraseCompleted});
        }
        auto r = s.process (p);
        good (r);
        if (n < s.latency ())
        {
            require (!r.valid, "initial delay must be invalid");
            continue;
        }
        require (r.valid && r.packet.sourceFrame == n - s.latency (), "measured exact packet delay");
        const auto source = r.packet.sourceFrame;
        require (r.packet.raw.left == static_cast<float> (source) / 100000.f, "original dry aligned");
        require (r.packet.output.globalMix == (source % 19) / 19.f, "owned scalar settings aligned");
        const auto expected = source == impulse ? 1. : 0.;
        if (source > 8192)
            maxError = std::max (maxError, std::abs (r.spectralWet.left - expected));
        if (source == impulse)
        {
            seen = true;
            require (r.availability == Availability::ready, "impulse must be Ready");
        }
        if (source == exclusiveEnd - 1)
        {
            require (r.packet.before.count == 4 && r.packet.after.count == 1, "all bounded events retained");
            require (r.packet.before.items[0].kind == EventKind::discontinuity &&
                         r.packet.before.items[1].kind == EventKind::phraseCompleted && r.packet.before.items[2].bar == 43 &&
                         r.packet.before.items[3].phraseQuarterNotes == 16,
                     "event order/payload identity");
            require (r.packet.sourceFrame + 1 == exclusiveEnd, "completion exclusive end unchanged");
            ++completions;
        }
    }
    s.endCallback ();
    require (seen && completions == 1 && maxError < 3e-7, "unity reconstruction and exclusive-end completion");
}
void rapidEpochs ()
{
    Stage s ({48000, true});
    s.beginCallback (Execution::offline);
    const int boundary = 15001, churn = 1300;
    int oldSuffix = 0, waiting = 0, ready = 0;
    for (int n = 0; n < 44000; ++n)
    {
        auto epoch = n < boundary           ? 1ull
                     : n < boundary + churn ? static_cast<unsigned long long> (2 + n - boundary)
                                            : static_cast<unsigned long long> (2 + churn);
        auto p = packet (n, epoch);
        p.processed = {n == boundary - 2 ? 1.f : 0.f, 0};
        p.raw = {.31f, -.42f};
        auto r = s.process (p);
        good (r);
        if (!r.valid)
            continue;
        const auto source = r.packet.sourceFrame;
        require (r.packet.epoch == (source < boundary           ? 1ull
                                    : source < boundary + churn ? static_cast<unsigned long long> (2 + source - boundary)
                                                                : static_cast<unsigned long long> (2 + churn)),
                 "old/new output epoch identity");
        require (r.packet.raw.left == .31f, "explicit dry remains aligned");
        if (source == boundary - 2)
        {
            require (std::abs (r.spectralWet.left - 1) < 3e-7 && r.availability == Availability::ready,
                     "old admitted suffix retained once");
            ++oldSuffix;
        }
        if (source >= boundary && source < boundary + churn)
        {
            require (r.availability == Availability::waiting && r.spectralWet.left == 0, "short epochs deliberately Waiting");
            ++waiting;
        }
        if (source > boundary + churn + 6000 && r.availability == Availability::ready)
            ++ready;
    }
    s.endCallback ();
    require (oldSuffix == 1 && waiting == churn && ready > 1000, "long/short/long recovery without replacing stage");
}
void helperPolicy ()
{
    Result r;
    r.valid = true;
    r.packet.output.running = true;
    r.packet.processed = {.75f, -.5f};
    r.packet.raw = {.2f, .3f};
    r.packet.spectral.localMix = .25f;
    r.spectralWet = {0, 0};
    r.availability = Availability::waiting;
    require (r.localWet ().left == .5625f, "Waiting keeps only explicitly selected processed amount");
    require (!r.holdEligible () && r.captureHasWarmup (), "inner Hold rejects, heard Capture annotates warm-up");
    r.packet.output.bypass = true;
    require (!r.holdEligible () && !r.captureHasWarmup (), "bypass exempts heard Capture only");
    r.packet.spectral.localMix = 0;
    require (r.localWet ().left == .75f && r.holdEligible () && !r.captureHasWarmup (), "localMix0 exact and eligible");
    r.packet.output.bypass = false;
    r.packet.spectral.localMix = 1;
    r.packet.output.globalMix = 0;
    require (r.captureHasWarmup (), "Capture annotation describes path state independent gain");
    r.packet.output.running = false;
    require (!r.captureHasWarmup (), "stopped frames do not annotate a captured phrase");
    r.packet.output.running = true;
    r.availability = Availability::priming;
    require (!r.holdEligible (), "Priming cannot publish held candidate");
    r.availability = Availability::ready;
    require (r.holdEligible (), "Ready eligible");
}
void disabled ()
{
    Stage s ({48000, false});
    require (s.latency () == 0, "disabled declares zero added delay");
    s.beginCallback (Execution::realtime);
    for (int n = 0; n < 100; ++n)
    {
        auto p = packet (n);
        p.processed = {.125f, -.75f};
        p.raw = {.1f, .2f};
        auto r = s.process (p);
        good (r);
        require (r.valid && r.packet.sourceFrame == n && r.localWet ().right == -.75f && r.availability == Availability::disabled,
                 "unconfigured exact prior processed path");
    }
    s.endCallback ();
}
void contractFaults ()
{
    {
        Stage s ({48000, true});
        s.beginCallback (Execution::offline);
        auto p = packet (0);
        for (int i = 0; i < 5; ++i)
            p.before.append ({});
        auto r = s.process (p);
        require (r.fault == Fault::packetContract && r.recoveryRequired && !r.valid, "before overflow latches contract fault");
        p = packet (0);
        require (s.process (p).fault == Fault::packetContract, "fault cannot silently recover");
        s.endCallback ();
    }
    {
        Stage s ({48000, true});
        s.beginCallback (Execution::offline);
        auto p = packet (0);
        p.after.append ({EventKind::phraseCompleted});
        p.after.append ({EventKind::phraseCompleted});
        require (s.process (p).fault == Fault::packetContract, "after overflow explicit");
        s.endCallback ();
    }
    {
        Stage s ({48000, true});
        s.beginCallback (Execution::offline);
        auto p = packet (0);
        p.spectral.smearMs = 2001;
        require (s.process (p).fault == Fault::packetContract, "invalid control explicit");
        s.endCallback ();
    }
    {
        Stage s ({48000, true});
        s.beginCallback (Execution::offline);
        good (s.process (packet (0, 2)));
        require (s.process (packet (1, 1)).fault == Fault::packetContract, "epoch cannot go backwards");
        s.endCallback ();
    }
    {
        Stage s ({48000, true});
        require (s.process (packet (0)).fault == Fault::callbackContract, "callback lifecycle explicit");
    }
}

void boundedWorkerFaults ()
{
    {
        TestHooks hooks;
        hooks.pause = true;
        hooks.inputCapacityOverride = 1;
        Stage s ({48000, true, &hooks});
        s.beginCallback (Execution::realtime);
        Result r;
        for (int n = 0; n < 200; ++n)
        {
            r = s.process (packet (n));
            if (r.recoveryRequired)
                break;
        }
        require (r.fault == Fault::inputAdmission && !r.valid, "bounded input overflow cannot overwrite/substitute");
        s.endCallback ();
    }
    {
        TestHooks hooks;
        hooks.pause = true;
        Stage s ({48000, true, &hooks});
        s.beginCallback (Execution::realtime);
        Result r;
        for (int n = 0; n <= s.guardFrames (); ++n)
        {
            r = s.process (packet (n));
            if (r.recoveryRequired)
                break;
        }
        require (r.fault == Fault::deadline && !r.valid, "missing guard deadline is explicit fault");
        s.endCallback ();
    }
    {
        TestHooks hooks;
        hooks.corruptOutput = true;
        Stage s ({48000, true, &hooks});
        s.beginCallback (Execution::offline);
        Result r;
        for (int n = 0; n < 9000; ++n)
        {
            r = s.process (packet (n));
            if (r.recoveryRequired)
                break;
        }
        require (r.fault == Fault::staleResult && !r.valid, "worker output tag mismatch cannot escape");
        s.endCallback ();
    }
}
void cancellationAndReentry ()
{
    TestHooks hooks;
    hooks.pause = true;
    auto stage = std::make_unique<Stage> (Preparation{48000, true, &hooks});
    auto stop = stage->stopHandle ();
    std::mutex callbackGuard, lifecycleGate;
    auto audio = std::async (std::launch::async,
                             [&]
                             {
                                 std::lock_guard guard (callbackGuard);
                                 stage->beginCallback (Execution::offline);
                                 Result r;
                                 for (int n = 0; n < 30000; ++n)
                                 {
                                     r = stage->process (packet (n));
                                     if (r.recoveryRequired)
                                         break;
                                 }
                                 stage->endCallback ();
                                 return r.fault;
                             });
    const auto until = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (stage->offlineWaits () == 0 && std::chrono::steady_clock::now () < until)
        std::this_thread::yield ();
    require (stage->offlineWaits () > 0, "actual offline callback reached a blocking wait");
    {
        std::lock_guard gate (lifecycleGate);
        stop.requestStop ();
        std::unique_ptr<Stage> retired;
        {
            std::lock_guard guard (callbackGuard);
            retired = std::move (stage);
        }
        retired.reset ();
    }
    require (audio.wait_for (std::chrono::seconds (2)) == std::future_status::ready,
             "offline cancellation wakes before lifecycle callback exclusion");
    require (audio.get () == Fault::stopped, "cancelled callback gates stopped explicitly");
    stop.requestStop ();
    require (stop.requested (), "retained safe cancellation handle survives Stage destruction");
    Stage replacement ({48000, true});
    replacement.beginCallback (Execution::offline);
    for (int n = 0; n < 16000; ++n)
        good (replacement.process (packet (n)));
    replacement.endCallback ();
    const auto waits = replacement.offlineWaits ();
    std::array<Result, 64> realtimeResults;
    std::size_t callbackAllocations = 0;
    {
        livecut::engine::testing::AllocationCounter allocations;
        replacement.beginCallback (Execution::realtime);
        for (std::size_t i = 0; i < realtimeResults.size (); ++i)
            realtimeResults[i] = replacement.process (packet (16000 + static_cast<int> (i)));
        replacement.endCallback ();
        callbackAllocations = allocations.count ();
    }
    for (const auto &result : realtimeResults)
        good (result);
    require (callbackAllocations == 0, "replacement realtime callback cannot allocate");
    require (replacement.offlineWaits () == waits, "offline-to-realtime callback cannot wait");
    replacement.beginCallback (Execution::offline);
    for (int n = 16064; n < 20000; ++n)
        good (replacement.process (packet (n)));
    replacement.endCallback ();
    require (replacement.fault () == Fault::none, "realtime-to-offline reentry uses same ordered worker");
}
void transitionAudio ()
{
    Stage stage ({48000, true});
    const int freezeAt = 20003, releaseAt = 35019, silenceAt = 50000;
    std::vector<float> left (75000), right (left.size ());
    stage.beginCallback (Execution::offline);
    for (int n = 0; n < static_cast<int> (left.size ()) + stage.latency (); ++n)
    {
        auto p = packet (n);
        const auto angle = 2 * std::numbers::pi * (n >= 23000 && n < releaseAt ? 330 : 440) * n / 48000.;
        const auto input = n < silenceAt ? .35f * static_cast<float> (std::sin (angle)) : 0.f;
        p.processed = {input, -input};
        p.spectral.freeze = n >= freezeAt && n < releaseAt;
        p.spectral.smearMs = n >= releaseAt ? 180 : 0;
        auto r = stage.process (p);
        good (r);
        if (r.valid && r.packet.sourceFrame < static_cast<int> (left.size ()))
        {
            left[static_cast<std::size_t> (r.packet.sourceFrame)] = r.spectralWet.left;
            right[static_cast<std::size_t> (r.packet.sourceFrame)] = r.spectralWet.right;
            require (std::abs (r.spectralWet.left + r.spectralWet.right) < 2e-7, "linked phase retains anti-phase stereo");
        }
    }
    stage.endCallback ();
    double frozenEnergy = 0, smearTail = 0, lateTail = 0;
    for (int n = 25000; n < 32000; ++n)
        frozenEnergy += left[static_cast<std::size_t> (n)] * left[static_cast<std::size_t> (n)];
    for (int n = 55000; n < 57000; ++n)
        smearTail += left[static_cast<std::size_t> (n)] * left[static_cast<std::size_t> (n)];
    for (int n = 70000; n < 72000; ++n)
        lateTail += left[static_cast<std::size_t> (n)] * left[static_cast<std::size_t> (n)];
    require (std::sqrt (frozenEnergy / 7000) > .23 && std::sqrt (frozenEnergy / 7000) < .26, "Freeze continues captured tonal energy");
    std::printf ("  transition energies frozen=%.9g smear=%.9g late=%.9g ratio=%.9g\n", frozenEnergy, smearTail, lateTail,
                 lateTail / smearTail);
    require (smearTail > 0 && lateTail < smearTail, "Smear is an actual decaying magnitude tail after source silence");
    int crossings = 0, first = -1, last = -1;
    for (int n = 25001; n < 32000; ++n)
        if (left[static_cast<std::size_t> (n - 1)] <= 0 && left[static_cast<std::size_t> (n)] > 0)
        {
            if (first < 0)
                first = n;
            last = n;
            ++crossings;
        }
    const auto measured = 48000. * (crossings - 1) / (last - first);
    require (std::abs (1200 * std::log2 (measured / 440)) < 1, "Freeze keeps phase-continuous frequency");
    Wola smear (48000, 4096);
    smear.setControls ({false, 180});
    for (int n = 0; n < 30720; ++n)
        smear.process ({n < 20000 ? .3f * static_cast<float> (std::sin (2 * std::numbers::pi * 375 * n / 48000.)) : 0.f, 0});
    const auto magnitude = smear.diagnostics ().dominantMagnitude;
    for (int n = 0; n < 20 * smear.configuration ().hop; ++n)
        smear.process ({0, 0});
    const auto expectedMagnitude = magnitude * std::exp (-20. * smear.configuration ().hop / (48000 * .180));
    require (magnitude > 0 && std::abs (smear.diagnostics ().dominantMagnitude / expectedMagnitude - 1) < 1e-10,
             "Smear temporal magnitude obeys declared exponential time constant");
    Wola core (48000, 4096);
    for (int n = 0; n < freezeAt; ++n)
        core.process ({.3f * static_cast<float> (std::sin (2 * std::numbers::pi * 440 * n / 48000.)), 0});
    core.setControls ({true, 0});
    const auto expectedEnd = (freezeAt / core.configuration ().hop + 1) * core.configuration ().hop;
    for (int n = freezeAt; n < expectedEnd; ++n)
        core.process ({.3f, 0});
    require (core.diagnostics ().freezeCaptureEnd == expectedEnd, "hold Freeze captures exactly at next eligible analysis");
    core.setControls ({false, 0});
    for (int n = 0; n < core.configuration ().hop; ++n)
        core.process ({0, 0});
    require (!core.diagnostics ().frozen, "Freeze release returns to live analysis");
}

} // namespace
TEST_CASE ("Spectral worker preserves measured delay, unity, ordered events and exclusive end", "[spectral]")
{
    identityAndEvents ();
}
TEST_CASE ("Spectral worker preserves old suffix through rapid epochs and returns to Ready", "[spectral]")
{
    rapidEpochs ();
}
TEST_CASE ("Spectral routes keep explicit local dry and consumer eligibility", "[spectral]")
{
    helperPolicy ();
}
TEST_CASE ("Unconfigured Spectral preserves the exact prior path", "[spectral]")
{
    disabled ();
}
TEST_CASE ("Spectral event, control and callback bounds fault explicitly", "[spectral]")
{
    contractFaults ();
}

TEST_CASE ("Spectral worker faults bound admission, deadlines and stale output", "[spectral]")
{
    boundedWorkerFaults ();
}
TEST_CASE ("Spectral offline cancellation permits safe destruction and realtime reentry", "[spectral]")
{
    cancellationAndReentry ();
}
TEST_CASE ("Spectral Freeze and Smear retain stereo phase and declared magnitude time", "[spectral]")
{
    transitionAudio ();
}

TEST_CASE ("Spectral epoch closure retains previous Freeze and Smear controls", "[spectral][epochs][controls]")
{
    const bool freeze = GENERATE (false, true);
    {
        INFO ((freeze ? "Freeze release at seek" : "Smear removal at seek"));
        Stage actual ({ 8000, true }), reference ({ 8000, true });
        actual.beginCallback (Execution::offline);
        reference.beginCallback (Execution::offline);
        constexpr int boundary = 9003;
        double suffixError = 0.0;
        int suffixFrames = 0;
        for (int n = 0; n < boundary + actual.latency(); ++n)
        {
            auto original = packet (n);
            const auto frequency = n < 4000 ? 440.0 : 330.0;
            const auto sample = n < boundary ? .3f * static_cast<float> (std::sin (2 * std::numbers::pi * frequency * n / 8000)) : 0.0f;
            original.processed = { sample, -sample };
            original.spectral = { freeze, freeze ? 0.0 : 180.0, 1.0f };
            auto changed = original;
            if (n >= boundary)
            {
                changed.epoch = 2;
                changed.spectral = { false, 0.0, 1.0f };
            }
            const auto expected = reference.process (original);
            const auto result = actual.process (changed);
            good (expected); good (result);
            if (result.valid && result.packet.sourceFrame >= boundary - actual.configuration().size && result.packet.sourceFrame < boundary)
            {
                suffixError = std::max (suffixError, static_cast<double> (std::abs (result.spectralWet.left - expected.spectralWet.left)));
                require (result.packet.epoch == 1, "old suffix keeps source epoch");
                ++suffixFrames;
            }
        }
        actual.endCallback(); reference.endCallback();
        require (suffixFrames == actual.configuration().size, "complete old suffix compared");
        INFO (suffixError);
        require (suffixError < 3e-7, "new epoch controls must not alter admitted old suffix");
    }
}

TEST_CASE ("An uncaptured Freeze pulse does not mute an admitted epoch suffix", "[spectral][epochs][controls]")
{
    Stage actual ({ 8000, true }), reference ({ 8000, true });
    actual.beginCallback (Execution::offline); reference.beginCallback (Execution::offline);
    constexpr int boundary = 9003; // pending pulse is not on the 256-frame analysis lattice
    double suffixError = 0.0;
    for (int n = 0; n < boundary + actual.latency(); ++n)
    {
        auto original = packet (n, n < boundary ? 1 : 2);
        const auto sample = n < boundary ? .3f * static_cast<float> (std::sin (2 * std::numbers::pi * 330 * n / 8000)) : 0.0f;
        original.processed = { sample, -sample };
        auto pulse = original; pulse.spectral.freeze = n == boundary - 1;
        const auto expected = reference.process (original), result = actual.process (pulse);
        good (expected); good (result);
        if (result.valid && result.packet.sourceFrame >= boundary - actual.configuration().size && result.packet.sourceFrame < boundary)
        {
            require (result.availability == Availability::ready, "old admitted output remains Ready");
            suffixError = std::max (suffixError, static_cast<double> (std::abs (result.spectralWet.left - expected.spectralWet.left)));
        }
    }
    actual.endCallback(); reference.endCallback();
    INFO (suffixError);
    require (suffixError < 3e-7, "a Freeze request without any eligible capture must leave live old suffix intact");
}
