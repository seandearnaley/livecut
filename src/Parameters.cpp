// SPDX-License-Identifier: GPL-3.0-or-later

#include "Parameters.h"

#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>

namespace livecut
{

namespace
{
using FloatRange = juce::NormalisableRange<float>;

// How a float parameter reads in hosts and in the editor, and how typed text is read back.
struct Format
{
    std::function<juce::String (float)> toText;
    std::function<float (const juce::String&)> fromText;
    const char* label = "";
};

float numberIn (const juce::String& text)
{
    return text.retainCharacters ("-+0123456789.").getFloatValue();
}

Format decimals (int places, const char* label)
{
    return { [places] (float value) { return juce::String (value, places); }, numberIn, label };
}

Format percent()
{
    return { [] (float value) { return juce::String (juce::roundToInt (value * 100.0f)) + "%"; },
             [] (const juce::String& text) { return numberIn (text) / 100.0f; } };
}

// Pan as a mixing desk shows it: 20L, C, 20R.
Format pan()
{
    return { [] (float value)
             {
                 const auto amount = juce::roundToInt (value * 100.0f);
                 return amount == 0 ? juce::String ("C") : juce::String (std::abs (amount)) + (amount < 0 ? "L" : "R");
             },
             [] (const juce::String& text)
             {
                 const auto trimmed = text.trim().toUpperCase();
                 const auto amount = numberIn (trimmed) / 100.0f;
                 return trimmed.endsWith ("L") ? -std::abs (amount) : trimmed.endsWith ("R") ? std::abs (amount) : amount;
             } };
}

Format cents()
{
    return { [] (float value)
             {
                 const auto amount = juce::roundToInt (value);
                 return (amount > 0 ? "+" : "") + juce::String (amount);
             },
             numberIn, "cents" };
}

juce::AudioParameterIntAttributes semitones()
{
    return juce::AudioParameterIntAttributes()
        .withStringFromValueFunction ([] (int value, int)
        {
            return (value > 0 ? "+" : "") + juce::String (value) + " st";
        })
        .withValueFromStringFunction ([] (const juce::String& text) { return juce::roundToInt (numberIn (text)); });
}

// Stored in Hz, shown in kHz with three significant figures.
Format kilohertz()
{
    return { [] (float value)
             {
                 const auto khz = value / 1000.0f;
                 return juce::String (khz, khz < 10.0f ? 2 : khz < 100.0f ? 1 : 0);
             },
             [] (const juce::String& text) { return numberIn (text) * 1000.0f; }, "kHz" };
}

std::unique_ptr<juce::AudioParameterFloat> number (const char* id, const char* name, FloatRange range, float defaultValue, const Format& format, int version = 1)
{
    const auto attributes = juce::AudioParameterFloatAttributes()
                                .withStringFromValueFunction ([toText = format.toText] (float value, int) { return toText (value); })
                                .withValueFromStringFunction (format.fromText)
                                .withLabel (format.label);
    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, version }, name, range, defaultValue, attributes);
}

std::unique_ptr<juce::AudioParameterInt> integer (const char* id, const char* name, int min, int max, int defaultValue, const char* label = "", int version = 1,
                                                  const juce::AudioParameterIntAttributes& attributes = {})
{
    return std::make_unique<juce::AudioParameterInt> (juce::ParameterID { id, version }, name, min, max, defaultValue,
                                                      attributes.withLabel (label));
}

std::unique_ptr<juce::AudioParameterChoice> choice (const char* id, const char* name, const juce::StringArray& choices, int defaultIndex, int version = 1)
{
    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, version }, name, choices, defaultIndex);
}

std::unique_ptr<juce::AudioParameterBool> toggle (const char* id, const char* name, bool defaultValue, int version = 1)
{
    return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, version }, name, defaultValue);
}

FloatRange skewed (float min, float max, float centre)
{
    FloatRange range (min, max);
    range.setSkewForCentre (centre);
    return range;
}

