// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"
#include "engine/AllocationCounter.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <thread>
#include <vector>
namespace
{
void setResonance (LivecutAudioProcessor &processor, const char *id, float value)
{
    auto *parameter = processor.getParameterState ().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}
struct ResonanceHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition () const override
    {
        return info;
    }
    void at (double ppq, bool running = true)
    {
        info.setIsPlaying (running);
        info.setBpm (120.0);
        info.setTimeSignature (TimeSignature{4, 4});
        info.setPpqPosition (ppq);
        info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
        info.setBarCount (static_cast<std::int64_t> (std::floor (ppq / 4.0)));
    }
    PositionInfo info;
};
} // namespace
TEST_CASE ("Prepared Spectral adds its measured delay once to Live and Studio dry and bypass", "[processor][resonance][spectral]")
{
    const auto studio = GENERATE (false, true);
    const auto bypass = GENERATE (false, true);
    LivecutAudioProcessor processor;
    setResonance (processor, "mode", studio ? 1.0f : 0.0f);
    setResonance (processor, "resonanceSpectralEnabled", 1.0f);
    setResonance (processor, "wet", bypass ? 1.0f : 0.0f);
    processor.setNonRealtime (true);
    processor.prepareToPlay (8000.0, 137);
    const auto expectedDelay = (studio ? 4000 : 0) + 2048;
    REQUIRE (processor.getLatencySamples () == expectedDelay);
    REQUIRE (processor.getDisplayState ().spectralConfigured.load ());
    juce::MidiBuffer midi;
    bool exact = true;
    for (int first = 0; first < expectedDelay + 1000; first += 137)
    {
        const auto count = std::min (137, expectedDelay + 1000 - first);
        juce::AudioBuffer<float> buffer (2, count);
        buffer.clear ();
        if (first <= 37 && first + count > 37)
        {
            buffer.setSample (0, 37 - first, .75f);
            buffer.setSample (1, 37 - first, -.25f);
        }
        if (bypass)
            processor.processBlockBypassed (buffer, midi);
        else
            processor.processBlock (buffer, midi);
        for (int i = 0; i < count; ++i)
            exact &= buffer.getSample (0, i) == (first + i == expectedDelay + 37 ? .75f : 0.0f) &&
                     buffer.getSample (1, i) == (first + i == expectedDelay + 37 ? -.25f : 0.0f);
    }
    CHECK (exact);
    CHECK (processor.getDisplayState ().spectralFault.load () == livecut::spectral::Fault::none);
    processor.releaseResources ();
    CHECK (processor.getLatencySamples () == expectedDelay);
    processor.prepareToPlay (8000.0, 137);
    CHECK (processor.getLatencySamples () == expectedDelay);
}
TEST_CASE ("Spectral Capture stores heard output at exclusive end and retains warm-up metadata", "[processor][resonance][capture]")
{
    LivecutAudioProcessor processor;
    setResonance (processor, "resonanceSpectralEnabled", 1.0f);
    setResonance (processor, "minPhraseBars", 1.0f);
    setResonance (processor, "maxPhraseBars", 1.0f);
    setResonance (processor, "wet", 0.0f);
    processor.setNonRealtime (true);
    processor.prepareToPlay (8000.0, 16000);
    ResonanceHead head;
    processor.setPlayHead (&head);
    juce::MidiBuffer midi;
    std::vector<float> heard;
    for (const auto [first, count] : {std::pair{0, 16000}, std::pair{16000, 3000}})
    {
        head.at (first / 4000.0);
        juce::AudioBuffer<float> buffer (2, count);
        for (int i = 0; i < count; ++i)
        {
            buffer.setSample (0, i, static_cast<float> (first + i) / 19000.0f);
            buffer.setSample (1, i, -.2f);
        }
        processor.processBlock (buffer, midi);
        heard.insert (heard.end (), buffer.getReadPointer (0), buffer.getReadPointer (0) + count);
    }
    auto phrase = processor.getCaptureService ().rolling ().acquireLatest ();
    REQUIRE (phrase);
    REQUIRE (phrase.channel (0).size () == 16000);
    CHECK (std::memcmp (phrase.channel (0).data (), heard.data () + 2048, 16000 * sizeof (float)) == 0);
    CHECK (phrase.spectralWarmup ());
    CHECK (processor.getDisplayState ().hold.load () != livecut::engine::WetHold::State::held);
}

