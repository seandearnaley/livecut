// SPDX-License-Identifier: GPL-3.0-or-later

#include "Parameters.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/RangeSlider.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <set>
#include <chrono>
#include <thread>

using Catch::Matchers::WithinAbs;

namespace
{
void collectIds (juce::Component& component, std::set<juce::String>& ids)
{
    for (const auto& id : juce::StringArray::fromTokens (component.getComponentID(), ",", ""))
        if (id.isNotEmpty())
            ids.insert (id);

    for (auto* child : component.getChildren())
        collectIds (*child, ids);
}

juce::Component* findById (juce::Component& component, const juce::String& id)
{
    if (component.getComponentID() == id)
        return &component;

    for (auto* child : component.getChildren())
        if (auto* found = findById (*child, id))
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

struct GestureCounter final : juce::AudioProcessorListener
{
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override { begun.insert (index); }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override { ended.insert (index); }

    std::set<int> begun, ended;
};
} // namespace

TEST_CASE ("The editor has a control for every parameter", "[editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);

    std::set<juce::String> ids;
    collectIds (editor, ids);

    for (auto* parameter : processor.getParameters())
    {
        const auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
        REQUIRE (ranged != nullptr);
        INFO (ranged->getParameterID());
        CHECK (ids.count (ranged->getParameterID()) == 1);
    }
}

// Livecut's editor replaced the process-wide default look and feel with a static instance.
TEST_CASE ("The editor leaves the global look and feel alone", "[editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    auto& globalBefore = juce::LookAndFeel::getDefaultLookAndFeel();

    LivecutAudioProcessor processor;
    {
        LivecutAudioProcessorEditor editor (processor);
        CHECK (&juce::LookAndFeel::getDefaultLookAndFeel() == &globalBefore);
        CHECK (&editor.getLookAndFeel() != &globalBefore);
    }

    CHECK (&juce::LookAndFeel::getDefaultLookAndFeel() == &globalBefore);
}

// Livecut's controls set parameters without gestures, so hosts couldn't record automation from them reliably.
TEST_CASE ("A range control moves its parameters inside gestures", "[editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    GestureCounter gestures;
    processor.addListener (&gestures);
    LivecutAudioProcessorEditor editor (processor);

    auto* range = dynamic_cast<livecut::ui::RangeSlider*> (findById (editor, juce::String (livecut::ids::minAmp) + "," + livecut::ids::maxAmp));
    REQUIRE (range != nullptr);

    range->getSlider().setMinValue (0.25, juce::sendNotificationSync);

    auto& state = processor.getParameterState();
    CHECK_THAT (state.getRawParameterValue (livecut::ids::minAmp)->load(), WithinAbs (0.25, 1.0e-4));
    CHECK_THAT (state.getRawParameterValue (livecut::ids::maxAmp)->load(), WithinAbs (1.0, 1.0e-6));

    const auto index = state.getParameter (livecut::ids::minAmp)->getParameterIndex();
    CHECK (gestures.begun.count (index) == 1);
    CHECK (gestures.ended.count (index) == 1);

    processor.removeListener (&gestures);
}

namespace
{
juce::MouseEvent mouseAt (juce::Component& component, juce::Point<float> position, juce::Point<float> downAt, bool dragged)
{
    const auto now = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), position, {},
             juce::MouseInputSource::defaultPressure, juce::MouseInputSource::defaultOrientation,
             juce::MouseInputSource::defaultRotation, juce::MouseInputSource::defaultTiltX,
             juce::MouseInputSource::defaultTiltY, &component, &component, now, downAt, now, 1, dragged };
}
} // namespace

// Found in review: dragging one thumb past the other nudged the second thumb
// on screen without moving its parameter.
TEST_CASE ("Dragging one range thumb past the other moves both parameters", "[editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    GestureCounter gestures;
    processor.addListener (&gestures);
    auto& state = processor.getParameterState();
    auto& minimum = *state.getParameter (livecut::ids::minPan);
    auto& maximum = *state.getParameter (livecut::ids::maxPan);

    livecut::ui::RangeSlider range (minimum, maximum, "Pan");
    range.setBounds (0, 0, 300, 40);
    auto& slider = range.getSlider();
    const auto y = static_cast<float> (slider.getHeight()) / 2.0f;
    const juce::Point<float> start { slider.getPositionOfValue (slider.getMinValue()), y };
    const juce::Point<float> end { slider.getPositionOfValue (0.6), y };

    slider.mouseDown (mouseAt (slider, start, start, false));
    slider.mouseDrag (mouseAt (slider, end, start, true));
    slider.mouseUp (mouseAt (slider, end, start, true));

    CHECK_THAT (state.getRawParameterValue (livecut::ids::minPan)->load(), WithinAbs (0.6, 0.02));
    CHECK_THAT (state.getRawParameterValue (livecut::ids::maxPan)->load(), WithinAbs (slider.getMaxValue(), 1.0e-4));
    CHECK (slider.getMaxValue() >= slider.getMinValue());
    CHECK (gestures.begun.count (minimum.getParameterIndex()) == 1);
    CHECK (gestures.ended.count (minimum.getParameterIndex()) == 1);
    CHECK (gestures.begun.count (maximum.getParameterIndex()) == 1);
    CHECK (gestures.ended.count (maximum.getParameterIndex()) == 1);

    processor.removeListener (&gestures);
}

