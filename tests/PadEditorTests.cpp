// SPDX-License-Identifier: GPL-3.0-or-later

#include "Parameters.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ProcessorTestSupport.h"
#include "ui/LivecutLookAndFeel.h"
#include "ui/PadPage.h"
#include "ui/PhraseRuler.h"
#include "ui/XYPad.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <map>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
juce::Component* findById (juce::Component& component, const juce::String& id)
{
    if (component.getComponentID() == id)
        return &component;

    for (auto* child : component.getChildren())
        if (auto* found = findById (*child, id))
            return found;

    return nullptr;
}

template <typename Control>
Control* findControl (juce::Component& component)
{
    if (auto* found = dynamic_cast<Control*> (&component))
        return found;
    for (auto* child : component.getChildren())
        if (auto* found = findControl<Control> (*child))
            return found;
    return nullptr;
}

bool isShowing (juce::Component& component, juce::Component& root)
{
    for (auto* c = &component; c != nullptr && c != &root; c = c->getParentComponent())
        if (! c->isVisible())
            return false;

    return true;
}

juce::Component& required (juce::Component& root, const char* id)
{
    auto* found = findById (root, id);
    REQUIRE (found != nullptr);
    return *found;
}

juce::StringArray labelTexts (juce::Component& component)
{
    juce::StringArray texts;
    if (auto* label = dynamic_cast<juce::Label*> (&component))
        texts.add (label->getText());
    for (auto* child : component.getChildren())
        texts.addArray (labelTexts (*child));
    return texts;
}

// A choice or switch parameter's index.
int index (LivecutAudioProcessor& processor, const char* id)
{
    return juce::roundToInt (processor.getParameterState().getRawParameterValue (id)->load());
}

void set (LivecutAudioProcessor& processor, const char* id, float value)
{
    auto& parameter = *processor.getParameterState().getParameter (id);
    parameter.setValueNotifyingHost (parameter.convertTo0to1 (value));
}

// Source literals are UTF-8; juce::String's char* constructor only reads ASCII.
juce::String utf8 (const char* text)
{
    return juce::String::fromUTF8 (text);
}

struct GestureCounter final : juce::AudioProcessorListener
{
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override { ++begun[index]; }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override { ++ended[index]; }

    std::map<int, int> begun, ended;
};

juce::MouseEvent pointerAt (juce::Component& component, juce::Point<float> position, bool down)
{
    const auto now = juce::Time::getCurrentTime();
    const juce::ModifierKeys modifiers (down ? juce::ModifierKeys::leftButtonModifier : 0);
    return { juce::Desktop::getInstance().getMainMouseSource(), position, modifiers,
             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
             juce::MouseInputSource::defaultTiltY, &component, &component, now, position, now, 1, false };
}

// The pad on its own, on the desktop so focus and visibility behave as in a host.
struct PadFixture
{
    PadFixture()
        : x (*processor.getParameterState().getParameter (livecut::ids::padX)),
          y (*processor.getParameterState().getParameter (livecut::ids::padY)),
          touch (*processor.getParameterState().getParameter (livecut::ids::padTouch)),
          pad (x, y, touch)
    {
        processor.addListener (&gestures);
        pad.setSize (840, 368);
        pad.setVisible (true);
        pad.addToDesktop (0);
    }

    ~PadFixture()
    {
        pad.endInteraction();
        processor.removeListener (&gestures);
    }

    void press (juce::Point<float> at) { pad.mouseDown (pointerAt (pad, at, true)); }

    void checkOneGestureEach() const
    {
        for (auto* parameter : { &x, &y, &touch })
        {
            INFO (parameter->getParameterID());
            const auto index = parameter->getParameterIndex();
            CHECK (gestures.begun.count (index) == 1);
            CHECK (gestures.ended.count (index) == 1);
            if (gestures.begun.count (index) == 1)
                CHECK (gestures.begun.at (index) == 1);
            if (gestures.ended.count (index) == 1)
                CHECK (gestures.ended.at (index) == 1);
        }
    }

    LivecutAudioProcessor processor;
    juce::RangedAudioParameter& x;
    juce::RangedAudioParameter& y;
    juce::RangedAudioParameter& touch;
    GestureCounter gestures;
    livecut::ui::XYPad pad;
};

void checkNoOverlaps (juce::Component& editor, std::initializer_list<const char*> ids)
{
    std::vector<std::pair<juce::String, juce::Rectangle<int>>> placed;
    for (const auto* id : ids)
    {
        INFO (id);
        auto& control = required (editor, id);
        const auto area = editor.getLocalArea (&control, control.getLocalBounds());
        CHECK_FALSE (area.isEmpty());
        CHECK (editor.getLocalBounds().contains (area));
        for (const auto& [otherId, other] : placed)
        {
            INFO ("against " << otherId);
            CHECK_FALSE (area.intersects (other));
        }
        placed.emplace_back (id, area);
    }
}
} // namespace

