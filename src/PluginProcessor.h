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

class LivecutAudioProcessor final : public juce::AudioProcessor, private livecut::engine::Cutter::Observer, private juce::Timer
{
public:
    LivecutAudioProcessor();
    ~LivecutAudioProcessor() override;
    enum class ModeState { unprepared, active, pendingObservation, pendingPlaying, pendingHostChange, error };
    void applyRequestedMode();
    livecut::ProcessingMode activeMode() const { return publishedMode.load (std::memory_order_acquire); }
    ModeState modeState() const { return publishedModeState.load (std::memory_order_acquire); }
    int reportedLatencySamples() const { return publishedLatency.load (std::memory_order_acquire); }
    juce::String modeStatusText() const;
    juce::String transformStatusText() const;

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
    void timerCallback() override;
    void tryApplyMode();
    void reconcileLatency();
    void observeTransport();
    void observeTransport (const juce::Optional<juce::AudioPlayHead::PositionInfo>&);
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
    std::atomic<livecut::ProcessingMode> publishedMode { livecut::ProcessingMode::live };
    std::atomic<ModeState> publishedModeState { ModeState::unprepared };
    enum class AudioObservation { unknown, stopped, playing };
    std::atomic<AudioObservation> observedTransport { AudioObservation::unknown };
    std::atomic<bool> applyPending { false };
    std::atomic<int> publishedLatency { 0 };
    std::uint64_t configurationGeneration = 0, publishedGeneration = 0; // callback-guard protected
    bool latencyUpdateInProgress = false, preparationFailed = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LivecutAudioProcessor)
};
