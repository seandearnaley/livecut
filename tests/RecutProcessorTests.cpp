// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"
#include "HostSync.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "engine/AllocationCounter.h"
#include <cmath>
#include <cstring>
#include <vector>
#include <thread>

namespace
{
struct RecutHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return available ? juce::Optional<PositionInfo> { info } : juce::Optional<PositionInfo> {}; }
    PositionInfo info;
    bool available = true;
    void at (double beats, bool running = true)
    {
        info.setIsPlaying (running);
        info.setBpm (120.0);
        info.setTimeSignature (TimeSignature { 4, 4 });
        info.setPpqPosition (beats);
        info.setPpqPositionOfLastBarStart (std::floor (beats / 4.0) * 4.0);
        info.setBarCount (static_cast<std::int64_t> (std::floor (beats / 4.0)));
    }
};
void setRecut (LivecutAudioProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

TEST_CASE ("Studio capture completes before delayed bar entry across original context splits", "[processor][recut][capture]")
{
    LivecutAudioProcessor processor;
    setRecut (processor, "mode", 1.0f);
    setRecut (processor, "wet", 0.0f);
    setRecut (processor, "minPhraseBars", 1.0f);
    setRecut (processor, "maxPhraseBars", 1.0f);
    processor.prepareToPlay (8000.0, 16000);
    RecutHead head;
    processor.setPlayHead (&head);
    juce::MidiBuffer midi;
    std::vector<float> heard;
    for (const auto [first, frames] : { std::pair { 0, 16000 }, std::pair { 16000, 11000 } })
    {
        head.at (first / 4000.0);
        if (first != 0)
            setRecut (processor, "hold", 1.0f);
        juce::AudioBuffer<float> buffer (2, frames);
        for (int i = 0; i < frames; ++i)
        {
            const auto input = static_cast<float> (first + i) / 27000.0f;
            buffer.setSample (0, i, input);
            buffer.setSample (1, i, -input);
        }
        processor.processBlock (buffer, midi);
        heard.insert (heard.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + frames);
    }
    // Complete at physical20000, before the new bar's first sample selects Hold.
    // Original context0 ends exactly on the phrase line at physical20000,
    // offset4000 inside the second host buffer, before its next bar starts.
    CHECK (processor.getDisplayState().hold.load() == livecut::engine::WetHold::State::held);
    auto phrase = processor.getCaptureService().rolling().acquireLatest();
    REQUIRE (phrase);
    REQUIRE (phrase.channel (0).size() == 16000);
    CHECK (std::memcmp (phrase.channel (0).data(), heard.data() + 4000, 16000 * sizeof (float)) == 0);
    phrase = {};
    head.at (6.75, false);
    juce::AudioBuffer<float> stopped (2, 5000);
    stopped.clear();
    processor.processBlock (stopped, midi);
    CHECK_FALSE (processor.getCutter().isCutting());
    CHECK (processor.getCaptureService().rolling().hasLatest());
}

TEST_CASE ("Short loops retain a single Studio fill and do not cross raw transport epochs", "[processor][recut][epochs]")
{
    LivecutAudioProcessor processor;
    setRecut (processor, "mode", 1.0f);
    setRecut (processor, "wet", 0.0f);
    processor.prepareToPlay (8000.0, 1000);
    RecutHead head;
    processor.setPlayHead (&head);
    juce::MidiBuffer midi;
    bool exact = true;
    for (int first = 0; first < 12000; first += 1000)
    {
        head.at (0.0); // a 125ms loop, much shorter than the500ms lead
        juce::AudioBuffer<float> buffer (2, 1000);
        for (int i = 0; i < 1000; ++i)
        {
            buffer.setSample (0, i, static_cast<float> (first + i + 1) / 12000.0f);
            buffer.setSample (1, i, 0.0f);
        }
        processor.processBlock (buffer, midi);
        for (int i = 0; i < 1000; ++i)
            exact &= buffer.getSample (0, i) == (first + i < 4000 ? 0.0f : static_cast<float> (first + i - 4000 + 1) / 12000.0f);
        CHECK (processor.getDisplayState().status.load() != livecut::DisplayState::Status::streamError);
    }
    CHECK (exact);
}

TEST_CASE ("Unsupported preparation gates explicitly and valid reprepare recovers", "[processor][recut][prepare]")
{
    LivecutAudioProcessor processor;
    processor.prepareToPlay (1000.0, 32);
    CHECK (processor.getDisplayState().status.load() == livecut::DisplayState::Status::preparationError);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    for (int channel = 0; channel < 2; ++channel)
        juce::FloatVectorOperations::fill (buffer.getWritePointer (channel), .5f, 32);
    processor.processBlock (buffer, midi);
    CHECK (buffer.getMagnitude (0, 32) == 0.0f);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (0), .5f, 32);
    processor.processBlockBypassed (buffer, midi);
    CHECK (buffer.getMagnitude (0, 32) == 0.0f);
    processor.prepareToPlay (8000.0, 32);
    for (int channel = 0; channel < 2; ++channel)
        juce::FloatVectorOperations::fill (buffer.getWritePointer (channel), .5f, 32);
    processor.processBlock (buffer, midi);
    CHECK (buffer.getSample (0, 0) == .5f);
}
}

