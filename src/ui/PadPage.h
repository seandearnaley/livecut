// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "DisplayState.h"
#include "engine/PadPrograms.h"
#include "ui/Controls.h"
#include "ui/XYPad.h"

#include <juce_audio_processors/juce_audio_processors.h>

namespace livecut::ui
{

// The pad surface beneath the phrase ruler: the pad, a row of eight program
// keys, then Hold, Beat / Bar and Mix. Where a control owns a parameter, its
// component ID is the parameter's ID. Mix is the Wet parameter under its own ID.
class PadPage final : public juce::Component
{
public:
    // A key like a drum machine's: lit while its program plays, outlined and
    // pulsing while it waits for the grid, dimmed when it isn't here yet.
    class ProgramButton final : public juce::Button
    {
    public:
        ProgramButton (const juce::String& name, const juce::String& caption);

        void setPending (bool pending, const juce::String& when);
        bool isPending() const { return pending; }
        const juce::String& captionText() const { return caption; }

        void paintButton (juce::Graphics&, bool highlighted, bool down) override;

    private:
        juce::String caption, pendingText;
        bool pending = false;
    };

    explicit PadPage (juce::AudioProcessorValueTreeState& state);

    // Follows the processor's display: which program plays or waits, the
    // pad's captions and its touch mark.
    void refresh (const DisplayState& display);

    // The ruler's line: the program and the bar, a pending switch, a chosen
    // program that isn't here yet, or, while the host plays but nothing is
    // cut yet, an invitation to press the pad or a note that it's starting.
    juce::String headline (const DisplayState& display, const juce::String& status) const;

    void resized() override;
    void visibilityChanged() override;

private:
    engine::PadProgram chosenProgram() const { return static_cast<engine::PadProgram> (chosen); }
    engine::PadProgram playingProgram (const DisplayState& display) const;
    bool landsOnBar() const;
    juce::String programName (int index) const;

    juce::AudioProcessorValueTreeState& state;
    juce::AudioParameterChoice& programParameter;
    XYPad pad;
    juce::Component programRow;
    juce::OwnedArray<ProgramButton> programs;
    juce::ParameterAttachment programAttachment;
    PowerSwitch hold;
    ProcedureSwitch quantize;
    SliderRow mix;
    int chosen = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PadPage)
};

} // namespace livecut::ui
