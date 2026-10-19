// SPDX-License-Identifier: GPL-3.0-or-later

#include "Parameters.h"
#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <limits>

using namespace livecut;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr std::array allIds {
    ids::procedure, ids::subdivision, ids::seed, ids::minPhraseBars, ids::maxPhraseBars, ids::fade,
    ids::minAmp, ids::maxAmp, ids::minPan, ids::maxPan, ids::minPitch, ids::maxPitch, ids::duty,
    ids::fillDuty, ids::minRepeats, ids::maxRepeats, ids::stutterChance, ids::stutterArea,
    ids::straightChance, ids::regularChance, ids::ritardChance, ids::accel, ids::activity,
    ids::crusher, ids::minBits, ids::maxBits, ids::minRate, ids::maxRate, ids::comb, ids::combType,
    ids::combFeedback, ids::minDelay, ids::maxDelay
};

juce::RangedAudioParameter& parameter (juce::AudioProcessorValueTreeState& state, const char* id)
{
    auto* found = state.getParameter (id);
    REQUIRE (found != nullptr);
    return *found;
}

float defaultValue (juce::AudioProcessorValueTreeState& state, const char* id)
{
    auto& p = parameter (state, id);
    return p.convertFrom0to1 (p.getDefaultValue());
}

void setNormalised (juce::AudioProcessorValueTreeState& state, const char* id, float value)
{
    parameter (state, id).setValueNotifyingHost (value);
}

void setValue (juce::AudioProcessorValueTreeState& state, const char* id, float value)
{
    auto& p = parameter (state, id);
    p.setValueNotifyingHost (p.convertTo0to1 (value));
}
} // namespace

TEST_CASE ("Every parameter has a stable ID", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();

    for (const auto* id : allIds)
        CHECK (state.getParameter (id) != nullptr);

    CHECK (processor.getParameters().size() == static_cast<int> (allIds.size()));
}

// Livecut exposed every parameter as continuous, so hosts drew choices as knobs
// and SQPusher was only reachable at exactly 100%.
TEST_CASE ("Choice parameters are discrete steps for the host", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();

    CHECK (parameter (state, ids::procedure).getNumSteps() == 3);
    CHECK (parameter (state, ids::subdivision).getNumSteps() == 7);
    CHECK (parameter (state, ids::combType).getNumSteps() == 2);
    CHECK (parameter (state, ids::seed).getNumSteps() == 16);
    CHECK (parameter (state, ids::crusher).isBoolean());

    // SQPusher now owns the top third of the knob instead of only its very end.
    setNormalised (state, ids::procedure, 0.8f);
    CHECK (SettingsReader (state).read().procedure == engine::Procedure::sqPusher);
}

TEST_CASE ("Defaults match the original plugin", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();

    CHECK_THAT (defaultValue (state, ids::stutterChance), WithinAbs (0.8, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::minPan), WithinAbs (-0.2, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::accel), WithinAbs (0.9, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::maxPhraseBars), WithinAbs (4.0, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::maxBits), WithinAbs (32.0, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::minDelay), WithinAbs (10.0, 1.0e-6));

    const auto settings = SettingsReader (state).read();
    const engine::CutParameters original;

    CHECK (settings.procedure == engine::Procedure::cutProc11);
    CHECK (settings.unitsPerBar == 8);
    CHECK (settings.seed == 1);
    CHECK_THAT (settings.fadeMilliseconds, WithinAbs (0.0, 1.0e-9));
    CHECK (settings.cut.minPhraseBars == original.minPhraseBars);
    CHECK (settings.cut.maxPhraseBars == original.maxPhraseBars);
    CHECK_THAT (settings.cut.minPan, WithinAbs (original.minPan, 1.0e-6));
    CHECK_THAT (settings.cut.maxPan, WithinAbs (original.maxPan, 1.0e-6));
    CHECK_THAT (settings.cut.stutterChance, WithinAbs (original.stutterChance, 1.0e-6));
    CHECK_THAT (settings.cut.straightChance, WithinAbs (original.straightChance, 1.0e-6));
    CHECK_THAT (settings.cut.accel, WithinAbs (original.accel, 1.0e-6));
    CHECK_THAT (settings.cut.activity, WithinAbs (original.activity, 1.0e-6));
    CHECK (settings.cut.minRepeats == original.minRepeats);
    CHECK (settings.cut.maxRepeats == original.maxRepeats);
    CHECK_FALSE (settings.crusher.enabled);
    CHECK_THAT (settings.crusher.minBits, WithinAbs (32.0, 1.0e-6));
    CHECK_THAT (settings.crusher.maxHz, WithinAbs (192000.0, 1.0e-3));
    CHECK_FALSE (settings.comb.enabled);
    CHECK (settings.comb.type == engine::CombParameters::Type::feedForward);
    CHECK_THAT (settings.comb.feedback, WithinAbs (0.5, 1.0e-6));
    CHECK_THAT (settings.comb.minDelayMs, WithinAbs (10.0, 1.0e-6));
}