TEST_CASE ("Recut parameters append with neutral defaults and explicit nonautomatable mode", "[processor][recut][parameters]")
{
    LivecutAudioProcessor processor;
    const auto& parameters = processor.getParameters();
    for (const auto* id : { "mode", "attackAlign", "attackThreshold", "reverseChance" })
        REQUIRE (processor.getParameterState().getParameter (id) != nullptr);
    CHECK_FALSE (processor.getParameterState().getParameter ("mode")->isAutomatable());
    CHECK (dynamic_cast<juce::AudioProcessorParameterWithID*> (parameters[42])->paramID == "mode");
    CHECK (processor.getParameterState().getRawParameterValue ("mode")->load() == 0.0f);
    CHECK (processor.getParameterState().getRawParameterValue ("attackAlign")->load() == 0.0f);
    CHECK (processor.getParameterState().getRawParameterValue ("reverseChance")->load() == 0.0f);
}

TEST_CASE ("Studio delays raw dry and unity bypass by exactly half a second", "[processor][recut][latency]")
{
    const auto block = GENERATE (1, 137, 512);
    const auto bypass = GENERATE (false, true);
    LivecutAudioProcessor processor;
    setRecut (processor, "mode", 1.0f);
    setRecut (processor, "wet", bypass ? 1.0f : 0.0f);
    setRecut (processor, "outputTrim", bypass ? 12.0f : 0.0f);
    processor.prepareToPlay (8000.0, block);
    REQUIRE (processor.getLatencySamples() == 4000);
    juce::MidiBuffer midi;
    bool exact = true;
    std::size_t allocations = 0;
    for (int done = 0; done < 6000; done += block)
    {
        const auto frames = std::min (block, 6000 - done);
        juce::AudioBuffer<float> buffer (2, frames);
        buffer.clear();
        if (done <= 37 && done + frames > 37)
        {
            buffer.setSample (0, 37 - done, .75f);
            buffer.setSample (1, 37 - done, -.25f);
        }
        {
            const livecut::engine::testing::AllocationCounter counter;
            if (bypass)
                processor.processBlockBypassed (buffer, midi);
            else
                processor.processBlock (buffer, midi);
            allocations += counter.count();
        }
        for (int i = 0; i < frames; ++i)
            exact &= buffer.getSample (0, i) == (done + i == 4037 ? .75f : 0.0f)
                  && buffer.getSample (1, i) == (done + i == 4037 ? -.25f : 0.0f);
    }
    CHECK (exact);
    CHECK (allocations == 0);
}

