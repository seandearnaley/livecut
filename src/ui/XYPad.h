// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace livecut::ui
{

// The pad: a dark LCD surface that plays two parameters at once, with the
// mouse button as the third. Pressing sets Pad Touch and letting go clears
// it, and the X and Y gestures open and close together, so a host records one
// drag as one automation pass on each. Two native sliders beneath the surface
// give each axis precise keyboard and screen-reader editing.
class XYPad final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    XYPad (juce::RangedAudioParameter& x, juce::RangedAudioParameter& y, juce::RangedAudioParameter& touch);
    ~XYPad() override;

    // What left/right and up/down do, shown at the corners and on the sliders.
    void setCaptions (juce::String x, juce::String y);

    // Lights the TOUCH mark while the pad is playing, pressed or held.
    void showEngaged (bool engaged);

    // The surface that responds to the pointer, in local coordinates.
    juce::Rectangle<float> padBounds() const;
    juce::Slider& xControl() { return x.slider; }
    juce::Slider& yControl() { return y.slider; }

    // Lets go: ends the drag, the touch and both axis gestures.
    void endInteraction();

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusLost (FocusChangeType) override;
    void visibilityChanged() override;
    void enablementChanged() override;

private:
    class AxisSlider final : public juce::Slider
    {
    public:
        AxisSlider (XYPad& owner, bool horizontal);
        bool keyPressed (const juce::KeyPress&) override;

    private:
        XYPad& owner;
        bool horizontal;
    };

    struct Axis
    {
        Axis (XYPad& owner, juce::RangedAudioParameter& parameter, bool horizontal);

        juce::RangedAudioParameter& parameter;
        juce::ParameterAttachment attachment;
        AxisSlider slider;
        juce::Label caption, readout;
        float value = 0.0f; // normalised
        bool inGesture = false;
    };

    Axis& axis (bool horizontal) { return horizontal ? x : y; }
    juce::Rectangle<float> plotArea() const;
    juce::Point<float> values() const { return { x.value, y.value }; }
    void layoutAxis (Axis&, juce::Rectangle<int> area);

    void timerCallback() override;
    void movePointer (const juce::MouseEvent&, bool updateFine = true);
    bool axisKey (bool horizontal, const juce::KeyPress&);
    void beginGesture (Axis&);
    void endGesture (Axis&);
    void setAxis (Axis&, float normalised);
    void receive (bool horizontal, float value);
    void pressTouch();
    void releaseTouch();

    Axis x, y;
    juce::ParameterAttachment touchAttachment;
    juce::Point<float> dragOrigin, dragValues;
    bool dragging = false, fineDragging = false, touching = false, engaged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XYPad)
};

} // namespace livecut::ui
