// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginEditor.h"

#include "Parameters.h"
#include "ui/Controls.h"
#include "ui/PhraseRuler.h"
#include "ui/PerformancePage.h"
#include "ui/RangeSlider.h"

#include <array>
#include <cmath>
#include <vector>

namespace ids = livecut::ids;
namespace ui = livecut::ui;

namespace
{
constexpr int margin = 20;
constexpr int gap = 10;
constexpr int sectionPadding = 12;
constexpr int sliderRowHeight = 40;
constexpr int choiceRowHeight = 28;
constexpr int rowGap = 4;

struct ProcedureInfo
{
    const char* name;
    const char* description;
};

constexpr std::array procedures {
    ProcedureInfo { "CutProc11", "Odd-length cuts, repeated, with a stutter to close each phrase" },
    ProcedureInfo { "WarpCut", "Straight slices and rolls that speed up or slow down" },
    ProcedureInfo { "SQPusher", "Quaver and semiquaver cuts, with a Squarepusher fill on each phrase's last bar" },
    ProcedureInfo { "Bounce", "Straight hits and triplet bounces, a ratchet roll that climbs to close each phrase, and the odd halftime bar" },
};

livecut::ProcessingMode requestedModeOf (const juce::AudioProcessorValueTreeState& state)
{
    if (state.getRawParameterValue (ids::performanceEnabled)->load() > .5f)
        return livecut::ProcessingMode::perform;
    return state.getRawParameterValue (ids::mode)->load() > .5f
        ? livecut::ProcessingMode::studio : livecut::ProcessingMode::live;
}

const char* modeName (livecut::ProcessingMode mode)
{
    if (mode == livecut::ProcessingMode::perform) return "Perform";
    return mode == livecut::ProcessingMode::studio ? "Studio" : "Live";
}

// Stacks rows down a column, top to bottom.
struct Column
{
    juce::Rectangle<int> area;

    void place (juce::Component& component, int height)
    {
        component.setBounds (area.removeFromTop (height));
        area.removeFromTop (rowGap);
    }
};
} // namespace

namespace
{
// Dedicated page keeps the existing Pattern and Recut rows at their original size.
class ImpactPage final : public juce::Component
{
public:
    explicit ImpactPage (juce::AudioProcessorValueTreeState& state)
    {
        const auto add = [&] (const char* id, const char* caption, const char* help, int column)
        {
            auto* parameter = state.getParameter (id);
            jassert (parameter != nullptr);
            std::unique_ptr<juce::Component> control;
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (parameter))
                control = std::make_unique<ui::ChoiceRow> (*choice, "");
            else if (parameter->isBoolean())
                control = std::make_unique<ui::PowerSwitch> (*parameter, caption);
            else
                control = std::make_unique<ui::SliderRow> (*parameter, caption);
            control->setTitle (parameter->getName (80));
            for (auto* child : control->getChildren())
            {
                child->setTitle (parameter->getName (80));
                if (auto* tooltip = dynamic_cast<juce::SettableTooltipClient*> (child))
                    tooltip->setTooltip (help);
            }
            addAndMakeVisible (*control);
            rows.push_back ({ std::move (control), column, parameter->isBoolean() ? 26 : dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr ? 28 : 35 });
        };
        add (ids::impactAttackMs, "Env attack", "Shared cut envelope attack, 0 to 25 ms. Restarts on each cut/repeat.", -1);
        add (ids::impactDecayMs, "Env decay", "Shared cut envelope decay to -60 dB, 5 to 1000 ms.", -1);
        add (ids::impactFilterEnabled, "Slice filter", "Enable the cut-aware stereo filter. Changes apply at the next planned block; FX gates ramp over 2 ms.", 0);
        add (ids::impactFilterType, "Filter type", "Low-pass, band-pass or high-pass filter.", 0);
        add (ids::impactFilterChance, "Chance", "Deterministic probability per cut, independent of cutter randomness.", 0);
        add (ids::impactFilterCutoff, "Cutoff", "Base cutoff in Hz; also limited to 45% of sample rate.", 0);
        add (ids::impactFilterQ, "Resonance", "Filter Q from 0.5 to 8; high values emphasize the cutoff.", 0);
        add (ids::impactFilterEnvOctaves, "Env depth", "Cut envelope modulation, -6 to +6 octaves.", 0);
        add (ids::impactFilterRepeatOctaves, "Repeat", "Cutoff offset from first to last repeat, -4 to +4 octaves. A single repeat starts at zero.", 0);
        add (ids::impactFilterMix, "Filter mix", "Filter amount; 0% is neutral after the gate ramp settles.", 0);
        add (ids::impactDriveEnabled, "Drive / fold", "Enable first-order ADAA waveshaping. No added buffered latency; active processing adds half-sample phase and high-frequency coloration.", 1);
        add (ids::impactDriveMode, "Drive mode", "Soft saturation or wavefolding. Enabled Drive at 0 dB is still nonlinear.", 1);
        add (ids::impactDriveChance, "Chance", "Deterministic probability per cut, independent of cutter randomness.", 1);
        add (ids::impactDriveDb, "Drive", "Input drive from 0 to 30 dB. Enabled 0 dB is not neutral; use power off or mix 0% for neutral output.", 1);
        add (ids::impactDriveEnvDb, "Env depth", "Drive offset from the shared cut envelope, -24 to +24 dB.", 1);
        add (ids::impactDriveRepeatDb, "Repeat", "Drive offset from first to last repeat, -24 to +24 dB.", 1);
        add (ids::impactDriveMix, "Drive mix", "Drive amount; 0% is neutral after the gate ramp settles.", 1);
        add (ids::impactTransientEnabled, "Transient", "Enable stereo-linked transient shaping. Changes apply at the next planned block.", 2);
        add (ids::impactTransientChance, "Chance", "Deterministic probability per cut, independent of cutter randomness.", 2);
        add (ids::impactTransientAttackDb, "Attack", "Transient gain, -18 to +18 dB. Attack and sustain both 0 dB are neutral.", 2);
        add (ids::impactTransientSustainDb, "Sustain", "Body gain, -18 to +18 dB. Attack and sustain both 0 dB are neutral.", 2);
        add (ids::impactTransientTimeMs, "Response", "Stereo-linked detector response, 10 to 200 ms.", 2);
        add (ids::impactTransientMix, "Transient mix", "Transient effect amount; 0% is neutral after the gate ramp settles.", 2);
        help.setComponentID ("impactHelp");
        help.setTitle ("Impact timing and signal path");
        help.setText ("Next planned block  |  2 ms FX gates  |  Filter > Drive > Transient", juce::dontSendNotification);
        help.setFont (ui::uiFont (12.0f));
        help.setColour (juce::Label::textColourId, ui::palette::inkMuted);
        help.setBorderSize ({});
        addAndMakeVisible (help);
    }