TEST_CASE ("Continuous host PPQ preserves the input epoch through a meter-only change", "[processor][recut][meter]")
{
    const auto explicitBars = GENERATE (false, true);
    livecut::StreamResources resources (8000.0, livecut::ProcessingMode::studio);
    std::uint64_t epochCounter = 1;
    RecutHead head;
    const auto context = [&] (double ppq, int meter)
    {
        head.at (ppq);
        head.info.setTimeSignature (juce::AudioPlayHead::TimeSignature { meter, 4 });
        if (explicitBars)
        {
            head.info.setBarCount (static_cast<std::int64_t> (std::floor (ppq / meter)));
            head.info.setPpqPositionOfLastBarStart (std::floor (ppq / meter) * meter);
        }
        else
        {
            head.info.setBarCount ({});
            head.info.setPpqPositionOfLastBarStart ({});
        }
        const auto sync = livecut::hostSync (head.info);
        REQUIRE (sync);
        livecut::RenderContext result;
        result.running = true;
        result.timing = sync->timing;
        result.position = sync->position;
        result.ppq = ppq;
        return result;
    };
    REQUIRE (resources.appendContext (context (8.0, 4), 4000, epochCounter));
    const auto originalEpoch = resources.inputEpoch;
    bool ingested = true;
    for (int i = 0; i < 4000; ++i)
    {
        ingested &= resources.cutter.ingestInputFrame (resources.inputFrame, { 0.0f, 0.0f });
        ingested &= resources.detector.process (resources.inputFrame, 0.0f, 0.0f);
        ++resources.inputFrame;
    }
    REQUIRE (ingested);
    REQUIRE (resources.appendContext (context (9.0, 3), 100, epochCounter));
    CHECK (resources.inputEpoch == originalEpoch);
}

TEST_CASE ("Studio uses original delayed settings and tempo throughout split output buffers", "[processor][recut][contexts]")
{
    const auto swing = GENERATE (50.0f, 75.0f);
    LivecutAudioProcessor live, studio;
    setRecut (studio, "mode", 1.0f);
    for (auto* processor : { &live, &studio })
    {
        setRecut (*processor, "procedure", 1.0f);
        setRecut (*processor, "wet", .5f);
        setRecut (*processor, "outputTrim", -6.0f);
        setRecut (*processor, "minPitch", -700.0f);
        setRecut (*processor, "maxPitch", 900.0f);
        setRecut (*processor, "swing", swing);
        setRecut (*processor, "crusher", 1.0f);
        setRecut (*processor, "minBits", 8.0f);
        processor->prepareToPlay (8000.0, 137);
    }
    RecutHead liveHead, studioHead;
    live.setPlayHead (&liveHead);
    studio.setPlayHead (&studioHead);
    juce::MidiBuffer midi;
    std::array<std::vector<float>, 2> expected, actual;
    double ppq = 0.0;
    for (int done = 0; done < 40000; done += 137)
    {
        const auto count = std::min (137, 40000 - done);
        const auto bpm = done < 10000 ? 120.0 : 83.0;
        liveHead.at (ppq);
        studioHead.at (ppq);
        liveHead.info.setBpm (bpm);
        studioHead.info.setBpm (bpm);
        if (done == 137 * 40)
        {
            for (auto* processor : { &live, &studio })
            {
                setRecut (*processor, "wet", .8f);
                setRecut (*processor, "outputTrim", 3.0f);
                setRecut (*processor, "accent", 1.0f);
            }
        }
        juce::AudioBuffer<float> input (2, count);
        for (int i = 0; i < count; ++i)
        {
            input.setSample (0, i, std::sin (static_cast<float> (done + i) * .013f));
            input.setSample (1, i, std::cos (static_cast<float> (done + i) * .017f));
        }
        juce::AudioBuffer<float> delayed (input);
        live.processBlock (input, midi);
        studio.processBlock (delayed, midi);
        for (std::size_t channel = 0; channel < 2; ++channel)
        {
            expected[channel].insert (expected[channel].end(), input.getReadPointer (static_cast<int> (channel)), input.getReadPointer (static_cast<int> (channel)) + count);
            actual[channel].insert (actual[channel].end(), delayed.getReadPointer (static_cast<int> (channel)), delayed.getReadPointer (static_cast<int> (channel)) + count);
        }
        ppq += count * bpm / 60.0 / 8000.0;
    }
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        CHECK (std::all_of (actual[channel].begin(), actual[channel].begin() + 4000, [] (float sample) { return sample == 0.0f; }));
        CHECK (std::memcmp (actual[channel].data() + 4000, expected[channel].data(), 36000 * sizeof (float)) == 0);
    }
}

