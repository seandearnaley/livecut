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
struct StoppedHead final : juce::AudioPlayHead
{
    juce::Optional<PositionInfo> getPosition() const override { return PositionInfo {}; }
};

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
    auto* pattern = dynamic_cast<juce::TextButton*> (findById (editor, "patternTab"));
    REQUIRE (pattern != nullptr);
    pattern->onClick();
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
    buffer.clear();
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
    buffer.clear();
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
    auto* pattern = dynamic_cast<juce::TextButton*> (findById (editor, "patternTab"));
    REQUIRE (pattern != nullptr);
    pattern->onClick();
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
    set (livecut::ids::procedure, 1.0f);
    set (livecut::ids::subdivision, 6.0f);
    set (livecut::ids::straightChance, 1.0f);
    set (livecut::ids::minPhraseBars, 1.0f);
    set (livecut::ids::maxPhraseBars, 1.0f);
    auto* recut = dynamic_cast<juce::TextButton*> (findById (editor, "recutTab"));
    REQUIRE (recut != nullptr);
    recut->onClick();
    editor.refreshDisplay();
    saveImage ("editor-recut-requested.png");
    processor.applyRequestedMode();
    REQUIRE (processor.activeMode() == livecut::ProcessingMode::studio);
    playHead.info.setIsPlaying (true);
    playHead.info.setBpm (120.0);
    ppq = 0.0;
    for (int block = 0; block < 160; ++block)
    {
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const auto frame = block * buffer.getNumSamples() + sample;
            const auto position = frame % 3000;
            const auto value = position >= 96 && position < 576
                ? 0.7f * std::exp (-static_cast<float> (position - 96) / 100.0f) : 0.0f;
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, -value);
        }
        playHead.info.setPpqPosition (ppq);
        playHead.info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
        playHead.info.setBarCount (static_cast<int64_t> (ppq / 4.0));
        processor.processBlock (buffer, midi);
        ppq += 512.0 * 120.0 / 60.0 / 48000.0;
        if (block == 0)
        {
            editor.refreshDisplay();
            saveImage ("editor-recut-filling.png");
        }
    }
    REQUIRE (processor.getDisplayState().attackApplied.load() > 0);
    REQUIRE (processor.getDisplayState().reverseApplied.load() > 0);
    editor.refreshDisplay();
    saveImage ("editor-recut.png");
    const auto skippedBefore = processor.getDisplayState().transformIneligible.load();
    for (int block = 0; block < 220; ++block)
    {
        for (int channel = 0; channel < 2; ++channel)
            juce::FloatVectorOperations::fill (buffer.getWritePointer (channel), .2f, buffer.getNumSamples());
        playHead.info.setPpqPosition (ppq);
        playHead.info.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
        playHead.info.setBarCount (static_cast<int64_t> (ppq / 4.0));
        processor.processBlock (buffer, midi);
        ppq += 512.0 * 120.0 / 60.0 / 48000.0;
    }
    REQUIRE (processor.getDisplayState().transformIneligible.load() > skippedBefore);
    REQUIRE (processor.getDisplayState().transformReason.load() == livecut::DisplayState::TransformReason::noAttack);
    editor.refreshDisplay();
    saveImage ("editor-recut-ineligible.png");
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
    StoppedHead stopped;
    processor.setPlayHead (&stopped);
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
    StoppedHead stopped;
    processor.setPlayHead (&stopped);
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

