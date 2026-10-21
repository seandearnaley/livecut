// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace livecut::ui
{
// A two-axis performance control. The independent native sliders also expose
// precise keyboard and screen-reader editing of each automation parameter.
class XYPad final : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    XYPad (juce::RangedAudioParameter& x, juce::RangedAudioParameter& y,
           const juce::String& title, const juce::String& xCaption, const juce::String& yCaption);
    ~XYPad() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;
    void focusLost (FocusChangeType) override;
    void visibilityChanged() override;
    void enablementChanged() override;

    void showApplied (float x, float y, bool available);
    juce::Rectangle<float> padBounds() const;
    juce::Slider& xControl() { return xSlider; }
    juce::Slider& yControl() { return ySlider; }
    void endInteraction();

private:
    class AxisSlider final : public juce::Slider
    {
    public:
        AxisSlider (XYPad& owner, bool isX);
        bool keyPressed (const juce::KeyPress&) override;
    private:
        XYPad& owner;
        bool isX;
    };
    void timerCallback() override;
    void movePointer (const juce::MouseEvent&, bool updateFine = true);
    bool axisKey (bool isX, const juce::KeyPress&);
    void beginAxis (bool isX);
    void endAxis (bool isX);
    void setAxis (bool isX, float normalized);
    void receiveAxis (bool isX, float value);

    juce::RangedAudioParameter& xParameter;
    juce::RangedAudioParameter& yParameter;
    juce::Label heading, xLabel, yLabel;
    AxisSlider xSlider { *this, true }, ySlider { *this, false };
    juce::ParameterAttachment xAttachment, yAttachment;
    juce::Point<float> values, applied, dragOrigin, dragValues;
    bool xGesture = false, yGesture = false, dragging = false, fineDragging = false, hasApplied = false;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (XYPad)
};
} // namespace livecut::ui