juce::StringArray subdivisionNames()
{
    juce::StringArray names;

    for (const auto units : subdivisions)
        names.add (juce::String (units));

    return names;
}

} // namespace

SettingsReader::Source::Source (const juce::AudioProcessorValueTreeState& state, const char* id)
    : value (state.getRawParameterValue (id))
{
    const auto* parameter = state.getParameter (id);
    jassert (value != nullptr && parameter != nullptr);
    range = parameter->getNormalisableRange();
    defaultValue = parameter->convertFrom0to1 (parameter->getDefaultValue());
}

float SettingsReader::Source::read() const
{
    const auto raw = value->load (std::memory_order_relaxed);
    return std::isfinite (raw) ? range.snapToLegalValue (raw) : defaultValue;
}

int SettingsReader::Source::readIndex() const
{
    return static_cast<int> (std::lround (read()));
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    // Existing ranges and defaults are the original plugin's.
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (choice (ids::procedure, "Cut Procedure", { "CutProc11", "WarpCut", "SQPusher", "Bounce" }, 0),
                choice (ids::subdivision, "Subdivision", subdivisionNames(), 1),
                integer (ids::seed, "Seed", 1, 16, 1),
                integer (ids::minPhraseBars, "Min Phrase", 1, 8, 1, "bars"),
                integer (ids::maxPhraseBars, "Max Phrase", 1, 8, 4, "bars"),
                number (ids::fade, "Fade", skewed (0.0f, 100.0f, 5.0f), 0.0f, decimals (1, "ms")),
                number (ids::minAmp, "Min Amp", FloatRange (0.0f, 1.0f), 1.0f, percent()),
                number (ids::maxAmp, "Max Amp", FloatRange (0.0f, 1.0f), 1.0f, percent()),
                number (ids::minPan, "Min Pan", FloatRange (-1.0f, 1.0f), -0.2f, pan()),
                number (ids::maxPan, "Max Pan", FloatRange (-1.0f, 1.0f), 0.2f, pan()),
                number (ids::minPitch, "Min Pitch", FloatRange (-2400.0f, 2400.0f), 0.0f, cents()),
                number (ids::maxPitch, "Max Pitch", FloatRange (-2400.0f, 2400.0f), 0.0f, cents()),
                number (ids::duty, "Duty", FloatRange (0.0f, 1.0f), 1.0f, percent()),
                number (ids::fillDuty, "Fill Duty", FloatRange (0.0f, 1.0f), 1.0f, percent()),
                integer (ids::minRepeats, "Min Repeats", 0, 4, 0),
                integer (ids::maxRepeats, "Max Repeats", 0, 4, 1),
                number (ids::stutterChance, "Stutter", FloatRange (0.0f, 1.0f), 0.8f, percent()),
                number (ids::stutterArea, "Stutter Area", FloatRange (0.0f, 1.0f), 0.5f, percent()),
                number (ids::straightChance, "Straight", FloatRange (0.0f, 1.0f), 0.3f, percent()),
                number (ids::regularChance, "Regular", FloatRange (0.0f, 1.0f), 0.5f, percent()),
                number (ids::ritardChance, "Ritard", FloatRange (0.0f, 1.0f), 0.5f, percent()),
                number (ids::accel, "Accel", FloatRange (0.5f, 0.999f), 0.9f, decimals (2, "")),
                number (ids::activity, "Activity", FloatRange (0.0f, 1.0f), 0.5f, percent()),
                toggle (ids::crusher, "Crusher", false),
                number (ids::minBits, "Min Bits", FloatRange (1.0f, 32.0f), 32.0f, decimals (1, "bits")),
                number (ids::maxBits, "Max Bits", FloatRange (1.0f, 32.0f), 32.0f, decimals (1, "bits")),
                number (ids::minRate, "Min Rate", skewed (100.0f, 192000.0f, 4410.0f), 192000.0f, kilohertz()),
                number (ids::maxRate, "Max Rate", skewed (100.0f, 192000.0f, 4410.0f), 192000.0f, kilohertz()),
                toggle (ids::comb, "Comb", false),
                choice (ids::combType, "Comb Type", { "Feedforward", "Feedback" }, 0),
                number (ids::combFeedback, "Comb Feedback", FloatRange (0.0f, 0.9f), 0.5f, percent()),
                number (ids::minDelay, "Min Delay", FloatRange (1.0f, 50.0f), 10.0f, decimals (1, "ms")),
                number (ids::maxDelay, "Max Delay", FloatRange (1.0f, 50.0f), 10.0f, decimals (1, "ms")),
                // Added in 0.2.0; parameters added in later releases need higher version hints so hosts (AU in particular) keep parameter order stable.
                number (ids::bounceChance, "Bounce", FloatRange (0.0f, 1.0f), 0.4f, percent(), 2),
                number (ids::ratchetChance, "Ratchet", FloatRange (0.0f, 1.0f), 0.7f, percent(), 2),
                integer (ids::climb, "Climb", 0, 12, 7, "", 2, semitones()),
                number (ids::halftimeChance, "Halftime", FloatRange (0.0f, 1.0f), 0.15f, percent(), 2),
                number (ids::wet, "Wet", FloatRange (0.0f, 1.0f), 1.0f, percent(), 3),
                number (ids::outputTrim, "Output Trim", FloatRange (-24.0f, 24.0f), 0.0f, decimals (1, "dB"), 3),
                number (ids::swing, "Swing", FloatRange (50.0f, 75.0f), 50.0f, decimals (1, "%"), 4),
                number (ids::accent, "Accent", FloatRange (0.0f, 1.0f), 0.0f, percent(), 4),
                toggle (ids::hold, "Audio Hold", false, 4),
                std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { ids::mode, 5 }, "Processing Mode",
                    juce::StringArray { "Live", "Studio" }, 0, juce::AudioParameterChoiceAttributes().withAutomatable (false)),
                toggle (ids::attackAlign, "Attack Align", false, 5),
                number (ids::attackThreshold, "Attack Threshold", FloatRange (3.0f, 24.0f), 6.0f, decimals (1, "dB"), 5),
                number (ids::reverseChance, "Reverse Chance", FloatRange (0.0f, 1.0f), 0.0f, percent(), 5));

    // 0.6 appends new IDs and AU ordering hints without altering earlier parameters.
    layout.add (toggle (ids::impactFilterEnabled, "Impact Filter", false, 6),
                choice (ids::impactFilterType, "Filter Type", { "Low-pass", "Band-pass", "High-pass" }, 0, 6),
                number (ids::impactFilterChance, "Filter Chance", FloatRange (0, 1), 1, percent(), 6),
                number (ids::impactFilterCutoff, "Filter Cutoff", skewed (30, 18000, 1000), 12000, decimals (0, "Hz"), 6),
                number (ids::impactFilterQ, "Filter Resonance", FloatRange (.5f, 8), .707f, decimals (3, "Q"), 6),
                number (ids::impactFilterEnvOctaves, "Filter Envelope", FloatRange (-6, 6), 0, decimals (1, "oct"), 6),
                number (ids::impactFilterRepeatOctaves, "Filter Repeat", FloatRange (-4, 4), 0, decimals (1, "oct"), 6),
                number (ids::impactFilterMix, "Filter Mix", FloatRange (0, 1), 1, percent(), 6),
                toggle (ids::impactDriveEnabled, "Impact Drive", false, 6),
                choice (ids::impactDriveMode, "Drive Mode", { "Drive", "Wavefold" }, 0, 6),
                number (ids::impactDriveChance, "Drive Chance", FloatRange (0, 1), 1, percent(), 6),
                number (ids::impactDriveDb, "Drive Gain", FloatRange (0, 30), 6, decimals (1, "dB"), 6),
                number (ids::impactDriveEnvDb, "Drive Envelope", FloatRange (-24, 24), 0, decimals (1, "dB"), 6),
                number (ids::impactDriveRepeatDb, "Drive Repeat", FloatRange (-24, 24), 0, decimals (1, "dB"), 6),
                number (ids::impactDriveMix, "Drive Mix", FloatRange (0, 1), 1, percent(), 6),
                toggle (ids::impactTransientEnabled, "Impact Transient", false, 6),
                number (ids::impactTransientChance, "Transient Chance", FloatRange (0, 1), 1, percent(), 6),
                number (ids::impactTransientAttackDb, "Transient Attack", FloatRange (-18, 18), 0, decimals (1, "dB"), 6),
                number (ids::impactTransientSustainDb, "Transient Sustain", FloatRange (-18, 18), 0, decimals (1, "dB"), 6),
                number (ids::impactTransientTimeMs, "Transient Response", FloatRange (10, 200), 40, decimals (1, "ms"), 6),
                number (ids::impactTransientMix, "Transient Mix", FloatRange (0, 1), 1, percent(), 6),
                number (ids::impactAttackMs, "Impact Envelope Attack", FloatRange (0, 25), 1, decimals (1, "ms"), 6),
                number (ids::impactDecayMs, "Impact Envelope Decay", FloatRange (5, 1000), 80, decimals (1, "ms"), 6));

    // 0.7 preserves all preceding host indices and appends its frozen family/FX IDs.
    layout.add (choice (ids::fractureProcedure, "Fracture Family", { "Classic", "GrainCut", "TapeCut" }, 0, 7),
                number (ids::fractureActivity, "Grain Activity", FloatRange (0, 1), .5f, percent(), 7),
                number (ids::fractureGrainSizeMs, "Grain Size", FloatRange (10, 160), 40, decimals (1, "ms"), 7),
                number (ids::fractureGrainDensity, "Grain Density", FloatRange (.5f, 4), 2, decimals (2, "x"), 7),
                number (ids::fractureGrainPositionSpreadMs, "Grain Position Spread", FloatRange (0, 250), 40, decimals (1, "ms"), 7),
                number (ids::fractureGrainPitchSpreadCents, "Grain Pitch Spread", FloatRange (0, 1200), 300, decimals (0, "cents"), 7),
                number (ids::fractureGrainPanSpread, "Grain Pan Spread", FloatRange (0, 1), .3f, percent(), 7),
                number (ids::fractureTapeStartSpeed, "Tape Start Speed", FloatRange (0, 1), 1, decimals (2, "x"), 7),
                number (ids::fractureTapeEndSpeed, "Tape End Speed", FloatRange (0, 1), 0, decimals (2, "x"), 7),
                choice (ids::fractureTapeCurve, "Tape Curve", { "Linear", "Smooth" }, 1, 7),
                choice (ids::fractureTapeCycle, "Tape Cycle", { "1/2 beat", "1 beat", "2 beats", "4 beats" }, 1, 7),
                toggle (ids::fractureTapeAlternate, "Tape Alternate", false, 7),
                toggle (ids::fractureFrequencyEnabled, "Ring / Frequency Shift", false, 7),
                choice (ids::fractureFrequencyMode, "Frequency Mode", { "Ring", "Frequency Shift" }, 0, 7),
                number (ids::fractureFrequencyChance, "Frequency Chance", FloatRange (0, 1), 1, percent(), 7),
                number (ids::fractureFrequencyHz, "Frequency Rate", FloatRange (-2000, 2000), 100, decimals (1, "Hz"), 7),
                number (ids::fractureFrequencyEnvHz, "Frequency Envelope", FloatRange (-2000, 2000), 0, decimals (1, "Hz"), 7),
                number (ids::fractureFrequencyRepeatHz, "Frequency Repeat", FloatRange (-2000, 2000), 0, decimals (1, "Hz"), 7),
                number (ids::fractureFrequencyMix, "Frequency Mix", FloatRange (0, 1), 1, percent(), 7));

    return layout;
}