TEST_CASE ("New instances open on the pad page at 880 by 724", "[pad][editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);

    CHECK (editor.getWidth() == 880);
    CHECK (editor.getHeight() == 724);

    for (const auto* id : { "padTouch", "padX", "padY", "padProgram", "padHold", "padQuantize", "padMix", "surface" })
    {
        INFO (id);
        auto& control = required (editor, id);
        CHECK (isShowing (control, editor));
        CHECK (control.getTitle().isNotEmpty());
    }
    checkNoOverlaps (editor, { "surface", "padTouch", "program0", "program1", "program2", "program3", "program4",
                               "program5", "program6", "program7", "padHold", "padQuantize", "padMix",
                               "padStraight" });

    for (const auto* id : { "patternTab", "recutTab", "impactTab", "fractureTab", "resonanceTab", "procedure",
                            "subdivision", "crusher", "wet", "outputTrim", "capture", "saveCapture", "swing", "accent", "hold" })
    {
        INFO (id);
        CHECK_FALSE (isShowing (required (editor, id), editor));
    }

    auto* surface = dynamic_cast<juce::TextButton*> (findById (editor, "surface"));
    REQUIRE (surface != nullptr);
    CHECK (surface->getButtonText() == "Classic");
}

TEST_CASE ("Program buttons select programs, and later programs are disabled", "[pad][editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    const auto& names = dynamic_cast<juce::AudioParameterChoice*> (processor.getParameterState().getParameter (livecut::ids::padProgram))->choices;
    REQUIRE (names.size() == 8);

    std::vector<juce::Button*> buttons;
    for (int i = 0; i < 8; ++i)
    {
        INFO (i);
        auto* button = dynamic_cast<juce::Button*> (findById (editor, "program" + juce::String (i)));
        REQUIRE (button != nullptr);
        buttons.push_back (button);
        CHECK (button->getButtonText() == names[i]);
        CHECK (button->getTitle().isNotEmpty());
        if (i < 4)
        {
            CHECK (button->isEnabled());
            CHECK (button->getTooltip().isNotEmpty());
            CHECK (button->getTooltip() != "Coming in a later update");
        }
        else
        {
            CHECK_FALSE (button->isEnabled());
            CHECK (button->getTooltip() == "Coming in a later update");
        }
    }

    CHECK (buttons[0]->getToggleState());
    buttons[1]->onClick();
    CHECK (index (processor, livecut::ids::padProgram) == 1);
    editor.refreshDisplay();
    CHECK (buttons[1]->getToggleState());
    CHECK_FALSE (buttons[0]->getToggleState());

    buttons[2]->onClick();
    CHECK (index (processor, livecut::ids::padProgram) == 2);

    buttons[3]->onClick(); // Suction
    CHECK (index (processor, livecut::ids::padProgram) == 3);
    editor.refreshDisplay();
    CHECK (buttons[3]->getToggleState());
    CHECK_FALSE (buttons[2]->getToggleState());

    set (processor, livecut::ids::padProgram, 0); // the host moving the parameter lights the matching button
    editor.refreshDisplay();
    CHECK (buttons[0]->getToggleState());
    CHECK_FALSE (buttons[2]->getToggleState());

    // While the pad plays, the switch waits for the grid, so the playing program stays lit.
    auto& display = const_cast<livecut::DisplayState&> (processor.getDisplayState());
    display.padEngaged.store (true);
    display.padActiveProgram.store (0);
    display.padPendingProgram.store (2);
    editor.refreshDisplay();
    CHECK (buttons[0]->getToggleState());
    CHECK_FALSE (buttons[2]->getToggleState());
}

