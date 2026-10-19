// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Parameters.h"
#include "engine/Cutter.h"

#include <juce_audio_processors/juce_audio_processors.h>

class LivecutAudioProcessor final : public juce::AudioProcessor
{
public:
    LivecutAudioProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
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
    const livecut::engine::Cutter& getCutter() const { return cutter; }

private:
    juce::AudioProcessorValueTreeState parameters;
    livecut::SettingsReader settingsReader;
    livecut::engine::Cutter cutter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LivecutAudioProcessor)
};
