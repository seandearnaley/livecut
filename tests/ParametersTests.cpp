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
    ids::combFeedback, ids::minDelay, ids::maxDelay,
    ids::bounceChance, ids::ratchetChance, ids::climb, ids::halftimeChance, ids::wet, ids::outputTrim, ids::swing, ids::accent, ids::hold,
    ids::mode, ids::attackAlign, ids::attackThreshold, ids::reverseChance
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

    CHECK (parameter (state, ids::procedure).getNumSteps() == 4);
    CHECK (parameter (state, ids::subdivision).getNumSteps() == 7);
    CHECK (parameter (state, ids::combType).getNumSteps() == 2);
    CHECK (parameter (state, ids::seed).getNumSteps() == 16);
    CHECK (parameter (state, ids::crusher).isBoolean());

    // SQPusher remains reachable below the new final choice.
    setNormalised (state, ids::procedure, 0.8f);
    CHECK (SettingsReader (state).read().procedure == engine::Procedure::sqPusher);
    setNormalised (state, ids::procedure, 1.0f);
    CHECK (SettingsReader (state).read().procedure == engine::Procedure::bounce);
}

TEST_CASE ("Defaults preserve the original plugin and add Bounce", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();

    CHECK_THAT (defaultValue (state, ids::stutterChance), WithinAbs (0.8, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::minPan), WithinAbs (-0.2, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::accel), WithinAbs (0.9, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::maxPhraseBars), WithinAbs (4.0, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::maxBits), WithinAbs (32.0, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::minDelay), WithinAbs (10.0, 1.0e-6));

    CHECK_THAT (defaultValue (state, ids::bounceChance), WithinAbs (0.4, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::ratchetChance), WithinAbs (0.7, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::climb), WithinAbs (7.0, 1.0e-6));
    CHECK_THAT (defaultValue (state, ids::halftimeChance), WithinAbs (0.15, 1.0e-6));

    const auto settings = SettingsReader (state).read();
    CHECK_THAT (settings.cut.bounceChance, WithinAbs (0.4, 1.0e-6));
    CHECK_THAT (settings.cut.ratchetChance, WithinAbs (0.7, 1.0e-6));
    CHECK (settings.cut.climb == 7);
    CHECK_THAT (settings.cut.halftimeChance, WithinAbs (0.15, 1.0e-6));
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

    setValue (state, ids::bounceChance, 0.2f);
    setValue (state, ids::ratchetChance, 0.9f);
    setValue (state, ids::climb, 11.0f);
    setValue (state, ids::halftimeChance, 0.6f);
    setValue (state, ids::subdivision, 3.0f); // "16"
    setValue (state, ids::seed, 7.0f);
    setValue (state, ids::stutterChance, 0.25f);
    setValue (state, ids::minPitch, -1200.0f);
    setValue (state, ids::combType, 1.0f);
    setValue (state, ids::maxDelay, 33.0f);

    const auto settings = SettingsReader (state).read();

    CHECK_THAT (settings.cut.bounceChance, WithinAbs (0.2, 1.0e-6));
    CHECK_THAT (settings.cut.ratchetChance, WithinAbs (0.9, 1.0e-6));
    CHECK (settings.cut.climb == 11);
    CHECK_THAT (settings.cut.halftimeChance, WithinAbs (0.6, 1.0e-6));
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
    CHECK (settings.procedure == engine::Procedure::bounce);
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

    CHECK (text (ids::bounceChance, 0.4f) == "40%");
    CHECK (text (ids::ratchetChance, 0.7f) == "70%");
    CHECK (text (ids::halftimeChance, 0.15f) == "15%");
    CHECK (text (ids::climb, 7.0f) == "+7 st");
    CHECK (text (ids::climb, 0.0f) == "0 st");
    CHECK (text (ids::climb, 12.0f) == "+12 st");
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

    CHECK_THAT (valueOf (ids::climb, "+7 st"), WithinAbs (7.0, 1.0e-6));
    CHECK_THAT (valueOf (ids::climb, "0 st"), WithinAbs (0.0, 1.0e-6));
    CHECK_THAT (valueOf (ids::bounceChance, "40%"), WithinAbs (0.4, 1.0e-6));
    CHECK_THAT (valueOf (ids::ratchetChance, "70%"), WithinAbs (0.7, 1.0e-6));
    CHECK_THAT (valueOf (ids::halftimeChance, "15%"), WithinAbs (0.15, 1.0e-6));
    CHECK_THAT (valueOf (ids::stutterChance, "80%"), WithinAbs (0.8, 1.0e-4));
    CHECK_THAT (valueOf (ids::minPan, "20L"), WithinAbs (-0.2, 1.0e-4));
    CHECK_THAT (valueOf (ids::minPan, "C"), WithinAbs (0.0, 1.0e-4));
    CHECK_THAT (valueOf (ids::maxPan, "35R"), WithinAbs (0.35, 1.0e-4));
    CHECK_THAT (valueOf (ids::minRate, "4.41"), WithinAbs (4410.0, 0.5));
    CHECK_THAT (valueOf (ids::minPitch, "+300"), WithinAbs (300.0, 1.0e-2));
}