TEST_CASE ("The pad's words describe the sound", "[pad][editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);

    // One short noun under each key, fitting its key with room to spare.
    const juce::StringArray captions { "re-edits", "triplets", "repeats", "squeeze", "resonance", "stops", "nested loops", "rolls" };
    for (int i = 0; i < captions.size(); ++i)
    {
        INFO (captions[i]);
        auto* key = dynamic_cast<livecut::ui::PadPage::ProgramButton*> (findById (editor, "program" + juce::String (i)));
        REQUIRE (key != nullptr);
        CHECK (key->captionText() == captions[i]);
        CHECK (juce::GlyphArrangement::getStringWidth (livecut::ui::uiFont (11.0f), key->captionText()) <= static_cast<float> (key->getWidth() - 12));
    }

    int quantizeButtons = 0;
    for (auto* child : required (editor, "padQuantize").getChildren())
    {
        if (auto* button = dynamic_cast<juce::TextButton*> (child))
        {
            ++quantizeButtons;
            INFO (button->getButtonText());
            CHECK (button->getTooltip() == (button->getButtonText() == "Bar" ? "Releases and program changes land on the next bar"
                                                                            : "Releases and program changes land on the next beat"));
        }
    }
    CHECK (quantizeButtons == 2);

    auto* pad = dynamic_cast<juce::TooltipClient*> (findById (editor, "padTouch"));
    REQUIRE (pad != nullptr);
    CHECK (pad->getTooltip().startsWith ("Press to cut; let go to stop on the grid."));
    CHECK_FALSE (pad->getTooltip().containsIgnoreCase ("axes"));
}

TEST_CASE ("Pressing the pad sets Pad Touch and releasing clears it, with paired gestures", "[pad][editor][xy]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    PadFixture fixture;
    const auto area = fixture.pad.padBounds();

    fixture.press (area.getCentre());
    CHECK_THAT (fixture.touch.getValue(), WithinAbs (1.0, .0001));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (.5, .01));
    CHECK_THAT (fixture.y.getValue(), WithinAbs (.5, .01));

    fixture.pad.mouseDrag (pointerAt (fixture.pad, { area.getRight() + 100.0f, area.getY() - 100.0f }, true));
    CHECK_THAT (fixture.touch.getValue(), WithinAbs (1.0, .0001));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (1.0, .001));
    CHECK_THAT (fixture.y.getValue(), WithinAbs (1.0, .001));

    fixture.pad.mouseUp (pointerAt (fixture.pad, { area.getX() - 100.0f, area.getBottom() + 100.0f }, false));
    CHECK_THAT (fixture.touch.getValue(), WithinAbs (0.0, .0001));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (0.0, .001));
    CHECK_THAT (fixture.y.getValue(), WithinAbs (0.0, .001));

    fixture.checkOneGestureEach();
}

TEST_CASE ("Hiding the pad or losing focus releases a held touch", "[pad][editor][xy]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    for (const auto* cancellation : { "focus lost", "hidden", "disabled" })
    {
        INFO (cancellation);
        PadFixture fixture;
        fixture.press (fixture.pad.padBounds().getCentre());
        REQUIRE_THAT (fixture.touch.getValue(), WithinAbs (1.0, .0001));

        if (juce::String (cancellation) == "focus lost") fixture.pad.focusLost (juce::Component::focusChangedDirectly);
        if (juce::String (cancellation) == "hidden") fixture.pad.setVisible (false);
        if (juce::String (cancellation) == "disabled") fixture.pad.setEnabled (false);

        CHECK_THAT (fixture.touch.getValue(), WithinAbs (0.0, .0001));
        fixture.checkOneGestureEach();
    }
}

TEST_CASE ("Axis captions follow the program", "[pad][editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto& pad = required (editor, "padTouch");
    auto& display = const_cast<livecut::DisplayState&> (processor.getDisplayState());

    editor.refreshDisplay();
    CHECK (labelTexts (pad).contains ("Density"));
    CHECK (labelTexts (pad).contains ("Re-edit"));

    // Nothing is playing, so choosing a program recaptions the pad at once.
    set (processor, livecut::ids::padProgram, 1);
    editor.refreshDisplay();
    CHECK (labelTexts (pad).contains ("Bounce"));
    CHECK (labelTexts (pad).contains ("Ratchet"));
    CHECK_FALSE (labelTexts (pad).contains ("Density"));

    // While the pad plays, the captions belong to the program you hear.
    display.padEngaged.store (true);
    display.padActiveProgram.store (2);
    editor.refreshDisplay();
    CHECK (labelTexts (pad).contains ("Rate"));
    CHECK (labelTexts (pad).contains ("Pitch"));
    CHECK_FALSE (labelTexts (pad).contains ("Bounce"));

    display.padActiveProgram.store (3);
    editor.refreshDisplay();
    CHECK (labelTexts (pad).contains ("Band"));
    CHECK (labelTexts (pad).contains ("Squeeze"));

    display.padActiveProgram.store (0);
    editor.refreshDisplay();
    CHECK (labelTexts (pad).contains ("Density"));
    CHECK (labelTexts (pad).contains ("Re-edit"));
}

