// SPDX-License-Identifier: GPL-3.0-or-later

#include "ui/PadPage.h"

#include "Parameters.h"
#include "ui/LivecutLookAndFeel.h"

#include <array>
#include <cmath>

namespace livecut::ui
{

namespace
{
constexpr int padHeight = 376;
constexpr int keyHeight = 72;
constexpr int keyGap = 8;
constexpr int rowHeight = 40;
constexpr int gapAbovePrograms = 14;
constexpr int gapBelowPrograms = 16;

const juce::String rightArrow = juce::CharPointer_UTF8 ("\xe2\x86\x92"); // →
const juce::String middleDot = juce::CharPointer_UTF8 ("\xc2\xb7");      // ·

struct ProgramInfo
{
    const char* caption;
    const char* tooltip;
};

// One line under each key, and what the pad does for the programs that are here.
constexpr std::array<ProgramInfo, engine::padProgramCount> programInfo {
    ProgramInfo { "re-edits", "Left to right: 8ths to 32nds with more repeats. Up: from the break as it is to a full re-edit." },
    ProgramInfo { "triplets", "Left to right: more triplet bounce, with halftime bars at the far right. Up: ratchets climb faster and higher." },
    ProgramInfo { "repeats", "Left to right: faster repeats that get crunchier as they speed up. Up: each repeat steps from falling to rising pitch." },
    ProgramInfo { "squeeze", "" },
    ProgramInfo { "resonance", "" },
    ProgramInfo { "stops", "" },
    ProgramInfo { "nested loops", "" },
    ProgramInfo { "rolls", "" },
};

juce::RangedAudioParameter& parameter (juce::AudioProcessorValueTreeState& state, const char* id)
{
    auto* found = state.getParameter (id);
    jassert (found != nullptr);
    return *found;
}

juce::AudioParameterChoice& choice (juce::AudioProcessorValueTreeState& state, const char* id)
{
    auto* found = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (id));
    jassert (found != nullptr);
    return *found;
}
} // namespace

PadPage::ProgramButton::ProgramButton (const juce::String& name, const juce::String& captionText)
    : juce::Button (name), caption (captionText)
{
}

void PadPage::ProgramButton::setPending (bool next, const juce::String& when)
{
    if (pending == next && pendingText == when)
    {
        if (pending)
            repaint(); // keeps the pulse moving

        return;
    }

    pending = next;
    pendingText = when;
    repaint();
}

void PadPage::ProgramButton::paintButton (juce::Graphics& g, bool highlighted, bool down)
{
    const auto lit = getToggleState();
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const auto face = bounds.withTrimmedBottom (2.0f);

    // A key with a visible edge, lit like a small LCD while its program plays.
    g.setColour (palette::track);
    g.fillRoundedRectangle (face.translated (0.0f, 2.0f), 5.0f);
    g.setColour (lit ? palette::lcd : down ? palette::track : highlighted ? palette::well : palette::panel.brighter (0.3f));
    g.fillRoundedRectangle (face, 5.0f);
    g.setColour (palette::ink);
    g.drawRoundedRectangle (face, 5.0f, 1.0f);

    if (lit || pending)
    {
        const auto pulse = pending ? 0.45f + 0.55f * static_cast<float> (0.5 + 0.5 * std::sin (juce::Time::getMillisecondCounterHiRes() * 0.006))
                                   : 1.0f;
        g.setColour (palette::lcdOn.withAlpha (pulse));
        g.drawRoundedRectangle (face.reduced (2.5f), 3.5f, 2.0f);
    }

    auto text = face.toNearestInt().reduced (4, 7);
    g.setColour (lit ? palette::lcdOn : palette::ink);
    g.setFont (uiFont (14.0f, true));
    g.drawText (getButtonText(), text.removeFromTop (text.getHeight() / 2 + 3), juce::Justification::centredBottom);
    g.setColour (lit ? palette::lcdOn.withAlpha (0.7f) : palette::inkMuted);
    g.setFont (uiFont (11.0f));
    g.drawText (pending ? pendingText : caption, text, juce::Justification::centredTop);
}