TEST_CASE ("Impact page exposes three modules without displacing shared performance controls", "[editor][impact]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* impact = dynamic_cast<juce::TextButton*> (findById (editor, "impactTab"));
    REQUIRE (impact != nullptr);
    impact->onClick();
    CHECK (editor.getWidth() == 880);
    CHECK (editor.getHeight() == 724);
    std::vector<juce::Rectangle<int>> bounds;
    for (const auto* id : { "impactFilterEnabled", "impactFilterType", "impactFilterChance", "impactFilterCutoff",
                           "impactFilterQ", "impactFilterEnvOctaves", "impactFilterRepeatOctaves", "impactFilterMix",
                           "impactDriveEnabled", "impactDriveMode", "impactDriveChance", "impactDriveDb",
                           "impactDriveEnvDb", "impactDriveRepeatDb", "impactDriveMix", "impactTransientEnabled",
                           "impactTransientChance", "impactTransientAttackDb", "impactTransientSustainDb",
                           "impactTransientTimeMs", "impactTransientMix", "impactAttackMs", "impactDecayMs" })
    {
        INFO (id);
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        CHECK (isShowing (*control, editor));
        CHECK (control->getTitle().isNotEmpty());
        const auto area = editor.getLocalArea (control, control->getLocalBounds());
        CHECK_FALSE (area.isEmpty());
        CHECK (editor.getLocalBounds().contains (area));
        for (const auto& other : bounds)
            CHECK_FALSE (area.intersects (other));
        bounds.push_back (area);
    }
    for (const auto* id : { "wet", "outputTrim", "capture", "saveCapture", "swing", "accent", "hold" })
    {
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        CHECK (isShowing (*control, editor));
    }
    CHECK_FALSE (isShowing (*findById (editor, "subdivision"), editor));
    CHECK_FALSE (isShowing (*findById (editor, "crusher"), editor));
    auto* pattern = dynamic_cast<juce::TextButton*> (findById (editor, "patternTab"));
    REQUIRE (pattern != nullptr);
    pattern->onClick();
    CHECK (isShowing (*findById (editor, "subdivision"), editor));
    CHECK (isShowing (*findById (editor, "crusher"), editor));
    CHECK_FALSE (isShowing (*findById (editor, "impactFilterCutoff"), editor));
}

TEST_CASE ("Impact controls bind both ways and have named accessible input controls", "[editor][impact]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    editor.addToDesktop (0);
    auto* tab = dynamic_cast<juce::TextButton*> (findById (editor, "impactTab"));
    REQUIRE (tab != nullptr);
    tab->onClick();
    auto& state = processor.getParameterState();
    for (const auto* id : { "impactFilterCutoff", "impactDriveDb", "impactTransientAttackDb", "impactDecayMs" })
    {
        auto* row = findById (editor, id);
        REQUIRE (row != nullptr);
        auto* slider = findControl<juce::Slider> (*row);
        REQUIRE (slider != nullptr);
        CHECK (slider->getTitle().isNotEmpty());
        CHECK (slider->getAccessibilityHandler() != nullptr);
        CHECK (slider->getTooltip().isNotEmpty());
        auto& parameter = *state.getParameter (id);
        const auto target = parameter.convertFrom0to1 (.6f);
        slider->setValue (target, juce::sendNotificationSync);
        CHECK_THAT (state.getRawParameterValue (id)->load(), WithinAbs (target, .001));
        parameter.setValueNotifyingHost (.3f);
        CHECK_THAT (slider->getValue(), WithinAbs (parameter.convertFrom0to1 (.3f), .001));
    }
    for (const auto* id : { "impactFilterEnabled", "impactDriveEnabled", "impactTransientEnabled" })
    {
        auto* row = findById (editor, id);
        REQUIRE (row != nullptr);
        auto* button = findControl<juce::ToggleButton> (*row);
        REQUIRE (button != nullptr);
        CHECK (button->getAccessibilityHandler() != nullptr);
        CHECK (button->getTitle().isNotEmpty());
        button->setToggleState (true, juce::sendNotificationSync);
        CHECK (state.getRawParameterValue (id)->load() == 1);
    }
    for (const auto* id : { "impactFilterType", "impactDriveMode" })
    {
        auto* row = findById (editor, id);
        REQUIRE (row != nullptr);
        auto* box = findControl<juce::ComboBox> (*row);
        REQUIRE (box != nullptr);
        CHECK (box->getAccessibilityHandler() != nullptr);
        CHECK (box->getTitle().isNotEmpty());
        box->setSelectedId (2, juce::sendNotificationSync);
        CHECK (state.getRawParameterValue (id)->load() == 1);
    }
}

TEST_CASE ("Render the Impact editor for design review", "[.screenshot][impact]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* tab = dynamic_cast<juce::TextButton*> (findById (editor, "impactTab"));
    REQUIRE (tab != nullptr);
    tab->onClick();
    auto& state = processor.getParameterState();
    for (const auto* id : { "impactFilterEnabled", "impactDriveEnabled", "impactTransientEnabled" })
        state.getParameter (id)->setValueNotifyingHost (1);
    const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 2.0f);
    juce::FileOutputStream stream (juce::File::getCurrentWorkingDirectory().getChildFile ("editor-impact.png"));
    REQUIRE (stream.openedOk());
    stream.setPosition (0);
    stream.truncate();
    REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
}

