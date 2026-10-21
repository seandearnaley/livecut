// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginProcessor.h"
#include "engine/AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
struct ImpactHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return info; }
    PositionInfo info;
    void at (int frame, double bpm = 120.0, double beats = -1.0)
    {
        if (beats < 0.0)
            beats = frame / 4000.0;
        info.setIsPlaying (true);
        info.setBpm (bpm);
        info.setTimeSignature (TimeSignature { 4, 4 });
        info.setPpqPosition (beats);
        info.setPpqPositionOfLastBarStart (std::floor (beats / 4.0) * 4.0);
        info.setBarCount (static_cast<std::int64_t> (std::floor (beats / 4.0)));
    }
};

void setImpactParameter (LivecutAudioProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void configureImpactProcessor (LivecutAudioProcessor& processor, bool studio)
{
    using namespace livecut::ids;
    setImpactParameter (processor, mode, studio ? 1.0f : 0.0f);
    setImpactParameter (processor, procedure, 1.0f);
    setImpactParameter (processor, straightChance, 1.0f);
    setImpactParameter (processor, minPhraseBars, 1.0f);
    setImpactParameter (processor, maxPhraseBars, 1.0f);
    setImpactParameter (processor, minPan, 0.0f);
    setImpactParameter (processor, maxPan, 0.0f);
    setImpactParameter (processor, impactFilterEnabled, 1.0f);
    setImpactParameter (processor, impactFilterCutoff, 900.0f);
    setImpactParameter (processor, impactFilterEnvOctaves, 1.0f);
    setImpactParameter (processor, impactFilterRepeatOctaves, -2.0f);
    setImpactParameter (processor, impactDriveEnabled, 1.0f);
    setImpactParameter (processor, impactDriveDb, 15.0f);
    setImpactParameter (processor, impactTransientEnabled, 1.0f);
    setImpactParameter (processor, impactTransientAttackDb, 7.0f);
    setImpactParameter (processor, impactTransientSustainDb, -4.0f);
}

float processorInput (int frame, int channel)
{
    const auto frequency = channel == 0 ? .19f : .27f;
    return .4f * std::sin (static_cast<float> (frame) * frequency)
         + .15f * std::cos (static_cast<float> (frame) * frequency * .13f);
}

void fillImpactInput (juce::AudioBuffer<float>& buffer, int first)
{
    for (int channel = 0; channel < 2; ++channel)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            buffer.setSample (channel, i, processorInput (first + i, channel));
}

using StereoOutput = std::array<std::vector<float>, 2>;

void appendOutput (StereoOutput& output, const juce::AudioBuffer<float>& buffer)
{
    for (std::size_t channel = 0; channel < 2; ++channel)
        output[channel].insert (output[channel].end(), buffer.getReadPointer (static_cast<int> (channel)),
                                buffer.getReadPointer (static_cast<int> (channel)) + buffer.getNumSamples());
}
} // namespace

TEST_CASE ("Studio Impact follows original delayed settings and tempo", "[impact][processor][contexts]")
{
    const auto swing = GENERATE (50.0f, 75.0f);
    LivecutAudioProcessor live, studio;
    for (auto* processor : { &live, &studio })
    {
        configureImpactProcessor (*processor, processor == &studio);
        setImpactParameter (*processor, livecut::ids::swing, swing);
        setImpactParameter (*processor, livecut::ids::wet, .6f);
        setImpactParameter (*processor, livecut::ids::outputTrim, -3.0f);
        processor->prepareToPlay (8000.0, 137);
    }
    ImpactHead liveHead, studioHead;
    live.setPlayHead (&liveHead);
    studio.setPlayHead (&studioHead);
    juce::MidiBuffer midi;
    StereoOutput expected, actual;
    double beats = 0.0;
    for (int first = 0; first < 28000; first += 137)
    {
        const auto count = std::min (137, 28000 - first);
        const auto bpm = first < 10000 ? 120.0 : 83.0;
        liveHead.at (first, bpm, beats);
        studioHead.at (first, bpm, beats);
        if (first == 137 * 40)
            for (auto* processor : { &live, &studio })
            {
                setImpactParameter (*processor, livecut::ids::impactDriveMode, 1.0f);
                setImpactParameter (*processor, livecut::ids::impactDriveChance, .53f);
                setImpactParameter (*processor, livecut::ids::impactFilterQ, 3.0f);
                setImpactParameter (*processor, livecut::ids::impactAttackMs, 7.0f);
                setImpactParameter (*processor, livecut::ids::impactDecayMs, 170.0f);
            }
        juce::AudioBuffer<float> input (2, count);
        fillImpactInput (input, first);
        juce::AudioBuffer<float> delayed (input);
        live.processBlock (input, midi);
        studio.processBlock (delayed, midi);
        appendOutput (expected, input);
        appendOutput (actual, delayed);
        beats += count * bpm / 60.0 / 8000.0;
    }
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        CHECK (std::all_of (actual[channel].begin(), actual[channel].begin() + 4000,
                           [] (float value) { return value == 0.0f; }));
        CHECK (std::memcmp (actual[channel].data() + 4000, expected[channel].data(), 24000 * sizeof (float)) == 0);
    }
    CHECK (live.reportedLatencySamples() == 0);
    CHECK (studio.reportedLatencySamples() == 4000);
}

