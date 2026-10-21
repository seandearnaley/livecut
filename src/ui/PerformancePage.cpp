// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/PerformancePage.h"
#include "ui/LivecutLookAndFeel.h"
#include "Parameters.h"
#include <cmath>

namespace livecut::ui
{
namespace
{
juce::RangedAudioParameter& parameter (LivecutAudioProcessor& owner, const char* id)
{
    auto* found = owner.getParameterState().getParameter (id);
    jassert (found != nullptr);
    return *found;
}
}

PerformancePage::FillButton::FillButton (juce::RangedAudioParameter& parameter)
    : TextButton ("Fill (hold)"), attachment (parameter, [this] (float value)
      {
          const juce::ScopedValueSetter<bool> guard (receiving, true);
          setToggleState (value > .5f, juce::dontSendNotification);
      })
{
    setComponentID (parameter.getParameterID());
    setTitle ("Momentary Fill");
    setTooltip ("Hold for denser rhythmic repeats. Release to return. Changes apply at the next 1/8-bar boundary; also available as host automation.");
    onStateChange = [this]
    {
        if (receiving)
            return;
        if (isDown() && ! gesture)
        {
            gesture = true;
            attachment.beginGesture();
            attachment.setValueAsPartOfGesture (1.0f);
        }
        else if (! isDown())
            release();
    };
    attachment.sendInitialUpdate();
}
PerformancePage::FillButton::~FillButton() { release(); }
void PerformancePage::FillButton::release()
{
    stopTimer();
    if (! gesture)
        return;
    attachment.setValueAsPartOfGesture (0.0f);
    attachment.endGesture();
    gesture = false;
}
void PerformancePage::FillButton::mouseDown (const juce::MouseEvent& event)
{
    juce::TextButton::mouseDown (event);
    if (gesture) startTimerHz (30);
}
void PerformancePage::FillButton::mouseUp (const juce::MouseEvent& event) { juce::TextButton::mouseUp (event); release(); }
void PerformancePage::FillButton::focusLost (FocusChangeType cause) { juce::TextButton::focusLost (cause); release(); }
void PerformancePage::FillButton::visibilityChanged() { juce::TextButton::visibilityChanged(); if (! isVisible()) release(); }
void PerformancePage::FillButton::enablementChanged() { juce::TextButton::enablementChanged(); if (! isEnabled()) release(); }
void PerformancePage::FillButton::timerCallback()
{
    if (! juce::ModifierKeys::getCurrentModifiersRealtime().isLeftButtonDown() || ! isShowing()) release();
}

PerformancePage::PerformancePage (LivecutAudioProcessor& processor)
    : owner (processor), enabled (parameter (owner, ids::performanceEnabled), "Perform engine"),
      rhythm (parameter (owner, ids::performanceVariation), parameter (owner, ids::performanceDensity), "Rhythm", "Variation X", "Density Y"),
      texture (parameter (owner, ids::performanceGrain), parameter (owner, ids::performanceCharacter), "Texture", "Grain X", "Tail Y"),
      seed (parameter (owner, ids::performanceSeed), "Pattern seed"),
      fill (parameter (owner, ids::performanceFill))
{
    setComponentID ("performancePage");
    setTitle ("Performance controls");
    enabled.setTitle ("Perform engine");
    seed.setTitle ("Performance pattern seed");
    rhythm.xControl().setTooltip ("Variation rearranges source slices, from local edits to wider jumps. " + rhythm.getTooltip());
    rhythm.yControl().setTooltip ("Density moves from sparse gated hits through the original pulse to rapid repeats. " + rhythm.getTooltip());
    texture.xControl().setTooltip ("Grain blends overlapping grains into slice tails while keeping the opening attack clear. " + texture.getTooltip());
    texture.yControl().setTooltip ("Tail changes grain length from 12 to 90 ms. Grain at zero leaves the captured slice unchanged. " + texture.getTooltip());
    for (auto* component : std::initializer_list<juce::Component*> { &enabled, &prepare, &rhythm, &texture, &seed, &fill, &grab, &keep, &mutate, &returnHome, &status, &timing })
        addAndMakeVisible (*component);
    prepare.setComponentID ("preparePerformance");
    prepare.setTitle ("Apply requested Perform engine configuration while stopped");
    prepare.setTooltip ("Stop the host, enable Perform engine, then Prepare. Perform uses no added latency and switches Spectral off. Disable Perform engine and Apply to return to the requested Live / Studio mode.");
    prepare.onClick = [this]
    {
        if (enabled.isOn() || owner.activeMode() != ProcessingMode::perform) owner.preparePerformanceMode();
        else owner.leavePerformanceMode();
        refresh();
    };
    const auto action = [this] (juce::TextButton& button, const char* id, const char* help, LivecutAudioProcessor::PerformanceAction command)
    {
        button.setComponentID (id);
        button.setTitle (button.getButtonText());
        button.setTooltip (help);
        button.onClick = [this, command] { owner.requestPerformanceAction (command); refresh(); };
    };
    action (grab, "performanceGrab", "Capture the next full input bar as a fixed source. The first bar is armed automatically when Perform starts.", LivecutAudioProcessor::PerformanceAction::capture);
    action (keep, "performanceKeep", "Keep the current four axis targets and pattern seed as your Return position. This snapshot is saved in the project.", LivecutAudioProcessor::PerformanceAction::keep);
    action (mutate, "performanceMutate", "Choose the next deterministic pattern seed. The new pattern starts at the next bar.", LivecutAudioProcessor::PerformanceAction::mutate);
    action (returnHome, "performanceReturn", "Restore the four axis targets and seed saved by Keep. Rhythm changes at the next 1/8-bar boundary; seed at the next bar.", LivecutAudioProcessor::PerformanceAction::returnToKept);
    seed.setTooltip ("Deterministic pattern seed. Changes apply at the next bar. Mutate advances the seed; Keep saves it with the four axes.");
    for (auto* label : { &status, &timing })
    {
        label->setBorderSize ({});
        label->setFont (uiFont (12.0f));
        label->setColour (juce::Label::textColourId, palette::inkMuted);
    }
    status.setComponentID ("performanceStatus");
    status.setTitle ("Performance source and pending state");
    timing.setText ("Rhythm: next 1/8 bar | Texture: 3 ms | Solid dot: target / ring: applied", juce::dontSendNotification);
    refresh();
}

void PerformancePage::resized()
{
    enabled.setBounds (0, 0, 150, 30);
    prepare.setBounds (160, 0, 175, 30);
    seed.setBounds (getWidth() - 275, 0, 275, 38);
    const auto width = (getWidth() - 12) / 2;
    rhythm.setBounds (0, 46, width, 254);
    texture.setBounds (width + 12, 46, width, 254);
    const auto buttonWidth = (getWidth() - 4 * 10) / 5;
    int x = 0;
    for (auto* button : std::initializer_list<juce::TextButton*> { &grab, &fill, &keep, &mutate, &returnHome })
    {
        button->setBounds (x, 310, buttonWidth, 32);
        x += buttonWidth + 10;
    }
    status.setBounds (0, 347, getWidth(), 20);
    timing.setBounds (0, 368, getWidth(), 16);
}

void PerformancePage::refresh()
{
    const auto& display = owner.getDisplayState();
    const auto active = owner.activeMode() == ProcessingMode::perform;
    const auto available = active && display.performanceHasSource.load();
    rhythm.showApplied (display.performanceVariation.load(), display.performanceDensity.load(), available);
    texture.showApplied (display.performanceGrain.load(), display.performanceCharacter.load(), available);
    const auto state = owner.modeState();
    const auto requestedSpectral = owner.getParameterState().getRawParameterValue (ids::resonanceSpectralEnabled)->load() > .5f;
    const auto engineHealthy = ! active || (display.performanceState.load() != engine::PerformanceEngine::State::error
        && display.status.load() != DisplayState::Status::streamError);
    const auto configurationReady = active == enabled.isOn() && state == LivecutAudioProcessor::ModeState::active
        && (! active || ! requestedSpectral) && engineHealthy;
    prepare.setButtonText (configurationReady ? (active ? "Perform ready | 0 latency" : "Prepare Perform") : "Apply while stopped");
    prepare.setEnabled ((! configurationReady || ! active) && state != LivecutAudioProcessor::ModeState::pendingPlaying
                        && state != LivecutAudioProcessor::ModeState::pendingHostChange && state != LivecutAudioProcessor::ModeState::unprepared);
    grab.setEnabled (active && ! display.performanceCaptureArmed.load());
    fill.setEnabled (available);
    for (auto* button : std::initializer_list<juce::TextButton*> { &grab, &fill, &prepare })
        button->setAlpha (button->isEnabled() ? 1.0f : .5f);
    const auto message = active && requestedSpectral ? owner.modeStatusText() : owner.performanceStatusText();
    status.setText (message, juce::dontSendNotification);
    status.setTooltip (message);
}

void PerformancePage::cancelInteractions()
{
    rhythm.endInteraction();
    texture.endInteraction();
    fill.release();
}
} // namespace livecut::ui