TEST_CASE ("Fracture page exposes reader families and frequency controls at the existing size", "[editor][fracture-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* tab = dynamic_cast<juce::TextButton*> (findById (editor, "fractureTab"));
    REQUIRE (tab != nullptr);
    tab->onClick();
    CHECK (editor.getWidth() == 880);
    CHECK (editor.getHeight() == 724);
    for (const auto* id : { "fractureProcedure", "fractureActivity", "fractureGrainSizeMs", "fractureGrainDensity",
                           "fractureGrainPositionSpreadMs", "fractureGrainPitchSpreadCents", "fractureGrainPanSpread",
                           "fractureTapeStartSpeed", "fractureTapeEndSpeed", "fractureTapeCurve", "fractureTapeCycle",
                           "fractureTapeAlternate", "fractureFrequencyEnabled", "fractureFrequencyMode", "fractureFrequencyChance",
                           "fractureFrequencyHz", "fractureFrequencyEnvHz", "fractureFrequencyRepeatHz", "fractureFrequencyMix",
                           "fractureStatus", "fractureHelp" })
    {
        INFO (id);
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        CHECK (isShowing (*control, editor));
        CHECK (control->getTitle().isNotEmpty());
        const auto area = editor.getLocalArea (control, control->getLocalBounds());
        CHECK_FALSE (area.isEmpty());
        CHECK (editor.getLocalBounds().contains (area));
    }
    for (const auto* id : { "wet", "outputTrim", "capture", "saveCapture", "swing", "accent", "hold" })
    {
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        CHECK (isShowing (*control, editor));
    }
}

TEST_CASE ("Fracture families disable incompatible controls and preserve common performance", "[editor][fracture-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    editor.addToDesktop (0);
    auto* tab = dynamic_cast<juce::TextButton*> (findById (editor, "fractureTab"));
    REQUIRE (tab != nullptr);
    tab->onClick();
    auto* familyRow = findById (editor, "fractureProcedure");
    REQUIRE (familyRow != nullptr);
    auto* family = findControl<juce::ComboBox> (*familyRow);
    REQUIRE (family != nullptr);
    CHECK (family->getAccessibilityHandler() != nullptr);
    family->setSelectedId (3, juce::sendNotificationSync);
    CHECK (processor.getParameterState().getRawParameterValue ("fractureProcedure")->load() == 2);
    auto* pitch = findById (editor, "minPitch,maxPitch");
    REQUIRE (pitch != nullptr);
    CHECK_FALSE (pitch->isEnabled());
    CHECK (pitch->getTitle().contains ("TapeCut"));
    CHECK_FALSE (findById (editor, "procedure")->isEnabled());
    CHECK_THAT (findById (editor, "procedure")->getAlpha(), WithinAbs (.5, 1.0 / 255.0));
    CHECK_FALSE (findById (editor, "stutterChance")->isEnabled());
    CHECK_FALSE (findById (editor, "fractureGrainSizeMs")->isEnabled());
    CHECK (findById (editor, "fractureTapeStartSpeed")->isEnabled());
    CHECK (findById (editor, "fractureFrequencyHz")->isEnabled());
    auto* help = dynamic_cast<juce::Label*> (findById (editor, "fractureHelp"));
    REQUIRE (help != nullptr);
    CHECK (help->getText().contains ("replaces Pattern pitch"));
    CHECK (help->getText().contains ("Impact AD"));

    family->setSelectedId (2, juce::sendNotificationSync);
    CHECK (pitch->isEnabled());
    CHECK (findById (editor, "fractureGrainSizeMs")->isEnabled());
    CHECK_FALSE (findById (editor, "fractureTapeStartSpeed")->isEnabled());
    for (const auto* id : { "fractureGrainSizeMs", "fractureFrequencyHz" })
    {
        auto* row = findById (editor, id);
        REQUIRE (row != nullptr);
        auto* slider = findControl<juce::Slider> (*row);
        REQUIRE (slider != nullptr);
        CHECK (slider->getAccessibilityHandler() != nullptr);
        CHECK (slider->getTitle().isNotEmpty());
        CHECK (slider->getTooltip().isNotEmpty());
        auto& p = *processor.getParameterState().getParameter (id);
        const auto target = p.convertFrom0to1 (.7f);
        slider->setValue (target, juce::sendNotificationSync);
        CHECK_THAT (processor.getParameterState().getRawParameterValue (id)->load(), WithinAbs (target, .001));
        p.setValueNotifyingHost (.3f);
        CHECK_THAT (slider->getValue(), WithinAbs (p.convertFrom0to1 (.3f), .001));
    }
    family->setSelectedId (1, juce::sendNotificationSync);
    CHECK (pitch->isEnabled());
    CHECK (findById (editor, "procedure")->isEnabled());
    CHECK_THAT (findById (editor, "procedure")->getAlpha(), WithinAbs (1, 1.0 / 255.0));
    CHECK (findById (editor, "stutterChance")->isEnabled());
    auto* pattern = dynamic_cast<juce::TextButton*> (findById (editor, "patternTab"));
    REQUIRE (pattern != nullptr);
    pattern->onClick();
    CHECK (isShowing (*pitch, editor));
    CHECK_FALSE (isShowing (*familyRow, editor));
    CHECK (isShowing (*findById (editor, "crusher"), editor));
}