    void resized() override
    {
        const auto width = (getWidth() - 2 * gap) / 3;
        std::array<int, 3> tops { 94, 94, 94 };
        int shared = 0;
        for (auto& row : rows)
        {
            if (row.column < 0)
            {
                row.control->setBounds (166 + shared++ * 254, 8, 230, 40);
                continue;
            }
            const auto col = static_cast<std::size_t> (row.column);
            row.control->setBounds (row.column * (width + gap) + sectionPadding, tops[col], width - 2 * sectionPadding, row.height);
            tops[col] += row.height + 3;
        }
        help.setBounds (sectionPadding, 53, getWidth() - 2 * sectionPadding, 22);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (ui::palette::well);
        g.fillRoundedRectangle (getLocalBounds().removeFromTop (81).toFloat(), 4.0f);
        const auto width = (getWidth() - 2 * gap) / 3;
        for (int col = 0; col < 3; ++col)
            g.fillRoundedRectangle (juce::Rectangle<int> (col * (width + gap), 84, width, getHeight() - 84).toFloat(), 4.0f);
        g.setColour (ui::palette::ink);
        g.setFont (ui::uiFont (14.0f, true));
        g.drawFittedText ("Filter / drive\nenvelope", { sectionPadding, 8, 140, 40 }, juce::Justification::centredLeft, 2);
    }

private:
    struct Row { std::unique_ptr<juce::Component> control; int column, height; };
    std::vector<Row> rows;
    juce::Label help;
};
// Resonator tuning and the prepared spectral suffix share one page.
class ResonancePage final : public juce::Component
{
public:
    explicit ResonancePage (LivecutAudioProcessor& processor) : owner (processor)
    {
        auto& state = owner.getParameterState();
        const auto add = [&] (const char* id, const char* caption, const char* help, int column)
        {
            auto* parameter = state.getParameter (id);
            jassert (parameter != nullptr);
            std::unique_ptr<juce::Component> control;
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (parameter))
                control = std::make_unique<ui::ChoiceRow> (*choice, "");
            else if (parameter->isBoolean())
                control = std::make_unique<ui::PowerSwitch> (*parameter, caption);
            else
                control = std::make_unique<ui::SliderRow> (*parameter, caption);
            control->setTitle (parameter->getName (80));
            for (auto* child : control->getChildren())
            {
                child->setTitle (parameter->getName (80));
                if (auto* tooltip = dynamic_cast<juce::SettableTooltipClient*> (child))
                    tooltip->setTooltip (help);
            }
            addAndMakeVisible (*control);
            rows.push_back ({ std::move (control), column, parameter->isBoolean() ? 27 : dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr ? 30 : 35 });
        };
        add (ids::resonanceEnabled, "Resonator", "Enable four tuned resonances. Controls apply at the next planned block; tuning changes crossfade over 50 ms.", 0);
        add (ids::resonanceRootHz, "Root", "Root frequency, 40 to 800 Hz. Four voices use semitone offsets from this frequency.", 0);
        add (ids::resonanceDecayMs, "Decay", "Pole decay time to -60 dB, 40 to 5000 ms. Excitation and source content affect the heard tail.", 0);
        add (ids::resonanceExcitationDb, "Excitation", "Resonator input gain, -12 to +12 dB. There is no hidden limiter.", 0);
        add (ids::resonanceMix, "Resonator mix", "Local resonator blend, with 2 ms gate smoothing. Zero is neutral after settling.", 0);
        add (ids::resonanceOrder, "Effect order", "Three fixed orders for Impact, Resonator and Frequency. Changes begin on a bar boundary with a 5 ms crossfade; Spectral always follows them.", 0);
        add (ids::resonanceInterval1, "Voice 1 interval", "Semitone offset from root, -12 to +24. Retuning crossfades fixed banks over 50 ms.", 1);
        add (ids::resonanceWeight1, "Voice 1 weight", "Relative voice weight. Weights normalize together; all four at zero explicitly disable resonator wet output.", 1);
        add (ids::resonanceInterval2, "Voice 2 interval", "Semitone offset from root, -12 to +24. Retuning crossfades fixed banks over 50 ms.", 1);
        add (ids::resonanceWeight2, "Voice 2 weight", "Relative voice weight. Weights normalize together; all four at zero explicitly disable resonator wet output.", 1);
        add (ids::resonanceInterval3, "Voice 3 interval", "Semitone offset from root, -12 to +24. Retuning crossfades fixed banks over 50 ms.", 1);
        add (ids::resonanceWeight3, "Voice 3 weight", "Relative voice weight. Weights normalize together; all four at zero explicitly disable resonator wet output.", 1);
        add (ids::resonanceInterval4, "Voice 4 interval", "Semitone offset from root, -12 to +24. Retuning crossfades fixed banks over 50 ms.", 1);
        add (ids::resonanceWeight4, "Voice 4 weight", "Relative voice weight. Weights normalize together; all four at zero explicitly disable resonator wet output.", 1);
        add (ids::resonanceSpectralEnabled, "Spectral engine", "Requested prepared configuration. Apply while stopped. Adds 8192 samples at 48 kHz; mix zero and host bypass retain this delay.", 2);
        add (ids::resonanceFreeze, "Freeze", "Hold magnitudes and continue phase evolution from the next eligible full analysis frame. This is spectral freeze, separate from Audio Hold.", 2);
        add (ids::resonanceSmearMs, "Smear", "Magnitude smoothing time constant, 0 to 2000 ms. Zero disables smear; it is not an amplitude decay guarantee.", 2);
        add (ids::resonanceSpectralMix, "Spectral mix", "Blend the pre-spectral processed signal with spectral output. While warming, the spectral contribution is explicitly silent.", 2);
        apply.setComponentID ("applySpectral");
        apply.setTitle ("Apply requested Mode and Spectral configuration while stopped");
        apply.setTooltip ("Stop the host, then apply the requested Live / Studio mode and Spectral engine together, or replace a faulted prepared stream.");
        apply.onClick = [this] { owner.applyRequestedMode(); refresh(); };
        addAndMakeVisible (apply);
        status.setComponentID ("spectralStatus");
        status.setTitle ("Spectral readiness and latency");
        status.setFont (ui::uiFont (12.0f));
        status.setColour (juce::Label::textColourId, ui::palette::inkMuted);
        status.setJustificationType (juce::Justification::topLeft);
        status.setBorderSize ({});
        addAndMakeVisible (status);
        tuningStatus.setComponentID ("resonatorStatus");
        tuningStatus.setTitle ("Resonator validity and effect order");
        tuningStatus.setFont (ui::uiFont (12.0f));
        tuningStatus.setColour (juce::Label::textColourId, ui::palette::inkMuted);
        tuningStatus.setJustificationType (juce::Justification::topLeft);
        tuningStatus.setBorderSize ({});
        addAndMakeVisible (tuningStatus);
        help.setComponentID ("resonanceHelp");
        help.setTitle ("Resonance signal path instructions");
        help.setText ("Crusher / Comb precede this rack. Spectral follows all three orders. Hold and Capture follow Spectral.", juce::dontSendNotification);
        help.setFont (ui::uiFont (12.0f));
        help.setColour (juce::Label::textColourId, ui::palette::inkMuted);
        help.setBorderSize ({});
        addAndMakeVisible (help);
    }
    void refresh()
    {
        const auto& display = owner.getDisplayState();
        juce::String tuning;
        switch (display.resonatorState.load())
        {
            case livecut::engine::Resonator::State::ready:
                tuning = display.resonatorRetuning.load() ? "Retuning: 50 ms crossfade" : "Resonator tuning ready";
                if (display.resonatorPending.load()) tuning += "; latest tuning queued";
                break;
            case livecut::engine::Resonator::State::invalidWeights: tuning = "All weights zero: resonator wet is silent"; break;
            case livecut::engine::Resonator::State::invalidParameters: tuning = "Invalid tuning: resonator wet is silent"; break;
            case livecut::engine::Resonator::State::unsupportedRate: tuning = "Unsupported resonator sample rate"; break;
        }
        if (display.effectRouteTransition.load()) tuning += "\nEffect order crossfading";
        else if (display.effectRoute.load() != display.requestedEffectRoute.load()) tuning += "\nEffect order pending next bar";
        tuningStatus.setText (tuning, juce::dontSendNotification);
        const auto state = owner.modeState();
        const auto& parameters = owner.getParameterState();
        const auto requestedSpectral = parameters.getRawParameterValue (ids::resonanceSpectralEnabled)->load() > .5f;
        const auto requestedMode = requestedModeOf (parameters);
        const auto fault = display.spectralFault.load() != livecut::spectral::Fault::none;
        juce::String summary = juce::String ("Requested: ") + modeName (requestedMode) + (requestedSpectral ? " / Spectral on" : " / Spectral off");
        if (state == LivecutAudioProcessor::ModeState::unprepared)
            summary += "\nActive: not prepared";
        else if (state == LivecutAudioProcessor::ModeState::pendingHostChange)
            summary += "\nActive: host configuration changing";
        else if (state == LivecutAudioProcessor::ModeState::error)
            summary += "\nActive: configuration error";
        else
            summary += juce::String ("\nActive: ") + modeName (owner.activeMode()) + (owner.activeSpectral() ? " / Spectral on" : " / Spectral off");
        const auto configurationStatus = !fault && (state != LivecutAudioProcessor::ModeState::active || requestedMode != owner.activeMode())
            ? owner.modeStatusText() : owner.spectralStatusText();
        status.setText (summary + "\n" + configurationStatus, juce::dontSendNotification);
        const auto canApply = state != LivecutAudioProcessor::ModeState::unprepared
            && state != LivecutAudioProcessor::ModeState::pendingPlaying
            && state != LivecutAudioProcessor::ModeState::pendingHostChange
            && (requestedMode != owner.activeMode() || requestedSpectral != owner.activeSpectral()
                || state != LivecutAudioProcessor::ModeState::active || fault);
        apply.setEnabled (canApply);
        apply.setAlpha (canApply ? 1.0f : .5f);
    }
    void resized() override
    {
        const auto width = (getWidth() - 2 * gap) / 3;
        std::array<int, 3> tops { 38, 38, 38 };
        for (auto& row : rows)
        {
            const auto col = static_cast<std::size_t> (row.column);
            row.control->setBounds (row.column * (width + gap) + sectionPadding, tops[col], width - 2 * sectionPadding, row.height);
            tops[col] += row.height + 3;
        }
        tuningStatus.setBounds (sectionPadding, 263, width - 2 * sectionPadding, 80);
        apply.setBounds (2 * (width + gap) + sectionPadding, 185, width - 2 * sectionPadding, 28);
        status.setBounds (2 * (width + gap) + sectionPadding, 221, width - 2 * sectionPadding, 129);
        help.setBounds (sectionPadding, getHeight() - 26, getWidth() - 2 * sectionPadding, 22);
    }
    void paint (juce::Graphics& g) override
    {
        const auto width = (getWidth() - 2 * gap) / 3;
        const std::array titles { "Tuned resonator", "Four voices", "Spectral Freeze / Smear" };
        for (int col = 0; col < 3; ++col)
        {
            g.setColour (ui::palette::well);
            g.fillRoundedRectangle (juce::Rectangle<int> (col * (width + gap), 0, width, getHeight() - 30).toFloat(), 4.0f);
            g.setColour (ui::palette::ink);
            g.setFont (ui::uiFont (14.0f, true));
            g.drawText (titles[static_cast<std::size_t> (col)], juce::Rectangle<int> { col * (width + gap) + sectionPadding, 8, width - 2 * sectionPadding, 22 }, juce::Justification::centredLeft);
        }
    }
