// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/XYPad.h"
#include "ui/LivecutLookAndFeel.h"
#include <cmath>

namespace livecut::ui
{
XYPad::AxisSlider::AxisSlider (XYPad& pad, bool axis) : owner (pad), isX (axis)
{
    setSliderStyle (LinearHorizontal);
    setTextBoxStyle (TextBoxRight, false, 42, 18);
    setRange (0.0, 1.0, .001);
    textFromValueFunction = [] (double value) { return juce::String (value * 100.0, 1) + "%"; };
    valueFromTextFunction = [] (const juce::String& text) { return text.getDoubleValue() / 100.0; };
}

bool XYPad::AxisSlider::keyPressed (const juce::KeyPress& key) { return owner.axisKey (isX, key); }

XYPad::XYPad (juce::RangedAudioParameter& x, juce::RangedAudioParameter& y,
              const juce::String& title, const juce::String& xCaption, const juce::String& yCaption)
    : xParameter (x), yParameter (y),
      xAttachment (x, [this] (float value) { receiveAxis (true, value); }),
      yAttachment (y, [this] (float value) { receiveAxis (false, value); })
{
    setComponentID (x.getParameterID() + "," + y.getParameterID());
    setTitle (title + " XY pad");
    setDescription ("Left/right changes " + xCaption + "; up/down changes " + yCaption);
    setWantsKeyboardFocus (true);
    setTooltip ("Drag to play both axes. Shift-drag makes fine relative changes. Arrow keys: 1%; Shift: 0.1%; Ctrl: 10%. Home resets both axes.");
    heading.setText (title, juce::dontSendNotification);
    heading.setFont (uiFont (17.0f, true));
    xLabel.setText (xCaption, juce::dontSendNotification);
    yLabel.setText (yCaption, juce::dontSendNotification);
    for (auto* label : { &heading, &xLabel, &yLabel })
    {
        label->setBorderSize ({});
        addAndMakeVisible (*label);
    }
    for (const auto axis : { true, false })
    {
        auto& slider = axis ? xSlider : ySlider;
        auto& parameter = axis ? xParameter : yParameter;
        slider.setComponentID (parameter.getParameterID());
        slider.setTitle (parameter.getName (80));
        slider.setTooltip (getTooltip());
        slider.setDoubleClickReturnValue (true, parameter.getDefaultValue());
        slider.onDragStart = [this, axis] { beginAxis (axis); };
        slider.onDragEnd = [this, axis] { endAxis (axis); };
        slider.onValueChange = [this, axis] { setAxis (axis, static_cast<float> ((axis ? xSlider : ySlider).getValue())); };
        addAndMakeVisible (slider);
    }
    xAttachment.sendInitialUpdate();
    yAttachment.sendInitialUpdate();
}

XYPad::~XYPad() { endInteraction(); }

juce::Rectangle<float> XYPad::padBounds() const
{
    return getLocalBounds().toFloat().reduced (12.0f).withTrimmedTop (24.0f).withTrimmedBottom (52.0f);
}

void XYPad::resized()
{
    heading.setBounds (12, 6, getWidth() - 24, 24);
    auto axes = getLocalBounds().reduced (12).removeFromBottom (44);
    auto xArea = axes.removeFromLeft ((axes.getWidth() - 12) / 2);
    axes.removeFromLeft (12);
    xLabel.setBounds (xArea.removeFromTop (18));
    xSlider.setBounds (xArea);
    yLabel.setBounds (axes.removeFromTop (18));
    ySlider.setBounds (axes);
    endInteraction();
}

void XYPad::paint (juce::Graphics& g)
{
    g.setColour (palette::well);
    g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
    const auto area = padBounds();
    g.setColour (palette::lcd);
    g.fillRoundedRectangle (area, 4.0f);
    g.setColour (palette::lcdDim.withAlpha (.55f));
    for (int division = 1; division < 4; ++division)
    {
        const auto fraction = static_cast<float> (division) / 4.0f;
        g.drawVerticalLine (juce::roundToInt (area.getX() + area.getWidth() * fraction), area.getY(), area.getBottom());
        g.drawHorizontalLine (juce::roundToInt (area.getY() + area.getHeight() * fraction), area.getX(), area.getRight());
    }
    const auto point = [&] (juce::Point<float> value)
    {
        return juce::Point<float> { area.getX() + value.x * area.getWidth(), area.getBottom() - value.y * area.getHeight() };
    };
    const auto target = point (values);
    if (hasApplied)
    {
        const auto active = point (applied);
        g.setColour (palette::lcdOn.withAlpha (.4f));
        g.drawLine (juce::Line<float> (active, target), 1.0f);
        g.drawEllipse (active.x - 8.0f, active.y - 8.0f, 16.0f, 16.0f, 1.5f);
    }
    g.setColour (palette::lcdOn.withAlpha (.45f));
    g.drawHorizontalLine (juce::roundToInt (target.y), area.getX(), area.getRight());
    g.drawVerticalLine (juce::roundToInt (target.x), area.getY(), area.getBottom());
    g.setColour (palette::lcdOn);
    g.fillEllipse (target.x - 4.5f, target.y - 4.5f, 9.0f, 9.0f);
    g.setFont (lcdFont (11.0f));
    g.drawText ("0", area.reduced (7.0f).removeFromBottom (16.0f), juce::Justification::bottomLeft);
    g.drawText ("100", area.reduced (7.0f).removeFromTop (16.0f), juce::Justification::topRight);
    if (hasKeyboardFocus (true))
    {
        g.setColour (palette::ink);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.0f), 5.0f, 2.0f);
    }
}