SettingsReader::SettingsReader (const juce::AudioProcessorValueTreeState& state)
    : procedure (state, ids::procedure),
      subdivision (state, ids::subdivision),
      seed (state, ids::seed),
      minPhraseBars (state, ids::minPhraseBars),
      maxPhraseBars (state, ids::maxPhraseBars),
      fade (state, ids::fade),
      minAmp (state, ids::minAmp),
      maxAmp (state, ids::maxAmp),
      minPan (state, ids::minPan),
      maxPan (state, ids::maxPan),
      minPitch (state, ids::minPitch),
      maxPitch (state, ids::maxPitch),
      duty (state, ids::duty),
      fillDuty (state, ids::fillDuty),
      minRepeats (state, ids::minRepeats),
      maxRepeats (state, ids::maxRepeats),
      stutterChance (state, ids::stutterChance),
      stutterArea (state, ids::stutterArea),
      straightChance (state, ids::straightChance),
      regularChance (state, ids::regularChance),
      ritardChance (state, ids::ritardChance),
      accel (state, ids::accel),
      activity (state, ids::activity),
      crusher (state, ids::crusher),
      minBits (state, ids::minBits),
      maxBits (state, ids::maxBits),
      minRate (state, ids::minRate),
      maxRate (state, ids::maxRate),
      comb (state, ids::comb),
      combType (state, ids::combType),
      combFeedback (state, ids::combFeedback),
      minDelay (state, ids::minDelay),
      maxDelay (state, ids::maxDelay),
      bounceChance (state, ids::bounceChance),
      ratchetChance (state, ids::ratchetChance),
      climb (state, ids::climb),
      halftimeChance (state, ids::halftimeChance),
      wet (state, ids::wet),
      outputTrim (state, ids::outputTrim),
      swing (state, ids::swing),
      accent (state, ids::accent),
      hold (state, ids::hold),
      mode (state, ids::mode),
      attackAlign (state, ids::attackAlign),
      attackThreshold (state, ids::attackThreshold),
      reverseChance (state, ids::reverseChance),
      impactFilterEnabled (state, ids::impactFilterEnabled),
      impactFilterType (state, ids::impactFilterType),
      impactFilterChance (state, ids::impactFilterChance),
      impactFilterCutoff (state, ids::impactFilterCutoff),
      impactFilterQ (state, ids::impactFilterQ),
      impactFilterEnvOctaves (state, ids::impactFilterEnvOctaves),
      impactFilterRepeatOctaves (state, ids::impactFilterRepeatOctaves),
      impactFilterMix (state, ids::impactFilterMix),
      impactDriveEnabled (state, ids::impactDriveEnabled),
      impactDriveMode (state, ids::impactDriveMode),
      impactDriveChance (state, ids::impactDriveChance),
      impactDriveDb (state, ids::impactDriveDb),
      impactDriveEnvDb (state, ids::impactDriveEnvDb),
      impactDriveRepeatDb (state, ids::impactDriveRepeatDb),
      impactDriveMix (state, ids::impactDriveMix),
      impactTransientEnabled (state, ids::impactTransientEnabled),
      impactTransientChance (state, ids::impactTransientChance),
      impactTransientAttackDb (state, ids::impactTransientAttackDb),
      impactTransientSustainDb (state, ids::impactTransientSustainDb),
      impactTransientTimeMs (state, ids::impactTransientTimeMs),
      impactTransientMix (state, ids::impactTransientMix),
      impactAttackMs (state, ids::impactAttackMs),
      impactDecayMs (state, ids::impactDecayMs),
      fractureProcedure (state, ids::fractureProcedure),
      fractureActivity (state, ids::fractureActivity),
      fractureGrainSizeMs (state, ids::fractureGrainSizeMs),
      fractureGrainDensity (state, ids::fractureGrainDensity),
      fractureGrainPositionSpreadMs (state, ids::fractureGrainPositionSpreadMs),
      fractureGrainPitchSpreadCents (state, ids::fractureGrainPitchSpreadCents),
      fractureGrainPanSpread (state, ids::fractureGrainPanSpread),
      fractureTapeStartSpeed (state, ids::fractureTapeStartSpeed),
      fractureTapeEndSpeed (state, ids::fractureTapeEndSpeed),
      fractureTapeCurve (state, ids::fractureTapeCurve),
      fractureTapeCycle (state, ids::fractureTapeCycle),
      fractureTapeAlternate (state, ids::fractureTapeAlternate),
      fractureFrequencyEnabled (state, ids::fractureFrequencyEnabled),
      fractureFrequencyMode (state, ids::fractureFrequencyMode),
      fractureFrequencyChance (state, ids::fractureFrequencyChance),
      fractureFrequencyHz (state, ids::fractureFrequencyHz),
      fractureFrequencyEnvHz (state, ids::fractureFrequencyEnvHz),
      fractureFrequencyRepeatHz (state, ids::fractureFrequencyRepeatHz),
      fractureFrequencyMix (state, ids::fractureFrequencyMix)
{
}