private:
    struct Row { std::unique_ptr<juce::Component> control; int column, height; };
    LivecutAudioProcessor& owner;
    std::vector<Row> rows;
    juce::TextButton apply { "Apply while stopped" };
    juce::Label status, help, tuningStatus;
};
// A fixed three-column reader/FX page; it does not own audio or a second AD clock.
class FracturePage final : public juce::Component
{
public:
    explicit FracturePage (juce::AudioProcessorValueTreeState& state)
        : familyAttachment (*state.getParameter (ids::fractureProcedure), [this] (float value)
          {
              family = static_cast<livecut::engine::FractureProcedure> (juce::jlimit (0, 2, juce::roundToInt (value)));
              updateEnabled();
              if (onFamilyChange)
                  onFamilyChange();
          })
    {
        const auto add = [&] (const char* id, const char* caption, const char* help, int column)
        {
            auto* parameter = state.getParameter (id);
            jassert (parameter != nullptr);
            std::unique_ptr<juce::Component> control;
            const auto isChoice = dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr;
            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (parameter))
                control = std::make_unique<ui::ChoiceRow> (*choice, column < 0 ? "Family" : "");
            else if (parameter->isBoolean())
                control = std::make_unique<ui::PowerSwitch> (*parameter, caption);
            else
                control = std::make_unique<ui::SliderRow> (*parameter, caption);
            control->setTitle (parameter->getName (80));
            for (auto* child : control->getChildren())
            {
                child->setTitle (parameter->getName (80));
                if (auto* tooltip = dynamic_cast<juce::SettableTooltipClient*> (child))
                    tooltip->setTooltip (help);
            }
            addAndMakeVisible (*control);
            rows.push_back ({ std::move (control), column, parameter->isBoolean() ? 26 : isChoice ? 28 : 35 });
        };
        add (ids::fractureProcedure, "Family", "Classic retains the original four procedures. GrainCut and TapeCut replace the wet source reader. Family changes apply at the next bar.", -1);
        add (ids::fractureActivity, "Activity", "Grain outer rhythm: 0%, 50%, 100% select 1, 2, 4 cuts per beat. Intermediate values choose adjacent densities deterministically.", 0);
        add (ids::fractureGrainSizeMs, "Size", "Grain lifetime, 10 to 160 ms. Complete source history is required; otherwise wet audio waits explicitly.", 0);
        add (ids::fractureGrainDensity, "Density", "Overlap density from 0.5 to 4. Low density intentionally leaves gaps. Fixed eight-voice storage.", 0);
        add (ids::fractureGrainPositionSpreadMs, "Position spread", "Source scatter within a fixed complete pool, 0 to 250 ms. Missing source is never replaced with Classic audio.", 0);
        add (ids::fractureGrainPitchSpreadCents, "Pitch spread", "Seeded grain pitch variation, 0 to 1200 cents around common Pitch. Bounded antialiasing resampling reduces aliasing; it is not alias-free.", 0);
        add (ids::fractureGrainPanSpread, "Pan spread", "Seeded per-grain pan variation around common Pan, clamped to the stereo range.", 0);
        add (ids::fractureTapeStartSpeed, "Start speed", "Tape speed from 0 to 1x. Speed also changes pitch and replaces the common Pitch controls. At 0x the reader is silent.", 1);
        add (ids::fractureTapeEndSpeed, "End speed", "Target tape speed from 0 to 1x. Both 0x endpoints produce silence; 1x to 1x retains unit-rate playback.", 1);
        add (ids::fractureTapeCurve, "Tape curve", "Linear or smooth speed trajectory. Normal forward playback keeps source position and speed continuous through tempo changes.", 1);
        add (ids::fractureTapeCycle, "Tape cycle", "Outer tape gesture: half, one, two or four quarter-note beats, shortened at bar/phrase end.", 1);
        add (ids::fractureTapeAlternate, "Alternate sweep", "Swap start/end speed on alternate outer cuts. This does not reverse source audio.", 1);
        add (ids::fractureFrequencyEnabled, "Ring / shift", "Enable the post-Impact frequency effect for any reader family. No fixed buffered latency; frequency shift has nonlinear phase and is not dry-phase-aligned.", 2);
        add (ids::fractureFrequencyMode, "Frequency mode", "Ring retains both sidebands and uses absolute Hz. Frequency Shift selects a signed sideband. Shifted content can fold at Nyquist; DC is outside the quadrature guarantee.", 2);
        add (ids::fractureFrequencyChance, "Chance", "Independent deterministic effect probability once per admitted outer cut, not per internal grain.", 2);
        add (ids::fractureFrequencyHz, "Rate", "Signed -2000 to +2000 Hz; Ring uses absolute rate, Frequency Shift preserves sign. Settled combined 0 Hz is dry.", 2);
        add (ids::fractureFrequencyEnvHz, "Env depth", "Hz offset driven by the existing Impact cut attack/decay clock. Edit AD timing on the Impact page; internal grains do not retrigger it.", 2);
        add (ids::fractureFrequencyRepeatHz, "Repeat", "Hz offset from first to last outer cut in the block. A single cut uses zero progression.", 2);
        add (ids::fractureFrequencyMix, "Frequency mix", "Frequency amount. Off or 0% is exact dry after a 2 ms transition. IIR phase coloration can affect mixed audio.", 2);
        const auto label = [&] (juce::Label& target, const char* id, const char* title)
        {
            target.setComponentID (id);
            target.setTitle (title);
            target.setFont (ui::uiFont (12.0f));
            target.setColour (juce::Label::textColourId, ui::palette::inkMuted);
            target.setBorderSize ({});
            target.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (target);
        };
        label (status, "fractureStatus", "Active reader and source status");
        label (help, "fractureHelp", "Reader timing and shared envelope instructions");
        familyAttachment.sendInitialUpdate();
    }

    std::function<void()> onFamilyChange;
    livecut::engine::FractureProcedure requestedFamily() const { return family; }

    void refresh (const livecut::DisplayState& display)
    {
        const auto main = ui::PhraseRuler::Snapshot::of (display);
        if (main.status != livecut::DisplayState::Status::cutting)
        {
            status.setText (ui::PhraseRuler::describe (main), juce::dontSendNotification);
            return;
        }
        const auto active = display.fractureFamily.load();
        juce::String text;
        switch (display.fractureState.load())
        {
            case livecut::engine::FractureState::off: text = "Classic reader; Ring / Shift available"; break;
            case livecut::engine::FractureState::waiting: text = "Waiting for Grain history; wet silent"; break;
            case livecut::engine::FractureState::playing:
                text = active == livecut::engine::FractureProcedure::grainCut
                    ? "GrainCut playing | voices " + juce::String (display.fractureVoices.load())
                    : "TapeCut playing | speed " + juce::String (display.fractureTapeSpeed.load(), 2) + "x";
                break;
            case livecut::engine::FractureState::ended: text = "Cut ended; waiting for next cut"; break;
            case livecut::engine::FractureState::sourceLost: text = "Source lost; wet silent"; break;
            case livecut::engine::FractureState::transformBudgetExhausted: text = "Transform budget exhausted; wet silent"; break;
            case livecut::engine::FractureState::capacityError: text = "Voice capacity error; wet silent"; break;
            case livecut::engine::FractureState::contextUnavailable: text = "Source context unavailable; wet silent"; break;
        }
        if (active != family)
            text = "Pending next bar: " + familyName (family) + " | " + text;
        status.setText (text, juce::dontSendNotification);
    }

    void resized() override
    {
        const auto width = (getWidth() - 2 * gap) / 3;
        std::array<int, 3> tops { 120, 120, 94 };
        for (auto& row : rows)
        {
            if (row.column < 0)
            {
                row.control->setBounds (sectionPadding, 8, 275, 28);
                continue;
            }
            const auto col = static_cast<std::size_t> (row.column);
            row.control->setBounds (row.column * (width + gap) + sectionPadding, tops[col], width - 2 * sectionPadding, row.height);
            tops[col] += row.height + 3;
        }
        status.setBounds (303, 6, getWidth() - 315, 32);
        help.setBounds (sectionPadding, 43, getWidth() - 2 * sectionPadding, 32);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (ui::palette::well);
        g.fillRoundedRectangle (getLocalBounds().removeFromTop (81).toFloat(), 4.0f);
        const auto width = (getWidth() - 2 * gap) / 3;
        for (int col = 0; col < 3; ++col)
            g.fillRoundedRectangle (juce::Rectangle<int> (col * (width + gap), 84, width, getHeight() - 84).toFloat(), 4.0f);
        g.setColour (ui::palette::ink);
        g.setFont (ui::uiFont (14.0f, true));
        g.drawText ("GrainCut", juce::Rectangle<int> (sectionPadding, 94, width - 24, 22), juce::Justification::centredLeft);
        g.drawText ("TapeCut", juce::Rectangle<int> (width + gap + sectionPadding, 94, width - 24, 22), juce::Justification::centredLeft);
    }