void XYPad::showApplied (float x, float y, bool available)
{
    const juce::Point<float> next { juce::jlimit (0.0f, 1.0f, x), juce::jlimit (0.0f, 1.0f, y) };
    if (applied == next && hasApplied == available)
        return;
    applied = next;
    hasApplied = available;
    repaint();
}

void XYPad::receiveAxis (bool isX, float value)
{
    auto& parameter = isX ? xParameter : yParameter;
    const auto normalized = parameter.convertTo0to1 (value);
    if (isX) values.x = normalized; else values.y = normalized;
    (isX ? xSlider : ySlider).setValue (normalized, juce::dontSendNotification);
    repaint();
}

void XYPad::beginAxis (bool isX)
{
    auto& active = isX ? xGesture : yGesture;
    if (active)
        return;
    (isX ? xAttachment : yAttachment).beginGesture();
    active = true;
}

void XYPad::endAxis (bool isX)
{
    auto& active = isX ? xGesture : yGesture;
    if (! active)
        return;
    (isX ? xAttachment : yAttachment).endGesture();
    active = false;
}

void XYPad::setAxis (bool isX, float normalized)
{
    auto& parameter = isX ? xParameter : yParameter;
    auto& attachment = isX ? xAttachment : yAttachment;
    const auto value = parameter.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, normalized));
    if (isX ? xGesture : yGesture)
        attachment.setValueAsPartOfGesture (value);
    else
        attachment.setValueAsCompleteGesture (value);
}

void XYPad::mouseDown (const juce::MouseEvent& event)
{
    if (! isEnabled() || ! event.mods.isLeftButtonDown() || ! padBounds().contains (event.position))
        return;
    endInteraction();
    grabKeyboardFocus();
    dragging = true;
    dragOrigin = event.position;
    dragValues = values;
    fineDragging = event.mods.isShiftDown();
    beginAxis (true);
    beginAxis (false);
    movePointer (event);
    startTimerHz (30);
}

void XYPad::movePointer (const juce::MouseEvent& event, bool updateFine)
{
    const auto area = padBounds();
    if (area.isEmpty())
        return;
    if (updateFine && event.mods.isShiftDown() && ! fineDragging)
    {
        dragOrigin = event.position;
        dragValues = values;
    }
    if (updateFine) fineDragging = event.mods.isShiftDown();
    auto next = juce::Point<float> { (event.position.x - area.getX()) / area.getWidth(), (area.getBottom() - event.position.y) / area.getHeight() };
    if (fineDragging)
        next = dragValues + juce::Point<float> { (event.position.x - dragOrigin.x) / area.getWidth(), (dragOrigin.y - event.position.y) / area.getHeight() } * .1f;
    setAxis (true, next.x);
    setAxis (false, next.y);
}

void XYPad::mouseDrag (const juce::MouseEvent& event) { if (dragging) movePointer (event); }
void XYPad::mouseUp (const juce::MouseEvent& event)
{
    if (! dragging)
        return;
    movePointer (event, false);
    endInteraction();
}

void XYPad::endInteraction()
{
    stopTimer();
    dragging = false;
    endAxis (true);
    endAxis (false);
}

void XYPad::timerCallback()
{
    if (! juce::ModifierKeys::getCurrentModifiersRealtime().isLeftButtonDown() || ! isShowing() || ! isEnabled())
        endInteraction();
}

bool XYPad::axisKey (bool isX, const juce::KeyPress& key)
{
    if (! isEnabled())
        return false;
    const auto code = key.getKeyCode();
    if (code == juce::KeyPress::homeKey)
    {
        endInteraction();
        setAxis (isX, (isX ? xParameter : yParameter).getDefaultValue());
        return true;
    }
    const auto direction = code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey ? 1.0f
                         : code == juce::KeyPress::leftKey || code == juce::KeyPress::downKey ? -1.0f : 0.0f;
    if (direction == 0.0f || ! isEnabled())
        return false;
    endInteraction();
    const auto step = key.getModifiers().isShiftDown() ? .001f : key.getModifiers().isCtrlDown() ? .1f : .01f;
    setAxis (isX, (isX ? values.x : values.y) + direction * step);
    return true;
}

bool XYPad::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::homeKey)
    {
        axisKey (true, key);
        axisKey (false, key);
        return true;
    }
    return axisKey (key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey, key);
}
void XYPad::focusLost (FocusChangeType) { endInteraction(); repaint(); }
void XYPad::visibilityChanged() { if (! isVisible()) endInteraction(); }
void XYPad::enablementChanged() { if (! isEnabled()) endInteraction(); }
} // namespace livecut::ui