TEST_CASE ("Impact Hold stores effected wet while Capture stores the heard mix", "[impact][processor][hold][capture]")
{
    const auto studio = GENERATE (false, true);
    const auto latency = studio ? 4000 : 0;
    const auto total = latency + 16000 + 8800;
    LivecutAudioProcessor processor, wetReference;
    configureImpactProcessor (processor, studio);
    configureImpactProcessor (wetReference, studio);
    setImpactParameter (processor, livecut::ids::wet, .5f);
    setImpactParameter (processor, livecut::ids::outputTrim, -6.0f);
    for (auto* p : { &processor, &wetReference })
        p->prepareToPlay (8000.0, 137);
    ImpactHead head, referenceHead;
    processor.setPlayHead (&head);
    wetReference.setPlayHead (&referenceHead);
    juce::MidiBuffer midi;
    StereoOutput heard, wet;
    livecut::PhraseCapture::Lease captured;
    std::size_t allocations = 0;
    for (int first = 0; first < total;)
    {
        if (first == 16000)
        {
            setImpactParameter (processor, livecut::ids::hold, 1.0f);
            for (const auto* id : { livecut::ids::impactFilterEnabled, livecut::ids::impactDriveEnabled,
                                    livecut::ids::impactTransientEnabled })
                setImpactParameter (processor, id, 0.0f);
        }
        if (first == latency + 16000)
            captured = processor.getCaptureService().rolling().acquireLatest();
        const auto boundary = first < 16000 ? 16000
                            : first < latency + 16000 ? latency + 16000 : total;
        const auto count = std::min (137, boundary - first);
        head.at (first);
        referenceHead.at (first);
        juce::AudioBuffer<float> buffer (2, count);
        fillImpactInput (buffer, first);
        juce::AudioBuffer<float> reference (buffer);
        {
            const livecut::engine::testing::AllocationCounter counter;
            processor.processBlock (buffer, midi);
            wetReference.processBlock (reference, midi);
            allocations += counter.count();
        }
        appendOutput (heard, buffer);
        appendOutput (wet, reference);
        first += count;
    }
    CHECK (processor.getDisplayState().hold.load() == livecut::engine::WetHold::State::held);
    const auto gain = juce::Decibels::decibelsToGain (-6.0f);
    for (std::size_t channel = 0; channel < 2; ++channel)
    {
        constexpr int phase = 8424;
        const auto expected = .5f * (processorInput (16000 + phase, static_cast<int> (channel))
                                     + wet[channel][static_cast<std::size_t> (latency + phase)]) * gain;
        CHECK_THAT (heard[channel][static_cast<std::size_t> (latency + 16000 + phase)],
                    Catch::Matchers::WithinAbs (expected, 1.0e-5));
        REQUIRE (captured);
        REQUIRE (captured.channel (channel).size() == 16000);
        CHECK (std::memcmp (captured.channel (channel).data(), heard[channel].data() + latency, 16000 * sizeof (float)) == 0);
    }
    CHECK (allocations == 0);
}

TEST_CASE ("An active Impact configuration retains Studio latency through release and preparation", "[impact][processor][latency]")
{
    LivecutAudioProcessor processor;
    configureImpactProcessor (processor, true);
    processor.prepareToPlay (8000.0, 137);
    REQUIRE (processor.reportedLatencySamples() == 4000);
    processor.releaseResources();
    CHECK (processor.reportedLatencySamples() == 4000);
    CHECK (processor.getLatencySamples() == 4000);
    juce::AudioBuffer<float> released (2, 137);
    fillImpactInput (released, 0);
    juce::MidiBuffer midi;
    processor.processBlock (released, midi);
    CHECK (released.getMagnitude (0, released.getNumSamples()) == 0.0f);
    processor.prepareToPlay (8000.0, 137);
    CHECK (processor.reportedLatencySamples() == 4000);
    CHECK (processor.getLatencySamples() == 4000);
}