private:
    static juce::String familyName (livecut::engine::FractureProcedure value)
    {
        switch (value)
        {
            case livecut::engine::FractureProcedure::classic: return "Classic";
            case livecut::engine::FractureProcedure::grainCut: return "GrainCut";
            case livecut::engine::FractureProcedure::tapeCut: return "TapeCut";
        }
        jassertfalse;
        return {};
    }

    void updateEnabled()
    {
        for (auto& row : rows)
        {
            if (row.column == 0)
                row.control->setEnabled (family == livecut::engine::FractureProcedure::grainCut);
            if (row.column == 1)
                row.control->setEnabled (family == livecut::engine::FractureProcedure::tapeCut);
        }
        help.setText (family == livecut::engine::FractureProcedure::tapeCut
            ? "Tape speed replaces Pattern pitch. Family: next bar; other settings: next block. Frequency Env uses Impact AD."
            : "Family: next bar; other settings: next block. Frequency Env uses the cut AD timing on the Impact page.", juce::dontSendNotification);
    }

    struct Row { std::unique_ptr<juce::Component> control; int column, height; };
    std::vector<Row> rows;
    juce::Label status, help;
    livecut::engine::FractureProcedure family = livecut::engine::FractureProcedure::classic;
    juce::ParameterAttachment familyAttachment;
};
} // namespace

