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
    startTimer (100);
}

LivecutAudioProcessor::~LivecutAudioProcessor() { stopTimer(); }

void LivecutAudioProcessor::prepareToPlay (double sampleRate, int)
{
    std::unique_ptr<livecut::StreamResources> retired;
    std::uint64_t operation;
    {
        const juce::ScopedLock guard (getCallbackLock());
        suspendProcessing (true);
        operation = ++configurationGeneration;
        haveAudioObservation = false;
        retired = std::move (stream);
        processingSampleRate = sampleRate;
        preparationFailed = false;
        publishedModeState = ModeState::unprepared;
    }
    retired.reset(); // no heavy destruction under our callback guard
    try
    {
        const auto settings = settingsReader.read();
        auto prepared = std::make_unique<livecut::StreamResources> (sampleRate, settings.requestedMode);
        livecut::engine::WetHold preparedHold;
        preparedHold.prepare (sampleRate);
        preparedHold.request (settings.hold);
        auto preparedCapture = livecut::PhraseCapture::prepareStorage (sampleRate);
        livecut::PhraseCapture::Prepared retiredCapture;
        {
            const juce::ScopedLock guard (getCallbackLock());
            if (configurationGeneration != operation)
                return;
            std::swap (wetHold, preparedHold);
            retiredCapture = capture.rolling().installPrepared (std::move (preparedCapture));
            mixer.prepare (sampleRate, settings.wet, settings.outputTrimDb);
            processingRunning = false;
            stream = std::move (prepared);
            ++configurationGeneration;
            display.hold = wetHold.state();
            display.status = livecut::DisplayState::Status::waitingForHost;
        }
    }
    catch (const std::exception&)
    {
        const juce::ScopedLock guard (getCallbackLock());
        if (configurationGeneration != operation)
            return;
        preparationFailed = true;
        capture.rolling().interrupt();
        wetHold.interrupt();
        display.hold = wetHold.state();
        display.status = livecut::DisplayState::Status::preparationError;
        publishedModeState = ModeState::error;
    }
    reconcileLatency();
}

void LivecutAudioProcessor::releaseResources()
{
    std::unique_ptr<livecut::StreamResources> retired;
    {
        const juce::ScopedLock guard (getCallbackLock());
        suspendProcessing (true);
        ++configurationGeneration;
        retired = std::move (stream);
        capture.rolling().interrupt();
        wetHold.interrupt();
        display.hold = wetHold.state();
        lastAudioRunning = false;
        haveAudioObservation = true;
        preparationFailed = false;
        publishedModeState = ModeState::unprepared;
    }
    retired.reset();
    reconcileLatency();
}

void LivecutAudioProcessor::reconcileLatency()
{
    // Claim setter ownership under the guard, then call listeners off-guard.
    // Reentrant/concurrent lifecycle calls return pending instead of invoking
    // another setter while the owner is inside a host notification.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        std::uint64_t generation;
        int desired;
        {
            const juce::ScopedLock guard (getCallbackLock());
            if (latencyUpdateInProgress)
            {
                applyPending = true;
                publishedModeState = ModeState::pendingHostChange;
                return;
            }
            latencyUpdateInProgress = true;
            generation = configurationGeneration;
            desired = stream != nullptr ? stream->latency : 0;
        }
        if (getLatencySamples() != desired)
            setLatencySamples (desired);
        {
            const juce::ScopedLock guard (getCallbackLock());
            latencyUpdateInProgress = false;
            if (generation != configurationGeneration)
                continue;
            if (getLatencySamples() != desired)
                continue;
            publishedLatency.store (desired, std::memory_order_release);
            if (stream != nullptr)
            {
                publishedMode.store (stream->mode, std::memory_order_release);
                publishedModeState = ModeState::active;
                applyPending = settingsReader.read().requestedMode != stream->mode;
            }
            else
            {
                publishedModeState = preparationFailed ? ModeState::error : ModeState::unprepared;
                applyPending = false;
            }
            suspendProcessing (false); // null resources deliberately produce gated output
            return;
        }
    }
    applyPending = true;
    publishedModeState = ModeState::pendingHostChange;
}

void LivecutAudioProcessor::applyRequestedMode()
{
    applyPending = true;
    const auto* messages = juce::MessageManager::getInstanceWithoutCreating();
    if (messages != nullptr && messages->isThisTheMessageThread())
        tryApplyMode();
}

void LivecutAudioProcessor::timerCallback()
{
    if (applyPending.load (std::memory_order_acquire))
        tryApplyMode();
}

