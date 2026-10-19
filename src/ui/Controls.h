// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>

namespace livecut::ui
{

// Each control's component ID is its parameter's ID.

// A caption, a horizontal slider and the parameter's value as text.
class SliderRow final : public juce::Component
{
public:
    SliderRow (juce::RangedAudioParameter& parameter, const juce::String& caption);

    void resized() override;

private:
    void showValue();

    juce::RangedAudioParameter& parameter;
    juce::Label caption, readout;
    juce::Slider slider { juce::Slider::LinearHorizontal, juce::Slider::NoTextBox };
    juce::SliderParameterAttachment attachment;
};

// A caption and a drop-down of the parameter's choices. With an empty caption
// the drop-down fills the row and the caption is only its accessible title.
class ChoiceRow final : public juce::Component
{
public:
    ChoiceRow (juce::AudioParameterChoice& parameter, const juce::String& caption);

    void resized() override;

private:
    juce::Label caption;
    juce::ComboBox box;
    bool showsCaption;
    std::unique_ptr<juce::ComboBoxParameterAttachment> attachment;
};

// An on/off switch drawn as an LED.
class PowerSwitch final : public juce::Component
{
public:
    PowerSwitch (juce::RangedAudioParameter& parameter, const juce::String& caption);

    // Called with the switch's state whenever it changes, from any source.
    std::function<void (bool)> onChange;
    bool isOn() const { return button.getToggleState(); }

    void resized() override;

private:
    juce::ToggleButton button;
    juce::ButtonParameterAttachment attachment;
};

// One button per cut procedure, so switching is a single click while playing.
class ProcedureSwitch final : public juce::Component
{
public:
    explicit ProcedureSwitch (juce::AudioParameterChoice& parameter);

    // Called with the selected procedure's index whenever it changes, from any source.
    std::function<void (int)> onChange;

    void resized() override;

private:
    void select (int index);

    juce::OwnedArray<juce::TextButton> buttons;
    juce::ParameterAttachment attachment;
};

} // namespace livecut::ui