Settings SettingsReader::read() const
{
    Settings settings;
    settings.procedure = static_cast<engine::Procedure> (procedure.readIndex());
    settings.unitsPerBar = subdivisions[static_cast<std::size_t> (subdivision.readIndex())];
    settings.seed = static_cast<std::uint32_t> (seed.readIndex());
    settings.fadeMilliseconds = fade.read();
    settings.wet = wet.read();
    settings.outputTrimDb = outputTrim.read();
    settings.swingPercent = swing.read();
    settings.accent = accent.read();
    settings.hold = hold.read() > 0.5f;
    settings.requestedMode = mode.readIndex() == 0 ? ProcessingMode::live : ProcessingMode::studio;
    settings.attackAlign = attackAlign.read() > 0.5f;
    settings.attackThresholdDb = attackThreshold.read();
    settings.reverseChance = reverseChance.read();

    auto& cut = settings.cut;
    cut.minPhraseBars = minPhraseBars.readIndex();
    cut.maxPhraseBars = maxPhraseBars.readIndex();
    cut.minAmp = minAmp.read();
    cut.maxAmp = maxAmp.read();
    cut.minPan = minPan.read();
    cut.maxPan = maxPan.read();
    cut.minCents = minPitch.read();
    cut.maxCents = maxPitch.read();
    cut.dutyCycle = duty.read();
    cut.fillDutyCycle = fillDuty.read();
    cut.minRepeats = minRepeats.readIndex();
    cut.maxRepeats = maxRepeats.readIndex();
    cut.stutterChance = stutterChance.read();
    cut.stutterArea = stutterArea.read();
    cut.straightChance = straightChance.read();
    cut.regularChance = regularChance.read();
    cut.ritardChance = ritardChance.read();
    cut.accel = accel.read();
    cut.activity = activity.read();
    cut.bounceChance = bounceChance.read();
    cut.ratchetChance = ratchetChance.read();
    cut.climb = climb.readIndex();
    cut.halftimeChance = halftimeChance.read();

    auto& crusherSettings = settings.crusher;
    crusherSettings.enabled = crusher.read() > 0.5f;
    crusherSettings.minBits = minBits.read();
    crusherSettings.maxBits = maxBits.read();
    crusherSettings.minHz = minRate.read();
    crusherSettings.maxHz = maxRate.read();

    auto& combSettings = settings.comb;
    combSettings.enabled = comb.read() > 0.5f;
    combSettings.type = combType.readIndex() == 0 ? engine::CombParameters::Type::feedForward
                                                  : engine::CombParameters::Type::feedBack;
    combSettings.feedback = combFeedback.read();
    combSettings.minDelayMs = minDelay.read();
    combSettings.maxDelayMs = maxDelay.read();

    settings.impact.filter.enabled = impactFilterEnabled.read() > 0.5f;
    settings.impact.filter.type = static_cast<engine::ImpactParameters::Filter::Type> (impactFilterType.readIndex());
    settings.impact.filter.chance = impactFilterChance.read();
    settings.impact.filter.cutoffHz = impactFilterCutoff.read();
    settings.impact.filter.q = impactFilterQ.read();
    settings.impact.filter.envelopeOctaves = impactFilterEnvOctaves.read();
    settings.impact.filter.repeatOctaves = impactFilterRepeatOctaves.read();
    settings.impact.filter.mix = impactFilterMix.read();
    settings.impact.drive.enabled = impactDriveEnabled.read() > 0.5f;
    settings.impact.drive.mode = static_cast<engine::ImpactParameters::Drive::Mode> (impactDriveMode.readIndex());
    settings.impact.drive.chance = impactDriveChance.read();
    settings.impact.drive.driveDb = impactDriveDb.read();
    settings.impact.drive.envelopeDb = impactDriveEnvDb.read();
    settings.impact.drive.repeatDb = impactDriveRepeatDb.read();
    settings.impact.drive.mix = impactDriveMix.read();
    settings.impact.transient.enabled = impactTransientEnabled.read() > 0.5f;
    settings.impact.transient.chance = impactTransientChance.read();
    settings.impact.transient.attackDb = impactTransientAttackDb.read();
    settings.impact.transient.sustainDb = impactTransientSustainDb.read();
    settings.impact.transient.timeMs = impactTransientTimeMs.read();
    settings.impact.transient.mix = impactTransientMix.read();
    settings.impact.attackMs = impactAttackMs.read();
    settings.impact.decayMs = impactDecayMs.read();

    settings.fracture.procedure = static_cast<engine::FractureProcedure> (fractureProcedure.readIndex());
    settings.fracture.activity = fractureActivity.read();
    settings.fracture.grain.sizeMs = fractureGrainSizeMs.read();
    settings.fracture.grain.density = fractureGrainDensity.read();
    settings.fracture.grain.positionSpreadMs = fractureGrainPositionSpreadMs.read();
    settings.fracture.grain.pitchSpreadCents = fractureGrainPitchSpreadCents.read();
    settings.fracture.grain.panSpread = fractureGrainPanSpread.read();
    settings.fracture.tape.startSpeed = fractureTapeStartSpeed.read();
    settings.fracture.tape.endSpeed = fractureTapeEndSpeed.read();
    settings.fracture.tape.curve = static_cast<engine::FractureParameters::Tape::Curve> (fractureTapeCurve.readIndex());
    settings.fracture.tape.cycle = static_cast<engine::FractureParameters::Tape::Cycle> (fractureTapeCycle.readIndex());
    settings.fracture.tape.alternate = fractureTapeAlternate.read() > 0.5f;
    settings.frequency.enabled = fractureFrequencyEnabled.read() > 0.5f;
    settings.frequency.mode = static_cast<engine::FrequencyParameters::Mode> (fractureFrequencyMode.readIndex());
    settings.frequency.chance = fractureFrequencyChance.read();
    settings.frequency.rateHz = fractureFrequencyHz.read();
    settings.frequency.envelopeHz = fractureFrequencyEnvHz.read();
    settings.frequency.repeatHz = fractureFrequencyRepeatHz.read();
    settings.frequency.mix = fractureFrequencyMix.read();
    return settings;
}

void applySettings (engine::Cutter& cutter, const Settings& settings)
{
    cutter.setProcedure (settings.procedure);
    cutter.setUnitsPerBar (settings.unitsPerBar);
    cutter.setSeed (settings.seed);
    cutter.setSwing (settings.swingPercent);
    cutter.setAccent (settings.accent);
    cutter.setTransforms ({ settings.attackAlign, settings.attackThresholdDb, settings.reverseChance });
    cutter.setFadeMilliseconds (settings.fadeMilliseconds);
    cutter.setParameters (settings.cut);
    cutter.setCrusher (settings.crusher);
    cutter.setComb (settings.comb);
    cutter.setImpact (settings.impact);
    cutter.setFracture (settings.fracture);
    cutter.setFrequency (settings.frequency);
}

} // namespace livecut
