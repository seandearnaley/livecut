// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "DisplayState.h"
#include "CaptureService.h"
#include "OutputMixer.h"
#include "StreamResources.h"
#include "Parameters.h"
#include "engine/Cutter.h"
#include "engine/WetHold.h"

#include <juce_audio_processors/juce_audio_processors.h>

class LivecutAudioProcessor final : public juce::AudioProcessor, private livecut::engine::Cutter::Observer
{
public:
    LivecutAudioProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    void processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState& getParameterState() { return parameters; }
    const livecut::engine::Cutter& getCutter() const { jassert (stream != nullptr); return stream->cutter; }
    const livecut::DisplayState& getDisplayState() const { return display; }
    livecut::CaptureService& getCaptureService() { return capture; }
    juce::String holdStatusText() const;

private:
    void process (juce::AudioBuffer<float>&, bool bypass);
    void publishDisplay();
    void discontinuity (int) override { capture.rolling().interrupt(); wetHold.interrupt(); }
    void barStarted (int, std::int64_t) override { wetHold.barStarted(); }
    void phraseCompleted (int) override { capture.rolling().completePhrase(); wetHold.completePhrase(); }
    void phraseStarted (int, bool fullStart) override { capture.rolling().startPhrase (fullStart); wetHold.startPhrase (fullStart, stream->cutter.phraseBarCount() * processingBeatsPerBar); }
    void processOutput (int, float dryLeft, float dryRight, float& left, float& right) override;

    juce::AudioProcessorValueTreeState parameters;
    livecut::SettingsReader settingsReader;
    std::unique_ptr<livecut::StreamResources> stream;
    livecut::DisplayState display;
    livecut::OutputMixer mixer;
    livecut::CaptureService capture;
    livecut::engine::WetHold wetHold;
    double processingSampleRate = 48000.0, processingBeatsPerSample = 0.0, processingBeatsPerBar = 4.0;
    std::atomic<bool> resetHoldRequested { false };
    std::uint64_t transportEpochCounter = 1;
    bool processingRunning = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LivecutAudioProcessor)
};