void LivecutAudioProcessor::tryApplyMode()
{
    const auto requested = settingsReader.read();
    double rate;
    std::uint64_t generation;
    bool latencyOnly = false;
    {
        const juce::ScopedLock guard (getCallbackLock());
        if (latencyUpdateInProgress)
            return; // the current publisher reconciles the newest configuration
        if (stream == nullptr)
        {
            publishedModeState = preparationFailed ? ModeState::error : ModeState::unprepared;
            return;
        }
        if (stream->mode == requested.requestedMode)
            latencyOnly = true; // always reconcile publication/suspension, not just the numeric delay
        if (! latencyOnly && ! haveAudioObservation.load (std::memory_order_acquire))
        {
            publishedModeState = ModeState::pendingObservation;
            return;
        }
        if (! latencyOnly && lastAudioRunning.load (std::memory_order_acquire))
        {
            publishedModeState = ModeState::pendingPlaying;
            return;
        }
        generation = configurationGeneration;
        rate = processingSampleRate;
    }
    if (latencyOnly)
    {
        reconcileLatency();
        return;
    }
    std::unique_ptr<livecut::StreamResources> prepared;
    try { prepared = std::make_unique<livecut::StreamResources> (rate, requested.requestedMode); }
    catch (const std::exception&)
    {
        publishedModeState = ModeState::error;
        applyPending = false;
        return;
    }
    std::unique_ptr<livecut::StreamResources> retired;
    {
        const juce::ScopedLock guard (getCallbackLock());
        if (generation != configurationGeneration || rate != processingSampleRate
            || requested.requestedMode != settingsReader.read().requestedMode)
        {
            publishedModeState = ModeState::pendingHostChange;
            return; // stale prebuild freed after this guard
        }
        if (lastAudioRunning.load (std::memory_order_acquire))
        {
            publishedModeState = ModeState::pendingPlaying;
            return;
        }
        suspendProcessing (true);
        retired = std::move (stream);
        stream = std::move (prepared);
        ++configurationGeneration;
        haveAudioObservation = false;
        capture.rolling().interrupt();
        wetHold.interrupt();
        wetHold.request (requested.hold);
        mixer.prepare (rate, requested.wet, requested.outputTrimDb);
        processingRunning = false;
        display.hold = wetHold.state();
    }
    retired.reset();
    reconcileLatency();
}

void LivecutAudioProcessor::observeTransport()
{
    const auto* head = getPlayHead();
    const auto info = head != nullptr ? head->getPosition() : juce::Optional<juce::AudioPlayHead::PositionInfo> {};
    lastAudioRunning.store (info && info->getIsPlaying(), std::memory_order_release);
    haveAudioObservation.store (true, std::memory_order_release);
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
    if (isSuspended() || stream == nullptr)
    {
        buffer.clear();
        return;
    }
    if (stream->mode == livecut::ProcessingMode::studio)
        process (buffer, true);
    else if (buffer.getNumSamples() > 0)
        observeTransport();
}

void LivecutAudioProcessor::process (juce::AudioBuffer<float>& buffer, bool bypass)
{
    juce::ScopedNoDenormals noDenormals;
    if (isSuspended() || stream == nullptr)
    {
        buffer.clear();
        if (stream == nullptr)
            display.status = livecut::DisplayState::Status::preparationError;
        return;
    }
    auto& resources = *stream;
    if (resources.fault)
    {
        if (buffer.getNumSamples() > 0)
            observeTransport();
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
    lastAudioRunning.store (info && info->getIsPlaying(), std::memory_order_release);
    haveAudioObservation.store (true, std::memory_order_release);
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
    applyPending.store (true, std::memory_order_release);
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

juce::String LivecutAudioProcessor::modeStatusText() const
{
    const auto requested = settingsReader.read().requestedMode;
    const auto name = [] (livecut::ProcessingMode mode) { return mode == livecut::ProcessingMode::live ? "Live" : "Studio"; };
    switch (modeState())
    {
        case ModeState::unprepared: return "Awaiting audio preparation";
        case ModeState::error: return "Cannot configure audio; check sample rate and available memory, then prepare again";
        case ModeState::pendingObservation: return "Apply pending: waiting for a stopped audio callback";
        case ModeState::pendingPlaying: return "Apply pending: stop host playback first";
        case ModeState::pendingHostChange: return "Apply pending: host audio configuration is changing";
        case ModeState::active: break;
    }
    const auto active = activeMode();
    if (requested != active)
        return juce::String ("Pending ") + name (requested) + "; active " + name (active) + ". Apply while stopped.";
    return juce::String (name (active)) + " active; " + juce::String (publishedLatency.load()) + " samples latency. Host compensation is host-controlled.";
}

juce::String LivecutAudioProcessor::transformStatusText() const
{
    using Reason = livecut::DisplayState::TransformReason;
    juce::String reason;
    switch (display.transformReason.load (std::memory_order_relaxed))
    {
        case Reason::off: reason = "Attack Align and Reverse are off"; break;
        case Reason::awaitingCut: reason = "Waiting for the next cut"; break;
        case Reason::searchPending: reason = "Attack search is still analysing; this cut keeps its planned source"; break;
        case Reason::noAttack: reason = "No qualifying attack; this cut keeps its planned source"; break;
        case Reason::sourceIncomplete: reason = "Complete source audio has not arrived for this cut"; break;
        case Reason::tooLong: reason = "This cut needs more source audio than the available budget"; break;
        case Reason::sourceLost: reason = "Selected source was lost; wet audio is gated"; break;
    }
    return reason + "\nEligible " + juce::String (display.transformEligible.load())
         + " | Attack aligned " + juce::String (display.attackApplied.load())
         + " | Reversed " + juce::String (display.reverseApplied.load())
         + " | Ineligible " + juce::String (display.transformIneligible.load());
}
