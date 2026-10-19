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

const std::atomic<float>* raw (const juce::AudioProcessorValueTreeState& state, const char* id)
{
    const auto* value = state.getRawParameterValue (id);
    jassert (value != nullptr);
    return value;
}

float load (const std::atomic<float>* value)
{
    return value->load (std::memory_order_relaxed);
}

int loadIndex (const std::atomic<float>* value)
{
    return static_cast<int> (std::lround (load (value)));
}
} // namespace

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
    : procedure (raw (state, ids::procedure)),
      subdivision (raw (state, ids::subdivision)),
      seed (raw (state, ids::seed)),
      minPhraseBars (raw (state, ids::minPhraseBars)),
      maxPhraseBars (raw (state, ids::maxPhraseBars)),
      fade (raw (state, ids::fade)),
      minAmp (raw (state, ids::minAmp)),
      maxAmp (raw (state, ids::maxAmp)),
      minPan (raw (state, ids::minPan)),
      maxPan (raw (state, ids::maxPan)),
      minPitch (raw (state, ids::minPitch)),
      maxPitch (raw (state, ids::maxPitch)),
      duty (raw (state, ids::duty)),
      fillDuty (raw (state, ids::fillDuty)),
      minRepeats (raw (state, ids::minRepeats)),
      maxRepeats (raw (state, ids::maxRepeats)),
      stutterChance (raw (state, ids::stutterChance)),
      stutterArea (raw (state, ids::stutterArea)),
      straightChance (raw (state, ids::straightChance)),
      regularChance (raw (state, ids::regularChance)),
      ritardChance (raw (state, ids::ritardChance)),
      accel (raw (state, ids::accel)),
      activity (raw (state, ids::activity)),
      crusher (raw (state, ids::crusher)),
      minBits (raw (state, ids::minBits)),
      maxBits (raw (state, ids::maxBits)),
      minRate (raw (state, ids::minRate)),
      maxRate (raw (state, ids::maxRate)),
      comb (raw (state, ids::comb)),
      combType (raw (state, ids::combType)),
      combFeedback (raw (state, ids::combFeedback)),
      minDelay (raw (state, ids::minDelay)),
      maxDelay (raw (state, ids::maxDelay))
{
}

Settings SettingsReader::read() const
{
    Settings settings;
    settings.procedure = static_cast<engine::Procedure> (loadIndex (procedure));
    settings.unitsPerBar = subdivisions[static_cast<std::size_t> (loadIndex (subdivision))];
    settings.seed = static_cast<std::uint32_t> (loadIndex (seed));
    settings.fadeMilliseconds = load (fade);

    auto& cut = settings.cut;
    cut.minPhraseBars = loadIndex (minPhraseBars);
    cut.maxPhraseBars = loadIndex (maxPhraseBars);
    cut.minAmp = load (minAmp);
    cut.maxAmp = load (maxAmp);
    cut.minPan = load (minPan);
    cut.maxPan = load (maxPan);
    cut.minCents = load (minPitch);
    cut.maxCents = load (maxPitch);
    cut.dutyCycle = load (duty);
    cut.fillDutyCycle = load (fillDuty);
    cut.minRepeats = loadIndex (minRepeats);
    cut.maxRepeats = loadIndex (maxRepeats);
    cut.stutterChance = load (stutterChance);
    cut.stutterArea = load (stutterArea);
    cut.straightChance = load (straightChance);
    cut.regularChance = load (regularChance);
    cut.ritardChance = load (ritardChance);
    cut.accel = load (accel);
    cut.activity = load (activity);

    auto& crusherSettings = settings.crusher;
    crusherSettings.enabled = load (crusher) > 0.5f;
    crusherSettings.minBits = load (minBits);
    crusherSettings.maxBits = load (maxBits);
    crusherSettings.minHz = load (minRate);
    crusherSettings.maxHz = load (maxRate);

    auto& combSettings = settings.comb;
    combSettings.enabled = load (comb) > 0.5f;
    combSettings.type = loadIndex (combType) == 0 ? engine::CombParameters::Type::feedForward
                                                  : engine::CombParameters::Type::feedBack;
    combSettings.feedback = load (combFeedback);
    combSettings.minDelayMs = load (minDelay);
    combSettings.maxDelayMs = load (maxDelay);
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