TEST_CASE ("Spectral Apply validates the complete prepared tuple and observed stopped transport", "[processor][resonance][apply]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (8000, 64);
    ResonanceHead head;
    head.at (0);
    processor.setPlayHead (&head);
    juce::AudioBuffer<float> buffer (2, 64);
    buffer.clear ();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    setResonance (processor, "resonanceSpectralEnabled", 1);
    processor.applyRequestedMode ();
    CHECK_FALSE (processor.activeSpectral ());
    CHECK (processor.reportedLatencySamples () == 0);
    CHECK (processor.modeState () == LivecutAudioProcessor::ModeState::pendingPlaying);
    head.at (.016, false);
    processor.processBlock (buffer, midi);
    processor.applyRequestedMode ();
    CHECK (processor.activeSpectral ());
    CHECK (processor.reportedLatencySamples () == 2048);
    setResonance (processor, "mode", 1);
    processor.applyRequestedMode ();
    CHECK (processor.modeState () == LivecutAudioProcessor::ModeState::pendingObservation);
    processor.setNonRealtime (true);
    processor.processBlock (buffer, midi);
    processor.applyRequestedMode ();
    CHECK (processor.activeSpectral ());
    CHECK (processor.activeMode () == livecut::ProcessingMode::studio);
    CHECK (processor.reportedLatencySamples () == 6048);
}

TEST_CASE ("A Spectral worker fault remains gated until same-configuration stopped replacement", "[processor][resonance][fault]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    livecut::spectral::TestHooks hooks;
    hooks.pause = true;
    LivecutAudioProcessor processor (&hooks);
    setResonance (processor, "resonanceSpectralEnabled", 1);
    processor.prepareToPlay (8000, 2048);
    ResonanceHead head;
    head.at (0, false);
    processor.setPlayHead (&head);
    juce::AudioBuffer<float> buffer (2, 2048);
    buffer.clear ();
    juce::MidiBuffer midi;
    std::size_t allocations = 0;
    {
        const livecut::engine::testing::AllocationCounter counter;
        processor.processBlock (buffer, midi);
        allocations = counter.count ();
    }
    CHECK (allocations == 0);
    REQUIRE (processor.getDisplayState ().spectralFault.load () == livecut::spectral::Fault::deadline);
    CHECK (processor.getDisplayState ().status.load () == livecut::DisplayState::Status::streamError);
    hooks.pause = false;
    juce::FloatVectorOperations::fill (buffer.getWritePointer (0), .4f, 2048);
    processor.processBlock (buffer, midi);
    CHECK (buffer.getMagnitude (0, 2048) == 0);
    CHECK (processor.getDisplayState ().spectralFault.load () == livecut::spectral::Fault::deadline);
    processor.applyRequestedMode ();
    CHECK (processor.modeState () == LivecutAudioProcessor::ModeState::active);
    CHECK (processor.getDisplayState ().spectralFault.load () == livecut::spectral::Fault::none);
    CHECK (processor.activeSpectral ());
    CHECK (processor.reportedLatencySamples () == 2048);
    processor.setNonRealtime (true);
    buffer.clear ();
    processor.processBlock (buffer, midi);
    CHECK (processor.getDisplayState ().spectralFault.load () == livecut::spectral::Fault::none);
}

TEST_CASE ("Release cancels an actual blocked offline callback before AU-style recursive reprepare", "[processor][resonance][lifecycle]")
{
    livecut::spectral::TestHooks hooks;
    hooks.pause = true;
    LivecutAudioProcessor processor (&hooks);
    setResonance (processor, "resonanceSpectralEnabled", 1);
    processor.setNonRealtime (true);
    processor.prepareToPlay (8000, 4096);
    auto audio = std::async (std::launch::async,
                             [&]
                             {
                                 const juce::ScopedLock hostCallback (processor.getCallbackLock ());
                                 juce::AudioBuffer<float> buffer (2, 4096);
                                 buffer.clear ();
                                 juce::MidiBuffer midi;
                                 processor.processBlock (buffer, midi);
                                 return buffer.getMagnitude (0, 4096);
                             });
    const auto until = std::chrono::steady_clock::now () + std::chrono::seconds (2);
    while (hooks.offlineWaitsObserved.load () == 0 && std::chrono::steady_clock::now () < until)
        std::this_thread::yield ();
    const auto waited = hooks.offlineWaitsObserved.load () > 0;
    processor.releaseResources (); // cancellation must precede waiting for hostCallback
    CHECK (waited);
    REQUIRE (audio.wait_for (std::chrono::seconds (2)) == std::future_status::ready);
    CHECK (audio.get () == 0);
    CHECK (processor.reportedLatencySamples () == 2048);
    hooks.pause = false;
    {
        const juce::ScopedLock auOuter (processor.getCallbackLock ());
        processor.prepareToPlay (16000, 64);
    }
    CHECK (processor.modeState () == LivecutAudioProcessor::ModeState::active);
    CHECK (processor.activeSpectral ());
    CHECK (processor.reportedLatencySamples () == 4096);
}

TEST_CASE ("A newer Spectral generation supersedes an obsolete preparation without republishing it", "[processor][resonance][lifecycle]")
{
    LivecutAudioProcessor processor;
    setResonance (processor, "resonanceSpectralEnabled", 1);
    struct Reprepare
    {
        LivecutAudioProcessor *processor;
        bool called = false;
    } context{&processor};
    {
        const livecut::engine::testing::NextAllocation interleave (
            [] (void *opaque)
            {
                auto &c = *static_cast<Reprepare *> (opaque);
                c.called = true;
                c.processor->prepareToPlay (16000, 64);
            },
            &context);
        processor.prepareToPlay (8000, 64);
    }
    REQUIRE (context.called);
    CHECK (processor.activeSpectral ());
    CHECK (processor.reportedLatencySamples () == 4096);
    CHECK (processor.modeState () == LivecutAudioProcessor::ModeState::active);
    CHECK_FALSE (processor.isSuspended ());
}

