// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace livecut::ui
{

// A sampler's front panel: cool hardware grey with ink labels, and one dark LCD
// whose amber segments are the only lit colour. Amber always means "on".
namespace palette
{
inline const juce::Colour panel { 0xffc8cbc6 };
inline const juce::Colour well { 0xffb7bbb5 };
inline const juce::Colour track { 0xff9ca19a };
inline const juce::Colour ink { 0xff1e2124 };
inline const juce::Colour inkMuted { 0xff4e5459 };
inline const juce::Colour lcd { 0xff20261f };
inline const juce::Colour lcdOn { 0xfff0b43c };
inline const juce::Colour lcdDim { 0xff5a5636 };
} // namespace palette

juce::Font uiFont (float height, bool bold = false);
juce::Font lcdFont (float height);

class LivecutLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    LivecutLookAndFeel();

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    int getSliderThumbRadius (juce::Slider&) override;

    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool highlighted, bool down) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool highlighted, bool down) override;
    void drawButtonText (juce::Graphics&, juce::TextButton&, bool highlighted, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool down,
                       int buttonX, int buttonY, int buttonW, int buttonH, juce::ComboBox&) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    juce::Font getLabelFont (juce::Label&) override;
};

} // namespace livecut::ui