class LivecutAudioProcessorEditor::Panel final : public juce::Component
{
public:
    explicit Panel (juce::AudioProcessorValueTreeState& state, livecut::CaptureService& captureService,
                    LivecutAudioProcessor& processor)
        : procedureSwitch (choice (state, ids::procedure)),
          subdivision (choice (state, ids::subdivision), "Subdivision"),
          phraseBars (parameter (state, ids::minPhraseBars), parameter (state, ids::maxPhraseBars), "Bars"),
          seed (parameter (state, ids::seed), "Seed"),
          fade (parameter (state, ids::fade), "Fade"),
          duty (parameter (state, ids::duty), "Duty"),
          fillDuty (parameter (state, ids::fillDuty), "Fill duty"),
          amp (parameter (state, ids::minAmp), parameter (state, ids::maxAmp), "Amp"),
          pan (parameter (state, ids::minPan), parameter (state, ids::maxPan), "Pan"),
          pitch (parameter (state, ids::minPitch), parameter (state, ids::maxPitch), "Pitch"),
          repeats (parameter (state, ids::minRepeats), parameter (state, ids::maxRepeats), "Repeats"),
          stutter (parameter (state, ids::stutterChance), "Stutter"),
          stutterArea (parameter (state, ids::stutterArea), "Stutter area"),
          straight (parameter (state, ids::straightChance), "Straight"),
          regular (parameter (state, ids::regularChance), "Regular"),
          ritard (parameter (state, ids::ritardChance), "Ritard"),
          accel (parameter (state, ids::accel), "Accel"),
          activity (parameter (state, ids::activity), "Activity"),
          bounce (parameter (state, ids::bounceChance), "Bounce"),
          ratchet (parameter (state, ids::ratchetChance), "Ratchet"),
          climb (parameter (state, ids::climb), "Climb"),
          halftime (parameter (state, ids::halftimeChance), "Halftime"),
          crusher (parameter (state, ids::crusher), "Crusher"),
          bits (parameter (state, ids::minBits), parameter (state, ids::maxBits), "Bits"),
          rate (parameter (state, ids::minRate), parameter (state, ids::maxRate), "Rate"),
          comb (parameter (state, ids::comb), "Comb"),
          combType (choice (state, ids::combType), {}),
          combFeedback (parameter (state, ids::combFeedback), "Feedback"),
          delay (parameter (state, ids::minDelay), parameter (state, ids::maxDelay), "Delay"),
          wet (parameter (state, ids::wet), "Wet"),
          trim (parameter (state, ids::outputTrim), "Trim"),
          swing (parameter (state, ids::swing), "Swing"),
          accent (parameter (state, ids::accent), "Accent"),
          hold (parameter (state, ids::hold), "Audio Hold"),
          processingMode (choice (state, ids::mode), "Requested"),
          attackAlign (parameter (state, ids::attackAlign), "Attack align"),
          attackThreshold (parameter (state, ids::attackThreshold), "Attack threshold"),
          reverseChance (parameter (state, ids::reverseChance), "Reverse chance"),
          performanceControls (processor),
          impactControls (state),
          fractureControls (state),
          resonanceControls (processor),
          captures (captureService),
          hostProcessor (processor)
    {
        for (auto* component : std::initializer_list<juce::Component*> {
                 &procedureSwitch, &ruler, &subdivision, &phraseBars, &seed, &fade, &duty, &fillDuty,
                 &amp, &pan, &pitch, &crusher, &bits, &rate, &comb, &combType, &combFeedback, &delay,
                 &wet, &trim, &captureButton, &saveButton, &captureStatus, &swing, &accent, &hold, &holdStatus,
                 &performanceTab, &patternTab, &recutTab, &impactTab, &fractureTab, &resonanceTab })
            addAndMakeVisible (component);

        for (auto* component : std::initializer_list<juce::Component*> {
                 &processingMode, &attackAlign, &attackThreshold, &reverseChance,
                 &applyMode, &modeSummary, &modeStatus, &modeHelp, &transformStatus })
            addChildComponent (component);

        addChildComponent (performanceControls);
        performanceTab.setComponentID ("performanceTab");
        performanceTab.setTitle ("Performance XY controls");
        performanceTab.setTooltip ("Play Rhythm and Texture with two XY pads, grab an input bar, and keep or return to a pattern");
        performanceTab.onClick = [this] { showPage (Page::performance); };
        addChildComponent (impactControls);
        addChildComponent (fractureControls);
        addChildComponent (resonanceControls);
        resonanceTab.setComponentID ("resonanceTab");
        resonanceTab.setTitle ("Resonance and spectral controls");
        resonanceTab.setTooltip ("Show tuned resonator, effect order and Spectral Freeze / Smear");
        resonanceTab.onClick = [this] { showPage (Page::resonance); };
        fractureTab.setComponentID ("fractureTab");
        fractureTab.setTitle ("Fracture reader and frequency controls");
        fractureTab.setTooltip ("Show Classic / GrainCut / TapeCut families and Ring / Frequency Shift");
        fractureTab.onClick = [this] { showPage (Page::fracture); };
        fractureControls.onFamilyChange = [this] { showProcedure (activeProcedure); };
        impactTab.setComponentID ("impactTab");
        impactTab.setTitle ("Impact effects controls");
        impactTab.setTooltip ("Show Slice Filter, Drive / Wavefold and Transient controls");
        impactTab.onClick = [this] { showPage (Page::impact); };

        patternTab.setComponentID ("patternTab");
        recutTab.setComponentID ("recutTab");
        patternTab.setTitle ("Pattern controls");
        recutTab.setTitle ("Recut and Studio controls");
        patternTab.setTooltip ("Show phrase, cut and procedure controls");
        recutTab.setTooltip ("Show processing mode, attack alignment and reverse controls");
        patternTab.onClick = [this] { showPage (Page::pattern); };
        recutTab.onClick = [this] { showPage (Page::recut); };

        processingMode.setTitle ("Requested processing mode");
        for (auto* child : processingMode.getChildren())
            if (auto* box = dynamic_cast<juce::ComboBox*> (child))
            {
                box->setTitle ("Requested processing mode");
                box->setTooltip ("Live is immediate. Studio adds 500 ms of monitoring delay. Apply changes while stopped.");
            }
        attackAlign.setTitle ("Attack align");
        for (auto* child : attackAlign.getChildren())
            if (auto* toggle = dynamic_cast<juce::ToggleButton*> (child))
                toggle->setTooltip ("Start eligible slices near detected attacks, linked across stereo. Without a qualifying complete aligned slice, both Attack Align and Reverse are ineligible.");
        attackThreshold.setTitle ("Attack threshold");
        reverseChance.setTitle ("Reverse chance");
        attackThreshold.setTooltip ("Minimum detected energy rise, 3 to 24 dB. Higher values select stronger attacks.");
        const juce::String reverseHelp = "Probability of reversing an eligible slice; 100% means every eligible slice. With Attack Align on, reverse requires a qualifying complete aligned slice. With Attack Align off, reverse can use a complete nominal slice.";
        reverseChance.setTooltip (reverseHelp);
        for (auto* child : reverseChance.getChildren())
            if (auto* slider = dynamic_cast<juce::Slider*> (child))
                slider->setTooltip (reverseHelp);
        applyMode.setComponentID ("applyMode");
        applyMode.setTitle ("Apply requested Mode and Spectral configuration while stopped");
        applyMode.setTooltip ("Stop playback, then apply the requested Live / Studio mode and Spectral engine together, or replace a faulted prepared stream.");
        applyMode.onClick = [this] { hostProcessor.applyRequestedMode(); refreshRecut(); };
        const auto statusLabel = [] (juce::Label& label, const char* id, const char* title)
        {
            label.setComponentID (id);
            label.setTitle (title);
            label.setFont (ui::uiFont (13.0f));
            label.setColour (juce::Label::textColourId, ui::palette::inkMuted);
            label.setJustificationType (juce::Justification::topLeft);
            label.setBorderSize ({});
        };
        statusLabel (modeSummary, "modeSummary", "Active processing mode and latency");
        statusLabel (modeStatus, "modeStatus", "Processing mode status");
        statusLabel (modeHelp, "modeHelp", "Processing mode instructions");
        statusLabel (transformStatus, "transformStatus", "Attack and reverse status");
        modeHelp.setText ("Studio adds 500 ms of delay so more slices can use attack alignment and reverse. Stop your host, choose a mode, then Apply.", juce::dontSendNotification);

        for (auto* component : std::initializer_list<juce::Component*> { &repeats, &stutter, &stutterArea })
            cutProc11Controls.addAndMakeVisible (component);

        for (auto* component : std::initializer_list<juce::Component*> { &straight, &regular, &ritard, &accel })
            warpCutControls.addAndMakeVisible (component);

        sqPusherControls.addAndMakeVisible (activity);

        for (auto* component : std::initializer_list<juce::Component*> { &bounce, &ratchet, &climb, &halftime })
            bounceControls.addAndMakeVisible (component);

        for (auto* controls : { &cutProc11Controls, &warpCutControls, &sqPusherControls, &bounceControls })
            addChildComponent (controls);

        holdStatus.setComponentID ("holdStatus");
        holdStatus.setFont (ui::uiFont (12.0f));
        holdStatus.setColour (juce::Label::textColourId, ui::palette::inkMuted);
        swing.setTooltip ("50% is straight; 75% delays each paired eighth. Changes apply at the next bar.");
        accent.setTooltip ("Deterministic amplitude variation, attenuation only, up to 25%");
        captureButton.setComponentID ("capture");
        saveButton.setComponentID ("saveCapture");
        captureStatus.setComponentID ("captureStatus");
        wet.setTooltip ("Linear dry/wet mix, smoothed over 20 ms");
        trim.setTooltip ("Output gain from -24 to +24 dB, smoothed over 20 ms");
        captureButton.setTooltip ("Keep the last fully completed phrase, including Wet and Trim");
        saveButton.setTooltip ("Save the captured phrase as stereo 32-bit float WAV");
        captureStatus.setColour (juce::Label::textColourId, ui::palette::inkMuted);
        captureStatus.setFont (ui::uiFont (12.0f));
        captureButton.onClick = [this] { captures.requestCapture(); refreshCapture(); };
        saveButton.onClick = [this] { chooseSaveFile(); };
        refreshCapture();

        procedureSwitch.onChange = [this] (int index) { showProcedure (index); };

        // A switched-off effect's controls stay adjustable but read as inactive.
        const auto dim = [] (bool on, std::initializer_list<juce::Component*> controls)
        {
            for (auto* control : controls)
                control->setAlpha (on ? 1.0f : 0.5f);
        };
        crusher.onChange = [this, dim] (bool on) { dim (on, { &bits, &rate }); };
        comb.onChange = [this, dim] (bool on) { dim (on, { &combType, &combFeedback, &delay }); };
        dim (crusher.isOn(), { &bits, &rate });
        dim (comb.isOn(), { &combType, &combFeedback, &delay });
        showProcedure (static_cast<int> (std::lround (state.getRawParameterValue (ids::procedure)->load())));
        showPage (Page::performance);
        setSize (baseWidth, baseHeight);
    }

