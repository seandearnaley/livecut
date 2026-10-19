// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace livecut::ui
{

// A minimum and a maximum parameter edited as one slider with two thumbs.
// Dragging a thumb is one gesture on its parameter, so hosts record it as
// automation; keyboard and programmatic changes are a complete gesture each.
class RangeSlider final : public juce::Component
{
public:
    RangeSlider (juce::RangedAudioParameter& minimum, juce::RangedAudioParameter& maximum, const juce::String& caption);

    juce::Slider& getSlider() { return slider; }

    void resized() override;

private:
    juce::ParameterAttachment& attachmentFor (int thumb);
    void sliderChanged();
    void showValues();

    juce::RangedAudioParameter& minimum;
    juce::RangedAudioParameter& maximum;
    juce::Label caption, readout;
    juce::Slider slider { juce::Slider::TwoValueHorizontal, juce::Slider::NoTextBox };
    juce::ParameterAttachment minimumAttachment, maximumAttachment;
    int draggedThumb = -1;
};

} // namespace livecut::ui
