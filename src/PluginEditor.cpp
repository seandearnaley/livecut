// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginEditor.h"

#include "Parameters.h"
#include "ui/Controls.h"
#include "ui/PhraseRuler.h"
#include "ui/RangeSlider.h"

#include <array>
#include <cmath>

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

class LivecutAudioProcessorEditor::Panel final : public juce::Component
{
public:
    explicit Panel (juce::AudioProcessorValueTreeState& state, livecut::CaptureService& captureService)
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
          captures (captureService)
    {
        for (auto* component : std::initializer_list<juce::Component*> {
                 &procedureSwitch, &ruler, &subdivision, &phraseBars, &seed, &fade, &duty, &fillDuty,
                 &amp, &pan, &pitch, &crusher, &bits, &rate, &comb, &combType, &combFeedback, &delay,
                 &wet, &trim, &captureButton, &saveButton, &captureStatus })
            addAndMakeVisible (component);

        for (auto* component : std::initializer_list<juce::Component*> { &repeats, &stutter, &stutterArea })
            cutProc11Controls.addAndMakeVisible (component);

        for (auto* component : std::initializer_list<juce::Component*> { &straight, &regular, &ritard, &accel })
            warpCutControls.addAndMakeVisible (component);

        sqPusherControls.addAndMakeVisible (activity);

        for (auto* component : std::initializer_list<juce::Component*> { &bounce, &ratchet, &climb, &halftime })
            bounceControls.addAndMakeVisible (component);

        for (auto* controls : { &cutProc11Controls, &warpCutControls, &sqPusherControls, &bounceControls })
            addChildComponent (controls);

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
        setSize (baseWidth, baseHeight);
    }

    void showProcedure (int index)
    {
        activeProcedure = juce::jlimit (0, static_cast<int> (procedures.size()) - 1, index);
        cutProc11Controls.setVisible (activeProcedure == 0);
        warpCutControls.setVisible (activeProcedure == 1);
        sqPusherControls.setVisible (activeProcedure == 2);
        bounceControls.setVisible (activeProcedure == 3);
        pitch.setAlpha (activeProcedure == static_cast<int> (livecut::engine::Procedure::bounce) ? 0.5f : 1.0f);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (ui::palette::panel);

        g.setColour (ui::palette::ink);
        g.setFont (ui::uiFont (26.0f, true));
        g.drawText (juce::String ("Livecut ") + JucePlugin_VersionString, juce::Rectangle<int> (margin, 16, 200, 34), juce::Justification::centredLeft);

        for (const auto& section : { phraseSection, cutsSection, procedureSection, crusherSection, combSection })
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

        title ("Phrase", phraseSection);
        title ("Cuts", cutsSection);

        const auto& info = procedures[static_cast<std::size_t> (activeProcedure)];
        title (info.name, procedureSection);
        g.setColour (ui::palette::inkMuted);
        g.setFont (ui::uiFont (13.0f));
        g.drawFittedText (info.description, procedureSection.reduced (sectionPadding).withTrimmedTop (24).removeFromTop (34),
                          juce::Justification::topLeft, 2);

        g.setFont (ui::uiFont (12.0f));
        g.drawText ("Ported from Livecut by Remy Muller. CutProc11, WarpCut and SQPusher from Nick Collins' BBCut.",
                    juce::Rectangle<int> (margin, baseHeight - 26, baseWidth - 2 * margin, 16),
                    juce::Justification::centredLeft);
    }

    void resized() override
    {
        procedureSwitch.setBounds (baseWidth - margin - 456, 20, 456, 30);
        ruler.setBounds (margin, 62, baseWidth - 2 * margin, 88);

        const auto middleTop = 166;
        const auto middleHeight = 276;
        phraseSection = { margin, middleTop, 262, middleHeight };
        cutsSection = { phraseSection.getRight() + gap, middleTop, 290, middleHeight };
        procedureSection = { cutsSection.getRight() + gap, middleTop, baseWidth - margin - cutsSection.getRight() - gap, middleHeight };

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
    }

    void refreshCapture()
    {
        captureButton.setEnabled (! captures.isBusy() && captures.rolling().hasLatest());
        saveButton.setEnabled (! captures.isBusy() && captures.captured() != nullptr);
        captureButton.setAlpha (captureButton.isEnabled() ? 1.0f : 0.5f);
        saveButton.setAlpha (saveButton.isEnabled() ? 1.0f : 0.5f);
        captureStatus.setText (captures.statusText(), juce::dontSendNotification);
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
    ui::SliderRow wet, trim;
    livecut::CaptureService& captures;
    juce::TextButton captureButton { "Capture last phrase" }, saveButton { "Save WAV..." };
    juce::Label captureStatus;
    std::unique_ptr<juce::FileChooser> chooser;

    juce::Component cutProc11Controls, warpCutControls, sqPusherControls, bounceControls;
    juce::Rectangle<int> phraseSection, cutsSection, procedureSection, crusherSection, combSection;
    int activeProcedure = 0;
};

LivecutAudioProcessorEditor::LivecutAudioProcessorEditor (LivecutAudioProcessor& p)
    : AudioProcessorEditor (p),
      livecutProcessor (p)
{
    setLookAndFeel (&lookAndFeel);
    panel = std::make_unique<Panel> (p.getParameterState(), p.getCaptureService());
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
