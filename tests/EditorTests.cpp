// SPDX-License-Identifier: GPL-3.0-or-later

#include "Parameters.h"
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/Controls.h"
#include "ui/RangeSlider.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <set>
#include <chrono>
#include <thread>
#include <vector>

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
    const auto saveImage = [&] (const char* name)
    {
        const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 2.0f);
        juce::FileOutputStream stream (juce::File::getCurrentWorkingDirectory().getChildFile (name));
        REQUIRE (stream.openedOk());
        stream.setPosition (0);
        stream.truncate();
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    };
    saveImage ("editor.png");

    playHead.info.setIsPlaying (false);
    processor.processBlock (buffer, midi);
    auto& state = processor.getParameterState();
    const auto set = [&] (const char* id, float value)
    {
        auto& parameter = *state.getParameter (id);
        parameter.setValueNotifyingHost (parameter.convertTo0to1 (value));
    };
    set (livecut::ids::mode, 1.0f);
    set (livecut::ids::attackAlign, 1.0f);
    set (livecut::ids::reverseChance, 0.5f);
    auto* recut = dynamic_cast<juce::TextButton*> (findById (editor, "recutTab"));
    REQUIRE (recut != nullptr);
    recut->onClick();
    editor.refreshDisplay();
    saveImage ("editor-recut-requested.png");
    processor.applyRequestedMode();
    REQUIRE (processor.activeMode() == livecut::ProcessingMode::studio);
    playHead.info.setIsPlaying (true);
    for (int block = 0; block < 160; ++block)
    {
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto frame = block * buffer.getNumSamples() + sample;
            const auto position = frame % 6000;
            const auto value = position < 480 ? 0.7f * std::exp (-static_cast<float> (position) / 100.0f) : 0.0f;
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, -value);
        }
        playHead.info.setPpqPosition (ppq);
        playHead.info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
        playHead.info.setBarCount (static_cast<int64_t> (ppq / 4.0));
        processor.processBlock (buffer, midi);
        ppq += 512.0 * 174.0 / 60.0 / 48000.0;
        if (block == 0)
        {
            editor.refreshDisplay();
            saveImage ("editor-recut-filling.png");
        }
    }
    editor.refreshDisplay();
    saveImage ("editor-recut.png");
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

TEST_CASE ("Perform editor displays groove Hold and its status", "[editor][perform]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    for (const auto* id : { "swing", "accent", "hold", "holdStatus" })
    {
        INFO (id);
        REQUIRE (findById (editor, id) != nullptr);
    }
}

TEST_CASE ("Recut has accessible mode and transform controls within the existing footprint", "[editor][recut]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    editor.addToDesktop (0); // JUCE creates accessibility handlers only when a native peer exists.
    CHECK (editor.getWidth() == 880);
    CHECK (editor.getHeight() == 724);
    auto* recut = dynamic_cast<juce::TextButton*> (findById (editor, "recutTab"));
    auto* pattern = dynamic_cast<juce::TextButton*> (findById (editor, "patternTab"));
    REQUIRE (recut != nullptr);
    REQUIRE (pattern != nullptr);
    CHECK (recut->getAccessibilityHandler() != nullptr);
    CHECK (recut->getTitle().isNotEmpty());
    recut->onClick();
    for (const auto* id : { "mode", "attackAlign", "attackThreshold", "reverseChance",
                           "applyMode", "modeSummary", "modeStatus", "transformStatus" })
    {
        INFO (id);
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        CHECK (isShowing (*control, editor));
        CHECK (control->getTitle().isNotEmpty());
        CHECK_FALSE (control->getBounds().isEmpty());
        CHECK (editor.getLocalBounds().contains (editor.getLocalArea (control, control->getLocalBounds())));
    }
    for (const auto* id : { "wet", "outputTrim", "capture", "saveCapture", "swing", "accent", "hold" })
    {
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        CHECK (isShowing (*control, editor));
    }
    auto* subdivision = findById (editor, "subdivision");
    REQUIRE (subdivision != nullptr);
    CHECK_FALSE (isShowing (*subdivision, editor));
    pattern->onClick();
    CHECK (isShowing (*subdivision, editor));
    CHECK_FALSE (isShowing (*findById (editor, "mode"), editor));

    recut->onClick();
    auto& procedure = *processor.getParameterState().getParameter (livecut::ids::procedure);
    procedure.setValueNotifyingHost (procedure.convertTo0to1 (3.0f));
    CHECK_FALSE (isShowing (*findById (editor, livecut::ids::bounceChance), editor));
    pattern->onClick();
    CHECK (isShowing (*findById (editor, livecut::ids::bounceChance), editor));
    CHECK_FALSE (isShowing (*findById (editor, livecut::ids::stutterChance), editor));
}