TEST_CASE ("Only the active procedure's controls are shown", "[editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto& state = processor.getParameterState();

    auto shows = [&] (const char* id)
    {
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        return isShowing (*control, editor);
    };

    CHECK (shows (livecut::ids::stutterChance));
    CHECK_FALSE (shows (livecut::ids::straightChance));
    CHECK_FALSE (shows (livecut::ids::activity));
    CHECK_FALSE (shows (livecut::ids::bounceChance));

    auto* pitch = findById (editor, "minPitch,maxPitch");
    REQUIRE (pitch != nullptr);
    CHECK_THAT (pitch->getAlpha(), WithinAbs (1.0, 1.0 / 255.0));

    auto* procedure = state.getParameter (livecut::ids::procedure);
    procedure->setValueNotifyingHost (procedure->convertTo0to1 (1.0f)); // WarpCut

    CHECK_FALSE (shows (livecut::ids::stutterChance));
    CHECK (shows (livecut::ids::straightChance));
    CHECK_FALSE (shows (livecut::ids::activity));

    procedure->setValueNotifyingHost (procedure->convertTo0to1 (3.0f)); // Bounce

    CHECK_FALSE (shows (livecut::ids::stutterChance));
    CHECK_FALSE (shows (livecut::ids::straightChance));
    CHECK_FALSE (shows (livecut::ids::activity));
    for (const auto* id : { livecut::ids::bounceChance, livecut::ids::ratchetChance,
                           livecut::ids::climb, livecut::ids::halftimeChance })
        CHECK (shows (id));
    CHECK_THAT (pitch->getAlpha(), WithinAbs (0.5, 1.0 / 255.0));

    procedure->setValueNotifyingHost (procedure->convertTo0to1 (0.0f)); // CutProc11
    CHECK (shows (livecut::ids::stutterChance));
    CHECK_FALSE (shows (livecut::ids::bounceChance));
    CHECK_THAT (pitch->getAlpha(), WithinAbs (1.0, 1.0 / 255.0));
}

TEST_CASE ("The status line says how to start cutting", "[editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);

    LivecutAudioProcessorEditor editor (processor);
    editor.refreshDisplay();

    CHECK (editor.statusText() == "Press play in your host to start cutting");
}

TEST_CASE ("The editor resizes at a fixed aspect ratio", "[editor]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);

    CHECK (editor.isResizable());
    REQUIRE (editor.getConstrainer() != nullptr);
    CHECK_THAT (editor.getConstrainer()->getFixedAspectRatio(),
                WithinAbs (static_cast<double> (editor.getWidth()) / editor.getHeight(), 1.0e-6));
}

// Hidden: run `LivecutTests "[.screenshot]"` to render the editor mid-phrase to
// editor.png in the working directory, for reviewing the design.
TEST_CASE ("Render the editor for design review", "[.screenshot]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;

    struct PlayHead final : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override { return info; }
        PositionInfo info;
    } playHead;

    playHead.info.setIsPlaying (true);
    playHead.info.setBpm (174.0);
    playHead.info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    double ppq = 0.0;

    for (int block = 0; block < 400; ++block)
    {
        playHead.info.setPpqPosition (ppq);
        playHead.info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
        playHead.info.setBarCount (static_cast<int64_t> (ppq / 4.0));
        processor.processBlock (buffer, midi);
        ppq += 512.0 * 174.0 / 60.0 / 48000.0;
    }

    LivecutAudioProcessorEditor editor (processor);
    editor.refreshDisplay();
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 2.0f);
    juce::FileOutputStream stream (juce::File::getCurrentWorkingDirectory().getChildFile ("editor.png"));
    stream.setPosition (0);
    stream.truncate();
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

TEST_CASE ("The editor exposes capture save and capture status", "[editor][capture]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    for (const auto* id : { "capture", "saveCapture", "captureStatus" })
    {
        INFO (id);
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        CHECK (isShowing (*control, editor));
    }
}

TEST_CASE ("Capture controls follow readiness and jobs survive editor destruction", "[editor][capture]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    auto& capture = processor.getCaptureService();
    capture.rolling().prepare (48000.0, 64);
    {
        LivecutAudioProcessorEditor editor (processor);
        auto* button = dynamic_cast<juce::TextButton*> (findById (editor, "capture"));
        auto* save = dynamic_cast<juce::TextButton*> (findById (editor, "saveCapture"));
        REQUIRE (button != nullptr);
        REQUIRE (save != nullptr);
        CHECK_FALSE (button->isEnabled());
        CHECK_FALSE (save->isEnabled());
        capture.rolling().startPhrase (true);
        for (int sample = 0; sample < 64; ++sample)
            capture.rolling().push (0.25f, -0.25f);
        capture.rolling().completePhrase();
        editor.refreshDisplay();
        CHECK (button->isEnabled());
        button->onClick();
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    while (capture.isBusy() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    REQUIRE_FALSE (capture.isBusy());
    REQUIRE (capture.captured() != nullptr);
    LivecutAudioProcessorEditor reopened (processor);
    auto* save = findById (reopened, "saveCapture");
    REQUIRE (save != nullptr);
    CHECK (save->isEnabled());
}
