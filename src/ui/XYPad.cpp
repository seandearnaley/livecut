// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/XYPad.h"

#include "ui/LivecutLookAndFeel.h"

#include <cmath>

namespace livecut::ui
{

namespace
{
constexpr int axisRowHeight = 40;
constexpr int axisRowGap = 10;
constexpr float plotInset = 14.0f;
constexpr float glowRadius = 64.0f;

const juce::String upArrow = juce::CharPointer_UTF8 ("\xe2\x86\x91");    // ↑
const juce::String rightArrow = juce::CharPointer_UTF8 ("\xe2\x86\x92"); // →
const juce::String touchMark = juce::CharPointer_UTF8 ("\xe2\x97\x8f");  // ●
} // namespace

XYPad::AxisSlider::AxisSlider (XYPad& pad, bool horizontalAxis)
    : juce::Slider (LinearHorizontal, NoTextBox), owner (pad), horizontal (horizontalAxis)
{
    setRange (0.0, 1.0, 0.001);
}

bool XYPad::AxisSlider::keyPressed (const juce::KeyPress& key)
{
    return owner.axisKey (horizontal, key);
}

XYPad::Axis::Axis (XYPad& owner, juce::RangedAudioParameter& p, bool horizontal)
    : parameter (p),
      attachment (p, [&owner, horizontal] (float denormalised) { owner.receive (horizontal, denormalised); }),
      slider (owner, horizontal)
{
}

XYPad::XYPad (juce::RangedAudioParameter& xParameter, juce::RangedAudioParameter& yParameter,
              juce::RangedAudioParameter& touchParameter)
    : x (*this, xParameter, true),
      y (*this, yParameter, false),
      touchAttachment (touchParameter, [] (float) {})
{
    setComponentID (touchParameter.getParameterID());
    setTitle ("Pad");
    setWantsKeyboardFocus (true);
    setTooltip ("Press to cut; let go to stop on the grid. Drag to change the sound side to side and up and down, "
                "Shift-drag for fine changes. Arrow keys nudge 1%, Shift 0.1%, Ctrl 10%. Home resets.");

    for (auto* a : { &x, &y })
    {
        a->slider.setComponentID (a->parameter.getParameterID());
        a->slider.setTitle (a->parameter.getName (80));
        a->slider.setTooltip (getTooltip());
        a->slider.setDoubleClickReturnValue (true, a->parameter.getDefaultValue());
        a->slider.onDragStart = [this, a] { beginGesture (*a); };
        a->slider.onDragEnd = [this, a] { endGesture (*a); };
        a->slider.onValueChange = [this, a] { setAxis (*a, static_cast<float> (a->slider.getValue())); };
        a->caption.setBorderSize ({});
        a->readout.setBorderSize ({});
        a->readout.setJustificationType (juce::Justification::centredRight);
        a->readout.setColour (juce::Label::textColourId, palette::inkMuted);
        addAndMakeVisible (a->caption);
        addAndMakeVisible (a->readout);
        addAndMakeVisible (a->slider);
    }

    x.attachment.sendInitialUpdate();
    y.attachment.sendInitialUpdate();
}

XYPad::~XYPad()
{
    endInteraction();
}

void XYPad::setCaptions (juce::String xCaption, juce::String yCaption)
{
    if (x.caption.getText() == xCaption && y.caption.getText() == yCaption)
        return;

    x.caption.setText (xCaption, juce::dontSendNotification);
    y.caption.setText (yCaption, juce::dontSendNotification);
    setDescription ("Left/right changes " + xCaption + "; up/down changes " + yCaption);
    repaint();
}

void XYPad::showEngaged (bool state)
{
    if (engaged == state)
        return;

    engaged = state;
    repaint();
}

juce::Rectangle<float> XYPad::padBounds() const
{
    return getLocalBounds().withTrimmedBottom (axisRowHeight + axisRowGap).toFloat();
}

juce::Rectangle<float> XYPad::plotArea() const
{
    return padBounds().reduced (plotInset);
}

void XYPad::layoutAxis (Axis& a, juce::Rectangle<int> area)
{
    auto top = area.removeFromTop (18);
    a.caption.setBounds (top.removeFromLeft (top.getWidth() / 2));
    a.readout.setBounds (top);
    a.slider.setBounds (area);
}

void XYPad::resized()
{
    auto rows = getLocalBounds().removeFromBottom (axisRowHeight);
    const auto width = (rows.getWidth() - 2 * axisRowGap) / 2;
    layoutAxis (x, rows.removeFromLeft (width));
    rows.removeFromLeft (2 * axisRowGap);
    layoutAxis (y, rows);
    endInteraction();
}

void XYPad::paint (juce::Graphics& g)
{
    const auto surface = padBounds();
    g.setColour (palette::lcd);
    g.fillRoundedRectangle (surface, 6.0f);

    const auto area = plotArea();
    g.setColour (palette::lcdDim.withAlpha (0.5f));
    g.drawRect (area, 1.0f);

    for (int division = 1; division < 8; ++division)
    {
        const auto lineX = area.getX() + area.getWidth() * static_cast<float> (division) / 8.0f;
        g.fillRect (juce::Rectangle<float> (lineX - 0.5f, area.getY(), 1.0f, area.getHeight()));
    }

    for (int division = 1; division < 4; ++division)
    {
        const auto lineY = area.getY() + area.getHeight() * static_cast<float> (division) / 4.0f;
        g.fillRect (juce::Rectangle<float> (area.getX(), lineY - 0.5f, area.getWidth(), 1.0f));
    }

    const juce::Point<float> target { area.getX() + x.value * area.getWidth(), area.getBottom() - y.value * area.getHeight() };

    if (dragging)
    {
        // A soft glow under the finger while pressed.
        juce::ColourGradient glow (palette::lcdOn.withAlpha (0.55f), target,
                                   palette::lcdOn.withAlpha (0.0f), target.translated (glowRadius, 0.0f), true);
        glow.addColour (0.35, palette::lcdOn.withAlpha (0.16f));
        g.setGradientFill (glow);
        g.fillEllipse (juce::Rectangle<float> (glowRadius * 2.0f, glowRadius * 2.0f).withCentre (target));
    }

    g.setColour (palette::lcdOn.withAlpha (0.45f));
    g.fillRect (juce::Rectangle<float> (area.getX(), target.y - 0.5f, area.getWidth(), 1.0f));
    g.fillRect (juce::Rectangle<float> (target.x - 0.5f, area.getY(), 1.0f, area.getHeight()));
    g.setColour (palette::lcdOn);
    g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (target));