    void showProcedure (int index)
    {
        activeProcedure = juce::jlimit (0, static_cast<int> (procedures.size()) - 1, index);
        cutProc11Controls.setVisible (selectedPage == Page::pattern && activeProcedure == 0);
        warpCutControls.setVisible (selectedPage == Page::pattern && activeProcedure == 1);
        sqPusherControls.setVisible (selectedPage == Page::pattern && activeProcedure == 2);
        bounceControls.setVisible (selectedPage == Page::pattern && activeProcedure == 3);
        const auto family = fractureControls.requestedFamily();
        const auto classic = family == livecut::engine::FractureProcedure::classic;
        const auto tape = family == livecut::engine::FractureProcedure::tapeCut;
        procedureSwitch.setEnabled (classic);
        procedureSwitch.setAlpha (classic ? 1.0f : 0.5f);
        for (auto* controls : { &cutProc11Controls, &warpCutControls, &sqPusherControls, &bounceControls })
            controls->setEnabled (classic);
        pitch.setEnabled (! tape);
        pitch.setAlpha (classic && activeProcedure == static_cast<int> (livecut::engine::Procedure::bounce) ? 0.5f : 1.0f);
        pitch.setTitle (tape ? "Pitch disabled: TapeCut uses speed" : "Pitch");
        pitch.getSlider().setTitle (pitch.getTitle());
        pitch.getSlider().setTooltip (tape ? "TapeCut uses Start / End speed on the Fracture page instead of common Pitch." : "Common cut pitch; TapeCut uses its speed controls instead.");
        for (auto* child : pitch.getChildren())
            if (auto* label = dynamic_cast<juce::Label*> (child); label != nullptr && label->getText().startsWith ("Pitch"))
                label->setText (tape ? "Pitch: use Tape" : "Pitch", juce::dontSendNotification);
        repaint();
    }

    enum class Page { performance, pattern, recut, impact, fracture, resonance };

    void showPage (Page page)
    {
        performanceControls.cancelInteractions();
        selectedPage = page;
        performanceControls.setVisible (page == Page::performance);
        performanceTab.setToggleState (page == Page::performance, juce::dontSendNotification);
        procedureSwitch.setVisible (page != Page::performance);
        for (auto* component : std::initializer_list<juce::Component*> {
                 &subdivision, &phraseBars, &seed, &fade, &duty, &fillDuty, &amp, &pan, &pitch })
            component->setVisible (page == Page::pattern);
        for (auto* component : std::initializer_list<juce::Component*> {
                 &processingMode, &attackAlign, &attackThreshold, &reverseChance,
                 &applyMode, &modeSummary, &modeStatus, &modeHelp, &transformStatus })
            component->setVisible (page == Page::recut);
        for (auto* component : std::initializer_list<juce::Component*> {
                 &crusher, &bits, &rate, &comb, &combType, &combFeedback, &delay })
            component->setVisible (page == Page::pattern || page == Page::recut);
        impactControls.setVisible (page == Page::impact);
        fractureControls.setVisible (page == Page::fracture);
        resonanceControls.setVisible (page == Page::resonance);
        patternTab.setToggleState (page == Page::pattern, juce::dontSendNotification);
        recutTab.setToggleState (page == Page::recut, juce::dontSendNotification);
        impactTab.setToggleState (page == Page::impact, juce::dontSendNotification);
        fractureTab.setToggleState (page == Page::fracture, juce::dontSendNotification);
        resonanceTab.setToggleState (page == Page::resonance, juce::dontSendNotification);
        showProcedure (activeProcedure);
    }

