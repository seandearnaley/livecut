// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "PluginProcessor.h"
#include "ui/LivecutLookAndFeel.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

class LivecutAudioProcessorEditor final : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    // Everything is laid out once at this size and scaled as a whole when the window is resized.
    static constexpr int baseWidth = 880;
    static constexpr int baseHeight = 660;

    explicit LivecutAudioProcessorEditor (LivecutAudioProcessor& p);
    ~LivecutAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // Reads the processor's display state into the phrase ruler. The timer calls it 30 times a second.
    void refreshDisplay();
    juce::String statusText() const;

private:
    class Panel;

    void timerCallback() override;

    LivecutAudioProcessor& livecutProcessor;
    livecut::ui::LivecutLookAndFeel lookAndFeel; // outlives the panel, which is destroyed first
    std::unique_ptr<Panel> panel;
    juce::TooltipWindow tooltips { this, 600 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LivecutAudioProcessorEditor)
};