TEST_CASE ("The Classic switch shows the legacy pages and sets Surface", "[pad][editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* surface = dynamic_cast<juce::TextButton*> (findById (editor, "surface"));
    REQUIRE (surface != nullptr);
    auto* pad = dynamic_cast<livecut::ui::XYPad*> (findById (editor, "padTouch"));
    REQUIRE (pad != nullptr);
    auto& touch = *processor.getParameterState().getParameter (livecut::ids::padTouch);

    pad->mouseDown (pointerAt (*pad, pad->padBounds().getCentre(), true));
    REQUIRE_THAT (touch.getValue(), WithinAbs (1.0, .0001));

    surface->onClick();
    CHECK (index (processor, livecut::ids::surface) == 1);
    CHECK (surface->getButtonText() == "Pad");
    CHECK_THAT (touch.getValue(), WithinAbs (0.0, .0001)); // leaving the pad lets go
    for (const auto* id : { "patternTab", "recutTab", "impactTab", "fractureTab", "resonanceTab", "procedure",
                            "subdivision", "stutterChance", "crusher", "wet", "outputTrim", "capture", "saveCapture", "swing", "accent", "hold" })
    {
        INFO (id);
        CHECK (isShowing (required (editor, id), editor));
    }
    for (const auto* id : { "padTouch", "padProgram", "padHold", "padQuantize", "padMix", "padStraight" })
    {
        INFO (id);
        CHECK_FALSE (isShowing (required (editor, id), editor));
    }
    checkNoOverlaps (editor, { "patternTab", "recutTab", "impactTab", "fractureTab", "resonanceTab", "procedure", "surface" });

    surface->onClick();
    CHECK (index (processor, livecut::ids::surface) == 0);
    CHECK (surface->getButtonText() == "Classic");
    CHECK (isShowing (required (editor, "padTouch"), editor));
    CHECK_FALSE (isShowing (required (editor, "patternTab"), editor));

    set (processor, livecut::ids::surface, 1); // a host or saved state can switch too
    CHECK (surface->getButtonText() == "Pad");
    CHECK (isShowing (required (editor, "patternTab"), editor));
    CHECK_FALSE (isShowing (required (editor, "padTouch"), editor));
}

TEST_CASE ("A Classic session opens on the legacy pages", "[pad][editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    useClassicSurface (processor);
    LivecutAudioProcessorEditor editor (processor);

    CHECK (editor.getWidth() == 880);
    CHECK (editor.getHeight() == 724);
    for (const auto* id : { "patternTab", "procedure", "subdivision", "stutterChance", "wet", "hold" })
    {
        INFO (id);
        CHECK (isShowing (required (editor, id), editor));
    }
    CHECK_FALSE (isShowing (required (editor, "padTouch"), editor));
    auto* surface = dynamic_cast<juce::TextButton*> (findById (editor, "surface"));
    REQUIRE (surface != nullptr);
    CHECK (surface->getButtonText() == "Pad");
}

TEST_CASE ("The Straight toggle sets Pad Straight", "[pad][editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* straight = dynamic_cast<livecut::ui::PowerSwitch*> (findById (editor, "padStraight"));
    REQUIRE (straight != nullptr);
    CHECK (isShowing (*straight, editor));
    CHECK (straight->getTitle() == "Pad Straight");
    auto* toggle = findControl<juce::ToggleButton> (*straight);
    REQUIRE (toggle != nullptr);
    CHECK (toggle->getButtonText() == "Straight");
    CHECK (toggle->getTooltip() == "Keeps every cut on even divisions of the beat: no triplets");

    CHECK (index (processor, livecut::ids::padStraight) == 0);
    toggle->setToggleState (true, juce::sendNotificationSync);
    CHECK (index (processor, livecut::ids::padStraight) == 1);
    toggle->setToggleState (false, juce::sendNotificationSync);
    CHECK (index (processor, livecut::ids::padStraight) == 0);

    set (processor, livecut::ids::padStraight, 1.0f); // the host moving the parameter lights the switch
    CHECK (straight->isOn());
}

TEST_CASE ("The bottom row holds Hold, Straight, Beat/Bar and Mix without overlap", "[pad][editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    REQUIRE (editor.getWidth() == 880);
    REQUIRE (editor.getHeight() == 724);

    // Left to right, and the hold and straight switches share a line.
    checkNoOverlaps (editor, { "padHold", "padStraight", "padQuantize", "padMix" });
    const auto& hold = required (editor, "padHold");
    const auto& straight = required (editor, "padStraight");
    const auto& quantize = required (editor, "padQuantize");
    const auto& mix = required (editor, "padMix");
    CHECK (hold.getRight() < straight.getX());
    CHECK (straight.getRight() < quantize.getX());
    CHECK (quantize.getRight() < mix.getX());
    CHECK (hold.getY() == straight.getY());
    CHECK (hold.getHeight() == straight.getHeight());
    CHECK (straight.getWidth() >= hold.getWidth());
}