    void refreshRecut()
    {
        const auto modeState = hostProcessor.modeState();
        const auto active = hostProcessor.activeMode();
        const auto requested = requestedModeOf (hostProcessor.getParameterState());
        juce::String summary;
        if (modeState == LivecutAudioProcessor::ModeState::unprepared)
            summary = "Active: not prepared";
        else if (modeState == LivecutAudioProcessor::ModeState::error)
            summary = "Mode error; see status";
        else if (modeState == LivecutAudioProcessor::ModeState::pendingHostChange)
            summary = "Updating host latency...";
        else
        {
            summary = "Active: ";
            summary += modeName (active);
            const auto latency = hostProcessor.reportedLatencySamples();
            summary += " | " + juce::String (latency) + " samples";
        }
        modeSummary.setText (summary, juce::dontSendNotification);
        modeStatus.setText (hostProcessor.modeStatusText(), juce::dontSendNotification);
        transformStatus.setText (hostProcessor.transformStatusText(), juce::dontSendNotification);
        const auto canApply = modeState != LivecutAudioProcessor::ModeState::unprepared
                              && modeState != LivecutAudioProcessor::ModeState::pendingPlaying
                              && modeState != LivecutAudioProcessor::ModeState::pendingHostChange
                              && (requested != active || modeState != LivecutAudioProcessor::ModeState::active
                                  || (hostProcessor.getParameterState().getRawParameterValue (ids::resonanceSpectralEnabled)->load() > .5f) != hostProcessor.activeSpectral()
                                  || hostProcessor.getDisplayState().spectralFault.load() != livecut::spectral::Fault::none);
        applyMode.setEnabled (canApply);
        applyMode.setAlpha (canApply ? 1.0f : 0.5f);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (ui::palette::panel);

        g.setColour (ui::palette::ink);
        g.setFont (ui::uiFont (25.0f, true));
        g.drawText ("Livecut", juce::Rectangle<int> (margin, 7, 100, 31), juce::Justification::centredLeft);
        g.setFont (ui::uiFont (12.0f));
        g.drawText (JucePlugin_VersionString, juce::Rectangle<int> (margin, 36, 100, 18), juce::Justification::centredLeft);

        if (selectedPage == Page::pattern || selectedPage == Page::recut)
        for (const auto& section : { crusherSection, combSection })
        {
            g.setColour (ui::palette::well);
            g.fillRoundedRectangle (section.toFloat(), 4.0f);
        }

        const auto title = [&] (const juce::String& text, juce::Rectangle<int> section)
        {
            g.setColour (ui::palette::ink);
            g.setFont (ui::uiFont (16.0f, true));
            g.drawText (text, section.reduced (sectionPadding).removeFromTop (22), juce::Justification::centredLeft);
        };

        if (selectedPage == Page::recut)
        {
            for (const auto& section : { modeSection, transformSection })
            {
                g.setColour (ui::palette::well);
                g.fillRoundedRectangle (section.toFloat(), 4.0f);
            }
            title ("Processing mode", modeSection);
            title ("Attack & reverse", transformSection);
        }
        else if (selectedPage == Page::pattern)
        {
            for (const auto& section : { phraseSection, cutsSection, procedureSection })
            {
                g.setColour (ui::palette::well);
                g.fillRoundedRectangle (section.toFloat(), 4.0f);
            }
            title ("Phrase", phraseSection);
            title ("Cuts", cutsSection);
            const auto& info = procedures[static_cast<std::size_t> (activeProcedure)];
            const auto family = fractureControls.requestedFamily();
            const auto classic = family == livecut::engine::FractureProcedure::classic;
            title (classic ? info.name : family == livecut::engine::FractureProcedure::grainCut ? "GrainCut override" : "TapeCut override", procedureSection);
            g.setColour (ui::palette::inkMuted);
            g.setFont (ui::uiFont (13.0f));
            g.drawFittedText (classic ? info.description : "Classic procedure controls are inactive. Edit the reader on the Fracture page.",
                              procedureSection.reduced (sectionPadding).withTrimmedTop (24).removeFromTop (34), juce::Justification::topLeft, 2);
        }

        g.setFont (ui::uiFont (12.0f));
        g.drawText ("Ported from Livecut by Remy Muller. CutProc11, WarpCut and SQPusher from Nick Collins' BBCut.",
                    juce::Rectangle<int> (margin, baseHeight - 26, baseWidth - 2 * margin, 16),
                    juce::Justification::centredLeft);
    }

    void resized() override
    {
        procedureSwitch.setBounds (572, 20, 288, 30);
        performanceTab.setBounds (122, 20, 94, 30);
        patternTab.setBounds (220, 20, 62, 30);
        recutTab.setBounds (286, 20, 51, 30);
        impactTab.setBounds (341, 20, 55, 30);
        fractureTab.setBounds (400, 20, 73, 30);
        resonanceTab.setBounds (477, 20, 89, 30);
        performanceControls.setBounds (margin, 166, baseWidth - 2 * margin, 384);
        resonanceControls.setBounds (margin, 166, baseWidth - 2 * margin, 384);
        fractureControls.setBounds (margin, 166, baseWidth - 2 * margin, 384);
        impactControls.setBounds (margin, 166, baseWidth - 2 * margin, 384);
        ruler.setBounds (margin, 62, baseWidth - 2 * margin, 88);

        const auto middleTop = 166;
        const auto middleHeight = 276;
        phraseSection = { margin, middleTop, 262, middleHeight };
        cutsSection = { phraseSection.getRight() + gap, middleTop, 290, middleHeight };
        procedureSection = { cutsSection.getRight() + gap, middleTop, baseWidth - margin - cutsSection.getRight() - gap, middleHeight };
        modeSection = { margin, middleTop, 405, middleHeight };
        transformSection = { modeSection.getRight() + gap, middleTop, 425, middleHeight };
        processingMode.setBounds (32, 208, 221, choiceRowHeight);
        applyMode.setBounds (263, 208, 150, choiceRowHeight);
        modeSummary.setBounds (32, 250, 381, 24);
        modeStatus.setBounds (32, 284, 381, 68);
        modeHelp.setBounds (32, 370, 381, 56);
        attackAlign.setBounds (447, 208, 240, choiceRowHeight);
        attackThreshold.setBounds (447, 248, 401, sliderRowHeight);
        reverseChance.setBounds (447, 296, 401, sliderRowHeight);
        transformStatus.setBounds (447, 350, 401, 76);

        Column phrase { phraseSection.reduced (sectionPadding).withTrimmedTop (30) };
        phrase.place (subdivision, choiceRowHeight);
        phrase.place (phraseBars, sliderRowHeight);
        phrase.place (seed, sliderRowHeight);
        phrase.place (fade, sliderRowHeight);

        Column cuts { cutsSection.reduced (sectionPadding).withTrimmedTop (30) };

        for (auto* row : std::initializer_list<juce::Component*> { &duty, &fillDuty, &amp, &pan, &pitch })
            cuts.place (*row, sliderRowHeight);

        const auto procedureArea = procedureSection.reduced (sectionPadding).withTrimmedTop (64);

        for (auto* controls : { &cutProc11Controls, &warpCutControls, &sqPusherControls, &bounceControls })
        {
            controls->setBounds (procedureArea);
            Column column { controls->getLocalBounds() };

            for (auto* row : controls->getChildren())
                column.place (*row, sliderRowHeight);
        }

        const auto bottomTop = middleTop + middleHeight + gap;
        const auto bottomHeight = 98;
        const auto halfWidth = (baseWidth - 2 * margin - gap) / 2;
        crusherSection = { margin, bottomTop, halfWidth, bottomHeight };
        combSection = { crusherSection.getRight() + gap, bottomTop, halfWidth, bottomHeight };

        auto crusherArea = crusherSection.reduced (sectionPadding);
        crusher.setBounds (crusherArea.removeFromTop (24).withWidth (140));
        crusherArea.removeFromTop (8);
        const auto crusherColumn = (crusherArea.getWidth() - gap) / 2;
        bits.setBounds (crusherArea.removeFromLeft (crusherColumn).withHeight (sliderRowHeight));
        crusherArea.removeFromLeft (gap);
        rate.setBounds (crusherArea.withHeight (sliderRowHeight));

        auto combArea = combSection.reduced (sectionPadding);
        auto combHeader = combArea.removeFromTop (24);
        comb.setBounds (combHeader.removeFromLeft (110));
        combType.setBounds (combHeader.removeFromLeft (150));
        combArea.removeFromTop (8);
        const auto combColumn = (combArea.getWidth() - gap) / 2;
        combFeedback.setBounds (combArea.removeFromLeft (combColumn).withHeight (sliderRowHeight));
        combArea.removeFromLeft (gap);
        delay.setBounds (combArea.withHeight (sliderRowHeight));

        wet.setBounds (margin, 562, 240, sliderRowHeight);
        trim.setBounds (270, 562, 240, sliderRowHeight);
        captureButton.setBounds (536, 566, 156, 30);
        saveButton.setBounds (702, 566, 158, 30);
        captureStatus.setBounds (margin, 604, baseWidth - 2 * margin, 24);
        swing.setBounds (margin, 638, 240, sliderRowHeight);
        accent.setBounds (270, 638, 240, sliderRowHeight);
        hold.setBounds (536, 644, 156, 28);
        holdStatus.setBounds (margin, 678, baseWidth - 2 * margin, 22);
    }