TEST_CASE ("Parameters added for Bounce carry version hint 2", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();

    for (const auto* id : allIds)
    {
        INFO (id);
        const juce::String name (id);
        const auto isBounce = name == ids::bounceChance || name == ids::ratchetChance
                           || name == ids::climb || name == ids::halftimeChance;
        const auto isMixer = name == ids::wet || name == ids::outputTrim;
        const auto isPerform = name == ids::swing || name == ids::accent || name == ids::hold;
        const auto isRecut = name == ids::mode || name == ids::attackAlign || name == ids::attackThreshold || name == ids::reverseChance;
        CHECK (parameter (state, id).getVersionHint() == (isRecut ? 5 : isPerform ? 4 : isMixer ? 3 : isBounce ? 2 : 1));
    }
}

TEST_CASE ("Bounce parameters have the specified names and ranges and are appended", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();
    constexpr std::array bounceIds { ids::bounceChance, ids::ratchetChance, ids::climb, ids::halftimeChance };
    constexpr std::array names { "Bounce", "Ratchet", "Climb", "Halftime" };

    for (std::size_t i = 0; i < bounceIds.size(); ++i)
    {
        auto& p = parameter (state, bounceIds[i]);
        CHECK (p.getName (32) == names[i]);
        CHECK (p.getParameterIndex() == static_cast<int> (37 - bounceIds.size() + i));
        CHECK_THAT (p.convertFrom0to1 (0.0f), WithinAbs (0.0, 1.0e-6));
        CHECK_THAT (p.convertFrom0to1 (1.0f), WithinAbs (i == 2 ? 12.0 : 1.0, 1.0e-6));
    }
    CHECK (parameter (state, ids::climb).getNumSteps() == 13);
}

TEST_CASE ("Bounce settings validate host values and round Climb", "[parameters]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();
    for (const auto* id : { ids::bounceChance, ids::ratchetChance, ids::climb, ids::halftimeChance })
        parameter (state, id);

    SettingsReader reader (state);
    for (const auto invalid : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
    {
        for (const auto* id : { ids::bounceChance, ids::ratchetChance, ids::climb, ids::halftimeChance })
            state.getRawParameterValue (id)->store (invalid);
        const auto cut = reader.read().cut;
        CHECK_THAT (cut.bounceChance, WithinAbs (0.4, 1.0e-6));
        CHECK_THAT (cut.ratchetChance, WithinAbs (0.7, 1.0e-6));
        CHECK (cut.climb == 7);
        CHECK_THAT (cut.halftimeChance, WithinAbs (0.15, 1.0e-6));
    }
    for (const auto raw : { -99.0f, 99.0f })
    {
        for (const auto* id : { ids::bounceChance, ids::ratchetChance, ids::climb, ids::halftimeChance })
            state.getRawParameterValue (id)->store (raw);
        const auto cut = reader.read().cut;
        const auto expected = raw < 0.0f ? 0.0 : 1.0;
        CHECK_THAT (cut.bounceChance, WithinAbs (expected, 1.0e-6));
        CHECK_THAT (cut.ratchetChance, WithinAbs (expected, 1.0e-6));
        CHECK (cut.climb == (raw < 0.0f ? 0 : 12));
        CHECK_THAT (cut.halftimeChance, WithinAbs (expected, 1.0e-6));
    }
    state.getRawParameterValue (ids::climb)->store (7.6f);
    CHECK (reader.read().cut.climb == 8);
    state.getRawParameterValue (ids::climb)->store (7.4f);
    CHECK (reader.read().cut.climb == 7);
}

TEST_CASE ("Capture release appends version-three mixer parameters", "[parameters][capture]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();
    auto* wet = state.getParameter ("wet");
    auto* trim = state.getParameter ("outputTrim");
    REQUIRE (wet != nullptr);
    REQUIRE (trim != nullptr);
    CHECK (wet->getVersionHint() == 3);
    CHECK (trim->getVersionHint() == 3);
    CHECK (wet->getParameterIndex() == 37);
    CHECK (trim->getParameterIndex() == 38);
    CHECK_THAT (wet->convertFrom0to1 (wet->getDefaultValue()), WithinAbs (1.0, 0.0));
    CHECK_THAT (trim->convertFrom0to1 (trim->getDefaultValue()), WithinAbs (0.0, 0.0));
}

TEST_CASE ("Perform appends neutral groove and Hold parameters", "[parameters][perform]")
{
    LivecutAudioProcessor processor;
    auto& state = processor.getParameterState();
    for (const auto* id : { "swing", "accent", "hold" })
    {
        INFO (id);
        auto* parameter = state.getParameter (id);
        REQUIRE (parameter != nullptr);
        CHECK (parameter->getVersionHint() == 4);
    }
    CHECK_THAT (state.getParameter ("swing")->convertFrom0to1 (state.getParameter ("swing")->getDefaultValue()), WithinAbs (50.0, 0.0));
    CHECK_THAT (state.getParameter ("accent")->convertFrom0to1 (state.getParameter ("accent")->getDefaultValue()), WithinAbs (0.0, 0.0));
    CHECK (state.getParameter ("hold")->isBoolean());
}