PadPage::PadPage (juce::AudioProcessorValueTreeState& s)
    : state (s),
      programParameter (choice (s, ids::padProgram)),
      pad (parameter (s, ids::padX), parameter (s, ids::padY), parameter (s, ids::padTouch)),
      programAttachment (programParameter, [this] (float value)
      {
          chosen = juce::jlimit (0, static_cast<int> (engine::padProgramCount) - 1, juce::roundToInt (value));
      }),
      hold (parameter (s, ids::padHold), "Hold"),
      straight (parameter (s, ids::padStraight), "Straight"),
      quantize (choice (s, ids::padQuantize)),
      mix (parameter (s, ids::wet), "Mix")
{
    addAndMakeVisible (pad);

    programRow.setComponentID (ids::padProgram);
    programRow.setTitle (programParameter.getName (64));
    addAndMakeVisible (programRow);

    for (int i = 0; i < programParameter.choices.size(); ++i)
    {
        const auto available = engine::isAvailable (static_cast<engine::PadProgram> (i));
        const auto& info = programInfo[static_cast<std::size_t> (i)];
        auto* button = programs.add (std::make_unique<ProgramButton> (programParameter.choices[i], info.caption));
        button->setComponentID ("program" + juce::String (i));
        button->setTitle (programParameter.choices[i]);
        button->setTooltip (available ? info.tooltip : "Coming in a later update");
        button->setEnabled (available);
        button->setAlpha (available ? 1.0f : 0.45f);
        button->onClick = [this, i] { programAttachment.setValueAsCompleteGesture (static_cast<float> (i)); };
        programRow.addAndMakeVisible (button);
    }

    const auto describeSwitch = [] (PowerSwitch& control, const juce::String& title, const juce::String& tooltip)
    {
        control.setTitle (title);
        for (auto* child : control.getChildren())
            if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child))
                toggle->setTooltip (tooltip);
    };
    describeSwitch (hold, "Pad Hold", "Keep cutting after you let go. Switch Hold off with nothing pressed to stop on the grid.");
    describeSwitch (straight, "Pad Straight", "Keeps every cut on even divisions of the beat: no triplets");

    for (auto* child : quantize.getChildren())
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
            button->setTooltip (button->getButtonText() == "Bar" ? "Releases and program changes land on the next bar"
                                                                  : "Releases and program changes land on the next beat");

    mix.setComponentID ("padMix");
    mix.setTitle ("Mix");
    mix.setTooltip ("How much of the program you hear while the pad plays. The same control as Wet on the Classic pages.");

    for (auto* component : std::initializer_list<juce::Component*> { &hold, &straight, &quantize, &mix })
        addAndMakeVisible (component);

    programAttachment.sendInitialUpdate();
}

engine::PadProgram PadPage::playingProgram (const DisplayState& display) const
{
    const auto active = static_cast<engine::PadProgram> (display.padActiveProgram.load());

    // While the pad plays, a switch waits for the grid. Idle, the chosen program plays next.
    if (display.padEngaged.load() || display.padPendingProgram.load() >= 0)
        return active;

    return engine::isAvailable (chosenProgram()) ? chosenProgram() : active;
}

bool PadPage::landsOnBar() const
{
    return state.getRawParameterValue (ids::padQuantize)->load() > 0.5f;
}

juce::String PadPage::programName (int index) const
{
    return programParameter.choices[index].toUpperCase();
}

void PadPage::refresh (const DisplayState& display)
{
    const auto playing = playingProgram (display);
    const auto pending = display.padPendingProgram.load();
    const juce::String when = landsOnBar() ? "next bar" : "next beat";

    for (int i = 0; i < programs.size(); ++i)
    {
        programs[i]->setToggleState (i == static_cast<int> (playing), juce::dontSendNotification);
        programs[i]->setPending (i == pending, when);
    }

    const auto captions = engine::padCaptions (playing);
    pad.setCaptions (captions.x, captions.y);
    pad.showEngaged (display.padEngaged.load());
}

juce::String PadPage::headline (const DisplayState& display, const juce::String& status) const
{
    if (! engine::isAvailable (chosenProgram()))
        return programName (chosen) + " ISN'T AVAILABLE YET";

    const auto pending = display.padPendingProgram.load();

    if (pending >= 0)
        return programName (display.padActiveProgram.load()) + " " + rightArrow + " " + programName (pending)
             + (landsOnBar() ? " ON THE BAR" : " ON THE BEAT");

    const auto line = programName (static_cast<int> (playingProgram (display))) + " " + middleDot + " ";

    // Playing but not cutting: nothing starts until a touch, and a touch waits for the next unit.
    if (display.status.load() == DisplayState::Status::startingSoon)
        return line + (display.padEngaged.load() ? "STARTING" : "PRESS THE PAD TO PLAY");

    return line + status.toUpperCase();
}

void PadPage::resized()
{
    auto area = getLocalBounds();
    pad.setBounds (area.removeFromTop (padHeight));
    area.removeFromTop (gapAbovePrograms);
    programRow.setBounds (area.removeFromTop (keyHeight));

    const auto count = programs.size();
    const auto keyWidth = (programRow.getWidth() - (count - 1) * keyGap) / count;

    for (int i = 0; i < count; ++i)
        programs[i]->setBounds (i * (keyWidth + keyGap), 0, keyWidth, programRow.getHeight());

    area.removeFromTop (gapBelowPrograms);
    auto row = area.removeFromTop (rowHeight);
    mix.setBounds (row.removeFromRight (240));
    hold.setBounds (row.getX(), row.getY() + 6, 90, 28);
    straight.setBounds (row.getX() + 104, row.getY() + 6, 90, 28);
    quantize.setBounds (row.getX() + 208, row.getY() + 5, 150, 30);
}

void PadPage::visibilityChanged()
{
    if (! isVisible())
        pad.endInteraction();
}

} // namespace livecut::ui
