// SPDX-License-Identifier: GPL-3.0-or-later
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "ui/XYPad.h"
#include "ui/PhraseRuler.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <map>

using Catch::Matchers::WithinAbs;
namespace
{
juce::Component* findPerformanceControl (juce::Component& root, const juce::String& id)
{
    if (root.getComponentID() == id) return &root;
    for (auto* child : root.getChildren())
        if (auto* found = findPerformanceControl (*child, id)) return found;
    return nullptr;
}
struct PerformanceGestures final : juce::AudioProcessorListener
{
    void audioProcessorParameterChanged (juce::AudioProcessor*, int, float) override {}
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override { ++begins[index]; }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override { ++ends[index]; }
    std::map<int, int> begins, ends;
};
juce::MouseEvent pointerEvent (juce::Component& component, juce::Point<float> point, bool down = true, bool shift = false)
{
    const auto modifiers = juce::ModifierKeys ((down ? juce::ModifierKeys::leftButtonModifier : 0) | (shift ? juce::ModifierKeys::shiftModifier : 0));
    const auto time = juce::Time::getCurrentTime();
    return { juce::Desktop::getInstance().getMainMouseSource(), point, modifiers, 1.0f, 0, 0, 0, 0, &component, &component, time, point, time, 1, true };
}
struct PadFixture
{
    PadFixture() : x (*processor.getParameterState().getParameter (livecut::ids::performanceVariation)),
                   y (*processor.getParameterState().getParameter (livecut::ids::performanceDensity)),
                   pad (x, y, "Rhythm", "Variation", "Density")
    {
        processor.addListener (&gestures);
        pad.setSize (400, 270);
        pad.setVisible (true);
        pad.addToDesktop (0);
    }
    ~PadFixture() { pad.endInteraction(); processor.removeListener (&gestures); }
    LivecutAudioProcessor processor;
    juce::RangedAudioParameter& x;
    juce::RangedAudioParameter& y;
    PerformanceGestures gestures;
    livecut::ui::XYPad pad;
};
}

TEST_CASE ("Performance is the first editor page and preserves deep controls", "[editor][performance-ui]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    LivecutAudioProcessorEditor editor (processor);
    editor.setVisible (true);
    editor.addToDesktop (0);
    auto* page = findPerformanceControl (editor, "performancePage");
    REQUIRE (page != nullptr);
    CHECK (page->isVisible());
    CHECK_FALSE (findPerformanceControl (editor, "procedure")->isVisible());
    for (const auto* id : { "performanceEnabled", "performanceVariation", "performanceDensity", "performanceGrain", "performanceCharacter", "performanceFill", "performanceSeed", "preparePerformance", "performanceGrab", "performanceKeep", "performanceMutate", "performanceReturn", "performanceStatus" })
    {
        INFO (id);
        auto* control = findPerformanceControl (editor, id);
        REQUIRE (control != nullptr);
        CHECK (control->isShowing());
        CHECK_FALSE (control->getBounds().isEmpty());
        CHECK (editor.getLocalBounds().contains (editor.getLocalArea (control, control->getLocalBounds())));
        CHECK (control->getTitle().isNotEmpty());
    }
    auto* pattern = dynamic_cast<juce::TextButton*> (findPerformanceControl (editor, "patternTab"));
    REQUIRE (pattern != nullptr);
    pattern->onClick();
    CHECK_FALSE (page->isVisible());
    CHECK (findPerformanceControl (editor, "procedure")->isVisible());
}

TEST_CASE ("XY pad maps absolute coordinates and closes both host gestures once", "[editor][performance-ui][xy]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    PadFixture fixture;
    const auto area = fixture.pad.padBounds();
    fixture.pad.mouseDown (pointerEvent (fixture.pad, area.getCentre()));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (.5, .001));
    CHECK_THAT (fixture.y.getValue(), WithinAbs (.5, .001));
    fixture.pad.mouseDrag (pointerEvent (fixture.pad, { area.getRight() + 100, area.getY() - 100 }));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (1.0, .001));
    CHECK_THAT (fixture.y.getValue(), WithinAbs (1.0, .001));
    fixture.pad.mouseUp (pointerEvent (fixture.pad, { area.getX() - 100, area.getBottom() + 100 }, false));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (0.0, .001));
    CHECK_THAT (fixture.y.getValue(), WithinAbs (0.0, .001));
    fixture.pad.endInteraction();
    for (auto* parameter : { &fixture.x, &fixture.y })
    {
        CHECK (fixture.gestures.begins[parameter->getParameterIndex()] == 1);
        CHECK (fixture.gestures.ends[parameter->getParameterIndex()] == 1);
    }
}

