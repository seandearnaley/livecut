// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "PluginProcessor.h"
#include "ui/Controls.h"
#include "ui/XYPad.h"

namespace livecut::ui
{
class PerformancePage final : public juce::Component
{
public:
    explicit PerformancePage (LivecutAudioProcessor&);
    void resized() override;
    void refresh();
    void cancelInteractions();

private:
    class FillButton final : public juce::TextButton, private juce::Timer
    {
    public:
        explicit FillButton (juce::RangedAudioParameter&);
        ~FillButton() override;
        void release();
        void mouseDown (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void focusLost (FocusChangeType) override;
        void visibilityChanged() override;
        void enablementChanged() override;
    private:
        void timerCallback() override;
        juce::ParameterAttachment attachment;
        bool gesture = false, receiving = false;
    };
    LivecutAudioProcessor& owner;
    PowerSwitch enabled;
    XYPad rhythm, texture;
    SliderRow seed;
    FillButton fill;
    juce::TextButton prepare { "Prepare Perform" }, grab { "Grab bar" }, keep { "Keep" }, mutate { "Mutate" }, returnHome { "Return" };
    juce::Label status, timing;
};
} // namespace livecut::ui
