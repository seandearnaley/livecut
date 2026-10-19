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
constexpr int version = 1;

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

std::unique_ptr<juce::AudioParameterFloat> number (const char* id, const char* name, FloatRange range, float defaultValue, const Format& format)
{
    const auto attributes = juce::AudioParameterFloatAttributes()
                                .withStringFromValueFunction ([toText = format.toText] (float value, int) { return toText (value); })
                                .withValueFromStringFunction (format.fromText)
                                .withLabel (format.label);
    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, version }, name, range, defaultValue, attributes);
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
                number (ids::maxDelay, "Max Delay", FloatRange (1.0f, 50.0f), 10.0f, decimals (1, "ms")));

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