    if (dragging)
        g.drawEllipse (juce::Rectangle<float> (22.0f, 22.0f).withCentre (target), 1.5f);

    // The axes at the corners, and the touch mark lit while the pad plays.
    const auto corners = surface.reduced (12.0f, 9.0f);
    const auto dim = palette::lcdDim.brighter (0.6f);
    g.setFont (lcdFont (11.0f));
    g.setColour (dim);
    g.drawText (upArrow + " " + y.caption.getText().toUpperCase(), corners, juce::Justification::topLeft);
    g.drawText (x.caption.getText().toUpperCase() + " " + rightArrow, corners, juce::Justification::bottomRight);
    g.setColour (engaged ? palette::lcdOn : dim);
    g.drawText (touchMark + " TOUCH", corners, juce::Justification::topRight);

    if (hasKeyboardFocus (true))
    {
        g.setColour (dim);
        g.drawRoundedRectangle (surface.reduced (1.0f), 6.0f, 1.5f);
    }
}

void XYPad::receive (bool horizontal, float value)
{
    auto& a = axis (horizontal);
    a.value = a.parameter.convertTo0to1 (value);
    a.slider.setValue (a.value, juce::dontSendNotification);
    const auto unit = a.parameter.getLabel();
    a.readout.setText (a.parameter.getText (a.value, 0) + (unit.isEmpty() ? "" : " " + unit), juce::dontSendNotification);
    repaint();
}

void XYPad::beginGesture (Axis& a)
{
    if (a.inGesture)
        return;

    a.attachment.beginGesture();
    a.inGesture = true;
}