template <typename T>
concept ModeApplyCoordinator = requires (T& processor)
{
    processor.applyRequestedMode();
    processor.activeMode();
    processor.modeState();
};

TEST_CASE ("Mode Apply waits for an observed stop and keeps capture service ownership stable", "[processor][recut][apply]")
{
    [&]<typename T>()
    {
        if constexpr (! ModeApplyCoordinator<T>)
            FAIL ("Safe requested/active mode coordinator is not implemented");
        else
        {
            juce::ScopedJuceInitialiser_GUI gui;
            T processor;
            auto* captureService = &processor.getCaptureService();
            processor.prepareToPlay (8000.0, 32);
            RecutHead head;
            head.at (0.0);
            processor.setPlayHead (&head);
            juce::AudioBuffer<float> buffer (2, 32);
            buffer.clear();
            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
            setRecut (processor, "mode", 1.0f);
            processor.applyRequestedMode();
            CHECK (processor.getLatencySamples() == 0);
            CHECK (processor.activeMode() == livecut::ProcessingMode::live);
            CHECK (processor.modeState() == T::ModeState::pendingPlaying);
            head.at (.008, false);
            processor.processBlock (buffer, midi);
            processor.applyRequestedMode();
            CHECK (processor.getLatencySamples() == 4000);
            CHECK (processor.activeMode() == livecut::ProcessingMode::studio);
            CHECK (processor.modeState() == T::ModeState::active);
            CHECK (&processor.getCaptureService() == captureService);
            buffer.clear();
            processor.processBlock (buffer, midi); // fresh stopped evidence in the new configuration
            setRecut (processor, "mode", 0.0f);
            processor.applyRequestedMode();
            CHECK (processor.getLatencySamples() == 0);
            CHECK (processor.activeMode() == livecut::ProcessingMode::live);
        }
    }.template operator()<LivecutAudioProcessor>();
}

TEST_CASE ("Healthy Live bypass observes transport for Apply without advancing its engine", "[processor][recut][apply]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (8000.0, 32);
    RecutHead head;
    head.at (0.0);
    processor.setPlayHead (&head);
    juce::AudioBuffer<float> buffer (2, 32);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (0), .25f, 32);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (1), -.5f, 32);
    juce::MidiBuffer midi;
    processor.processBlockBypassed (buffer, midi);
    CHECK (processor.getCutter().nextInputFrame() == 0);
    CHECK (buffer.getSample (0, 0) == .25f);
    CHECK (buffer.getSample (1, 0) == -.5f);
    setRecut (processor, "mode", 1.0f);
    processor.applyRequestedMode();
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingPlaying);
    CHECK (processor.reportedLatencySamples() == 0);
    head.at (0.0, false);
    processor.processBlockBypassed (buffer, midi);
    processor.applyRequestedMode();
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::active);
    CHECK (processor.reportedLatencySamples() == 4000);
}

