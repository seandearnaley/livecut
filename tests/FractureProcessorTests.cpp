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
constexpr double rate = 8000.0;
constexpr int studioLatency = 4000, barFrames = 16000;
using Stereo = std::array<std::vector<float>, 2>;

struct FractureHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return position; }
    void at (int frame, double bpm = 120.0, double beats = -1.0)
    {
        if (beats < 0.0)
            beats = frame / 4000.0;
        position.setIsPlaying (true);
        position.setBpm (bpm);
        position.setTimeSignature (TimeSignature { 4, 4 });
        position.setPpqPosition (beats);
        position.setPpqPositionOfLastBarStart (std::floor (beats / 4.0) * 4.0);
        position.setBarCount (static_cast<std::int64_t> (std::floor (beats / 4.0)));
    }
    PositionInfo position;
};

void set (LivecutAudioProcessor& processor, const char* id, float value)
{
    auto* parameter = processor.getParameterState().getParameter (id);
    REQUIRE (parameter != nullptr);
    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
}

void configure (LivecutAudioProcessor& processor, bool studio, float family)
{
    using namespace livecut::ids;
    set (processor, mode, studio ? 1.0f : 0.0f);
    set (processor, procedure, 1.0f);
    set (processor, straightChance, 1.0f);
    set (processor, minPhraseBars, 1.0f);
    set (processor, maxPhraseBars, 1.0f);
    set (processor, minPan, 0.0f);
    set (processor, maxPan, 0.0f);
    set (processor, fractureProcedure, family);
    set (processor, fractureTapeStartSpeed, 1.0f);
    set (processor, fractureTapeEndSpeed, .2f);
    set (processor, fractureFrequencyEnabled, 1.0f);
    set (processor, fractureFrequencyMode, 1.0f);
    set (processor, fractureFrequencyHz, 173.0f);
    set (processor, fractureFrequencyEnvHz, 70.0f);
    set (processor, fractureFrequencyRepeatHz, -90.0f);
}

float input (int frame, int channel)
{
    const auto phase = static_cast<double> (frame);
    const auto pulse = frame % 1000 < 48 ? .7 : .2;
    return static_cast<float> (pulse * std::sin (phase * (channel == 0 ? .71 : .53))
                               + .1 * std::cos (phase * .037));
}

void fill (juce::AudioBuffer<float>& buffer, int first)
{
    for (int channel = 0; channel < 2; ++channel)
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            buffer.setSample (channel, sample, input (first + sample, channel));
}

void append (Stereo& result, const juce::AudioBuffer<float>& buffer)
{
    for (std::size_t channel = 0; channel < result.size(); ++channel)
        result[channel].insert (result[channel].end(), buffer.getReadPointer (static_cast<int> (channel)),
                                buffer.getReadPointer (static_cast<int> (channel)) + buffer.getNumSamples());
}
} // namespace

TEST_CASE ("Grain processor publishes actual source waiting instead of substituting Classic audio", "[fracture][processor][waiting]")
{
    LivecutAudioProcessor live, studio;
    configure (live, false, 1.0f);
    configure (studio, true, 1.0f);
    for (auto* processor : { &live, &studio })
        processor->prepareToPlay (rate, 137);
    FractureHead liveHead, studioHead;
    live.setPlayHead (&liveHead);
    studio.setPlayHead (&studioHead);
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> first (2, 137);
    fill (first, 0);
    liveHead.at (0);
    live.processBlock (first, midi);
    CHECK (first.getMagnitude (0, first.getNumSamples()) == 0.0f);
    CHECK (live.getDisplayState().fractureFamily.load() == livecut::engine::FractureProcedure::grainCut);
    CHECK (live.getDisplayState().fractureState.load() == livecut::engine::FractureState::waiting);
    CHECK (live.getDisplayState().fractureVoices.load() == 0);

    // Default 8 kHz guarded pool is longer than this first callback. Studio's
    // complete 500 ms lead normally admits that same pool at its first render.
    // Do not compare Grain's different initial admission/launch ages for equality.
    double energy = 0.0;
    for (int frame = 0; frame < studioLatency + 137;)
    {
        const auto count = std::min (137, studioLatency + 137 - frame);
        juce::AudioBuffer<float> buffer (2, count);
        fill (buffer, frame);
        studioHead.at (frame);
        studio.processBlock (buffer, midi);
        for (int sample = 0; sample < count; ++sample)
            if (frame + sample >= studioLatency)
                energy += std::abs (buffer.getSample (0, sample)) + std::abs (buffer.getSample (1, sample));
        frame += count;
    }
    CHECK (studio.getDisplayState().fractureFamily.load() == livecut::engine::FractureProcedure::grainCut);
    CHECK (studio.getDisplayState().fractureState.load() == livecut::engine::FractureState::playing);
    CHECK (studio.getDisplayState().fractureVoices.load() > 0);
    CHECK (energy > .01);
}

