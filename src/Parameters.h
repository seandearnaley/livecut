// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/Cutter.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace livecut
{

// Parameter IDs are saved in projects and automation, so never rename one.
namespace ids
{
inline constexpr auto procedure = "procedure";
inline constexpr auto subdivision = "subdivision";
inline constexpr auto seed = "seed";
inline constexpr auto minPhraseBars = "minPhraseBars";
inline constexpr auto maxPhraseBars = "maxPhraseBars";
inline constexpr auto fade = "fade";
inline constexpr auto minAmp = "minAmp";
inline constexpr auto maxAmp = "maxAmp";
inline constexpr auto minPan = "minPan";
inline constexpr auto maxPan = "maxPan";
inline constexpr auto minPitch = "minPitch";
inline constexpr auto maxPitch = "maxPitch";
inline constexpr auto duty = "duty";
inline constexpr auto fillDuty = "fillDuty";
inline constexpr auto minRepeats = "minRepeats";
inline constexpr auto maxRepeats = "maxRepeats";
inline constexpr auto stutterChance = "stutterChance";
inline constexpr auto stutterArea = "stutterArea";
inline constexpr auto straightChance = "straightChance";
inline constexpr auto regularChance = "regularChance";
inline constexpr auto ritardChance = "ritardChance";
inline constexpr auto accel = "accel";
inline constexpr auto activity = "activity";
inline constexpr auto crusher = "crusher";
inline constexpr auto minBits = "minBits";
inline constexpr auto maxBits = "maxBits";
inline constexpr auto minRate = "minRate";
inline constexpr auto maxRate = "maxRate";
inline constexpr auto comb = "comb";
inline constexpr auto combType = "combType";
inline constexpr auto combFeedback = "combFeedback";
inline constexpr auto minDelay = "minDelay";
inline constexpr auto maxDelay = "maxDelay";
inline constexpr auto bounceChance = "bounceChance";
inline constexpr auto ratchetChance = "ratchetChance";
inline constexpr auto climb = "climb";
inline constexpr auto halftimeChance = "halftimeChance";
inline constexpr auto wet = "wet";
inline constexpr auto outputTrim = "outputTrim";
} // namespace ids

inline constexpr std::array subdivisions { 6, 8, 12, 16, 18, 24, 32 };

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

// Everything the cutter needs from the parameters, read in one go.
struct Settings
{
    engine::Procedure procedure = engine::Procedure::cutProc11;
    int unitsPerBar = 8;
    std::uint32_t seed = 1;
    double fadeMilliseconds = 0.0;
    float wet = 1.0f;
    float outputTrimDb = 0.0f;
    engine::CutParameters cut;
    engine::CrusherParameters crusher;
    engine::CombParameters comb;
};

// Reads every parameter's current value. It caches each parameter's atomic
// value and range when constructed, so read() is lock- and allocation-free on
// the audio thread. Values come from saved projects and hosts, so each is
// checked: a non-finite value reads as the parameter's default, and any other
// value is clamped to the parameter's range.
class SettingsReader
{
public:
    explicit SettingsReader (const juce::AudioProcessorValueTreeState& state);

    Settings read() const;

private:
    struct Source
    {
        Source (const juce::AudioProcessorValueTreeState& state, const char* id);

        float read() const;
        int readIndex() const;

        const std::atomic<float>* value;
        juce::NormalisableRange<float> range;
        float defaultValue;
    };

    Source procedure;
    Source subdivision;
    Source seed;
    Source minPhraseBars;
    Source maxPhraseBars;
    Source fade;
    Source minAmp;
    Source maxAmp;
    Source minPan;
    Source maxPan;
    Source minPitch;
    Source maxPitch;
    Source duty;
    Source fillDuty;
    Source minRepeats;
    Source maxRepeats;
    Source stutterChance;
    Source stutterArea;
    Source straightChance;
    Source regularChance;
    Source ritardChance;
    Source accel;
    Source activity;
    Source crusher;
    Source minBits;
    Source maxBits;
    Source minRate;
    Source maxRate;
    Source comb;
    Source combType;
    Source combFeedback;
    Source minDelay;
    Source maxDelay;
    Source bounceChance;
    Source ratchetChance;
    Source climb;
    Source halftimeChance;
    Source wet;
    Source outputTrim;
};

void applySettings (engine::Cutter& cutter, const Settings& settings);

} // namespace livecut