TEST_CASE ("Apply reconciles synchronous host reprepare or release outside the callback guard", "[processor][recut][apply][lifecycle]")
{
    const auto release = GENERATE (false, true);
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    setRecut (processor, "wet", 0.0f);
    processor.prepareToPlay (8000.0, 32);
    RecutHead head;
    head.at (0.0, false);
    processor.setPlayHead (&head);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    processor.processBlock (buffer, midi);
    struct Restart final : juce::AudioProcessorListener
    {
        explicit Restart (LivecutAudioProcessor& p, bool releases) : processor (p), release (releases) {}
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
        {
            if (! details.latencyChanged || triggered)
                return;
            triggered = true;
            std::thread probe ([&]
            {
                offGuard = processor.getCallbackLock().tryEnter();
                if (offGuard)
                    processor.getCallbackLock().exit();
            });
            probe.join();
            if (release)
                processor.releaseResources();
            else
                processor.prepareToPlay (16000.0, 137);
        }
        LivecutAudioProcessor& processor;
        bool release, triggered = false, offGuard = false;
    } restart (processor, release);
    processor.addListener (&restart);
    setRecut (processor, "mode", 1.0f);
    processor.applyRequestedMode();
    processor.removeListener (&restart);
    REQUIRE (restart.triggered);
    CHECK (restart.offGuard);
    CHECK_FALSE (processor.isSuspended());
    if (release)
    {
        CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::unprepared);
        CHECK (processor.reportedLatencySamples() == 0);
        processor.processBlockBypassed (buffer, midi);
        CHECK (buffer.getMagnitude (0, 32) == 0.0f);
        processor.prepareToPlay (8000.0, 32);
        CHECK (processor.reportedLatencySamples() == 4000);
    }
    else
    {
        CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::active);
        CHECK (processor.activeMode() == livecut::ProcessingMode::studio);
        CHECK (processor.reportedLatencySamples() == 8000);
        juce::AudioBuffer<float> impulse (2, 9000);
        impulse.clear();
        impulse.setSample (0, 17, .5f);
        processor.processBlock (impulse, midi);
        CHECK (impulse.getSample (0, 8017) == .5f);
        CHECK (impulse.getSample (0, 4017) == 0.0f);
    }
}

TEST_CASE ("A restored requested Studio mode uses preparation or waits while existing audio runs", "[processor][recut][state][apply]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor source;
    setRecut (source, "mode", 1.0f);
    juce::MemoryBlock saved;
    source.getStateInformation (saved);
    LivecutAudioProcessor fresh;
    fresh.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    fresh.prepareToPlay (8000.0, 32);
    CHECK (fresh.activeMode() == livecut::ProcessingMode::studio);
    CHECK (fresh.reportedLatencySamples() == 4000);
    LivecutAudioProcessor running;
    running.prepareToPlay (8000.0, 32);
    RecutHead head;
    head.at (0.0);
    running.setPlayHead (&head);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    running.processBlock (buffer, midi);
    running.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    running.applyRequestedMode();
    CHECK (running.modeState() == LivecutAudioProcessor::ModeState::pendingPlaying);
    CHECK (running.activeMode() == livecut::ProcessingMode::live);
    CHECK (running.reportedLatencySamples() == 0);
}

TEST_CASE ("A 0.4 state replaces every adjusted Recut control with its neutral default", "[processor][recut][state]")
{
    LivecutAudioProcessor processor;
    juce::MemoryBlock saved;
    processor.getStateInformation (saved);
    auto xml = juce::AudioProcessor::getXmlFromBinary (saved.getData(), static_cast<int> (saved.getSize()));
    REQUIRE (xml);
    xml->setAttribute ("schemaVersion", 4);
    for (const auto* id : { "mode", "attackAlign", "attackThreshold", "reverseChance" })
    {
        auto* child = xml->getChildByAttribute ("id", id);
        REQUIRE (child);
        xml->removeChildElement (child, true);
        setRecut (processor, id, juce::String (id) == "attackThreshold" ? 24.0f : 1.0f);
    }
    juce::AudioProcessor::copyXmlToBinary (*xml, saved);
    processor.setStateInformation (saved.getData(), static_cast<int> (saved.getSize()));
    const auto restored = livecut::SettingsReader (processor.getParameterState()).read();
    CHECK (restored.requestedMode == livecut::ProcessingMode::live);
    CHECK_FALSE (restored.attackAlign);
    CHECK (restored.attackThresholdDb == 6.0f);
    CHECK (restored.reverseChance == 0.0f);
}