TEST_CASE ("Recut mode and transform rows do not overlap each other or Apply", "[editor][recut]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* recut = dynamic_cast<juce::TextButton*> (findById (editor, "recutTab"));
    REQUIRE (recut != nullptr);
    recut->onClick();
    std::vector<juce::Rectangle<int>> bounds;
    for (const auto* id : { "mode", "attackAlign", "attackThreshold", "reverseChance", "applyMode",
                           "modeSummary", "modeStatus", "transformStatus" })
    {
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        const auto area = editor.getLocalArea (control, control->getLocalBounds());
        for (const auto& other : bounds)
            CHECK_FALSE (area.intersects (other));
        bounds.push_back (area);
    }
}

TEST_CASE ("Recut controls bind parameters and expose named keyboard controls", "[editor][recut]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    editor.addToDesktop (0);
    auto& state = processor.getParameterState();
    auto* mode = findById (editor, "mode");
    auto* attack = findById (editor, "attackAlign");
    auto* threshold = findById (editor, "attackThreshold");
    auto* reverse = findById (editor, "reverseChance");
    REQUIRE (mode != nullptr);
    REQUIRE (attack != nullptr);
    REQUIRE (threshold != nullptr);
    REQUIRE (reverse != nullptr);
    auto* box = findControl<juce::ComboBox> (*mode);
    auto* toggle = findControl<juce::ToggleButton> (*attack);
    auto* thresholdSlider = findControl<juce::Slider> (*threshold);
    auto* reverseSlider = findControl<juce::Slider> (*reverse);
    REQUIRE (box != nullptr);
    REQUIRE (toggle != nullptr);
    REQUIRE (thresholdSlider != nullptr);
    REQUIRE (reverseSlider != nullptr);
    for (auto* component : std::initializer_list<juce::Component*> { box, toggle, thresholdSlider, reverseSlider })
    {
        CHECK (component->getTitle().isNotEmpty());
        CHECK (component->getAccessibilityHandler() != nullptr);
    }
    box->setSelectedId (2, juce::sendNotificationSync);
    toggle->setToggleState (true, juce::sendNotificationSync);
    thresholdSlider->setValue (12.0, juce::sendNotificationSync);
    reverseSlider->setValue (0.75, juce::sendNotificationSync);
    CHECK_THAT (state.getRawParameterValue ("mode")->load(), WithinAbs (1.0, 0.0));
    CHECK_FALSE (state.getParameter ("mode")->isAutomatable());
    CHECK_THAT (state.getRawParameterValue ("attackAlign")->load(), WithinAbs (1.0, 0.0));
    CHECK_THAT (state.getRawParameterValue ("attackThreshold")->load(), WithinAbs (12.0, 0.001));
    CHECK_THAT (state.getRawParameterValue ("reverseChance")->load(), WithinAbs (0.75, 0.001));
}

TEST_CASE ("Recut distinguishes requested and active mode and applies through the processor", "[editor][recut]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* apply = dynamic_cast<juce::TextButton*> (findById (editor, "applyMode"));
    auto* summary = dynamic_cast<juce::Label*> (findById (editor, "modeSummary"));
    auto* status = dynamic_cast<juce::Label*> (findById (editor, "modeStatus"));
    auto* transforms = dynamic_cast<juce::Label*> (findById (editor, "transformStatus"));
    REQUIRE (apply != nullptr);
    REQUIRE (summary != nullptr);
    REQUIRE (status != nullptr);
    REQUIRE (transforms != nullptr);
    CHECK_FALSE (apply->isEnabled());
    CHECK_THAT (apply->getAlpha(), WithinAbs (0.5, 1.0 / 255.0));
    CHECK (summary->getText().containsIgnoreCase ("not prepared"));
    processor.prepareToPlay (8000.0, 32);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi); // A real stopped callback supplies transport observation.
    auto* mode = processor.getParameterState().getParameter ("mode");
    REQUIRE (mode != nullptr);
    mode->setValueNotifyingHost (mode->convertTo0to1 (1.0f));
    editor.refreshDisplay();
    CHECK (summary->getText().contains ("Active: Live"));
    CHECK (summary->getText().contains ("0 samples"));
    REQUIRE (apply->isEnabled());
    CHECK_THAT (apply->getAlpha(), WithinAbs (1.0, 1.0 / 255.0));
    apply->onClick();
    editor.refreshDisplay();
    CHECK (processor.activeMode() == livecut::ProcessingMode::studio);
    CHECK (summary->getText().contains ("Active: Studio"));
    CHECK (summary->getText().contains ("4000 samples"));
    CHECK (status->getText() == processor.modeStatusText());
    CHECK (transforms->getText() == processor.transformStatusText());
    CHECK_FALSE (apply->isEnabled());
    CHECK_THAT (apply->getAlpha(), WithinAbs (0.5, 1.0 / 255.0));
    processor.processBlock (buffer, midi);
    editor.refreshDisplay();
    CHECK (editor.statusText().containsIgnoreCase ("filling"));
}