TEST_CASE ("Fracture controls and header tabs do not overlap", "[editor][fracture-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* tab = dynamic_cast<juce::TextButton*> (findById (editor, "fractureTab"));
    REQUIRE (tab != nullptr);
    tab->onClick();
    std::vector<juce::Rectangle<int>> bounds;
    for (const auto* id : { "patternTab", "recutTab", "impactTab", "fractureTab", "procedure",
                           "fractureProcedure", "fractureActivity", "fractureGrainSizeMs", "fractureGrainDensity",
                           "fractureGrainPositionSpreadMs", "fractureGrainPitchSpreadCents", "fractureGrainPanSpread",
                           "fractureTapeStartSpeed", "fractureTapeEndSpeed", "fractureTapeCurve", "fractureTapeCycle",
                           "fractureTapeAlternate", "fractureFrequencyEnabled", "fractureFrequencyMode", "fractureFrequencyChance",
                           "fractureFrequencyHz", "fractureFrequencyEnvHz", "fractureFrequencyRepeatHz", "fractureFrequencyMix",
                           "fractureStatus", "fractureHelp", "wet", "outputTrim", "capture", "saveCapture", "swing", "accent", "hold" })
    {
        INFO (id);
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        const auto area = editor.getLocalArea (control, control->getLocalBounds());
        for (const auto& previous : bounds)
            CHECK_FALSE (area.intersects (previous));
        bounds.push_back (area);
    }
}

TEST_CASE ("Fracture source status distinguishes waiting failures and host conditions", "[editor][fracture-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    // This idle fixture owns the processor; publish diagnostic snapshots without an audio thread.
    auto& display = const_cast<livecut::DisplayState&> (processor.getDisplayState());
    auto* status = dynamic_cast<juce::Label*> (findById (editor, "fractureStatus"));
    REQUIRE (status != nullptr);
    auto& family = *processor.getParameterState().getParameter ("fractureProcedure");
    family.setValueNotifyingHost (family.convertTo0to1 (1));
    display.status.store (livecut::DisplayState::Status::cutting);
    display.fractureFamily.store (livecut::engine::FractureProcedure::grainCut);
    struct Expected { livecut::engine::FractureState state; const char* text; };
    const std::array cases {
        Expected { livecut::engine::FractureState::waiting, "Waiting" },
        Expected { livecut::engine::FractureState::sourceLost, "Source lost" },
        Expected { livecut::engine::FractureState::contextUnavailable, "context unavailable" },
        Expected { livecut::engine::FractureState::capacityError, "capacity error" },
        Expected { livecut::engine::FractureState::transformBudgetExhausted, "budget exhausted" },
        Expected { livecut::engine::FractureState::ended, "Cut ended" }
    };
    for (const auto& spec : cases)
    {
        display.fractureState.store (spec.state);
        editor.refreshDisplay();
        CHECK (status->getText().contains (spec.text));
    }
    display.fractureState.store (livecut::engine::FractureState::playing);
    display.fractureVoices.store (4);
    editor.refreshDisplay();
    CHECK (status->getText().contains ("voices 4"));
    family.setValueNotifyingHost (family.convertTo0to1 (2));
    editor.refreshDisplay();
    CHECK (status->getText().contains ("Pending next bar: TapeCut"));
    display.fractureFamily.store (livecut::engine::FractureProcedure::tapeCut);
    display.fractureTapeSpeed.store (.35);
    editor.refreshDisplay();
    CHECK (status->getText().contains ("speed 0.35x"));
    display.status.store (livecut::DisplayState::Status::waitingForHost);
    display.fractureState.store (livecut::engine::FractureState::waiting);
    editor.refreshDisplay();
    CHECK (status->getText().containsIgnoreCase ("Press play"));
    CHECK_FALSE (status->getText().contains ("Grain history"));
    display.status.store (livecut::DisplayState::Status::streamError);
    editor.refreshDisplay();
    CHECK (status->getText().containsIgnoreCase ("stream error"));
}