TEST_CASE ("Concurrent lifecycle preparation never restores an obsolete Apply candidate", "[processor][recut][apply][lifecycle]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    setRecut (processor, "wet", 0.0f);
    processor.prepareToPlay (8000.0, 32);
    RecutHead head;
    head.at (0.0, false);
    processor.setPlayHead (&head);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    processor.processBlock (buffer, midi);
    setRecut (processor, "mode", 1.0f);
    std::thread lifecycle ([&]
    {
        for (int restart = 0; restart < 8; ++restart)
            processor.prepareToPlay (restart % 2 == 0 ? 8000.0 : 16000.0, 32);
    });
    for (int attempt = 0; attempt < 8; ++attempt)
        processor.applyRequestedMode();
    lifecycle.join();
    processor.applyRequestedMode();
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::active);
    CHECK (processor.activeMode() == livecut::ProcessingMode::studio);
    CHECK (processor.reportedLatencySamples() == 8000);
    juce::AudioBuffer<float> impulse (2, 9000);
    impulse.clear();
    impulse.setSample (0, 17, .5f);
    processor.processBlock (impulse, midi);
    CHECK (impulse.getSample (0, 8017) == .5f);
    CHECK (impulse.getSample (0, 4017) == 0.0f);
}

TEST_CASE ("Every new configuration requires fresh audio transport evidence before another Apply", "[processor][recut][apply][observation]")
{
    const auto priorRunning = GENERATE (false, true);
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (8000.0, 32);
    RecutHead head;
    head.at (0.0, priorRunning);
    processor.setPlayHead (&head);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    processor.processBlock (buffer, midi);
    processor.releaseResources();
    processor.prepareToPlay (16000.0, 32);
    setRecut (processor, "mode", 1.0f);
    processor.applyRequestedMode();
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingObservation);
    CHECK (processor.reportedLatencySamples() == 0);
    head.at (0.0, false);
    processor.processBlockBypassed (buffer, midi);
    processor.applyRequestedMode();
    REQUIRE (processor.activeMode() == livecut::ProcessingMode::studio);
    CHECK (processor.reportedLatencySamples() == 8000);
    setRecut (processor, "mode", 0.0f);
    processor.applyRequestedMode();
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingObservation);
    CHECK (processor.reportedLatencySamples() == 8000);
    head.at (0.0, true);
    processor.processBlock (buffer, midi);
    processor.applyRequestedMode();
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingPlaying);
    head.at (.004, false);
    processor.processBlock (buffer, midi);
    processor.applyRequestedMode();
    CHECK (processor.activeMode() == livecut::ProcessingMode::live);
    CHECK (processor.reportedLatencySamples() == 0);
}

TEST_CASE ("Bounded latency reconciliation retries publish even when the numeric latency already matches", "[processor][recut][apply][retry]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (8000.0, 32);
    RecutHead head;
    head.at (0.0, false);
    processor.setPlayHead (&head);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    struct Churn final : juce::AudioProcessorListener
    {
        explicit Churn (LivecutAudioProcessor& p) : processor (p) {}
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
        {
            if (details.latencyChanged && restarts < 2)
            {
                ++restarts;
                processor.prepareToPlay (16000.0, 32);
            }
        }
        LivecutAudioProcessor& processor;
        int restarts = 0;
    } churn (processor);
    processor.addListener (&churn);
    setRecut (processor, "mode", 1.0f);
    processor.applyRequestedMode();
    processor.removeListener (&churn);
    REQUIRE (churn.restarts == 2);
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingHostChange);
    CHECK (processor.isSuspended());
    processor.applyRequestedMode();
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::active);
    CHECK_FALSE (processor.isSuspended());
    CHECK (processor.activeMode() == livecut::ProcessingMode::studio);
    CHECK (processor.reportedLatencySamples() == 8000);
}