TEST_CASE ("Studio Tape and Frequency render their original delayed setting and tempo contexts", "[fracture][processor][contexts]")
{
    LivecutAudioProcessor live, studio;
    for (auto* processor : { &live, &studio })
    {
        configure (*processor, processor == &studio, 2.0f);
        set (*processor, livecut::ids::wet, .65f);
        set (*processor, livecut::ids::outputTrim, -3.0f);
        set (*processor, livecut::ids::impactFilterEnabled, 1.0f);
        set (*processor, livecut::ids::impactFilterCutoff, 1100.0f);
        processor->prepareToPlay (rate, 137);
    }
    FractureHead liveHead, studioHead;
    live.setPlayHead (&liveHead);
    studio.setPlayHead (&studioHead);
    juce::MidiBuffer midi;
    Stereo expected, delayed;
    double beats = 0.0;
    for (int frame = 0; frame < 28000;)
    {
        const auto count = std::min (137, 28000 - frame);
        const auto bpm = frame < 10000 ? 120.0 : 83.0;
        liveHead.at (frame, bpm, beats);
        studioHead.at (frame, bpm, beats);
        if (frame == 137 * 19)
            for (auto* processor : { &live, &studio })
            {
                set (*processor, livecut::ids::fractureTapeEndSpeed, .8f);
                set (*processor, livecut::ids::fractureTapeCurve, 0.0f);
                set (*processor, livecut::ids::fractureFrequencyMode, 0.0f);
                set (*processor, livecut::ids::fractureFrequencyHz, -137.0f);
                set (*processor, livecut::ids::fractureFrequencyEnvHz, 123.0f);
            }
        juce::AudioBuffer<float> buffer (2, count);
        fill (buffer, frame);
        juce::AudioBuffer<float> other (buffer);
        live.processBlock (buffer, midi);
        studio.processBlock (other, midi);
        append (expected, buffer);
        append (delayed, other);
        beats += count * bpm / 60.0 / rate;
        frame += count;
    }
    for (std::size_t channel = 0; channel < expected.size(); ++channel)
    {
        CHECK (std::all_of (delayed[channel].begin(), delayed[channel].begin() + studioLatency,
                           [] (float sample) { return sample == 0.0f; }));
        CHECK (std::memcmp (delayed[channel].data() + studioLatency, expected[channel].data(),
                            24000 * sizeof (float)) == 0);
    }
    CHECK (live.getDisplayState().fractureFamily.load() == livecut::engine::FractureProcedure::tapeCut);
    CHECK (studio.getDisplayState().fractureFamily.load() == livecut::engine::FractureProcedure::tapeCut);
    CHECK (studio.getDisplayState().fractureState.load() != livecut::engine::FractureState::sourceLost);
    CHECK (studio.getDisplayState().fractureState.load() != livecut::engine::FractureState::contextUnavailable);
}