TEST_CASE ("Render the Fracture editor for design review", "[.screenshot][fracture-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* tab = dynamic_cast<juce::TextButton*> (findById (editor, "fractureTab"));
    REQUIRE (tab != nullptr);
    tab->onClick();
    auto& state = processor.getParameterState();
    auto& family = *state.getParameter ("fractureProcedure");
    state.getParameter ("fractureFrequencyEnabled")->setValueNotifyingHost (1);
    // Render controlled UI states; these images are not a DSP/audio validation.
    auto& display = const_cast<livecut::DisplayState&> (processor.getDisplayState());
    display.status.store (livecut::DisplayState::Status::cutting);
    display.phraseBars.store (4);
    const auto save = [&] (const char* name)
    {
        editor.refreshDisplay();
        const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 2.0f);
        juce::FileOutputStream stream (juce::File::getCurrentWorkingDirectory().getChildFile (name));
        REQUIRE (stream.openedOk());
        stream.setPosition (0);
        stream.truncate();
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, stream));
    };
    family.setValueNotifyingHost (family.convertTo0to1 (1));
    display.fractureFamily.store (livecut::engine::FractureProcedure::grainCut);
    display.fractureState.store (livecut::engine::FractureState::waiting);
    save ("editor-fracture-grain-waiting.png");
    family.setValueNotifyingHost (family.convertTo0to1 (2));
    display.fractureFamily.store (livecut::engine::FractureProcedure::tapeCut);
    display.fractureState.store (livecut::engine::FractureState::playing);
    display.fractureTapeSpeed.store (.5);
    save ("editor-fracture-tape.png");
    auto* pattern = dynamic_cast<juce::TextButton*> (findById (editor, "patternTab"));
    REQUIRE (pattern != nullptr);
    pattern->onClick();
    save ("editor-fracture-tape-pattern.png");
}

TEST_CASE ("Resonance controls bind parameters and fit the editor", "[editor][resonance-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    auto* tab = dynamic_cast<juce::TextButton*> (findById (editor, "resonanceTab"));
    REQUIRE (tab != nullptr);
    tab->onClick();
    std::vector<juce::Rectangle<int>> bounds;
    for (const auto* id : { "patternTab", "recutTab", "impactTab", "fractureTab", "resonanceTab", "procedure",
                           "resonanceEnabled", "resonanceRootHz", "resonanceDecayMs", "resonanceExcitationDb",
                           "resonanceInterval1", "resonanceInterval2", "resonanceInterval3", "resonanceInterval4",
                           "resonanceWeight1", "resonanceWeight2", "resonanceWeight3", "resonanceWeight4",
                           "resonanceMix", "resonanceOrder", "resonanceSpectralEnabled", "resonanceFreeze",
                           "resonanceSmearMs", "resonanceSpectralMix", "applySpectral", "spectralStatus", "resonatorStatus", "resonanceHelp",
                           "wet", "outputTrim", "capture", "saveCapture", "swing", "accent", "hold" })
    {
        INFO (id);
        auto* control = findById (editor, id);
        REQUIRE (control != nullptr);
        CHECK (isShowing (*control, editor));
        if (juce::String (id).startsWith ("resonance") || juce::String (id) == "resonatorStatus"
            || juce::String (id) == "spectralStatus" || juce::String (id) == "applySpectral")
            CHECK (control->getTitle().isNotEmpty());
        const auto area = editor.getLocalArea (control, control->getLocalBounds());
        CHECK (editor.getLocalBounds().contains (area));
        for (const auto& previous : bounds)
            CHECK_FALSE (area.intersects (previous));
        bounds.push_back (area);
    }
    auto* root = findControl<juce::Slider> (*findById (editor, "resonanceRootHz"));
    auto* freeze = findControl<juce::ToggleButton> (*findById (editor, "resonanceFreeze"));
    REQUIRE (root != nullptr);
    REQUIRE (freeze != nullptr);
    root->setValue (220, juce::sendNotificationSync);
    freeze->setToggleState (true, juce::sendNotificationSync);
    CHECK_THAT (processor.getParameterState().getRawParameterValue ("resonanceRootHz")->load(), WithinAbs (220, .001));
    CHECK (processor.getParameterState().getRawParameterValue ("resonanceFreeze")->load() == 1);
    CHECK (root->getTitle().isNotEmpty());
    CHECK (freeze->getTitle().isNotEmpty());
}

