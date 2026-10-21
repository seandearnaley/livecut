// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/Cutter.h"
#include "engine/ImpactEffects.h"
#include "engine/Fracture.h"
#include "engine/FrequencyEffects.h"
#include "engine/Resonator.h"
#include "engine/RouteRack.h"

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
inline constexpr auto swing = "swing";
inline constexpr auto accent = "accent";
inline constexpr auto hold = "hold";
inline constexpr auto mode = "mode";
inline constexpr auto attackAlign = "attackAlign";
inline constexpr auto attackThreshold = "attackThreshold";
inline constexpr auto reverseChance = "reverseChance";
inline constexpr auto impactFilterEnabled = "impactFilterEnabled";
inline constexpr auto impactFilterType = "impactFilterType";
inline constexpr auto impactFilterChance = "impactFilterChance";
inline constexpr auto impactFilterCutoff = "impactFilterCutoff";
inline constexpr auto impactFilterQ = "impactFilterQ";
inline constexpr auto impactFilterEnvOctaves = "impactFilterEnvOctaves";
inline constexpr auto impactFilterRepeatOctaves = "impactFilterRepeatOctaves";
inline constexpr auto impactFilterMix = "impactFilterMix";
inline constexpr auto impactDriveEnabled = "impactDriveEnabled";
inline constexpr auto impactDriveMode = "impactDriveMode";
inline constexpr auto impactDriveChance = "impactDriveChance";
inline constexpr auto impactDriveDb = "impactDriveDb";
inline constexpr auto impactDriveEnvDb = "impactDriveEnvDb";
inline constexpr auto impactDriveRepeatDb = "impactDriveRepeatDb";
inline constexpr auto impactDriveMix = "impactDriveMix";
inline constexpr auto impactTransientEnabled = "impactTransientEnabled";
inline constexpr auto impactTransientChance = "impactTransientChance";
inline constexpr auto impactTransientAttackDb = "impactTransientAttackDb";
inline constexpr auto impactTransientSustainDb = "impactTransientSustainDb";
inline constexpr auto impactTransientTimeMs = "impactTransientTimeMs";
inline constexpr auto impactTransientMix = "impactTransientMix";
inline constexpr auto impactAttackMs = "impactAttackMs";
inline constexpr auto impactDecayMs = "impactDecayMs";
inline constexpr auto fractureProcedure = "fractureProcedure";
inline constexpr auto fractureActivity = "fractureActivity";
inline constexpr auto fractureGrainSizeMs = "fractureGrainSizeMs";
inline constexpr auto fractureGrainDensity = "fractureGrainDensity";
inline constexpr auto fractureGrainPositionSpreadMs = "fractureGrainPositionSpreadMs";
inline constexpr auto fractureGrainPitchSpreadCents = "fractureGrainPitchSpreadCents";
inline constexpr auto fractureGrainPanSpread = "fractureGrainPanSpread";
inline constexpr auto fractureTapeStartSpeed = "fractureTapeStartSpeed";
inline constexpr auto fractureTapeEndSpeed = "fractureTapeEndSpeed";
inline constexpr auto fractureTapeCurve = "fractureTapeCurve";
inline constexpr auto fractureTapeCycle = "fractureTapeCycle";
inline constexpr auto fractureTapeAlternate = "fractureTapeAlternate";
inline constexpr auto fractureFrequencyEnabled = "fractureFrequencyEnabled";
inline constexpr auto fractureFrequencyMode = "fractureFrequencyMode";
inline constexpr auto fractureFrequencyChance = "fractureFrequencyChance";
inline constexpr auto fractureFrequencyHz = "fractureFrequencyHz";
inline constexpr auto fractureFrequencyEnvHz = "fractureFrequencyEnvHz";
inline constexpr auto fractureFrequencyRepeatHz = "fractureFrequencyRepeatHz";
inline constexpr auto fractureFrequencyMix = "fractureFrequencyMix";
inline constexpr auto resonanceEnabled = "resonanceEnabled";
inline constexpr auto resonanceRootHz = "resonanceRootHz";
inline constexpr auto resonanceDecayMs = "resonanceDecayMs";
inline constexpr auto resonanceExcitationDb = "resonanceExcitationDb";
inline constexpr auto resonanceInterval1 = "resonanceInterval1";
inline constexpr auto resonanceInterval2 = "resonanceInterval2";
inline constexpr auto resonanceInterval3 = "resonanceInterval3";
inline constexpr auto resonanceInterval4 = "resonanceInterval4";
inline constexpr auto resonanceWeight1 = "resonanceWeight1";
inline constexpr auto resonanceWeight2 = "resonanceWeight2";
inline constexpr auto resonanceWeight3 = "resonanceWeight3";
inline constexpr auto resonanceWeight4 = "resonanceWeight4";
inline constexpr auto resonanceMix = "resonanceMix";
inline constexpr auto resonanceOrder = "resonanceOrder";
inline constexpr auto resonanceSpectralEnabled = "resonanceSpectralEnabled";
inline constexpr auto resonanceFreeze = "resonanceFreeze";
inline constexpr auto resonanceSmearMs = "resonanceSmearMs";
inline constexpr auto resonanceSpectralMix = "resonanceSpectralMix";
} // namespace ids

