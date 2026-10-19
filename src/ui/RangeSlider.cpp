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
        // The touched thumb's gesture starts with the touch, before anything moves.
        dragging = true;
        auto& touched = slider.getThumbBeingDragged() == 2 ? maximumSide : minimumSide;
        touched.attachment.beginGesture();
        touched.inGesture = true;
    };
    slider.onDragEnd = [this]
    {
        // Two-value sliders report drags asynchronously, so take the final
        // values now, while the gestures are still open.
        pushValues();
        endGestures();
        dragging = false;
    };
    slider.onValueChange = [this] { pushValues(); };

    addAndMakeVisible (caption);
    addAndMakeVisible (slider);
    addAndMakeVisible (readout);

    minimumAttachment.sendInitialUpdate();
    maximumAttachment.sendInitialUpdate();
}

void RangeSlider::pushValues()
{
    push (minimumSide, static_cast<float> (slider.getMinValue()));
    push (maximumSide, static_cast<float> (slider.getMaxValue()));
    showValues();
}

void RangeSlider::push (Side& side, float value)
{
    if (! differs (side.parameter, value))
        return;

    // During a drag every move is part of a gesture; a thumb pushed along by
    // the other one opens its own gesture the first time it moves.
    if (dragging && ! side.inGesture)
    {
        side.attachment.beginGesture();
        side.inGesture = true;
    }

    if (side.inGesture)
        side.attachment.setValueAsPartOfGesture (value);
    else
        side.attachment.setValueAsCompleteGesture (value);
}

void RangeSlider::endGestures()
{
    for (auto* side : { &minimumSide, &maximumSide })
    {
        if (side->inGesture)
        {
            side->attachment.endGesture();
            side->inGesture = false;
        }
    }
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