TEST_CASE ("Render the Resonance editor for design review", "[.screenshot][resonance-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    struct Head final : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override { return info; }
        PositionInfo info;
    } head;
    head.info.setIsPlaying (false);
    head.info.setBpm (120);
    head.info.setPpqPosition (0);
    head.info.setPpqPositionOfLastBarStart (0);
    head.info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
    processor.setPlayHead (&head);
    processor.setNonRealtime (true);
    processor.prepareToPlay (48000, 256);
    juce::AudioBuffer<float> buffer (2, 256);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    LivecutAudioProcessorEditor editor (processor);
    auto* tab = dynamic_cast<juce::TextButton*> (findById (editor, "resonanceTab"));
    auto* apply = dynamic_cast<juce::TextButton*> (findById (editor, "applySpectral"));
    REQUIRE (tab != nullptr);
    REQUIRE (apply != nullptr);
    tab->onClick();
    const auto save = [&] (const char* name)
    {
        editor.refreshDisplay();
        const auto image = editor.createComponentSnapshot (editor.getLocalBounds(), true, 2.0f);
        juce::FileOutputStream output (juce::File::getCurrentWorkingDirectory().getChildFile (name));
        REQUIRE (output.openedOk());
        output.setPosition (0);
        output.truncate();
        REQUIRE (juce::PNGImageFormat().writeImageToStream (image, output));
    };
    auto& state = processor.getParameterState();
    state.getParameter ("resonanceEnabled")->setValueNotifyingHost (1);
    state.getParameter ("resonanceSpectralEnabled")->setValueNotifyingHost (1);
    state.getParameter ("resonanceFreeze")->setValueNotifyingHost (1);
    save ("editor-resonance-pending.png");
    REQUIRE (apply->isEnabled());
    apply->onClick();
    REQUIRE (processor.activeSpectral());
    save ("editor-resonance-waiting.png");
    head.info.setIsPlaying (true);
    double ppq = 0;
    for (int block = 0; block < 160; ++block)
    {
        buffer.clear();
        head.info.setPpqPosition (ppq);
        head.info.setPpqPositionOfLastBarStart (std::floor (ppq / 4) * 4);
        processor.processBlock (buffer, midi);
        ppq += 256.0 * 120 / 60 / 48000;
    }
    REQUIRE (processor.getDisplayState().spectralFault.load() == livecut::spectral::Fault::none);
    REQUIRE (processor.getDisplayState().spectralAvailability.load() == livecut::spectral::Availability::ready);
    save ("editor-resonance.png");
}

TEST_CASE ("Both configuration Apply buttons handle a Mode-only tuple change", "[editor][resonance-ui][apply-tuple]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    StoppedHead stopped;
    processor.setPlayHead (&stopped);
    processor.prepareToPlay (8000.0, 32);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    LivecutAudioProcessorEditor editor (processor);
    auto* spectralApply = dynamic_cast<juce::TextButton*> (findById (editor, "applySpectral"));
    auto* recutApply = dynamic_cast<juce::TextButton*> (findById (editor, "applyMode"));
    REQUIRE (spectralApply != nullptr);
    REQUIRE (recutApply != nullptr);
    auto& mode = *processor.getParameterState().getParameter (livecut::ids::mode);
    mode.setValueNotifyingHost (mode.convertTo0to1 (1));
    editor.refreshDisplay();
    CHECK (recutApply->isEnabled());
    REQUIRE (spectralApply->isEnabled());
    spectralApply->onClick();
    editor.refreshDisplay();
    CHECK (processor.activeMode() == livecut::ProcessingMode::studio);
    CHECK_FALSE (processor.activeSpectral());
    CHECK_FALSE (spectralApply->isEnabled());
    CHECK_FALSE (recutApply->isEnabled());
}