TEST_CASE ("XY pad exposes native accessible axes and follows host automation", "[editor][performance-ui][xy]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    PadFixture fixture;
    fixture.x.setValueNotifyingHost (.731f);
    fixture.y.setValueNotifyingHost (.219f);
    CHECK_THAT (fixture.pad.xControl().getValue(), WithinAbs (.731, .001));
    CHECK_THAT (fixture.pad.yControl().getValue(), WithinAbs (.219, .001));
    for (auto* slider : { &fixture.pad.xControl(), &fixture.pad.yControl() })
    {
        CHECK (slider->getAccessibilityHandler() != nullptr);
        CHECK (slider->getTitle().isNotEmpty());
        CHECK (slider->getTooltip().isNotEmpty());
    }
    fixture.pad.xControl().setValue (.42, juce::sendNotificationSync);
    CHECK_THAT (fixture.x.getValue(), WithinAbs (.42, .001));
    CHECK (fixture.gestures.begins[fixture.x.getParameterIndex()] == 1);
    CHECK (fixture.gestures.ends[fixture.x.getParameterIndex()] == 1);
}

TEST_CASE ("XY keyboard edits are fine coarse and resettable complete gestures", "[editor][performance-ui][xy]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    PadFixture fixture;
    fixture.x.setValueNotifyingHost (.5f);
    fixture.y.setValueNotifyingHost (.5f);
    REQUIRE (fixture.pad.keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (.51, .0001));
    REQUIRE (fixture.pad.keyPressed (juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0)));
    CHECK_THAT (fixture.y.getValue(), WithinAbs (.501, .0001));
    REQUIRE (fixture.pad.xControl().keyPressed (juce::KeyPress (juce::KeyPress::leftKey, juce::ModifierKeys::ctrlModifier, 0)));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (.41, .0001));
    REQUIRE (fixture.pad.keyPressed (juce::KeyPress (juce::KeyPress::homeKey)));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (fixture.x.getDefaultValue(), .0001));
    CHECK_THAT (fixture.y.getValue(), WithinAbs (fixture.y.getDefaultValue(), .0001));
    for (auto* parameter : { &fixture.x, &fixture.y })
        CHECK (fixture.gestures.begins[parameter->getParameterIndex()] == fixture.gestures.ends[parameter->getParameterIndex()]);
}

TEST_CASE ("XY fine dragging starts without a jump and cancellations close gestures", "[editor][performance-ui][xy]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    for (const auto cancellation : { 0, 1, 2, 3 })
    {
        INFO (cancellation);
        PadFixture fixture;
        fixture.x.setValueNotifyingHost (.3f);
        fixture.y.setValueNotifyingHost (.6f);
        const auto origin = fixture.pad.padBounds().getCentre();
        fixture.pad.mouseDown (pointerEvent (fixture.pad, origin, true, true));
        CHECK_THAT (fixture.x.getValue(), WithinAbs (.3, .0001));
        CHECK_THAT (fixture.y.getValue(), WithinAbs (.6, .0001));
        fixture.pad.mouseDrag (pointerEvent (fixture.pad, origin + juce::Point<float> { 30, -30 }, true, true));
        CHECK (fixture.x.getValue() > .3f);
        CHECK (fixture.y.getValue() > .6f);
        if (cancellation == 0) fixture.pad.focusLost (juce::Component::focusChangedDirectly);
        if (cancellation == 1) fixture.pad.setEnabled (false);
        if (cancellation == 2) fixture.pad.setVisible (false);
        if (cancellation == 3) fixture.pad.setSize (420, 280);
        for (auto* parameter : { &fixture.x, &fixture.y })
        {
            CHECK (fixture.gestures.begins[parameter->getParameterIndex()] == 1);
            CHECK (fixture.gestures.ends[parameter->getParameterIndex()] == 1);
        }
    }
}