TEST_CASE ("Settings follow the values the host sets", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();

    setValue (state, ids::subdivision, 3.0f); // "16"
    setValue (state, ids::seed, 7.0f);
    setValue (state, ids::stutterChance, 0.25f);
    setValue (state, ids::minPitch, -1200.0f);
    setValue (state, ids::combType, 1.0f);
    setValue (state, ids::maxDelay, 33.0f);

    const auto settings = SettingsReader (state).read();

    CHECK (settings.unitsPerBar == 16);
    CHECK (settings.seed == 7);
    CHECK_THAT (settings.cut.stutterChance, WithinAbs (0.25, 1.0e-5));
    CHECK_THAT (settings.cut.minCents, WithinAbs (-1200.0, 1.0e-2));
    CHECK (settings.comb.type == engine::CombParameters::Type::feedBack);
    CHECK_THAT (settings.comb.maxDelayMs, WithinAbs (33.0, 1.0e-3));
}

TEST_CASE ("Skewed ranges centre where the original plugin did", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();

    CHECK_THAT (parameter (state, ids::fade).convertFrom0to1 (0.5f), WithinAbs (5.0, 1.0e-3));
    CHECK_THAT (parameter (state, ids::minRate).convertFrom0to1 (0.5f), WithinAbs (4410.0, 1.0));
}

// Saved projects and hosts are untrusted: a NaN or out-of-range value must not
// reach the engine, where it could index past a table or a delay line.
TEST_CASE ("Settings stay usable when a parameter holds an invalid value", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();

    state.getRawParameterValue (ids::subdivision)->store (std::numeric_limits<float>::quiet_NaN());
    state.getRawParameterValue (ids::procedure)->store (99.0f);
    state.getRawParameterValue (ids::minDelay)->store (std::numeric_limits<float>::infinity());
    state.getRawParameterValue (ids::maxDelay)->store (-5.0f);
    state.getRawParameterValue (ids::maxRepeats)->store (std::numeric_limits<float>::quiet_NaN());

    const auto settings = SettingsReader (state).read();

    CHECK (settings.unitsPerBar == 8); // the default
    CHECK (settings.procedure == engine::Procedure::sqPusher);
    CHECK_THAT (settings.comb.minDelayMs, WithinAbs (10.0, 1.0e-6));
    CHECK_THAT (settings.comb.maxDelayMs, WithinAbs (1.0, 1.0e-6));
    CHECK (settings.cut.maxRepeats == 1);
}

// Livecut printed raw floats; JUCE's default text has seven decimal places.
TEST_CASE ("Parameter values read naturally in hosts", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();
    auto text = [&] (const char* id, float value)
    {
        auto& p = parameter (state, id);
        return p.getText (p.convertTo0to1 (value), 32);
    };

    CHECK (text (ids::stutterChance, 0.8f) == "80%");
    CHECK (text (ids::maxAmp, 1.0f) == "100%");
    CHECK (text (ids::combFeedback, 0.5f) == "50%");
    CHECK (text (ids::minPan, -0.2f) == "20L");
    CHECK (text (ids::minPan, 0.0f) == "C");
    CHECK (text (ids::maxPan, 0.2f) == "20R");
    CHECK (text (ids::minPitch, 300.0f) == "+300");
    CHECK (text (ids::minPitch, -1200.0f) == "-1200");
    CHECK (text (ids::minPitch, 0.0f) == "0");
    CHECK (text (ids::fade, 5.0f) == "5.0");
    CHECK (text (ids::maxDelay, 10.0f) == "10.0");
    CHECK (text (ids::minRate, 4410.0f) == "4.41");
    CHECK (text (ids::minRate, 44100.0f) == "44.1");
    CHECK (text (ids::maxRate, 192000.0f) == "192");
    CHECK (text (ids::maxBits, 32.0f) == "32.0");
    CHECK (text (ids::accel, 0.9f) == "0.90");

    CHECK (parameter (state, ids::minRate).getLabel() == "kHz");
    CHECK (parameter (state, ids::minPitch).getLabel() == "cents");
    CHECK (parameter (state, ids::stutterChance).getLabel().isEmpty());
}

TEST_CASE ("Typed values are read in the same format", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();
    auto valueOf = [&] (const char* id, const char* text)
    {
        auto& p = parameter (state, id);
        return p.convertFrom0to1 (p.getValueForText (text));
    };

    CHECK_THAT (valueOf (ids::stutterChance, "80%"), WithinAbs (0.8, 1.0e-4));
    CHECK_THAT (valueOf (ids::minPan, "20L"), WithinAbs (-0.2, 1.0e-4));
    CHECK_THAT (valueOf (ids::minPan, "C"), WithinAbs (0.0, 1.0e-4));
    CHECK_THAT (valueOf (ids::maxPan, "35R"), WithinAbs (0.35, 1.0e-4));
    CHECK_THAT (valueOf (ids::minRate, "4.41"), WithinAbs (4410.0, 0.5));
    CHECK_THAT (valueOf (ids::minPitch, "+300"), WithinAbs (300.0, 1.0e-2));
}
