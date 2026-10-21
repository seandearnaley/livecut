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
    const auto settings = settingsReader.read();
    mixer.prepare (sampleRate, settings.wet, settings.outputTrimDb);
    capture.rolling().prepare (sampleRate);
}

void LivecutAudioProcessor::releaseResources() { capture.rolling().interrupt(); }

bool LivecutAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // The cutter pans and rotates a stereo pair, so stereo in/out is the only layout that makes sense.
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void LivecutAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const auto settings = settingsReader.read();
    livecut::applySettings (cutter, settings);
    mixer.setTargets (settings.wet, settings.outputTrimDb);

    const auto* playHead = getPlayHead();
    const auto position = playHead != nullptr ? playHead->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};
    const auto sync = position ? livecut::hostSync (*position) : std::nullopt;

    // Cutting follows the host's bars, so without a running transport the input passes through.
    if (! sync)
    {
        cutter.reset();
        capture.rolling().interrupt();
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            auto& left = buffer.getWritePointer (0)[sample];
            auto& right = buffer.getWritePointer (1)[sample];
            mixer.process (left, right, left, right);
        }
        display.status = livecut::DisplayState::Status::waitingForHost;
        return;
    }

    cutter.process (buffer.getWritePointer (0), buffer.getWritePointer (1), buffer.getNumSamples(), sync->timing, sync->position, this);
    publishDisplay();
}

void LivecutAudioProcessor::processOutput (int, float dryLeft, float dryRight, float& left, float& right)
{
    mixer.process (dryLeft, dryRight, left, right);
    capture.rolling().push (left, right);
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
    auto state = parameters.copyState();
    state.setProperty ("schemaVersion", 3, nullptr);
    state.setProperty ("algorithmVersion", 2, nullptr);
    if (const auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void LivecutAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    // State that isn't ours, or isn't readable, leaves the current settings as they are.
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (parameters.state.getType()))
        return;
    auto restored = juce::ValueTree::fromXml (*xml);
    // A missing parameter means the default of that release, even when loading
    // an old set into an instance whose newer controls were already adjusted.
    for (auto* parameter : getParameters())
    {
        const auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter);
        if (ranged == nullptr || restored.getChildWithProperty ("id", ranged->getParameterID()).isValid())
            continue;
        juce::ValueTree child ("PARAM");
        child.setProperty ("id", ranged->getParameterID(), nullptr);
        child.setProperty ("value", ranged->convertFrom0to1 (ranged->getDefaultValue()), nullptr);
        restored.appendChild (child, nullptr);
    }
    parameters.replaceState (restored);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LivecutAudioProcessor();
}
