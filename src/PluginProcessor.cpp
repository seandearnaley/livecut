// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginProcessor.h"

#include "HostSync.h"
#include "PluginEditor.h"
#include <limits>

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
    stream.reset();
    try
    {
        const auto settings = settingsReader.read();
        auto prepared = std::make_unique<livecut::StreamResources> (sampleRate, settings.requestedMode);
        mixer.prepare (sampleRate, settings.wet, settings.outputTrimDb);
        capture.rolling().prepare (sampleRate);
        wetHold.prepare (sampleRate);
        wetHold.request (settings.hold);
        processingSampleRate = sampleRate;
        processingRunning = false;
        stream = std::move (prepared);
        setLatencySamples (stream->latency);
        display.status = livecut::DisplayState::Status::waitingForHost;
    }
    catch (const std::exception&)
    {
        capture.rolling().interrupt();
        wetHold.interrupt();
        display.status = livecut::DisplayState::Status::preparationError;
    }
}

void LivecutAudioProcessor::releaseResources()
{
    capture.rolling().interrupt();
    wetHold.interrupt();
    stream.reset();
    display.hold = wetHold.state();
}

bool LivecutAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // The cutter pans and rotates a stereo pair, so stereo in/out is the only layout that makes sense.
    return layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void LivecutAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    process (buffer, false);
}

void LivecutAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    // Keep Live's original host bypass contract. Studio always advances its
    // physical/analysis/render clocks, selecting unity delayed raw at the end.
    if (stream != nullptr && stream->mode == livecut::ProcessingMode::studio)
        process (buffer, true);
}

void LivecutAudioProcessor::process (juce::AudioBuffer<float>& buffer, bool bypass)
{
    juce::ScopedNoDenormals noDenormals;
    if (stream == nullptr)
    {
        buffer.clear();
        display.status = livecut::DisplayState::Status::preparationError;
        return;
    }
    auto& resources = *stream;
    if (resources.fault)
    {
        buffer.clear();
        display.status = livecut::DisplayState::Status::streamError;
        return;
    }
    const auto frames = buffer.getNumSamples();
    if (frames == 0)
        return;
    livecut::RenderContext input;
    input.settings = settingsReader.read();
    input.bypass = bypass;
    input.resetHold = resetHoldRequested.exchange (false, std::memory_order_acq_rel);
    const auto* head = getPlayHead();
    const auto info = head != nullptr ? head->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};
    const auto sync = info ? livecut::hostSync (*info) : std::nullopt;
    if (sync)
    {
        input.running = true;
        input.timing = sync->timing;
        input.position = sync->position;
        input.ppq = *info->getPpqPosition();
    }
    if (! resources.appendContext (input, frames, transportEpochCounter))
    {
        resources.fault = true;
        buffer.clear();
        display.status = livecut::DisplayState::Status::streamError;
        return;
    }
    bool produced = false;
    for (int i = 0; i < frames; ++i)
    {
        auto& left = buffer.getWritePointer (0)[i];
        auto& right = buffer.getWritePointer (1)[i];
        const auto t = resources.inputFrame;
        if (! resources.cutter.ingestInputFrame (t, { left, right }) || ! resources.detector.process (t, left, right))
        {
            resources.fault = true;
            buffer.clear (i, frames - i);
            break;
        }
        ++resources.inputFrame;
        const auto r = resources.timeline.renderFrame (t);
        if (! r)
        {
            left = right = 0.0f;
            continue;
        }
        produced = true;
        if (*r != resources.nextRender)
        {
            resources.fault = true;
            buffer.clear (i, frames - i);
            break;
        }
        if (! resources.rendering)
        {
            resources.rendering = resources.timeline.lookup (*r);
            if (! resources.rendering || resources.rendering->offset != 0)
            {
                resources.fault = true;
                buffer.clear (i, frames - i);
                break;
            }
            const auto& segment = resources.rendering->segment;
            const auto& context = segment.context;
            const auto newEpoch = resources.renderEpoch != segment.transportEpoch;
            resources.renderEpoch = segment.transportEpoch;
            if (newEpoch)
                resources.cutter.resetRendering();
            livecut::applySettings (resources.cutter, context.settings);
            mixer.setTargets (context.settings.wet, context.settings.outputTrimDb);
            if (context.resetHold)
                wetHold.interrupt();
            wetHold.request (context.settings.hold);
            processingRunning = context.running;
            processingBeatsPerSample = context.timing.bpm / 60.0 / processingSampleRate;
            processingBeatsPerBar = context.timing.beatsPerBar;
            if (context.running)
                resources.cutter.beginRenderContext (context.timing, context.position, *r, i, this);
            else if (newEpoch)
                discontinuity (i);
        }
        const auto raw = resources.cutter.rawInputFrame (*r);
        if (! raw)
        {
            resources.fault = true;
            buffer.clear (i, frames - i);
            break;
        }
        const auto& context = resources.rendering->segment.context;
        left = raw->left;
        right = raw->right;
        if (context.running)
            resources.cutter.renderOne (*r, left, right, i, this);
        else
            mixer.process (raw->left, raw->right, left, right);
        if (context.bypass)
        {
            left = raw->left;
            right = raw->right;
        }
        if (context.running)
            capture.rolling().push (left, right);
        ++resources.nextRender;
        if (++resources.rendering->offset == resources.rendering->segment.frameCount)
        {
            if (context.running)
                resources.cutter.endRenderContext (i + 1, this);
            resources.rendering.reset();
        }
        if (! resources.timeline.retireBefore (resources.nextRender))
        {
            resources.fault = true;
            break;
        }
    }
    if (resources.fault)
        display.status = livecut::DisplayState::Status::streamError;
    else if (! produced)
        display.status = livecut::DisplayState::Status::filling;
    else
        publishDisplay();
}