TEST_CASE ("Spectral localMix zero delays exact prior processing and original smoothed controls", "[processor][resonance][alignment]")
{
    const auto studio = GENERATE (false, true);
    LivecutAudioProcessor prior, spectral;
    for (auto *p : {&prior, &spectral})
    {
        setResonance (*p, "mode", studio ? 1 : 0);
        setResonance (*p, "wet", .37f);
        setResonance (*p, "outputTrim", 3);
        setResonance (*p, "crusher", 1);
        setResonance (*p, "minBits", 8);
    }
    setResonance (spectral, "resonanceSpectralEnabled", 1);
    setResonance (spectral, "resonanceSpectralMix", 0);
    prior.prepareToPlay (8000, 137);
    spectral.prepareToPlay (8000, 137);
    spectral.setNonRealtime (true);
    ResonanceHead priorHead, spectralHead;
    prior.setPlayHead (&priorHead);
    spectral.setPlayHead (&spectralHead);
    juce::MidiBuffer midi;
    std::vector<float> expected, actual;
    for (int first = 0; first < 18000; first += 137)
    {
        const auto count = std::min (137, 18000 - first);
        const auto ppq = first < 6850 ? first / 4000.0 : (first - 6850) / 4000.0; // an explicit epoch boundary
        priorHead.at (ppq);
        spectralHead.at (ppq);
        if (first == 4110)
            for (auto *p : {&prior, &spectral})
            {
                setResonance (*p, "wet", .81f);
                setResonance (*p, "outputTrim", -3);
            }
        juce::AudioBuffer<float> a (2, count);
        for (int i = 0; i < count; ++i)
        {
            a.setSample (0, i, std::sin (.013f * (first + i)));
            a.setSample (1, i, std::cos (.017f * (first + i)));
        }
        juce::AudioBuffer<float> b (a);
        prior.processBlock (a, midi);
        spectral.processBlock (b, midi);
        expected.insert (expected.end (), a.getReadPointer (0), a.getReadPointer (0) + count);
        actual.insert (actual.end (), b.getReadPointer (0), b.getReadPointer (0) + count);
    }
    CHECK (std::all_of (actual.begin (), actual.begin () + 2048, [] (float x) { return x == 0; }));
    CHECK (std::memcmp (actual.data () + 2048, expected.data (), (expected.size () - 2048) * sizeof (float)) == 0);
}

TEST_CASE ("Owned Spectral display values follow the heard source rather than the current Cutter", "[processor][resonance][display]")
{
    LivecutAudioProcessor prior, spectral;
    setResonance (spectral, "resonanceSpectralEnabled", 1);
    setResonance (spectral, "resonanceSpectralMix", 0);
    for (auto *p : {&prior, &spectral})
    {
        setResonance (*p, "minPhraseBars", 1);
        setResonance (*p, "maxPhraseBars", 1);
        p->prepareToPlay (8000, 1);
    }
    spectral.setNonRealtime (true);
    ResonanceHead priorHead, spectralHead;
    prior.setPlayHead (&priorHead);
    spectral.setPlayHead (&spectralHead);
    struct View
    {
        double position;
        livecut::engine::Resonator::State state;
        livecut::engine::EffectRoute route;
    };
    std::vector<View> expected (6500);
    bool aligned = true;
    juce::AudioBuffer<float> a (2, 1), b (2, 1);
    juce::MidiBuffer midi;
    for (int n = 0; n < 6500; ++n)
    {
        if (n == 1000)
            for (auto *p : {&prior, &spectral})
            {
                setResonance (*p, "resonanceEnabled", 1);
                for (const auto *id : {"resonanceWeight1", "resonanceWeight2", "resonanceWeight3", "resonanceWeight4"})
                    setResonance (*p, id, 0);
            }
        priorHead.at (n / 4000.0);
        spectralHead.at (n / 4000.0);
        a.clear ();
        b.clear ();
        prior.processBlock (a, midi);
        spectral.processBlock (b, midi);
        const auto &before = prior.getDisplayState ();
        expected[static_cast<std::size_t> (n)] = {before.position.load (), before.resonatorState.load (), before.effectRoute.load ()};
        if (n >= 2048)
        {
            const auto &want = expected[static_cast<std::size_t> (n - 2048)];
            const auto &heard = spectral.getDisplayState ();
            aligned &= std::abs (heard.position.load () - want.position) < 1e-9 && heard.resonatorState.load () == want.state &&
                       heard.effectRoute.load () == want.route;
        }
    }
    CHECK (aligned);
}