TEST_CASE ("Momentary Fill releases when leaving the performance page", "[editor][performance-ui][fill]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    LivecutAudioProcessor processor;
    PerformanceGestures gestures;
    processor.addListener (&gestures);
    {
        LivecutAudioProcessorEditor editor (processor);
        auto* fill = dynamic_cast<juce::TextButton*> (findPerformanceControl (editor, livecut::ids::performanceFill));
        auto* pattern = dynamic_cast<juce::TextButton*> (findPerformanceControl (editor, "patternTab"));
        REQUIRE (fill != nullptr);
        REQUIRE (pattern != nullptr);
        fill->setEnabled (true); // Exercise the input component independently of source readiness.
        fill->setState (juce::Button::buttonDown);
        auto& parameter = *processor.getParameterState().getParameter (livecut::ids::performanceFill);
        CHECK_THAT (parameter.getValue(), WithinAbs (1.0, .0001));
        pattern->onClick();
        CHECK_THAT (parameter.getValue(), WithinAbs (0.0, .0001));
        CHECK (gestures.begins[parameter.getParameterIndex()] == 1);
        CHECK (gestures.ends[parameter.getParameterIndex()] == 1);
    }
    processor.removeListener (&gestures);
}

TEST_CASE ("Performance ruler displays the supplied source state without Studio buffer text", "[editor][performance-ui]")
{
    livecut::ui::PhraseRuler::Snapshot snapshot;
    snapshot.status = livecut::DisplayState::Status::filling;
    snapshot.performanceStatus = "Grabbing one complete bar";
    CHECK (livecut::ui::PhraseRuler::describe (snapshot) == "Grabbing one complete bar");
    auto next = snapshot;
    next.performanceStatus = "Captured bar playing";
    CHECK_FALSE (snapshot.sameAs (next));
}

TEST_CASE ("XY fine modifier and mouse release do not jump the current values", "[editor][performance-ui][xy]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    PadFixture fixture;
    const auto origin = fixture.pad.padBounds().getCentre();
    fixture.pad.mouseDown (pointerEvent (fixture.pad, origin));
    const auto before = fixture.x.getValue();
    fixture.pad.mouseDrag (pointerEvent (fixture.pad, origin + juce::Point<float> { 30, 0 }, true, true));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (before, .0001));
    const auto destination = origin + juce::Point<float> { 60, 0 };
    fixture.pad.mouseDrag (pointerEvent (fixture.pad, destination, true, true));
    const auto fineValue = fixture.x.getValue();
    CHECK (fineValue > before);
    CHECK (fineValue < before + .02f);
    fixture.pad.mouseUp (pointerEvent (fixture.pad, destination, false, false));
    CHECK_THAT (fixture.x.getValue(), WithinAbs (fineValue, .0001));
}

