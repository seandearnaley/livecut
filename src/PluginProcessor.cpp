// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginProcessor.h"

#include "HostSync.h"
#include "PluginEditor.h"

LivecutAudioProcessor::LivecutAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      parameters (*this, nullptr, "Livecut", livecut::createParameterLayout()),
      settingsReader (parameters)
{
}

void LivecutAudioProcessor::prepareToPlay (double sampleRate, int)
{
    cutter.prepare (sampleRate);
}

void LivecutAudioProcessor::releaseResources() {}

bool LivecutAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // The cutter pans and rotates a stereo pair, so stereo in/out is the only layout that makes sense.
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void LivecutAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    livecut::applySettings (cutter, settingsReader.read());

    const auto* playHead = getPlayHead();
    const auto position = playHead != nullptr ? playHead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};
    const auto sync = position ? livecut::hostSync (*position) : std::nullopt;

    // Cutting follows the host's bars, so without a running transport the input passes through.
    if (! sync)
    {
        cutter.reset();
        display.status = livecut::DisplayState::Status::waitingForHost;
        return;
    }

    cutter.process (buffer.getWritePointer (0), buffer.getWritePointer (1), buffer.getNumSamples(), sync->timing, sync->position);
    publishDisplay();
}

void LivecutAudioProcessor::publishDisplay()
{
    using Status = livecut::DisplayState::Status;

    if (! cutter.isTimingSupported())
    {
        display.status = Status::barTooLong;
        return;
    }

    if (! cutter.isCutting())
    {
        display.status = Status::startingSoon;
        return;
    }

    display.phraseBars = cutter.phraseBarCount();
    display.unitsPerBar = cutter.activeUnitsPerBar();
    display.blockStart = cutter.blockStartUnits();
    display.blockEnd = cutter.blockEndUnits();
    display.position = cutter.positionInPhrase();
    display.status = Status::cutting;
}

juce::AudioProcessorEditor* LivecutAudioProcessor::createEditor()
{
    return new LivecutAudioProcessorEditor (*this);
}

bool LivecutAudioProcessor::hasEditor() const { return true; }

const juce::String LivecutAudioProcessor::getName() const { return JucePlugin_Name; }
bool LivecutAudioProcessor::acceptsMidi() const { return false; }
bool LivecutAudioProcessor::producesMidi() const { return false; }
// Cuts can replay up to a bar of input after the input itself has gone quiet.
double LivecutAudioProcessor::getTailLengthSeconds() const { return livecut::engine::Cutter::maxBarSeconds; }

// Some hosts misbehave when a plugin reports zero programs, so expose exactly one.
int LivecutAudioProcessor::getNumPrograms() { return 1; }
int LivecutAudioProcessor::getCurrentProgram() { return 0; }
void LivecutAudioProcessor::setCurrentProgram (int) {}
const juce::String LivecutAudioProcessor::getProgramName (int) { return "Default"; }
void LivecutAudioProcessor::changeProgramName (int, const juce::String&) {}

void LivecutAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (const auto xml = parameters.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void LivecutAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // State that isn't ours, or isn't readable, leaves the current settings as they are.
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml != nullptr && xml->hasTagName (parameters.state.getType()))
        parameters.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LivecutAudioProcessor();
}