TEST_CASE ("Fracture Hold retains the post Frequency wet chain and Capture retains the heard output", "[fracture][processor][hold][capture]")
{
    const auto studio = GENERATE (false, true);
    const auto family = GENERATE (1.0f, 2.0f);
    const auto latency = studio ? studioLatency : 0;
    const auto total = latency + barFrames + 8800;
    LivecutAudioProcessor processor, wetReference, unmodulated;
    for (auto* candidate : { &processor, &wetReference, &unmodulated })
        configure (*candidate, studio, family);
    set (processor, livecut::ids::wet, .35f);
    set (processor, livecut::ids::outputTrim, -6.0f);
    set (unmodulated, livecut::ids::fractureFrequencyEnabled, 0.0f);
    for (auto* candidate : { &processor, &wetReference, &unmodulated })
        candidate->prepareToPlay (rate, 137);
    FractureHead head, wetHead, unmodulatedHead;
    processor.setPlayHead (&head);
    wetReference.setPlayHead (&wetHead);
    unmodulated.setPlayHead (&unmodulatedHead);
    juce::MidiBuffer midi;
    Stereo heard, wet;
    livecut::PhraseCapture::Lease captured;
    double frequencyDifference = 0.0;
    std::size_t allocations = 0;
    for (int frame = 0; frame < total;)
    {
        if (frame == barFrames)
        {
            set (processor, livecut::ids::hold, 1.0f);
            // Heard Hold must retain the already effected wet phrase even
            // when the live chain's Frequency control is subsequently disabled.
            set (processor, livecut::ids::fractureFrequencyEnabled, 0.0f);
        }
        if (frame == latency + barFrames)
            captured = processor.getCaptureService().rolling().acquireLatest();
        const auto boundary = frame < barFrames ? barFrames
                            : frame < latency + barFrames ? latency + barFrames : total;
        const auto count = std::min (137, boundary - frame);
        head.at (frame);
        wetHead.at (frame);
        unmodulatedHead.at (frame);
        juce::AudioBuffer<float> buffer (2, count);
        fill (buffer, frame);
        juce::AudioBuffer<float> reference (buffer), neutral (buffer);
        {
            const livecut::engine::testing::AllocationCounter counter;
            processor.processBlock (buffer, midi);
            wetReference.processBlock (reference, midi);
            unmodulated.processBlock (neutral, midi);
            allocations += counter.count();
        }
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < count; ++sample)
                if (frame + sample >= latency && frame + sample < latency + barFrames)
                    frequencyDifference += std::abs (reference.getSample (channel, sample) - neutral.getSample (channel, sample));
        append (heard, buffer);
        append (wet, reference);
        frame += count;
    }
    CHECK (processor.getDisplayState().hold.load() == livecut::engine::WetHold::State::held);
    CHECK (frequencyDifference > 1.0);
    const auto gain = juce::Decibels::decibelsToGain (-6.0f);
    for (std::size_t channel = 0; channel < heard.size(); ++channel)
    {
        constexpr int phase = 8424;
        const auto dry = input (barFrames + phase, static_cast<int> (channel));
        const auto held = wet[channel][static_cast<std::size_t> (latency + phase)];
        const auto expected = (dry + .35f * (held - dry)) * gain;
        CHECK_THAT (heard[channel][static_cast<std::size_t> (latency + barFrames + phase)],
                    Catch::Matchers::WithinAbs (expected, 1.0e-5));
        REQUIRE (captured);
        REQUIRE (captured.channel (channel).size() == barFrames);
        CHECK (std::memcmp (captured.channel (channel).data(), heard[channel].data() + latency,
                            barFrames * sizeof (float)) == 0);
    }
    CHECK (allocations == 0);
}

TEST_CASE ("Zero speed Tape reaches the processor output as silence rather than held input", "[fracture][processor][tape-zero]")
{
    LivecutAudioProcessor processor;
    configure (processor, false, 2.0f);
    set (processor, livecut::ids::fractureTapeStartSpeed, 0.0f);
    set (processor, livecut::ids::fractureTapeEndSpeed, 0.0f);
    processor.prepareToPlay (rate, 137);
    FractureHead head;
    processor.setPlayHead (&head);
    juce::MidiBuffer midi;
    bool silent = true;
    for (int frame = 0; frame < 4000;)
    {
        const auto count = std::min (137, 4000 - frame);
        juce::AudioBuffer<float> buffer (2, count);
        fill (buffer, frame);
        head.at (frame);
        processor.processBlock (buffer, midi);
        silent &= buffer.getMagnitude (0, count) == 0.0f;
        frame += count;
    }
    CHECK (silent);
    CHECK (processor.getDisplayState().fractureFamily.load() == livecut::engine::FractureProcedure::tapeCut);
    CHECK (processor.getDisplayState().fractureTapeSpeed.load() == 0.0);
}