TEST_CASE ("Perform preparation remains available for an incompatible Spectral request", "[editor][performance-ui][configuration]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    struct Stopped final : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override { return PositionInfo {}; }
    } stopped;
    LivecutAudioProcessor processor;
    processor.setPlayHead (&stopped);
    processor.getParameterState().getParameter (livecut::ids::performanceEnabled)->setValueNotifyingHost (1.0f);
    processor.prepareToPlay (8000, 32);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    LivecutAudioProcessorEditor editor (processor);
    auto* prepare = dynamic_cast<juce::TextButton*> (findPerformanceControl (editor, "preparePerformance"));
    auto* status = dynamic_cast<juce::Label*> (findPerformanceControl (editor, "performanceStatus"));
    REQUIRE (prepare != nullptr);
    REQUIRE (status != nullptr);
    CHECK_FALSE (prepare->isEnabled());
    auto* modeSummary = dynamic_cast<juce::Label*> (findPerformanceControl (editor, "modeSummary"));
    auto* spectralStatus = dynamic_cast<juce::Label*> (findPerformanceControl (editor, "spectralStatus"));
    auto* legacyApply = dynamic_cast<juce::TextButton*> (findPerformanceControl (editor, "applyMode"));
    REQUIRE (modeSummary != nullptr);
    REQUIRE (spectralStatus != nullptr);
    REQUIRE (legacyApply != nullptr);
    CHECK (modeSummary->getText().startsWith ("Active: Perform"));
    CHECK (spectralStatus->getText().contains ("Requested: Perform"));
    CHECK (spectralStatus->getText().contains ("Active: Perform"));
    CHECK_FALSE (legacyApply->isEnabled());
    auto* patternTab = dynamic_cast<juce::TextButton*> (findPerformanceControl (editor, "patternTab"));
    auto* performanceTab = dynamic_cast<juce::TextButton*> (findPerformanceControl (editor, "performanceTab"));
    REQUIRE (patternTab != nullptr);
    REQUIRE (performanceTab != nullptr);
    patternTab->onClick();
    editor.refreshDisplay();
    CHECK (editor.statusText().contains ("Legacy controls inactive in Perform"));
    performanceTab->onClick();
    auto& spectral = *processor.getParameterState().getParameter (livecut::ids::resonanceSpectralEnabled);
    spectral.setValueNotifyingHost (1.0f);
    editor.refreshDisplay();
    CHECK (prepare->isEnabled());
    CHECK (status->getText().contains ("Spectral"));
    CHECK_FALSE (prepare->getButtonText().contains ("ready"));
    prepare->onClick();
    editor.refreshDisplay();
    CHECK_THAT (spectral.getValue(), WithinAbs (0.0, .0001));
    CHECK (processor.activeMode() == livecut::ProcessingMode::perform);
    CHECK_FALSE (processor.activeSpectral());
    CHECK_FALSE (prepare->isEnabled());
}

TEST_CASE ("Perform hides unused groove controls and legacy mode restores them", "[editor][performance-ui][groove]")
{
    juce::ScopedJuceInitialiser_GUI gui;
    struct Stopped final : juce::AudioPlayHead
    {
        juce::Optional<PositionInfo> getPosition() const override { return PositionInfo {}; }
    } stopped;
    LivecutAudioProcessor processor;
    processor.setPlayHead (&stopped);
    processor.prepareToPlay (8000, 32);
    juce::AudioBuffer<float> buffer (2, 32);
    buffer.clear();
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);
    LivecutAudioProcessorEditor editor (processor);
    auto* swing = findPerformanceControl (editor, livecut::ids::swing);
    auto* accent = findPerformanceControl (editor, livecut::ids::accent);
    auto* pattern = dynamic_cast<juce::TextButton*> (findPerformanceControl (editor, "patternTab"));
    auto* perform = dynamic_cast<juce::TextButton*> (findPerformanceControl (editor, "performanceTab"));
    REQUIRE (swing != nullptr);
    REQUIRE (accent != nullptr);
    REQUIRE (pattern != nullptr);
    REQUIRE (perform != nullptr);
    CHECK_FALSE (swing->isVisible());
    CHECK_FALSE (accent->isVisible());
    pattern->onClick();
    CHECK (swing->isVisible());
    CHECK (accent->isVisible());
    CHECK (swing->isEnabled());
    CHECK (accent->isEnabled());
    processor.preparePerformanceMode();
    editor.refreshDisplay();
    REQUIRE (processor.activeMode() == livecut::ProcessingMode::perform);
    CHECK (swing->isVisible());
    CHECK (accent->isVisible());
    CHECK_FALSE (swing->isEnabled());
    CHECK_FALSE (accent->isEnabled());
    perform->onClick();
    CHECK_FALSE (swing->isVisible());
    CHECK_FALSE (accent->isVisible());
    for (const auto* id : { "wet", "outputTrim", "hold", "capture", "saveCapture" })
    {
        INFO (id);
        auto* control = findPerformanceControl (editor, id);
        REQUIRE (control != nullptr);
        CHECK (control->isVisible());
    }
    processor.processBlock (buffer, midi); // Observe stopped transport in the new prepared stream.
    processor.leavePerformanceMode();
    editor.refreshDisplay();
    REQUIRE (processor.activeMode() == livecut::ProcessingMode::live);
    pattern->onClick();
    CHECK (swing->isVisible());
    CHECK (accent->isVisible());
    CHECK (swing->isEnabled());
    CHECK (accent->isEnabled());
}
