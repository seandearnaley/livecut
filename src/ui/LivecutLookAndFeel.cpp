// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/LivecutLookAndFeel.h"

namespace livecut::ui
{

juce::Font uiFont (float height, bool bold)
{
    return juce::Font (juce::FontOptions (height, bold ? juce::Font::bold : juce::Font::plain));
}

juce::Font lcdFont (float height)
{
    return juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), height, juce::Font::plain));
}

LivecutLookAndFeel::LivecutLookAndFeel()
{
    using namespace palette;

    setColour (juce::ResizableWindow::backgroundColourId, panel);
    setColour (juce::Label::textColourId, ink);
    setColour (juce::Slider::textBoxTextColourId, ink);
    setColour (juce::ComboBox::backgroundColourId, well);
    setColour (juce::ComboBox::textColourId, ink);
    setColour (juce::ComboBox::outlineColourId, track);
    setColour (juce::ComboBox::arrowColourId, ink);
    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, ink);
    setColour (juce::PopupMenu::highlightedTextColourId, panel);
    setColour (juce::ToggleButton::textColourId, ink);
    setColour (juce::TextButton::textColourOffId, ink);
    setColour (juce::TextButton::textColourOnId, lcdOn);
}

void LivecutLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    if (style != juce::Slider::LinearHorizontal && style != juce::Slider::TwoValueHorizontal)
    {
        LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minSliderPos, maxSliderPos, style, slider);
        return;
    }

    const auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat();
    const auto centreY = bounds.getCentreY();
    const auto track = juce::Rectangle<float> (bounds.getX(), centreY - 2.0f, bounds.getWidth(), 4.0f);

    g.setColour (palette::track);
    g.fillRoundedRectangle (track, 2.0f);

    const auto isRange = style == juce::Slider::TwoValueHorizontal;
    const auto fillStart = isRange ? minSliderPos : bounds.getX();
    const auto fillEnd = isRange ? maxSliderPos : sliderPos;
    g.setColour (palette::ink);
    g.fillRoundedRectangle (juce::Rectangle<float> (fillStart, centreY - 2.0f, fillEnd - fillStart, 4.0f), 2.0f);

    auto drawThumb = [&] (float position)
    {
        const auto thumb = juce::Rectangle<float> (8.0f, 18.0f).withCentre ({ position, centreY });
        g.setColour (palette::ink);
        g.fillRoundedRectangle (thumb, 2.0f);
        g.setColour (palette::panel);
        g.drawRoundedRectangle (thumb, 2.0f, 1.0f);
    };

    if (isRange)
    {
        drawThumb (minSliderPos);
        drawThumb (maxSliderPos);
    }
    else
    {
        drawThumb (sliderPos);
    }

    if (slider.hasKeyboardFocus (true))
    {
        g.setColour (palette::ink.withAlpha (0.5f));
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);
    }
}

int LivecutLookAndFeel::getSliderThumbRadius (juce::Slider&)
{
    return 5;
}

void LivecutLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& button, bool highlighted, bool)
{
    // An LED: lit amber when on, dark when off.
    const auto bounds = button.getLocalBounds().toFloat();
    const auto led = juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ bounds.getX() + 7.0f, bounds.getCentreY() });
    const auto on = button.getToggleState();

    g.setColour (on ? palette::lcdOn : palette::lcd.withAlpha (highlighted ? 0.7f : 0.55f));
    g.fillEllipse (led);
    g.setColour (palette::ink);
    g.drawEllipse (led, 1.0f);

    g.setFont (uiFont (14.0f, true));
    g.drawText (button.getButtonText(), bounds.withTrimmedLeft (20.0f), juce::Justification::centredLeft);

    if (button.hasKeyboardFocus (true))
        g.drawRoundedRectangle (bounds.reduced (0.5f), 3.0f, 1.0f);
}

void LivecutLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&, bool highlighted, bool down)
{
    // The procedure switch: the selected segment looks like a small lit LCD.
    const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
    const auto selected = button.getToggleState();

    g.setColour (selected ? palette::lcd : (highlighted || down ? palette::track : palette::well));
    g.fillRoundedRectangle (bounds, 3.0f);
    g.setColour (palette::ink);
    g.drawRoundedRectangle (bounds, 3.0f, 1.0f);
}

void LivecutLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool)
{
    g.setFont (getTextButtonFont (button, button.getHeight()));
    g.setColour (button.getToggleState() ? palette::lcdOn : palette::ink);
    g.drawText (button.getButtonText(), button.getLocalBounds(), juce::Justification::centred);
}

juce::Font LivecutLookAndFeel::getTextButtonFont (juce::TextButton& button, int)
{
    return button.getToggleState() ? lcdFont (14.0f) : uiFont (14.0f);
}

void LivecutLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<int> (width, height).toFloat().reduced (0.5f);
    g.setColour (palette::well);
    g.fillRoundedRectangle (bounds, 3.0f);
    g.setColour (box.hasKeyboardFocus (true) ? palette::ink : palette::track);
    g.drawRoundedRectangle (bounds, 3.0f, 1.0f);

    juce::Path chevron;
    const auto centre = juce::Point<float> (bounds.getRight() - 12.0f, bounds.getCentreY());
    chevron.startNewSubPath (centre.translated (-4.0f, -2.0f));
    chevron.lineTo (centre.translated (0.0f, 2.0f));
    chevron.lineTo (centre.translated (4.0f, -2.0f));
    g.setColour (palette::ink);
    g.strokePath (chevron, juce::PathStrokeType (1.5f));
}

juce::Font LivecutLookAndFeel::getComboBoxFont (juce::ComboBox&)
{
    return uiFont (14.0f);
}

void LivecutLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    label.setBounds (8, 0, box.getWidth() - 28, box.getHeight());
    label.setFont (getComboBoxFont (box));
}

juce::Font LivecutLookAndFeel::getLabelFont (juce::Label&)
{
    return uiFont (14.0f);
}

} // namespace livecut::ui
