// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace livecut::ui
{

// A minimum and a maximum parameter edited as one slider with two thumbs.
// A drag is one gesture on each parameter it moves, including the other
// thumb when the dragged one pushes it along, so hosts record it as
// automation; keyboard and programmatic changes are a complete gesture each.
class RangeSlider final : public juce::Component
{
public:
    RangeSlider (juce::RangedAudioParameter& minimum, juce::RangedAudioParameter& maximum, const juce::String& caption);

    juce::Slider& getSlider() { return slider; }

    void resized() override;

private:
    struct Side
    {
        juce::RangedAudioParameter& parameter;
        juce::ParameterAttachment& attachment;
        bool inGesture = false;
    };

    void pushValues();
    void push (Side& side, float value);
    void endGestures();
    void showValues();

    juce::RangedAudioParameter& minimum;
    juce::RangedAudioParameter& maximum;
    juce::Label caption, readout;
    juce::Slider slider { juce::Slider::TwoValueHorizontal, juce::Slider::NoTextBox };
    juce::ParameterAttachment minimumAttachment, maximumAttachment;
    Side minimumSide { minimum, minimumAttachment };
    Side maximumSide { maximum, maximumAttachment };
    bool dragging = false;
};

} // namespace livecut::ui