TEST_CASE ("Recut keeps the published Live latency visible when Apply waits for playback to stop", "[editor][recut]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    struct PlayHead final : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override { return info; }
        PositionInfo info;
    } playHead;
    playHead.info.setIsPlaying (true);
    playHead.info.setBpm (120.0);
    playHead.info.setPpqPosition (0.0);
    playHead.info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
    processor.setPlayHead (&playHead);
    processor.prepareToPlay (8000.0, 32);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    LivecutAudioProcessorEditor editor (processor);
    auto* apply = dynamic_cast<juce::TextButton*> (findById (editor, "applyMode"));
    auto* summary = dynamic_cast<juce::Label*> (findById (editor, "modeSummary"));
    auto* status = dynamic_cast<juce::Label*> (findById (editor, "modeStatus"));
    REQUIRE (apply != nullptr);
    REQUIRE (summary != nullptr);
    REQUIRE (status != nullptr);
    auto& mode = *processor.getParameterState().getParameter (livecut::ids::mode);
    mode.setValueNotifyingHost (mode.convertTo0to1 (1.0f));
    editor.refreshDisplay();
    REQUIRE (apply->isEnabled());
    apply->onClick();
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingPlaying);
    CHECK (summary->getText().contains ("Active: Live"));
    CHECK (summary->getText().contains ("0 samples"));
    CHECK (status->getText().containsIgnoreCase ("stop host playback"));
    CHECK_FALSE (apply->isEnabled());
}

TEST_CASE ("Recut does not claim an active mode after preparation fails", "[editor][recut]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (0.0, 32);
    REQUIRE (processor.modeState() == LivecutAudioProcessor::ModeState::error);
    LivecutAudioProcessorEditor editor (processor);
    auto* summary = dynamic_cast<juce::Label*> (findById (editor, "modeSummary"));
    auto* status = dynamic_cast<juce::Label*> (findById (editor, "modeStatus"));
    REQUIRE (summary != nullptr);
    REQUIRE (status != nullptr);
    CHECK (summary->getText().containsIgnoreCase ("mode error"));
    CHECK_FALSE (summary->getText().contains ("Active:"));
    CHECK (status->getText() == processor.modeStatusText());
}

TEST_CASE ("Recut waits for host latency reconciliation before claiming the new mode is active", "[editor][recut]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    processor.prepareToPlay (8000.0, 32);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    LivecutAudioProcessorEditor editor (processor);
    struct Reconfigure final : juce::AudioProcessorListener
    {
        Reconfigure (LivecutAudioProcessor& p, LivecutAudioProcessorEditor& e) : processor (p), editor (e) {}
        void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
        void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails& details) override
        {
            if (! details.latencyChanged || triggered)
                return;
            triggered = true;
            processor.prepareToPlay (16000.0, 32);
            pending = processor.modeState() == LivecutAudioProcessor::ModeState::pendingHostChange;
            editor.refreshDisplay();
            auto* summary = dynamic_cast<juce::Label*> (findById (editor, "modeSummary"));
            if (summary != nullptr)
                pendingSummary = summary->getText();
        }
        LivecutAudioProcessor& processor;
        LivecutAudioProcessorEditor& editor;
        bool triggered = false, pending = false;
        juce::String pendingSummary;
    } reconfigure (processor, editor);
    processor.addListener (&reconfigure);
    auto& mode = *processor.getParameterState().getParameter (livecut::ids::mode);
    mode.setValueNotifyingHost (mode.convertTo0to1 (1.0f));
    processor.applyRequestedMode();
    processor.removeListener (&reconfigure);
    REQUIRE (reconfigure.triggered);
    CHECK (reconfigure.pending);
    CHECK (reconfigure.pendingSummary.containsIgnoreCase ("updating host latency"));
    CHECK_FALSE (reconfigure.pendingSummary.contains ("Active:"));
    editor.refreshDisplay();
    auto* summary = dynamic_cast<juce::Label*> (findById (editor, "modeSummary"));
    REQUIRE (summary != nullptr);
    CHECK (summary->getText().contains ("Active: Studio"));
    CHECK (summary->getText().contains ("8000 samples"));
}