void LivecutAudioProcessor::processOutput (int, float dryLeft, float dryRight, float& left, float& right)
{
    wetHold.process (left, right, processingBeatsPerSample);
    mixer.process (dryLeft, dryRight, left, right);
}

void LivecutAudioProcessor::publishDisplay()
{
    using Status = livecut::DisplayState::Status;
    display.hold = wetHold.state();
    const auto& cutter = stream->cutter;
    if (! processingRunning)
    {
        display.status = Status::waitingForHost;
        return;
    }
    if (cutter.error() != livecut::engine::Cutter::Error::none)
    {
        display.status = cutter.error() == livecut::engine::Cutter::Error::planOverflow ? Status::planOverflow : Status::sourceUnavailable;
        return;
    }

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
// A requested Hold can play indefinitely; JUCE maps infinity to VST3 kInfiniteTail.
double LivecutAudioProcessor::getTailLengthSeconds() const { return std::numeric_limits<double>::infinity(); }

// Some hosts misbehave when a plugin reports zero programs, so expose exactly one.
int LivecutAudioProcessor::getNumPrograms() { return 1; }
int LivecutAudioProcessor::getCurrentProgram() { return 0; }
void LivecutAudioProcessor::setCurrentProgram (int) {}
const juce::String LivecutAudioProcessor::getProgramName (int) { return "Default"; }
void LivecutAudioProcessor::changeProgramName (int, const juce::String&) {}

void LivecutAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = parameters.copyState();
    state.setProperty ("schemaVersion", 5, nullptr);
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
    resetHoldRequested.store (true, std::memory_order_release);
}

juce::String LivecutAudioProcessor::holdStatusText() const
{
    using State = livecut::engine::WetHold::State;
    switch (display.hold.load (std::memory_order_relaxed))
    {
        case State::live: return "Live input";
        case State::waiting: return "Hold waiting for a fresh complete wet phrase";
        case State::pendingEntry: return "Hold enters on the next bar — varispeed";
        case State::held: return "Audio held — varispeed follows tempo (pitch changes)";
        case State::pendingRelease: return "Hold releases on the next bar";
        case State::oversize: return "Hold unavailable: wet phrase exceeds prepared capacity";
    }
    return {};
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new LivecutAudioProcessor();
}
