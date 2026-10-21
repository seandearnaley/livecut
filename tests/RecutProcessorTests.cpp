// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginProcessor.h"
#include "HostSync.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "engine/AllocationCounter.h"
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
struct RecutHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return info; }
    PositionInfo info;
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