    void refreshPerformance() { performanceControls.refresh(); }

    void refreshFracture() { fractureControls.refresh (hostProcessor.getDisplayState()); resonanceControls.refresh(); }

    void refreshCapture()
    {
        captureButton.setEnabled (! captures.isBusy() && captures.rolling().hasLatest());
        saveButton.setEnabled (! captures.isBusy() && captures.captured() != nullptr);
        captureButton.setAlpha (captureButton.isEnabled() ? 1.0f : 0.5f);
        saveButton.setAlpha (saveButton.isEnabled() ? 1.0f : 0.5f);
        captureStatus.setText (captures.statusText(), juce::dontSendNotification);
    }

    void showHoldStatus (const juce::String& text) { holdStatus.setText (text, juce::dontSendNotification); }

    void showPhrase (ui::PhraseRuler::Snapshot snapshot)
    {
        if (selectedPage != Page::performance && hostProcessor.activeMode() == livecut::ProcessingMode::perform)
            snapshot.performanceStatus = "Legacy controls inactive in Perform. Turn Perform off and Apply while stopped.";
        ruler.show (snapshot);
    }

    ui::ProcedureSwitch procedureSwitch;
    ui::PhraseRuler ruler;

private:
    void chooseSaveFile()
    {
        chooser = std::make_unique<juce::FileChooser> ("Save captured phrase", juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Livecut phrase.wav"), "*.wav");
        const juce::Component::SafePointer<Panel> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                               | juce::FileBrowserComponent::warnAboutOverwriting,
                             [safe] (const juce::FileChooser& dialog)
        {
            if (safe == nullptr)
                return;
            const auto target = dialog.getResult();
            if (target == juce::File())
                return;
            safe->captures.requestSave (target);
            safe->refreshCapture();
        });
    }

    static juce::RangedAudioParameter& parameter (juce::AudioProcessorValueTreeState& state, const char* id)
    {
        auto* found = state.getParameter (id);
        jassert (found != nullptr);
        return *found;
    }

    static juce::AudioParameterChoice& choice (juce::AudioProcessorValueTreeState& state, const char* id)
    {
        auto* found = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (id));
        jassert (found != nullptr);
        return *found;
    }

    ui::ChoiceRow subdivision;
    ui::RangeSlider phraseBars;
    ui::SliderRow seed, fade, duty, fillDuty;
    ui::RangeSlider amp, pan, pitch, repeats;
    ui::SliderRow stutter, stutterArea, straight, regular, ritard, accel, activity;
    ui::SliderRow bounce, ratchet, climb, halftime;
    ui::PowerSwitch crusher;
    ui::RangeSlider bits, rate;
    ui::PowerSwitch comb;
    ui::ChoiceRow combType;
    ui::SliderRow combFeedback;
    ui::RangeSlider delay;
    ui::SliderRow wet, trim, swing, accent;
    ui::PowerSwitch hold;
    ui::ChoiceRow processingMode;
    ui::PowerSwitch attackAlign;
    ui::SliderRow attackThreshold, reverseChance;
    juce::TextButton performanceTab { "Perform" }, patternTab { "Pattern" }, recutTab { "Recut" }, impactTab { "Impact" }, fractureTab { "Fracture" }, resonanceTab { "Resonance" }, applyMode { "Apply while stopped" };
    juce::Label modeSummary, modeStatus, modeHelp, transformStatus;
    juce::Label holdStatus;
    ui::PerformancePage performanceControls;
    ImpactPage impactControls;
    FracturePage fractureControls;
    ResonancePage resonanceControls;
    livecut::CaptureService& captures;
    LivecutAudioProcessor& hostProcessor;
    juce::TextButton captureButton { "Capture last phrase" }, saveButton { "Save WAV..." };
    juce::Label captureStatus;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Component cutProc11Controls, warpCutControls, sqPusherControls, bounceControls;
    juce::Rectangle<int> phraseSection, cutsSection, procedureSection, crusherSection, combSection;
    juce::Rectangle<int> modeSection, transformSection;
    int activeProcedure = 0;
    Page selectedPage = Page::performance;
};

LivecutAudioProcessorEditor::LivecutAudioProcessorEditor (LivecutAudioProcessor& p)
    : AudioProcessorEditor (p),
      livecutProcessor (p)
{
    setLookAndFeel (&lookAndFeel);
    panel = std::make_unique<Panel> (p.getParameterState(), p.getCaptureService(), p);
    addAndMakeVisible (*panel);

    setResizable (true, true);
    setResizeLimits (baseWidth * 3 / 4, baseHeight * 3 / 4, baseWidth * 2, baseHeight * 2);
    getConstrainer()->setFixedAspectRatio (static_cast<double> (baseWidth) / baseHeight);
    setSize (baseWidth, baseHeight);

    refreshDisplay();
    startTimerHz (30);
}

LivecutAudioProcessorEditor::~LivecutAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void LivecutAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (livecut::ui::palette::panel);
}

void LivecutAudioProcessorEditor::resized()
{
    panel->setBounds (0, 0, baseWidth, baseHeight);
    panel->setTransform (juce::AffineTransform::scale (static_cast<float> (getWidth()) / baseWidth));
}

void LivecutAudioProcessorEditor::refreshDisplay()
{
    panel->refreshPerformance();
    panel->refreshCapture();
    panel->refreshRecut();
    panel->refreshFracture();
    panel->showHoldStatus (livecutProcessor.holdStatusText());
    auto phrase = livecut::ui::PhraseRuler::Snapshot::of (livecutProcessor.getDisplayState());
    if (livecutProcessor.activeMode() == livecut::ProcessingMode::perform)
    {
        phrase.performanceStatus = livecutProcessor.performanceStatusText().upToFirstOccurrenceOf (";", false, false).upToFirstOccurrenceOf (".", false, false);
        phrase.performanceCapturing = livecutProcessor.getDisplayState().performanceState.load() == livecut::engine::PerformanceEngine::State::capturing;
    }
    panel->showPhrase (phrase);
}

juce::String LivecutAudioProcessorEditor::statusText() const
{
    return panel->ruler.statusText();
}

void LivecutAudioProcessorEditor::timerCallback()
{
    refreshDisplay();
}
