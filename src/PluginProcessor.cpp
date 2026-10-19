// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginProcessor.h"

LivecutAudioProcessor::LivecutAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void LivecutAudioProcessor::prepareToPlay (double, int) {}

void LivecutAudioProcessor::releaseResources() {}

bool LivecutAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // The cutter pans and rotates a stereo pair, so stereo in/out is the only layout that makes sense.
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void LivecutAudioProcessor::processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&)
{
    // The buffer is processed in place and the layout is stereo-to-stereo,
    // so leaving it untouched is a pass-through until the cutting engine lands.
}

juce::AudioProcessorEditor* LivecutAudioProcessor::createEditor()
{
    return new juce::GenericAudioProcessorEditor (*this);
}

bool LivecutAudioProcessor::hasEditor() const { return true; }

const juce::String LivecutAudioProcessor::getName() const { return JucePlugin_Name; }
bool LivecutAudioProcessor::acceptsMidi() const { return false; }
bool LivecutAudioProcessor::producesMidi() const { return false; }
double LivecutAudioProcessor::getTailLengthSeconds() const { return 0.0; }

// Some hosts misbehave when a plugin reports zero programs, so expose exactly one.
int LivecutAudioProcessor::getNumPrograms() { return 1; }
int LivecutAudioProcessor::getCurrentProgram() { return 0; }
void LivecutAudioProcessor::setCurrentProgram (int) {}
const juce::String LivecutAudioProcessor::getProgramName (int) { return "Default"; }
void LivecutAudioProcessor::changeProgramName (int, const juce::String&) {}

// There are no parameters yet; state arrives with the parameter layout.
void LivecutAudioProcessor::getStateInformation (juce::MemoryBlock&) {}
void LivecutAudioProcessor::setStateInformation (const void*, int) {}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LivecutAudioProcessor();
}