void XYPad::endGesture (Axis& a)
{
    if (! a.inGesture)
        return;

    a.attachment.endGesture();
    a.inGesture = false;
}

void XYPad::setAxis (Axis& a, float normalised)
{
    const auto value = a.parameter.convertFrom0to1 (juce::jlimit (0.0f, 1.0f, normalised));

    if (a.inGesture)
        a.attachment.setValueAsPartOfGesture (value);
    else
        a.attachment.setValueAsCompleteGesture (value);
}

void XYPad::pressTouch()
{
    if (touching)
        return;

    touching = true;
    touchAttachment.beginGesture();
    touchAttachment.setValueAsPartOfGesture (1.0f);
}

void XYPad::releaseTouch()
{
    if (! touching)
        return;

    touching = false;
    touchAttachment.setValueAsPartOfGesture (0.0f);
    touchAttachment.endGesture();
}

void XYPad::mouseDown (const juce::MouseEvent& event)
{
    if (! isEnabled() || ! event.mods.isLeftButtonDown() || ! padBounds().contains (event.position))
        return;

    endInteraction();
    grabKeyboardFocus();
    dragging = true;
    dragOrigin = event.position;
    dragValues = values();
    fineDragging = event.mods.isShiftDown();
    beginGesture (x);
    beginGesture (y);
    movePointer (event);
    pressTouch();
    startTimerHz (30);
    repaint();
}

void XYPad::movePointer (const juce::MouseEvent& event, bool updateFine)
{
    const auto area = plotArea();

    if (area.isEmpty())
        return;

    if (updateFine && event.mods.isShiftDown() && ! fineDragging)
    {
        dragOrigin = event.position;
        dragValues = values();
    }

    if (updateFine)
        fineDragging = event.mods.isShiftDown();

    auto next = juce::Point<float> { (event.position.x - area.getX()) / area.getWidth(),
                                     (area.getBottom() - event.position.y) / area.getHeight() };

    if (fineDragging)
        next = dragValues + juce::Point<float> { (event.position.x - dragOrigin.x) / area.getWidth(),
                                                 (dragOrigin.y - event.position.y) / area.getHeight() } * 0.1f;

    setAxis (x, next.x);
    setAxis (y, next.y);
}

void XYPad::mouseDrag (const juce::MouseEvent& event)
{
    if (dragging)
        movePointer (event);
}

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
    const auto wasDragging = dragging;
    dragging = false;
    releaseTouch();
    endGesture (x);
    endGesture (y);

    if (wasDragging)
        repaint();
}

void XYPad::timerCallback()
{
    if (! juce::ModifierKeys::getCurrentModifiersRealtime().isLeftButtonDown() || ! isShowing() || ! isEnabled())
        endInteraction();
}

bool XYPad::axisKey (bool horizontal, const juce::KeyPress& key)
{
    if (! isEnabled())
        return false;

    auto& a = axis (horizontal);
    const auto code = key.getKeyCode();

    if (code == juce::KeyPress::homeKey)
    {
        endInteraction();
        setAxis (a, a.parameter.getDefaultValue());
        return true;
    }

    const auto direction = code == juce::KeyPress::rightKey || code == juce::KeyPress::upKey ? 1.0f
                         : code == juce::KeyPress::leftKey || code == juce::KeyPress::downKey ? -1.0f : 0.0f;

    if (direction == 0.0f)
        return false;

    endInteraction();
    const auto step = key.getModifiers().isShiftDown() ? 0.001f : key.getModifiers().isCtrlDown() ? 0.1f : 0.01f;
    setAxis (a, a.value + direction * step);
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

    const auto horizontal = key.getKeyCode() == juce::KeyPress::leftKey || key.getKeyCode() == juce::KeyPress::rightKey;
    return axisKey (horizontal, key);
}

void XYPad::focusLost (FocusChangeType)
{
    endInteraction();
    repaint();
}

void XYPad::visibilityChanged()
{
    if (! isVisible())
        endInteraction();
}

void XYPad::enablementChanged()
{
    if (! isEnabled())
        endInteraction();
}

} // namespace livecut::ui
