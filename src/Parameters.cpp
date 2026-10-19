// SPDX-License-Identifier: GPL-3.0-or-later

#include "Parameters.h"

#include <cmath>
#include <memory>

namespace livecut
{

namespace
{
constexpr int version = 1;

using FloatRange = juce::NormalisableRange<float>;

std::unique_ptr<juce::AudioParameterFloat> number (const char* id, const char* name, FloatRange range, float defaultValue, const char* label = "")
{
    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, version }, name, range, defaultValue,
                                                        juce::AudioParameterFloatAttributes().withLabel (label));
}

std::unique_ptr<juce::AudioParameterInt> integer (const char* id, const char* name, int min, int max, int defaultValue, const char* label = "")
{
    return std::make_unique<juce::AudioParameterInt> (juce::ParameterID { id, version }, name, min, max, defaultValue,
                                                      juce::AudioParameterIntAttributes().withLabel (label));
}

std::unique_ptr<juce::AudioParameterChoice> choice (const char* id, const char* name, const juce::StringArray& choices, int defaultIndex)
{
    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, version }, name, choices, defaultIndex);
}

std::unique_ptr<juce::AudioParameterBool> toggle (const char* id, const char* name, bool defaultValue)
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
    // Ranges and defaults are the original plugin's.
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (choice (ids::procedure, "Cut Procedure", { "CutProc11", "WarpCut", "SQPusher" }, 0),
                choice (ids::subdivision, "Subdivision", subdivisionNames(), 1),
                integer (ids::seed, "Seed", 1, 16, 1),
                integer (ids::minPhraseBars, "Min Phrase", 1, 8, 1, "bars"),
                integer (ids::maxPhraseBars, "Max Phrase", 1, 8, 4, "bars"),
                number (ids::fade, "Fade", skewed (0.0f, 100.0f, 5.0f), 0.0f, "ms"),
                number (ids::minAmp, "Min Amp", FloatRange (0.0f, 1.0f), 1.0f),
                number (ids::maxAmp, "Max Amp", FloatRange (0.0f, 1.0f), 1.0f),
                number (ids::minPan, "Min Pan", FloatRange (-1.0f, 1.0f), -0.2f),
                number (ids::maxPan, "Max Pan", FloatRange (-1.0f, 1.0f), 0.2f),
                number (ids::minPitch, "Min Pitch", FloatRange (-2400.0f, 2400.0f), 0.0f, "cents"),
                number (ids::maxPitch, "Max Pitch", FloatRange (-2400.0f, 2400.0f), 0.0f, "cents"),
                number (ids::duty, "Duty", FloatRange (0.0f, 1.0f), 1.0f),
                number (ids::fillDuty, "Fill Duty", FloatRange (0.0f, 1.0f), 1.0f),
                integer (ids::minRepeats, "Min Repeats", 0, 4, 0),
                integer (ids::maxRepeats, "Max Repeats", 0, 4, 1),
                number (ids::stutterChance, "Stutter", FloatRange (0.0f, 1.0f), 0.8f),
                number (ids::stutterArea, "Stutter Area", FloatRange (0.0f, 1.0f), 0.5f),
                number (ids::straightChance, "Straight", FloatRange (0.0f, 1.0f), 0.3f),
                number (ids::regularChance, "Regular", FloatRange (0.0f, 1.0f), 0.5f),
                number (ids::ritardChance, "Ritard", FloatRange (0.0f, 1.0f), 0.5f),
                number (ids::accel, "Accel", FloatRange (0.5f, 0.999f), 0.9f),
                number (ids::activity, "Activity", FloatRange (0.0f, 1.0f), 0.5f),
                toggle (ids::crusher, "Crusher", false),
                number (ids::minBits, "Min Bits", FloatRange (1.0f, 32.0f), 32.0f, "bits"),
                number (ids::maxBits, "Max Bits", FloatRange (1.0f, 32.0f), 32.0f, "bits"),
                number (ids::minRate, "Min Rate", skewed (100.0f, 192000.0f, 4410.0f), 192000.0f, "Hz"),
                number (ids::maxRate, "Max Rate", skewed (100.0f, 192000.0f, 4410.0f), 192000.0f, "Hz"),
                toggle (ids::comb, "Comb", false),
                choice (ids::combType, "Comb Type", { "Feedforward", "Feedback" }, 0),
                number (ids::combFeedback, "Comb Feedback", FloatRange (0.0f, 0.9f), 0.5f),
                number (ids::minDelay, "Min Delay", FloatRange (1.0f, 50.0f), 10.0f, "ms"),
                number (ids::maxDelay, "Max Delay", FloatRange (1.0f, 50.0f), 10.0f, "ms"));

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
      maxDelay (state, ids::maxDelay)
{
}

Settings SettingsReader::read() const
{
    Settings settings;
    settings.procedure = static_cast<engine::Procedure> (procedure.readIndex());
    settings.unitsPerBar = subdivisions[static_cast<std::size_t> (subdivision.readIndex())];
    settings.seed = static_cast<std::uint32_t> (seed.readIndex());
    settings.fadeMilliseconds = fade.read();

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
    return settings;
}

void applySettings (engine::Cutter& cutter, const Settings& settings)
{
    cutter.setProcedure (settings.procedure);
    cutter.setUnitsPerBar (settings.unitsPerBar);
    cutter.setSeed (settings.seed);
    cutter.setFadeMilliseconds (settings.fadeMilliseconds);
    cutter.setParameters (settings.cut);
    cutter.setCrusher (settings.crusher);
    cutter.setComb (settings.comb);
}

} // namespace livecut