TEST_CASE ("The ruler headline names the program and any pending switch", "[pad][editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* ruler = findControl<livecut::ui::PhraseRuler> (editor);
    REQUIRE (ruler != nullptr);
    // This idle fixture owns the processor; publish display snapshots without an audio thread.
    auto& display = const_cast<livecut::DisplayState&> (processor.getDisplayState());

    editor.refreshDisplay();
    CHECK (ruler->headlineText() == utf8 ("CUT · PRESS PLAY IN YOUR HOST TO START CUTTING"));
    CHECK (editor.statusText() == "Press play in your host to start cutting");

    // Playing, nothing starts until a touch; touched, the cuts wait for the next unit.
    display.status.store (livecut::DisplayState::Status::startingSoon);
    editor.refreshDisplay();
    CHECK (ruler->headlineText() == utf8 ("CUT · PRESS THE PAD TO PLAY"));
    display.padEngaged.store (true);
    editor.refreshDisplay();
    CHECK (ruler->headlineText() == utf8 ("CUT · STARTING"));
    display.padEngaged.store (false);

    display.status.store (livecut::DisplayState::Status::cutting);
    display.phraseBars.store (4);
    display.unitsPerBar.store (8);
    display.position.store (9.0);
    display.padEngaged.store (true);
    display.padActiveProgram.store (0);
    editor.refreshDisplay();
    CHECK (ruler->headlineText() == utf8 ("CUT · BAR 2 OF 4"));
    CHECK (editor.statusText() == "Bar 2 of 4");

    display.padPendingProgram.store (2);
    editor.refreshDisplay();
    CHECK (ruler->headlineText() == utf8 ("CUT → GLITCH ON THE BEAT"));

    set (processor, livecut::ids::padQuantize, 1);
    editor.refreshDisplay();
    CHECK (ruler->headlineText() == utf8 ("CUT → GLITCH ON THE BAR"));

    display.padPendingProgram.store (-1);
    set (processor, livecut::ids::padProgram, 4);
    editor.refreshDisplay();
    CHECK (ruler->headlineText() == "FILTER ISN'T AVAILABLE YET");

    // Idle, the chosen program is the one that will play.
    display.padEngaged.store (false);
    set (processor, livecut::ids::padProgram, 1);
    editor.refreshDisplay();
    CHECK (ruler->headlineText() == utf8 ("BOUNCE · BAR 2 OF 4"));

    useClassicSurface (processor);
    editor.refreshDisplay();
    CHECK (ruler->headlineText().isEmpty());
    CHECK (editor.statusText() == "Bar 2 of 4");
}

// Hidden: run `LIVECUT_SCREENSHOT_DIR=<dir> LivecutTests "[.screenshot][pad]"` to render the
// pad page mid-performance to editor-pad.png in that directory, for reviewing the design.
TEST_CASE ("Render the pad editor for design review", "[.screenshot][pad]")
{
    const auto directory = screenshotDirectory();
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto& display = const_cast<livecut::DisplayState&> (processor.getDisplayState());
    display.status.store (livecut::DisplayState::Status::cutting);
    display.phraseBars.store (4);
    display.unitsPerBar.store (16);
    display.blockStart.store (40.0);
    display.blockEnd.store (44.0);
    display.position.store (41.5);
    display.padEngaged.store (true);
    display.padActiveProgram.store (2);
    display.padPendingProgram.store (1);
    set (processor, livecut::ids::padProgram, 1);
    set (processor, livecut::ids::padX, .68f);
    set (processor, livecut::ids::padY, .66f);
    set (processor, livecut::ids::padHold, 1.0f);
    const auto save = [&] (const char* name)
    {
        editor.refreshDisplay();
        saveScreenshot (editor, directory, name);
    };
    auto* pad = dynamic_cast<livecut::ui::XYPad*> (findById (editor, "padTouch"));
    REQUIRE (pad != nullptr);
    const auto area = pad->padBounds();
    pad->mouseDown (pointerAt (*pad, { area.getX() + area.getWidth() * .68f, area.getBottom() - area.getHeight() * .66f }, true));
    save ("editor-pad.png");
    pad->endInteraction();
    display.padEngaged.store (false);
    display.padPendingProgram.store (-1);
    save ("editor-pad-idle.png");
    useClassicSurface (processor);
    save ("editor-pad-classic.png");
}