TEST_CASE ("Resonance inputs expose keyboard changes and bidirectional host bindings", "[editor][resonance-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    editor.addToDesktop (0);
    auto* tab = dynamic_cast<juce::TextButton*> (findById (editor, "resonanceTab"));
    REQUIRE (tab != nullptr);
    tab->onClick();
    auto& state = processor.getParameterState();
    for (const auto* id : { "resonanceRootHz", "resonanceDecayMs", "resonanceExcitationDb", "resonanceInterval1",
                           "resonanceInterval2", "resonanceInterval3", "resonanceInterval4", "resonanceWeight1",
                           "resonanceWeight2", "resonanceWeight3", "resonanceWeight4", "resonanceMix",
                           "resonanceSmearMs", "resonanceSpectralMix" })
    {
        INFO (id);
        auto* row = findById (editor, id);
        REQUIRE (row != nullptr);
        auto* slider = findControl<juce::Slider> (*row);
        REQUIRE (slider != nullptr);
        CHECK (slider->getAccessibilityHandler() != nullptr);
        CHECK (slider->getTitle().isNotEmpty());
        CHECK (slider->getTooltip().isNotEmpty());
        auto& parameter = *state.getParameter (id);
        const auto target = parameter.getNormalisableRange().snapToLegalValue (parameter.convertFrom0to1 (.4f));
        slider->setValue (target, juce::sendNotificationSync);
        CHECK_THAT (state.getRawParameterValue (id)->load(), WithinAbs (target, .001));
        parameter.setValueNotifyingHost (.6f);
        const auto before = slider->getValue();
        CHECK_THAT (before, WithinAbs (state.getRawParameterValue (id)->load(), .001));
        CHECK (slider->keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
        CHECK (slider->getValue() > before);
        CHECK_THAT (state.getRawParameterValue (id)->load(), WithinAbs (slider->getValue(), .001));
    }
    for (const auto* id : { "resonanceEnabled", "resonanceSpectralEnabled", "resonanceFreeze" })
    {
        auto* row = findById (editor, id);
        REQUIRE (row != nullptr);
        auto* toggle = findControl<juce::ToggleButton> (*row);
        REQUIRE (toggle != nullptr);
        CHECK (toggle->getAccessibilityHandler() != nullptr);
        CHECK (toggle->getTitle().isNotEmpty());
        toggle->setToggleState (true, juce::sendNotificationSync);
        CHECK (state.getRawParameterValue (id)->load() == 1);
        state.getParameter (id)->setValueNotifyingHost (0);
        CHECK_FALSE (toggle->getToggleState());
    }
    auto* order = findControl<juce::ComboBox> (*findById (editor, "resonanceOrder"));
    REQUIRE (order != nullptr);
    CHECK (order->getTitle().isNotEmpty());
    CHECK (order->getAccessibilityHandler() != nullptr);
    order->setSelectedId (3, juce::sendNotificationSync);
    CHECK (state.getRawParameterValue ("resonanceOrder")->load() == 2);
    state.getParameter ("resonanceOrder")->setValueNotifyingHost (0);
    CHECK (order->getSelectedId() == 1);
}

TEST_CASE ("Resonance displays invalid weights retuning and effect-order transitions", "[editor][resonance-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    // An idle fixture publishes diagnostic snapshots; no audio thread is running.
    auto& display = const_cast<livecut::DisplayState&> (processor.getDisplayState());
    auto* status = dynamic_cast<juce::Label*> (findById (editor, "resonatorStatus"));
    REQUIRE (status != nullptr);
    display.resonatorState.store (livecut::engine::Resonator::State::invalidWeights);
    editor.refreshDisplay();
    CHECK (status->getText().contains ("All weights zero"));
    CHECK (status->getText().contains ("wet is silent"));
    display.resonatorState.store (livecut::engine::Resonator::State::invalidParameters);
    editor.refreshDisplay();
    CHECK (status->getText().contains ("Invalid tuning"));
    display.resonatorState.store (livecut::engine::Resonator::State::unsupportedRate);
    editor.refreshDisplay();
    CHECK (status->getText().contains ("Unsupported"));
    display.resonatorState.store (livecut::engine::Resonator::State::ready);
    display.resonatorRetuning.store (true);
    display.resonatorPending.store (true);
    display.effectRoute.store (livecut::engine::EffectRoute::impactResonatorFrequency);
    display.requestedEffectRoute.store (livecut::engine::EffectRoute::resonatorImpactFrequency);
    editor.refreshDisplay();
    CHECK (status->getText().contains ("Retuning: 50 ms"));
    CHECK (status->getText().contains ("latest tuning queued"));
    CHECK (status->getText().contains ("pending next bar"));
    display.effectRouteTransition.store (true);
    editor.refreshDisplay();
    CHECK (status->getText().contains ("Effect order crossfading"));
    CHECK_FALSE (status->getText().contains ("pending next bar"));
}

TEST_CASE ("Resonance Apply distinguishes pending and active spectral configuration", "[editor][resonance-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    struct Head final : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override { return info; }
        PositionInfo info;
    } head;
    head.info.setIsPlaying (true);
    head.info.setBpm (120);
    head.info.setPpqPosition (0);
    head.info.setTimeSignature (juce::AudioPlayHead::TimeSignature { 4, 4 });
    processor.setPlayHead (&head);
    processor.setNonRealtime (true);
    processor.prepareToPlay (8000, 32);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    LivecutAudioProcessorEditor editor (processor);
    auto* apply = dynamic_cast<juce::TextButton*> (findById (editor, "applySpectral"));
    auto* recut = dynamic_cast<juce::TextButton*> (findById (editor, "applyMode"));
    auto* status = dynamic_cast<juce::Label*> (findById (editor, "spectralStatus"));
    REQUIRE (apply != nullptr);
    REQUIRE (recut != nullptr);
    REQUIRE (status != nullptr);
    auto& spectral = *processor.getParameterState().getParameter ("resonanceSpectralEnabled");
    spectral.setValueNotifyingHost (1);
    editor.refreshDisplay();
    REQUIRE (apply->isEnabled());
    CHECK (recut->isEnabled());
    CHECK_FALSE (processor.activeSpectral());
    CHECK (status->getText().containsIgnoreCase ("pending"));
    CHECK (status->getText().contains ("Requested: Live / Spectral on"));
    CHECK (status->getText().contains ("Active: Live / Spectral off"));
    apply->onClick();
    editor.refreshDisplay();
    CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::pendingPlaying);
    CHECK_FALSE (processor.activeSpectral());
    CHECK_FALSE (apply->isEnabled());
    CHECK_FALSE (recut->isEnabled());
    head.info.setIsPlaying (false);
    processor.processBlock (buffer, midi);
    // Repeat the existing queued request from the message thread after a real stopped callback.
    processor.applyRequestedMode();
    editor.refreshDisplay();
    REQUIRE (processor.activeSpectral());
    CHECK (processor.reportedLatencySamples() == 2048);
    CHECK_FALSE (apply->isEnabled());
    CHECK_FALSE (recut->isEnabled());
    CHECK (status->getText().contains ("2048"));
    CHECK (status->getText().contains ("Active: Live / Spectral on"));
    CHECK (status->getText().containsIgnoreCase ("Waiting"));
    // Every installed stream requires its own explicit transport observation.
    buffer.clear();
    processor.processBlock (buffer, midi);
    spectral.setValueNotifyingHost (0);
    editor.refreshDisplay();
    CHECK (processor.activeSpectral());
    CHECK (status->getText().containsIgnoreCase ("pending"));
    REQUIRE (recut->isEnabled());
    recut->onClick();
    editor.refreshDisplay();
    CHECK_FALSE (processor.activeSpectral());
    CHECK (processor.reportedLatencySamples() == 0);
}

