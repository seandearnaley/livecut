// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

namespace livecut
{
// Global linear wet/dry followed by output gain, both smoothed over 20 ms.
// Shared values for stereo prevent pan movement during automation.
class OutputMixer
{
public:
    void prepare (double sampleRate, float wet, float trimDb)
    {
        mix.reset (sampleRate, 0.02);
        gain.reset (sampleRate, 0.02);
        mix.setCurrentAndTargetValue (wet);
        gain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (trimDb));
    }
    void setTargets (float wet, float trimDb)
    {
        mix.setTargetValue (wet);
        gain.setTargetValue (juce::Decibels::decibelsToGain (trimDb));
    }
    void process (float dryLeft, float dryRight, float& left, float& right)
    {
        const auto wet = mix.getNextValue();
        const auto outputGain = gain.getNextValue();
        if (juce::exactlyEqual (wet, 0.0f))
        {
            left = dryLeft;
            right = dryRight;
        }
        else if (! juce::exactlyEqual (wet, 1.0f))
        {
            left = dryLeft + wet * (left - dryLeft);
            right = dryRight + wet * (right - dryRight);
        }
        if (! juce::exactlyEqual (outputGain, 1.0f))
        {
            left *= outputGain;
            right *= outputGain;
        }
    }

private:
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mix { 1.0f }, gain { 1.0f };
};
} // namespace livecut
