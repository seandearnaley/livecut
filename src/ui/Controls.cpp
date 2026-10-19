// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/Controls.h"

#include "ui/LivecutLookAndFeel.h"

#include <cmath>

namespace livecut::ui
{

SliderRow::SliderRow (juce::RangedAudioParameter& p, const juce::String& captionText)
    : parameter (p),
      attachment (p, slider)
{
    setComponentID (parameter.getParameterID());
    caption.setText (captionText, juce::dontSendNotification);
    readout.setJustificationType (juce::Justification::centredRight);
    readout.setColour (juce::Label::textColourId, palette::inkMuted);
    caption.setBorderSize ({});
    readout.setBorderSize ({});
    slider.setTitle (captionText);
    slider.onValueChange = [this] { showValue(); };

    addAndMakeVisible (caption);
    addAndMakeVisible (slider);
    addAndMakeVisible (readout);
    showValue();
}

void SliderRow::showValue()
{
    const auto unit = parameter.getLabel();
    readout.setText (parameter.getCurrentValueAsText() + (unit.isEmpty() ? "" : " " + unit), juce::dontSendNotification);
}

void SliderRow::resized()
{
    // Caption and value on one line, the slider full width beneath, so narrow columns still get a usable slider.
    auto area = getLocalBounds();
    auto top = area.removeFromTop (18);
    caption.setBounds (top.removeFromLeft (top.getWidth() / 2));
    readout.setBounds (top);
    slider.setBounds (area);
}

ChoiceRow::ChoiceRow (juce::AudioParameterChoice& parameter, const juce::String& captionText)
    : showsCaption (captionText.isNotEmpty())
{
    setComponentID (parameter.getParameterID());
    caption.setText (captionText, juce::dontSendNotification);
    caption.setBorderSize ({});
    box.setTitle (captionText);
    box.addItemList (parameter.choices, 1);
    attachment = std::make_unique<juce::ComboBoxParameterAttachment> (parameter, box);

    addChildComponent (caption);
    caption.setVisible (showsCaption);
    addAndMakeVisible (box);
}

void ChoiceRow::resized()
{
    auto area = getLocalBounds();

    if (showsCaption)
        caption.setBounds (area.removeFromLeft (78));

    box.setBounds (area.reduced (0, 1));
}

PowerSwitch::PowerSwitch (juce::RangedAudioParameter& parameter, const juce::String& caption)
    : attachment (parameter, button)
{
    setComponentID (parameter.getParameterID());
    button.setButtonText (caption);
    button.setTitle (caption);
    button.onStateChange = [this]
    {
        if (onChange)
            onChange (button.getToggleState());
    };
    addAndMakeVisible (button);
}

void PowerSwitch::resized()
{
    button.setBounds (getLocalBounds());
}

ProcedureSwitch::ProcedureSwitch (juce::AudioParameterChoice& parameter)
    : attachment (parameter, [this] (float value) { select (static_cast<int> (std::lround (value))); })
{
    setComponentID (parameter.getParameterID());
    setTitle (parameter.getName (64));

    for (int i = 0; i < parameter.choices.size(); ++i)
    {
        auto* button = buttons.add (std::make_unique<juce::TextButton> (parameter.choices[i]));
        button->setTitle (parameter.choices[i]);
        button->onClick = [this, i] { attachment.setValueAsCompleteGesture (static_cast<float> (i)); };
        addAndMakeVisible (button);
    }

    attachment.sendInitialUpdate();
}

void ProcedureSwitch::select (int index)
{
    for (int i = 0; i < buttons.size(); ++i)
        buttons[i]->setToggleState (i == index, juce::dontSendNotification);

    if (onChange)
        onChange (index);
}

void ProcedureSwitch::resized()
{
    auto area = getLocalBounds();
    const auto width = area.getWidth() / juce::jmax (1, buttons.size());

    for (auto* button : buttons)
        button->setBounds (area.removeFromLeft (width).reduced (2, 0));
}

} // namespace livecut::ui
