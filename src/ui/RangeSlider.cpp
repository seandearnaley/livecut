// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/RangeSlider.h"

#include "ui/LivecutLookAndFeel.h"

#include <cmath>

namespace livecut::ui
{

namespace
{
bool differs (const juce::RangedAudioParameter& parameter, float value)
{
    return std::abs (parameter.convertFrom0to1 (parameter.getValue()) - value) > 1.0e-6f;
}

juce::String valueText (const juce::RangedAudioParameter& parameter)
{
    return parameter.getCurrentValueAsText();
}
} // namespace

RangeSlider::RangeSlider (juce::RangedAudioParameter& minimumParameter,
                          juce::RangedAudioParameter& maximumParameter,
                          const juce::String& captionText)
    : minimum (minimumParameter),
      maximum (maximumParameter),
      minimumAttachment (minimumParameter, [this] (float value)
      {
          slider.setMinValue (value, juce::dontSendNotification, true);
          showValues();
      }),
      maximumAttachment (maximumParameter, [this] (float value)
      {
          slider.setMaxValue (value, juce::dontSendNotification, true);
          showValues();
      })
{
    setComponentID (minimum.getParameterID() + "," + maximum.getParameterID());

    caption.setText (captionText, juce::dontSendNotification);
    readout.setJustificationType (juce::Justification::centredRight);
    readout.setColour (juce::Label::textColourId, palette::inkMuted);
    caption.setBorderSize ({});
    readout.setBorderSize ({});

    const auto& range = minimum.getNormalisableRange();
    slider.setNormalisableRange ({ range.start, range.end, range.interval, range.skew, range.symmetricSkew });
    slider.setTitle (captionText);
    slider.onDragStart = [this]
    {
        draggedThumb = slider.getThumbBeingDragged();
        attachmentFor (draggedThumb).beginGesture();
    };
    slider.onDragEnd = [this]
    {
        attachmentFor (draggedThumb).endGesture();
        draggedThumb = -1;
    };
    slider.onValueChange = [this] { sliderChanged(); };

    addAndMakeVisible (caption);
    addAndMakeVisible (slider);
    addAndMakeVisible (readout);

    minimumAttachment.sendInitialUpdate();
    maximumAttachment.sendInitialUpdate();
}

juce::ParameterAttachment& RangeSlider::attachmentFor (int thumb)
{
    return thumb == 2 ? maximumAttachment : minimumAttachment;
}

void RangeSlider::sliderChanged()
{
    const auto newMinimum = static_cast<float> (slider.getMinValue());
    const auto newMaximum = static_cast<float> (slider.getMaxValue());

    if (draggedThumb == 1)
    {
        minimumAttachment.setValueAsPartOfGesture (newMinimum);
    }
    else if (draggedThumb == 2)
    {
        maximumAttachment.setValueAsPartOfGesture (newMaximum);
    }
    else
    {
        if (differs (minimum, newMinimum))
            minimumAttachment.setValueAsCompleteGesture (newMinimum);

        if (differs (maximum, newMaximum))
            maximumAttachment.setValueAsCompleteGesture (newMaximum);
    }

    showValues();
}

void RangeSlider::showValues()
{
    const auto low = valueText (minimum);
    const auto high = valueText (maximum);
    const auto unit = minimum.getLabel();
    readout.setText ((low == high ? low : low + " to " + high) + (unit.isEmpty() ? "" : " " + unit),
                     juce::dontSendNotification);
}

void RangeSlider::resized()
{
    // Caption and values on one line, the slider full width beneath.
    auto area = getLocalBounds();
    auto top = area.removeFromTop (18);
    caption.setBounds (top.removeFromLeft (top.getWidth() / 3));
    readout.setBounds (top);
    slider.setBounds (area);
}

} // namespace livecut::ui