TEST_CASE ("Missing host transport never provides stopped evidence for Apply", "[processor][recut][missing-transport]")
{
    const auto bypass = GENERATE (false, true);
    const auto keepHead = GENERATE (false, true);
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (8000.0, 32);
    RecutHead head;
    head.at (0.0);
    processor.setPlayHead (&head);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    const auto render = [&]
    {
        if (bypass)
            processor.processBlockBypassed (buffer, midi);
        else
            processor.processBlock (buffer, midi);
    };
    render();
    setRecut (processor, "mode", 1.0f);
    if (keepHead)
        head.available = false;
    else
        processor.setPlayHead (nullptr);
    render();
    processor.applyRequestedMode();
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingObservation);
    CHECK (processor.reportedLatencySamples() == 0);
    head.available = true;
    head.at (.016, false);
    processor.setPlayHead (&head);
    render();
    processor.applyRequestedMode();
    CHECK (processor.activeMode() == livecut::ProcessingMode::studio);
    CHECK (processor.reportedLatencySamples() == 4000);
}

TEST_CASE ("Bounded notifications reconcile empty and failed configurations on retry", "[processor][recut][empty-retry]")
{
    const auto failPreparation = GENERATE (false, true);
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (8000.0, 32);
    RecutHead head;
    head.at (0.0, false);
    processor.setPlayHead (&head);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    struct PrepareRelease final : juce::AudioProcessorListener
    {
        PrepareRelease (LivecutAudioProcessor& p, bool fail) : processor (p), failPreparation (fail) {}
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
        {
            if (! details.latencyChanged)
                return;
            if (++notifications == 1)
                processor.prepareToPlay (16000.0, 32);
            else if (notifications == 2)
            {
                if (failPreparation)
                    processor.prepareToPlay (1000.0, 32);
                else
                    processor.releaseResources();
            }
        }
        LivecutAudioProcessor& processor;
        int notifications = 0;
        bool failPreparation;
    } sequence (processor, failPreparation);
    processor.addListener (&sequence);
    setRecut (processor, "mode", 1.0f);
    processor.applyRequestedMode();
    processor.removeListener (&sequence);
    REQUIRE (sequence.notifications == 2);
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingHostChange);
    CHECK (processor.isSuspended());
    processor.applyRequestedMode();
    CHECK (processor.modeState() == (failPreparation ? LivecutAudioProcessor::ModeState::error
                                                    : LivecutAudioProcessor::ModeState::unprepared));
    CHECK_FALSE (processor.isSuspended());
    CHECK (processor.getLatencySamples() == 0);
    CHECK (processor.reportedLatencySamples() == 0);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (0), .5f, 32);
    processor.processBlockBypassed (buffer, midi);
    CHECK (buffer.getMagnitude (0, 32) == 0.0f);
    juce::FloatVectorOperations::fill (buffer.getWritePointer (0), .5f, 32);
    processor.processBlock (buffer, midi);
    CHECK (buffer.getMagnitude (0, 32) == 0.0f);
}


TEST_CASE ("Obsolete Apply prebuild cannot overwrite a newer published configuration", "[processor][recut][stale-prebuild]")
{
    const auto failObsoleteAllocation = GENERATE (false, true);
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (8000.0, 32);
    RecutHead head;
    head.at (0.0, false);
    processor.setPlayHead (&head);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    setRecut (processor, "mode", 1.0f);
    struct Interleave { LivecutAudioProcessor& processor; bool fail, triggered = false; } interleave { processor, failObsoleteAllocation };
    const livecut::engine::testing::NextAllocation pause ([] (void* state)
    {
        auto& operation = *static_cast<Interleave*> (state);
        operation.triggered = true;
        std::thread newer ([&] { operation.processor.prepareToPlay (16000.0, 32); });
        newer.join();
        if (operation.fail)
            throw std::bad_alloc();
    }, &interleave);
    processor.applyRequestedMode();
    REQUIRE (interleave.triggered);
    // No extra Apply: it would mask stale work corrupting newer publication.
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::active);
    CHECK (processor.activeMode() == livecut::ProcessingMode::studio);
    CHECK (processor.reportedLatencySamples() == 8000);
    CHECK_FALSE (processor.isSuspended());
}

