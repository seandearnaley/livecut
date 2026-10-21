// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginEditor.h"

#include "Parameters.h"
#include "ui/Controls.h"
#include "ui/PhraseRuler.h"
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
          impactControls (state),
          captures (captureService),
          hostProcessor (processor)
    {
        for (auto* component : std::initializer_list<juce::Component*> {
                 &procedureSwitch, &ruler, &subdivision, &phraseBars, &seed, &fade, &duty, &fillDuty,
                 &amp, &pan, &pitch, &crusher, &bits, &rate, &comb, &combType, &combFeedback, &delay,
                 &wet, &trim, &captureButton, &saveButton, &captureStatus, &swing, &accent, &hold, &holdStatus,
                 &patternTab, &recutTab, &impactTab })
            addAndMakeVisible (component);

        for (auto* component : std::initializer_list<juce::Component*> {
                 &processingMode, &attackAlign, &attackThreshold, &reverseChance,
                 &applyMode, &modeSummary, &modeStatus, &modeHelp, &transformStatus })
            addChildComponent (component);

        addChildComponent (impactControls);
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
        applyMode.setTitle ("Apply requested processing mode while stopped");
        applyMode.setTooltip ("Stop playback in your host, then apply the requested Live or Studio mode");
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
        showPage (Page::pattern);
        setSize (baseWidth, baseHeight);
    }

    void showProcedure (int index)
    {
        activeProcedure = juce::jlimit (0, static_cast<int> (procedures.size()) - 1, index);
        cutProc11Controls.setVisible (selectedPage == Page::pattern && activeProcedure == 0);
        warpCutControls.setVisible (selectedPage == Page::pattern && activeProcedure == 1);
        sqPusherControls.setVisible (selectedPage == Page::pattern && activeProcedure == 2);
        bounceControls.setVisible (selectedPage == Page::pattern && activeProcedure == 3);
        pitch.setAlpha (activeProcedure == static_cast<int> (livecut::engine::Procedure::bounce) ? 0.5f : 1.0f);
        repaint();
    }

    enum class Page { pattern, recut, impact };

    void showPage (Page page)
    {
        selectedPage = page;
        for (auto* component : std::initializer_list<juce::Component*> {
                 &subdivision, &phraseBars, &seed, &fade, &duty, &fillDuty, &amp, &pan, &pitch })
            component->setVisible (page == Page::pattern);
        for (auto* component : std::initializer_list<juce::Component*> {
                 &processingMode, &attackAlign, &attackThreshold, &reverseChance,
                 &applyMode, &modeSummary, &modeStatus, &modeHelp, &transformStatus })
            component->setVisible (page == Page::recut);
        for (auto* component : std::initializer_list<juce::Component*> {
                 &crusher, &bits, &rate, &comb, &combType, &combFeedback, &delay })
            component->setVisible (page != Page::impact);
        impactControls.setVisible (page == Page::impact);
        patternTab.setToggleState (page == Page::pattern, juce::dontSendNotification);
        recutTab.setToggleState (page == Page::recut, juce::dontSendNotification);
        impactTab.setToggleState (page == Page::impact, juce::dontSendNotification);
        showProcedure (activeProcedure);
    }

    void refreshRecut()
    {
        const auto modeState = hostProcessor.modeState();
        const auto active = hostProcessor.activeMode();
        const auto requested = hostProcessor.getParameterState().getRawParameterValue (ids::mode)->load() > 0.5f
                                   ? livecut::ProcessingMode::studio : livecut::ProcessingMode::live;
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
            summary += active == livecut::ProcessingMode::studio ? "Studio" : "Live";
            const auto latency = hostProcessor.reportedLatencySamples();
            summary += " | " + juce::String (latency) + " samples";
        }
        modeSummary.setText (summary, juce::dontSendNotification);
        modeStatus.setText (hostProcessor.modeStatusText(), juce::dontSendNotification);
        transformStatus.setText (hostProcessor.transformStatusText(), juce::dontSendNotification);
        const auto canApply = modeState != LivecutAudioProcessor::ModeState::unprepared
                              && modeState != LivecutAudioProcessor::ModeState::pendingPlaying
                              && modeState != LivecutAudioProcessor::ModeState::pendingHostChange
                              && (requested != active || modeState != LivecutAudioProcessor::ModeState::active);
        applyMode.setEnabled (canApply);
        applyMode.setAlpha (canApply ? 1.0f : 0.5f);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (ui::palette::panel);

        g.setColour (ui::palette::ink);
        g.setFont (ui::uiFont (26.0f, true));
        g.drawText (juce::String ("Livecut ") + JucePlugin_VersionString, juce::Rectangle<int> (margin, 16, 180, 34), juce::Justification::centredLeft);

        if (selectedPage != Page::impact)
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
            title (info.name, procedureSection);
            g.setColour (ui::palette::inkMuted);
            g.setFont (ui::uiFont (13.0f));
            g.drawFittedText (info.description, procedureSection.reduced (sectionPadding).withTrimmedTop (24).removeFromTop (34),
                              juce::Justification::topLeft, 2);
        }

        g.setFont (ui::uiFont (12.0f));
        g.drawText ("Ported from Livecut by Remy Muller. CutProc11, WarpCut and SQPusher from Nick Collins' BBCut.",
                    juce::Rectangle<int> (margin, baseHeight - 26, baseWidth - 2 * margin, 16),
                    juce::Justification::centredLeft);
    }

    void resized() override
    {
        procedureSwitch.setBounds (baseWidth - margin - 456, 20, 456, 30);
        patternTab.setBounds (212, 20, 67, 30);
        recutTab.setBounds (283, 20, 55, 30);
        impactTab.setBounds (342, 20, 58, 30);
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

    void refreshCapture()
    {
        captureButton.setEnabled (! captures.isBusy() && captures.rolling().hasLatest());
        saveButton.setEnabled (! captures.isBusy() && captures.captured() != nullptr);
        captureButton.setAlpha (captureButton.isEnabled() ? 1.0f : 0.5f);
        saveButton.setAlpha (saveButton.isEnabled() ? 1.0f : 0.5f);
        captureStatus.setText (captures.statusText(), juce::dontSendNotification);
    }

    void showHoldStatus (const juce::String& text) { holdStatus.setText (text, juce::dontSendNotification); }

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
    juce::TextButton patternTab { "Pattern" }, recutTab { "Recut" }, impactTab { "Impact" }, applyMode { "Apply while stopped" };
    juce::Label modeSummary, modeStatus, modeHelp, transformStatus;
    juce::Label holdStatus;
    ImpactPage impactControls;
    livecut::CaptureService& captures;
    LivecutAudioProcessor& hostProcessor;
    juce::TextButton captureButton { "Capture last phrase" }, saveButton { "Save WAV..." };
    juce::Label captureStatus;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Component cutProc11Controls, warpCutControls, sqPusherControls, bounceControls;
    juce::Rectangle<int> phraseSection, cutsSection, procedureSection, crusherSection, combSection;
    juce::Rectangle<int> modeSection, transformSection;
    int activeProcedure = 0;
    Page selectedPage = Page::pattern;
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
    panel->refreshCapture();
    panel->refreshRecut();
    panel->showHoldStatus (livecutProcessor.holdStatusText());
    panel->ruler.show (livecut::ui::PhraseRuler::Snapshot::of (livecutProcessor.getDisplayState()));
}

juce::String LivecutAudioProcessorEditor::statusText() const
{
    return panel->ruler.statusText();
}

void LivecutAudioProcessorEditor::timerCallback()
{
    refreshDisplay();
}