inline constexpr std::array subdivisions { 6, 8, 12, 16, 18, 24, 32 };

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

enum class ProcessingMode { live, studio };

// Everything the cutter needs from the parameters, read in one go.
struct Settings
{
    engine::Procedure procedure = engine::Procedure::cutProc11;
    int unitsPerBar = 8;
    std::uint32_t seed = 1;
    double fadeMilliseconds = 0.0;
    float wet = 1.0f;
    float outputTrimDb = 0.0f;
    double swingPercent = 50.0;
    float accent = 0.0f;
    bool hold = false;
    ProcessingMode requestedMode = ProcessingMode::live;
    bool attackAlign = false;
    float attackThresholdDb = 6.0f, reverseChance = 0.0f;
    engine::CutParameters cut;
    engine::CrusherParameters crusher;
    engine::CombParameters comb;
    engine::ImpactParameters impact;
    engine::FractureParameters fracture;
    engine::FrequencyParameters frequency;
    engine::ResonatorParameters resonance;
    engine::EffectRoute resonanceOrder = engine::EffectRoute::impactResonatorFrequency;
    bool requestedSpectral = false, spectralFreeze = false;
    float spectralSmearMs = 0.0f, spectralMix = 1.0f;
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
    Source swing;
    Source accent;
    Source hold, mode, attackAlign, attackThreshold, reverseChance;
    Source impactFilterEnabled;
    Source impactFilterType;
    Source impactFilterChance;
    Source impactFilterCutoff;
    Source impactFilterQ;
    Source impactFilterEnvOctaves;
    Source impactFilterRepeatOctaves;
    Source impactFilterMix;
    Source impactDriveEnabled;
    Source impactDriveMode;
    Source impactDriveChance;
    Source impactDriveDb;
    Source impactDriveEnvDb;
    Source impactDriveRepeatDb;
    Source impactDriveMix;
    Source impactTransientEnabled;
    Source impactTransientChance;
    Source impactTransientAttackDb;
    Source impactTransientSustainDb;
    Source impactTransientTimeMs;
    Source impactTransientMix;
    Source impactAttackMs;
    Source impactDecayMs;
    Source fractureProcedure;
    Source fractureActivity;
    Source fractureGrainSizeMs;
    Source fractureGrainDensity;
    Source fractureGrainPositionSpreadMs;
    Source fractureGrainPitchSpreadCents;
    Source fractureGrainPanSpread;
    Source fractureTapeStartSpeed;
    Source fractureTapeEndSpeed;
    Source fractureTapeCurve;
    Source fractureTapeCycle;
    Source fractureTapeAlternate;
    Source fractureFrequencyEnabled;
    Source fractureFrequencyMode;
    Source fractureFrequencyChance;
    Source fractureFrequencyHz;
    Source fractureFrequencyEnvHz;
    Source fractureFrequencyRepeatHz;
    Source fractureFrequencyMix;
    Source resonanceEnabled;
    Source resonanceRootHz;
    Source resonanceDecayMs;
    Source resonanceExcitationDb;
    Source resonanceInterval1;
    Source resonanceInterval2;
    Source resonanceInterval3;
    Source resonanceInterval4;
    Source resonanceWeight1;
    Source resonanceWeight2;
    Source resonanceWeight3;
    Source resonanceWeight4;
    Source resonanceMix;
    Source resonanceOrder;
    Source resonanceSpectralEnabled;
    Source resonanceFreeze;
    Source resonanceSmearMs;
    Source resonanceSpectralMix;


};

void applySettings (engine::Cutter& cutter, const Settings& settings);

} // namespace livecut