TEST_CASE ("Studio transforms feed one delayed mixer and final-output capture with published counters", "[processor][recut][selection][capture]")
{
    const auto reverse = GENERATE (false, true);
    LivecutAudioProcessor processor;
    for (const auto* id : { "mode", "attackAlign", "minPhraseBars", "maxPhraseBars", "straightChance" })
        setRecut (processor, id, 1.0f);
    setRecut (processor, "reverseChance", reverse ? 1.0f : 0.0f);
    setRecut (processor, "procedure", 1.0f);
    setRecut (processor, "subdivision", 6.0f); //32 units: complete first source fits the500ms lead
    setRecut (processor, "minPan", 0.0f);
    setRecut (processor, "maxPan", 0.0f);
    setRecut (processor, "wet", .5f);
    setRecut (processor, "outputTrim", -6.0f);
    processor.prepareToPlay (8000.0, 137);
    RecutHead head;
    processor.setPlayHead (&head);
    const auto raw = [] (int frame, int channel)
    {
        if (frame == 96)
            return channel == 0 ? .9f : -.6f;
        return channel == 0 ? .01f + static_cast<float> (frame) * .000001f
                            : -.03f - static_cast<float> (frame) * .000002f;
    };
    juce::MidiBuffer midi;
    std::array<std::vector<float>, 2> heard;
    int firstSoundingFrames = 0;
    std::size_t allocations = 0;
    for (int first = 0; first < 21000; first += 137)
    {
        const auto frames = std::min (137, 21000 - first);
        head.at (first / 4000.0);
        juce::AudioBuffer<float> buffer (2, frames);
        for (int channel = 0; channel < 2; ++channel)
            for (int i = 0; i < frames; ++i)
                buffer.setSample (channel, i, raw (first + i, channel));
        {
            const livecut::engine::testing::AllocationCounter counter;
            processor.processBlock (buffer, midi);
            allocations += counter.count();
        }
        if (firstSoundingFrames == 0 && processor.getDisplayState().attackApplied.load() > 0)
            firstSoundingFrames = processor.getCutter().player().currentSoundingFrames();
        for (int channel = 0; channel < 2; ++channel)
            heard[static_cast<std::size_t> (channel)].insert (heard[static_cast<std::size_t> (channel)].end(),
                buffer.getReadPointer (channel), buffer.getReadPointer (channel) + frames);
    }
    REQUIRE (firstSoundingFrames > 17);
    const auto gain = juce::Decibels::decibelsToGain (-6.0f);
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto source = raw (80 + (reverse ? firstSoundingFrames - 1 - 17 : 17), channel);
        const auto wet = livecut::engine::fadeEnvelope (17, 1, firstSoundingFrames) * source;
        const auto dry = raw (17, channel);
        CHECK (heard[static_cast<std::size_t> (channel)][4017] == (dry + .5f * (wet - dry)) * gain);
    }
    CHECK (processor.getDisplayState().attackApplied.load() >= 1);
    CHECK (processor.getDisplayState().reverseApplied.load() == (reverse ? processor.getDisplayState().attackApplied.load() : 0));
    CHECK (processor.transformStatusText().contains ("Attack aligned"));
    auto captured = processor.getCaptureService().rolling().acquireLatest();
    REQUIRE (captured);
    REQUIRE (captured.channel (0).size() == 16000);
    CHECK (std::memcmp (captured.channel (0).data(), heard[0].data() + 4000, 16000 * sizeof (float)) == 0);
    CHECK (std::memcmp (captured.channel (1).data(), heard[1].data() + 4000, 16000 * sizeof (float)) == 0);
    CHECK (allocations == 0);
}
