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
    engine::CutParameters cut;
    engine::CrusherParameters crusher;
    engine::CombParameters comb;
};

// Reads every parameter's current value. It caches each parameter's atomic
// value when constructed, so read() is lock- and allocation-free on the audio thread.
class SettingsReader
{
public:
    explicit SettingsReader (const juce::AudioProcessorValueTreeState& state);

    Settings read() const;

private:
    const std::atomic<float>* procedure;
    const std::atomic<float>* subdivision;
    const std::atomic<float>* seed;
    const std::atomic<float>* minPhraseBars;
    const std::atomic<float>* maxPhraseBars;
    const std::atomic<float>* fade;
    const std::atomic<float>* minAmp;
    const std::atomic<float>* maxAmp;
    const std::atomic<float>* minPan;
    const std::atomic<float>* maxPan;
    const std::atomic<float>* minPitch;
    const std::atomic<float>* maxPitch;
    const std::atomic<float>* duty;
    const std::atomic<float>* fillDuty;
    const std::atomic<float>* minRepeats;
    const std::atomic<float>* maxRepeats;
    const std::atomic<float>* stutterChance;
    const std::atomic<float>* stutterArea;
    const std::atomic<float>* straightChance;
    const std::atomic<float>* regularChance;
    const std::atomic<float>* ritardChance;
    const std::atomic<float>* accel;
    const std::atomic<float>* activity;
    const std::atomic<float>* crusher;
    const std::atomic<float>* minBits;
    const std::atomic<float>* maxBits;
    const std::atomic<float>* minRate;
    const std::atomic<float>* maxRate;
    const std::atomic<float>* comb;
    const std::atomic<float>* combType;
    const std::atomic<float>* combFeedback;
    const std::atomic<float>* minDelay;
    const std::atomic<float>* maxDelay;
};

void applySettings (engine::Cutter& cutter, const Settings& settings);

} // namespace livecut