TEST_CASE ("Both Apply buttons recover an actual stopped Spectral fault", "[editor][resonance-ui][fault]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    for (const auto* buttonId : { "applySpectral", "applyMode" })
    {
        INFO (buttonId);
        livecut::spectral::TestHooks hooks;
        hooks.pause = true;
        LivecutAudioProcessor processor (&hooks);
        processor.getParameterState().getParameter ("resonanceSpectralEnabled")->setValueNotifyingHost (1);
        StoppedHead stopped;
        processor.setPlayHead (&stopped);
        processor.prepareToPlay (8000, 2048);
        juce::AudioBuffer<float> buffer (2, 2048);
        buffer.clear();
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
        REQUIRE (processor.getDisplayState().spectralFault.load() == livecut::spectral::Fault::deadline);
        hooks.pause = false;
        LivecutAudioProcessorEditor editor (processor);
        auto* spectralApply = dynamic_cast<juce::TextButton*> (findById (editor, "applySpectral"));
        auto* recutApply = dynamic_cast<juce::TextButton*> (findById (editor, "applyMode"));
        auto* status = dynamic_cast<juce::Label*> (findById (editor, "spectralStatus"));
        REQUIRE (spectralApply != nullptr);
        REQUIRE (recutApply != nullptr);
        REQUIRE (status != nullptr);
        CHECK (status->getText().contains ("Spectral error"));
        CHECK (spectralApply->isEnabled());
        CHECK (recutApply->isEnabled());
        auto* selectedApply = dynamic_cast<juce::TextButton*> (findById (editor, buttonId));
        REQUIRE (selectedApply != nullptr);
        selectedApply->onClick();
        editor.refreshDisplay();
        CHECK (processor.modeState() == LivecutAudioProcessor::ModeState::active);
        CHECK (processor.activeSpectral());
        CHECK (processor.getDisplayState().spectralFault.load() == livecut::spectral::Fault::none);
        CHECK (processor.reportedLatencySamples() == 2048);
        CHECK_FALSE (status->getText().contains ("Spectral error"));
        CHECK_FALSE (spectralApply->isEnabled());
        CHECK_FALSE (recutApply->isEnabled());
    }
}
