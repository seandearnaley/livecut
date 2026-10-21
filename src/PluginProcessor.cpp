// SPDX-License-Identifier: GPL-3.0-or-later

#include "PluginProcessor.h"

#include "HostSync.h"
#include "PluginEditor.h"
#include <limits>
#include <thread>

LivecutAudioProcessor::LivecutAudioProcessor (livecut::spectral::TestHooks* hooks)
    : AudioProcessor (BusesProperties ()
                          .withInput ("Input", juce::AudioChannelSet::stereo (), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo (), true)),
      parameters (*this, nullptr, "Livecut", livecut::createParameterLayout ()), settingsReader (parameters), spectralTestHooks (hooks)
{
    startTimer (100);
}

namespace
{
class AdoptCallbackLock
{
  public:
    explicit AdoptCallbackLock (const juce::CriticalSection &entered) : lock (entered) {}
    ~AdoptCallbackLock ()
    {
        lock.exit ();
    }
    AdoptCallbackLock (const AdoptCallbackLock &) = delete;
    AdoptCallbackLock &operator= (const AdoptCallbackLock &) = delete;

  private:
    const juce::CriticalSection &lock;
};
} // namespace

void LivecutAudioProcessor::enterReplacementLock ()
{
    // Lifecycle only. A retry must cancel the latest published worker, rather
    // than wait behind an offline callback using a newer uncancelled worker.
    for (;;)
    {
        const auto stop = std::atomic_load_explicit (&activeStopPublication, std::memory_order_acquire);
        if (stop)
            stop->requestStop ();
        if (getCallbackLock ().tryEnter ())
            return;
        std::this_thread::yield ();
    }
}

bool LivecutAudioProcessor::enterCurrentOperation (std::uint64_t operation)
{
    while (configurationGeneration.load (std::memory_order_acquire) == operation)
    {
        if (getCallbackLock ().tryEnter ())
            return true;
        std::this_thread::yield ();
    }
    return false;
}

void LivecutAudioProcessor::publishStopHandle ()
{
    std::atomic_store_explicit (&activeStopPublication,
                                stream ? stream->stopPublication : std::shared_ptr<const livecut::spectral::StopHandle>{},
                                std::memory_order_release);
}

LivecutAudioProcessor::~LivecutAudioProcessor ()
{
    stopTimer ();
    std::unique_ptr<livecut::StreamResources> retired;
    enterReplacementLock ();
    {
        const AdoptCallbackLock guard (getCallbackLock ());
        retired = std::move (stream);
        publishStopHandle ();
    }
    retired.reset ();
}

void LivecutAudioProcessor::prepareToPlay (double sampleRate, int)
{
    std::unique_ptr<livecut::StreamResources> retired;
    std::uint64_t operation;
    enterReplacementLock ();
    {
        const AdoptCallbackLock guard (getCallbackLock ());
        suspendProcessing (true);
        operation = ++configurationGeneration;
        observedTransport = AudioObservation::unknown;
        retired = std::move (stream);
        publishStopHandle ();
        processingSampleRate = sampleRate;
        preparationFailed = false;
        publishedModeState = ModeState::unprepared;
    }
    retired.reset (); // no heavy destruction under our callback guard
    try
    {
        const auto settings = settingsReader.read ();
        auto prepared = std::make_unique<livecut::StreamResources> (sampleRate, settings.requestedMode, settings.requestedSpectral, spectralTestHooks);
        livecut::engine::WetHold preparedHold;
        preparedHold.prepare (sampleRate);
        preparedHold.request (settings.hold);
        auto preparedCapture = livecut::PhraseCapture::prepareStorage (sampleRate);
        livecut::PhraseCapture::Prepared retiredCapture;
        if (!enterCurrentOperation (operation))
            return;
        {
            const AdoptCallbackLock guard (getCallbackLock ());
            if (configurationGeneration != operation)
                return;
            std::swap (wetHold, preparedHold);
            retiredCapture = capture.rolling ().installPrepared (std::move (preparedCapture));
            mixer.prepare (sampleRate, settings.wet, settings.outputTrimDb);
            spectralMix.reset (sampleRate, 0.002);
            spectralMix.setCurrentAndTargetValue (settings.spectralMix);
            processingRunning = false;
            haveDelayedDisplay = false;
            stream = std::move (prepared);
            publishStopHandle ();
            publishSpectralConfiguration ();
            declaredLatency = stream->latency;
            publishTransformDisplay ();
            ++configurationGeneration;
            display.hold = wetHold.state ();
            display.status = livecut::DisplayState::Status::waitingForHost;
        }
    }
    catch (const std::exception &)
    {
        if (!enterCurrentOperation (operation))
            return;
        const AdoptCallbackLock guard (getCallbackLock ());
        if (configurationGeneration != operation)
            return;
        preparationFailed = true;
        declaredLatency = 0;
        capture.rolling ().interrupt ();
        wetHold.interrupt ();
        display.hold = wetHold.state ();
        display.status = livecut::DisplayState::Status::preparationError;
        publishedModeState = ModeState::error;
    }
    reconcileLatency ();
}

void LivecutAudioProcessor::releaseResources ()
{
    std::unique_ptr<livecut::StreamResources> retired;
    enterReplacementLock ();
    {
        const AdoptCallbackLock guard (getCallbackLock ());
        suspendProcessing (true);
        ++configurationGeneration;
        retired = std::move (stream);
        publishStopHandle ();
        capture.rolling ().interrupt ();
        wetHold.interrupt ();
        haveDelayedDisplay = false;
        display.hold = wetHold.state ();
        observedTransport = AudioObservation::unknown;
        preparationFailed = false;
        publishedModeState = ModeState::unprepared;
    }
    retired.reset ();
    // Deactivation discards PCM, retaining the complete declared L0+Ds contract.
    reconcileLatency ();
}

void LivecutAudioProcessor::reconcileLatency ()
{
    // Claim setter ownership under the guard, then call listeners off-guard.
    // Reentrant/concurrent lifecycle calls return pending instead of invoking
    // another setter while the owner is inside a host notification.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        std::uint64_t generation;
        int desired;
        {
            const juce::ScopedTryLock guard (getCallbackLock ());
            if (!guard.isLocked ())
            {
                applyPending = true;
                publishedModeState = ModeState::pendingHostChange;
                return;
            }
            if (latencyUpdateInProgress)
            {
                applyPending = true;
                publishedModeState = ModeState::pendingHostChange;
                return;
            }
            latencyUpdateInProgress = true;
            generation = configurationGeneration;
            desired = declaredLatency;
        }
        if (getLatencySamples () != desired)
            setLatencySamples (desired);
        {
            const juce::ScopedTryLock guard (getCallbackLock ());
            if (!guard.isLocked ())
            {
                // This setter has returned and remains the sole owner until
                // this release. Leave reconciliation for a later safe attempt.
                applyPending = true;
                publishedModeState = ModeState::pendingHostChange;
                latencyUpdateInProgress.store (false, std::memory_order_release);
                return;
            }
            latencyUpdateInProgress = false;
            if (generation != configurationGeneration)
                continue;
            if (getLatencySamples () != desired)
                continue;
            publishedLatency.store (desired, std::memory_order_release);
            publishedGeneration = configurationGeneration;
            if (stream != nullptr)
            {
                publishedMode.store (stream->mode, std::memory_order_release);
                publishedSpectral.store (stream->spectralEnabled, std::memory_order_release);
                publishedModeState = ModeState::active;
                const auto requested = settingsReader.read ();
                applyPending = requested.requestedMode != stream->mode || requested.requestedSpectral != stream->spectralEnabled;
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

void LivecutAudioProcessor::applyRequestedMode ()
{
    applyPending = true;
    const auto *messages = juce::MessageManager::getInstanceWithoutCreating ();
    if (messages != nullptr && messages->isThisTheMessageThread ())
        tryApplyMode ();
}

void LivecutAudioProcessor::timerCallback ()
{
    if (applyPending.load (std::memory_order_acquire))
        tryApplyMode ();
}

void LivecutAudioProcessor::tryApplyMode ()
{
    const auto requested = settingsReader.read ();
    double rate = 0.0;
    std::uint64_t generation = 0;
    bool reconcile = false;
    {
        const juce::ScopedTryLock guard (getCallbackLock ());
        if (!guard.isLocked ())
            return;
        if (latencyUpdateInProgress)
            return; // the setter owner must publish or leave reconciliation pending
        // Configuration coherence precedes EVERY mode-choice eligibility return,
        // including null/error resources and unavailable transport evidence.
        reconcile = publishedGeneration != configurationGeneration;
        if (!reconcile)
        {
            if (stream == nullptr)
            {
                publishedModeState = preparationFailed ? ModeState::error : ModeState::unprepared;
                applyPending = false; // a future prepare applies the persisted request
                return;
            }
            if (stream->mode == requested.requestedMode && stream->spectralEnabled == requested.requestedSpectral && !stream->fault &&
                (!stream->spectral || stream->spectral->fault () == livecut::spectral::Fault::none))
            {
                publishedModeState = ModeState::active;
                applyPending = false;
                return;
            }
            const auto observation = observedTransport.load (std::memory_order_acquire);
            if (observation != AudioObservation::stopped)
            {
                publishedModeState = observation == AudioObservation::playing ? ModeState::pendingPlaying : ModeState::pendingObservation;
                return;
            }
            generation = configurationGeneration;
            rate = processingSampleRate;
        }
    }
    if (reconcile)
    {
        reconcileLatency ();
        return;
    }
    std::unique_ptr<livecut::StreamResources> prepared;
    try
    {
        prepared = std::make_unique<livecut::StreamResources> (rate, requested.requestedMode, requested.requestedSpectral, spectralTestHooks);
    }
    catch (const std::exception &)
    {
        const juce::ScopedTryLock guard (getCallbackLock ());
        if (!guard.isLocked ())
            return;
        if (generation != configurationGeneration)
            return; // obsolete work cannot publish over the current generation
        if (requested.requestedMode != settingsReader.read ().requestedMode ||
            requested.requestedSpectral != settingsReader.read ().requestedSpectral)
        {
            applyPending = true;
            publishedModeState = ModeState::pendingHostChange;
            return;
        }
        publishedModeState = ModeState::error;
        applyPending = false;
        return;
    }
    std::unique_ptr<livecut::StreamResources> retired;
    {
        const juce::ScopedTryLock guard (getCallbackLock ());
        if (!guard.isLocked ())
            return;
        if (generation != configurationGeneration)
            return; // the newer lifecycle owns both its state and retry flag
        if (publishedGeneration != configurationGeneration || rate != processingSampleRate ||
            requested.requestedMode != settingsReader.read ().requestedMode ||
            requested.requestedSpectral != settingsReader.read ().requestedSpectral)
        {
            applyPending = true;
            publishedModeState = ModeState::pendingHostChange;
            return; // stale prebuild freed after this guard
        }
        const auto observation = observedTransport.load (std::memory_order_acquire);
        if (observation != AudioObservation::stopped)
        {
            publishedModeState = observation == AudioObservation::playing ? ModeState::pendingPlaying : ModeState::pendingObservation;
            return;
        }
        suspendProcessing (true);
        if (stream->spectral)
            stream->spectral->requestStop ();
        retired = std::move (stream);
        stream = std::move (prepared);
        publishStopHandle ();
        publishSpectralConfiguration ();
        haveDelayedDisplay = false;
        declaredLatency = stream->latency;
        publishTransformDisplay ();
        ++configurationGeneration;
        observedTransport = AudioObservation::unknown;
        capture.rolling ().interrupt ();
        wetHold.interrupt ();
        wetHold.request (requested.hold);
        mixer.prepare (rate, requested.wet, requested.outputTrimDb);
        spectralMix.reset (rate, 0.002);
        spectralMix.setCurrentAndTargetValue (requested.spectralMix);
        processingRunning = false;
        display.hold = wetHold.state ();
    }
    retired.reset ();
    reconcileLatency ();
}

void LivecutAudioProcessor::observeTransport ()
{
    const auto *head = getPlayHead ();
    const auto info = head != nullptr ? head->getPosition () : juce::Optional<juce::AudioPlayHead::PositionInfo>{};
    observeTransport (info);
}

void LivecutAudioProcessor::observeTransport (const juce::Optional<juce::AudioPlayHead::PositionInfo> &info)
{
    observedTransport.store (!info                   ? AudioObservation::unknown
                             : info->getIsPlaying () ? AudioObservation::playing
                                                     : AudioObservation::stopped,
                             std::memory_order_release);
}

bool LivecutAudioProcessor::isBusesLayoutSupported (const BusesLayout &layouts) const
{
    // The cutter pans and rotates a stereo pair, so stereo in/out is the only layout that makes sense.
    return layouts.getMainInputChannelSet () == juce::AudioChannelSet::stereo () &&
           layouts.getMainOutputChannelSet () == juce::AudioChannelSet::stereo ();
}

void LivecutAudioProcessor::processBlock (juce::AudioBuffer<float> &buffer, juce::MidiBuffer &)
{
    process (buffer, false);
}

void LivecutAudioProcessor::processBlockBypassed (juce::AudioBuffer<float> &buffer, juce::MidiBuffer &)
{
    // Keep Live's original host bypass contract. Studio always advances its
    // physical/analysis/render clocks, selecting unity delayed raw at the end.
    if (isSuspended () || stream == nullptr)
    {
        buffer.clear ();
        return;
    }
    if (stream->mode == livecut::ProcessingMode::studio || stream->spectralEnabled)
        process (buffer, true);
    else if (buffer.getNumSamples () > 0)
        observeTransport ();
}

void LivecutAudioProcessor::failStream ()
{
    stream->fault = true;
    capture.rolling ().interrupt ();
    wetHold.interrupt ();
    display.hold = wetHold.state ();
    if (stream->spectral)
        display.spectralFault = stream->spectral->fault ();
    display.status = livecut::DisplayState::Status::streamError;
}

void LivecutAudioProcessor::process (juce::AudioBuffer<float> &buffer, bool bypass)
{
    juce::ScopedNoDenormals noDenormals;
    if (isSuspended () || stream == nullptr)
    {
        buffer.clear ();
        if (stream == nullptr)
            display.status = livecut::DisplayState::Status::preparationError;
        return;
    }
    auto &resources = *stream;
    if (resources.fault || (resources.spectral && resources.spectral->fault () != livecut::spectral::Fault::none))
    {
        if (buffer.getNumSamples () > 0)
            observeTransport ();
        failStream ();
        buffer.clear ();
        return;
    }
    const auto frames = buffer.getNumSamples ();
    if (frames == 0)
        return;
    livecut::RenderContext input;
    input.settings = settingsReader.read ();
    input.bypass = bypass;
    input.resetHold = resetHoldRequested.exchange (false, std::memory_order_acq_rel);
    const auto *head = getPlayHead ();
    const auto info = head != nullptr ? head->getPosition () : juce::Optional<juce::AudioPlayHead::PositionInfo>{};
    observeTransport (info);
    const auto sync = info ? livecut::hostSync (*info) : std::nullopt;
    if (sync)
    {
        input.running = true;
        input.timing = sync->timing;
        input.position = sync->position;
        input.ppq = *info->getPpqPosition ();
    }
    if (!resources.appendContext (input, frames, transportEpochCounter))
    {
        failStream ();
        buffer.clear ();
        return;
    }
    struct SpectralCallback
    {
        livecut::spectral::Stage *stage;
        SpectralCallback (livecut::spectral::Stage *s, bool offline) : stage (s)
        {
            if (stage)
                stage->beginCallback (offline ? livecut::spectral::Execution::offline : livecut::spectral::Execution::realtime);
        }
        ~SpectralCallback ()
        {
            if (stage)
                stage->endCallback ();
        }
    } callback (resources.spectral.get (), isNonRealtime ()); // snapshot explicit host mode once
    bool produced = false;
    for (int i = 0; i < frames; ++i)
    {
        auto &left = buffer.getWritePointer (0)[i];
        auto &right = buffer.getWritePointer (1)[i];
        const auto physical = resources.inputFrame;
        if (!resources.cutter.ingestInputFrame (physical, {left, right}) || !resources.detector.process (physical, left, right))
        {
            failStream ();
            buffer.clear (i, frames - i);
            break;
        }
        ++resources.inputFrame;
        const auto rendered = resources.timeline.renderFrame (physical);
        if (!rendered)
        {
            left = right = 0.0f;
            continue;
        }
        if (*rendered != resources.nextRender)
        {
            failStream ();
            buffer.clear (i, frames - i);
            break;
        }
        livecut::spectral::Packet packet;
        packet.sourceFrame = *rendered;
        const juce::ScopedValueSetter<livecut::spectral::Packet *> collecting (collectingPacket, resources.spectral ? &packet : nullptr);
        const juce::ScopedValueSetter<bool> beforePhase (collectingAfter, false);
        const auto firstContextSample = !resources.rendering;
        if (firstContextSample)
        {
            resources.rendering = resources.timeline.lookup (*rendered);
            if (!resources.rendering || resources.rendering->offset != 0)
            {
                failStream ();
                buffer.clear (i, frames - i);
                break;
            }
            const auto &segment = resources.rendering->segment;
            const auto &context = segment.context;
            const auto newEpoch = resources.renderEpoch != segment.transportEpoch;
            resources.renderEpoch = segment.transportEpoch;
            if (newEpoch)
                resources.cutter.resetRendering ();
            livecut::applySettings (resources.cutter, context.settings);
            processingBeatsPerSample = context.timing.bpm / 60.0 / processingSampleRate;
            processingBeatsPerBar = context.timing.beatsPerBar;
            if (resources.spectral)
                spectralMix.setTargetValue (context.settings.spectralMix);
            else
            {
                mixer.setTargets (context.settings.wet, context.settings.outputTrimDb);
                if (context.resetHold)
                    wetHold.interrupt ();
                wetHold.request (context.settings.hold);
                processingRunning = context.running;
            }
            if (context.running)
                resources.cutter.beginRenderContext (context.timing, context.position, *rendered, i, this);
            else if (newEpoch)
                discontinuity (i);
        }
        const auto raw = resources.cutter.rawInputFrame (*rendered);
        if (!raw)
        {
            failStream ();
            buffer.clear (i, frames - i);
            break;
        }
        const auto &context = resources.rendering->segment.context;
        packet.epoch = resources.rendering->segment.transportEpoch;
        if (resources.spectral)
        {
            packet.raw = {raw->left, raw->right};
            packet.spectral = {context.settings.spectralFreeze, context.settings.spectralSmearMs, spectralMix.getNextValue ()};
            packet.output = {processingBeatsPerSample, context.settings.wet,  context.settings.outputTrimDb,          context.running,
                             context.bypass,           context.settings.hold, firstContextSample && context.resetHold};
        }
        left = raw->left;
        right = raw->right;
        if (context.running)
            resources.cutter.renderOne (*rendered, left, right, i, this);
        else if (!resources.spectral)
            mixer.process (raw->left, raw->right, left, right);
        if (resources.spectral)
            packet.processed = {left, right};
        else
        {
            produced = true;
            if (context.bypass)
            {
                left = raw->left;
                right = raw->right;
            }
            if (context.running)
                capture.rolling ().push (left, right);
        }
        // Original position at this sample's exclusive end. It cannot be read
        // later from a Cutter whose phrase/context may already have retired.
        const auto position =
            ((static_cast<double> (context.position.bar) - static_cast<double> (resources.cutter.phraseStartBar ())) +
             (context.position.beatsIntoBar + (resources.rendering->offset + 1) * processingBeatsPerSample) / context.timing.beatsPerBar) *
            resources.cutter.activeUnitsPerBar ();
        ++resources.nextRender;
        const auto contextEnded = ++resources.rendering->offset == resources.rendering->segment.frameCount;
        if (contextEnded && context.running)
        {
            collectingAfter = true;
            resources.cutter.endRenderContext (i + 1, this);
            collectingAfter = false;
        }
        if (resources.spectral)
        {
            packet.display = sourceDisplay ();
            packet.display.position = position;
        }
        if (contextEnded)
            resources.rendering.reset ();
        if (resources.spectral)
        {
            const auto result = resources.spectral->process (packet); // sealed after endRenderContext
            if (result.recoveryRequired)
            {
                failStream ();
                buffer.clear (i, frames - i);
                break;
            }
            if (result.valid)
            {
                produced = true;
                renderSpectralOutput (result, left, right);
            }
            else
                left = right = 0.0f;
        }
        if (!resources.timeline.retireBefore (resources.nextRender))
        {
            failStream ();
            buffer.clear (i, frames - i);
            break;
        }
    }
    publishTransformDisplay ();
    if (resources.fault)
        display.status = livecut::DisplayState::Status::streamError;
    else if (!produced)
        display.status = livecut::DisplayState::Status::filling;
    else
        publishDisplay ();
}

void LivecutAudioProcessor::dispatchEvent (const livecut::spectral::Event &event)
{
    using Event = livecut::spectral::EventKind;
    switch (event.kind)
    {
    case Event::discontinuity:
        capture.rolling ().interrupt ();
        wetHold.interrupt ();
        break;
    case Event::barStarted:
        wetHold.barStarted ();
        break;
    case Event::phraseCompleted:
        capture.rolling ().completePhrase ();
        wetHold.completePhrase ();
        break;
    case Event::phraseStarted:
        capture.rolling ().startPhrase (event.fullStart);
        wetHold.startPhrase (event.fullStart, event.phraseQuarterNotes);
        break;
    }
}
void LivecutAudioProcessor::collectOrDispatch (livecut::spectral::Event event)
{
    if (!collectingPacket)
    {
        dispatchEvent (event);
        return;
    }
    if (collectingAfter)
        collectingPacket->after.append (event);
    else
        collectingPacket->before.append (event);
}
void LivecutAudioProcessor::discontinuity (int)
{
    collectOrDispatch ({livecut::spectral::EventKind::discontinuity});
}
void LivecutAudioProcessor::barStarted (int, std::int64_t bar)
{
    collectOrDispatch ({livecut::spectral::EventKind::barStarted, false, bar});
}
void LivecutAudioProcessor::phraseCompleted (int)
{
    collectOrDispatch ({livecut::spectral::EventKind::phraseCompleted});
}
void LivecutAudioProcessor::phraseStarted (int, bool fullStart)
{
    collectOrDispatch (
        {livecut::spectral::EventKind::phraseStarted, fullStart, 0, stream->cutter.phraseBarCount () * processingBeatsPerBar});
}
void LivecutAudioProcessor::processOutput (int, float dryLeft, float dryRight, float &left, float &right)
{
    if (collectingPacket)
        return; // collect inner wet; consumers run at its delayed boundary
    wetHold.process (left, right, processingBeatsPerSample);
    mixer.process (dryLeft, dryRight, left, right);
}
void LivecutAudioProcessor::renderSpectralOutput (const livecut::spectral::Result &result, float &left, float &right)
{
    const auto &packet = result.packet;
    const auto &context = packet.output;
    if (context.resetHold)
        wetHold.interrupt ();
    wetHold.request (context.hold);
    mixer.setTargets (context.globalMix, context.trimDb);
    for (std::size_t i = 0; i < packet.before.count; ++i)
        dispatchEvent (packet.before.items[i]);
    const auto wet = result.localWet ();
    left = wet.left;
    right = wet.right;
    if (context.running)
    {
        if (!result.holdEligible ())
            wetHold.startPhrase (false, 0.0); // candidate only, retain held/latest
        wetHold.process (left, right, context.quarterNotesPerSample);
    }
    mixer.process (packet.raw.left, packet.raw.right, left, right);
    if (context.bypass)
    {
        left = packet.raw.left;
        right = packet.raw.right;
    }
    if (context.running)
        capture.rolling ().push (left, right, result.captureHasWarmup ());
    for (std::size_t i = 0; i < packet.after.count; ++i)
        dispatchEvent (packet.after.items[i]);
    delayedDisplay = packet.display;
    haveDelayedDisplay = true;
    processingRunning = context.running;
    display.spectralAvailability = result.availability;
}

void LivecutAudioProcessor::publishSpectralConfiguration ()
{
    display.spectralConfigured = stream->spectralEnabled;
    display.spectralFault = livecut::spectral::Fault::none;
    display.spectralAvailability =
        stream->spectralEnabled ? livecut::spectral::Availability::waiting : livecut::spectral::Availability::disabled;
    display.spectralFftSize = stream->spectral ? stream->spectral->configuration ().size : 0;
    display.spectralHop = stream->spectral ? stream->spectral->configuration ().hop : 0;
    display.spectralDelay = stream->spectral ? stream->spectral->latency () : 0;
    display.transformEligible = display.attackApplied = display.reverseApplied = display.transformIneligible = 0;
    display.transformReason = livecut::DisplayState::TransformReason::off;
}

livecut::spectral::DisplaySnapshot LivecutAudioProcessor::sourceDisplay () const
{
    const auto &cutter = stream->cutter;
    const auto &transforms = cutter.transformStats ();
    const auto &fracture = cutter.fractureDiagnostics ();
    const auto &resonator = cutter.resonator ().diagnostics ();
    const auto &route = cutter.routeDiagnostics ();
    livecut::spectral::DisplaySnapshot result;
    result.transformEligible = transforms.eligible;
    result.attackApplied = transforms.attackAligned;
    result.reverseApplied = transforms.reversed;
    result.transformIneligible = transforms.ineligible;
    result.transformReason = static_cast<int> (transforms.reason);
    result.phraseBars = cutter.phraseBarCount ();
    result.unitsPerBar = cutter.activeUnitsPerBar ();
    result.blockStart = cutter.blockStartUnits ();
    result.blockEnd = cutter.blockEndUnits ();
    result.position = cutter.positionInPhrase ();
    result.cutting = cutter.isCutting ();
    result.timingSupported = cutter.isTimingSupported ();
    result.cutterError = static_cast<int> (cutter.error ());
    result.fractureFamily = static_cast<int> (fracture.family);
    result.fractureState = static_cast<int> (fracture.state);
    result.fractureVoices = static_cast<int> (fracture.activeVoices);
    result.fractureTapeSpeed = fracture.tapeSpeed;
    result.resonatorState = static_cast<int> (resonator.state);
    result.resonatorRetuning = resonator.retuning;
    result.resonatorPending = resonator.pending;
    result.effectRoute = static_cast<int> (route.active);
    result.requestedEffectRoute = static_cast<int> (route.requested);
    result.effectRouteTransition = route.transitioning;
    return result;
}

void LivecutAudioProcessor::publishSourceDisplay (const livecut::spectral::DisplaySnapshot &source)
{
    display.transformEligible = source.transformEligible;
    display.attackApplied = source.attackApplied;
    display.reverseApplied = source.reverseApplied;
    display.transformIneligible = source.transformIneligible;
    display.transformReason = static_cast<livecut::DisplayState::TransformReason> (source.transformReason);
    display.fractureFamily = static_cast<livecut::engine::FractureProcedure> (source.fractureFamily);
    display.fractureState = static_cast<livecut::engine::FractureState> (source.fractureState);
    display.fractureVoices = source.fractureVoices;
    display.fractureTapeSpeed = source.fractureTapeSpeed;
    display.resonatorState = static_cast<livecut::engine::Resonator::State> (source.resonatorState);
    display.resonatorRetuning = source.resonatorRetuning;
    display.resonatorPending = source.resonatorPending;
    display.effectRoute = static_cast<livecut::engine::EffectRoute> (source.effectRoute);
    display.requestedEffectRoute = static_cast<livecut::engine::EffectRoute> (source.requestedEffectRoute);
    display.effectRouteTransition = source.effectRouteTransition;
    display.phraseBars = source.phraseBars;
    display.unitsPerBar = source.unitsPerBar;
    display.blockStart = source.blockStart;
    display.blockEnd = source.blockEnd;
    display.position = source.position;
}

void LivecutAudioProcessor::publishTransformDisplay ()
{
    if (stream->spectral)
    {
        if (haveDelayedDisplay)
            publishSourceDisplay (delayedDisplay);
    }
    else
        publishSourceDisplay (sourceDisplay ());
}

void LivecutAudioProcessor::publishDisplay ()
{
    using Status = livecut::DisplayState::Status;
    display.hold = wetHold.state ();
    const auto source = stream->spectral ? delayedDisplay : sourceDisplay ();
    publishSourceDisplay (source);
    if (!processingRunning)
    {
        display.status = Status::waitingForHost;
        return;
    }
    const auto error = static_cast<livecut::engine::Cutter::Error> (source.cutterError);
    if (error != livecut::engine::Cutter::Error::none)
    {
        display.status = error == livecut::engine::Cutter::Error::planOverflow ? Status::planOverflow : Status::sourceUnavailable;
        return;
    }
    if (!source.timingSupported)
    {
        display.status = Status::barTooLong;
        return;
    }
    if (!source.cutting)
    {
        display.status = Status::startingSoon;
        return;
    }
    display.status = Status::cutting;
}

juce::AudioProcessorEditor *LivecutAudioProcessor::createEditor ()
{
    return new LivecutAudioProcessorEditor (*this);
}

bool LivecutAudioProcessor::hasEditor () const
{
    return true;
}

const juce::String LivecutAudioProcessor::getName () const
{
    return JucePlugin_Name;
}
bool LivecutAudioProcessor::acceptsMidi () const
{
    return false;
}
bool LivecutAudioProcessor::producesMidi () const
{
    return false;
}
// A requested Hold can play indefinitely; JUCE maps infinity to VST3 kInfiniteTail.
double LivecutAudioProcessor::getTailLengthSeconds () const
{
    return std::numeric_limits<double>::infinity ();
}

// Some hosts misbehave when a plugin reports zero programs, so expose exactly one.
int LivecutAudioProcessor::getNumPrograms ()
{
    return 1;
}
int LivecutAudioProcessor::getCurrentProgram ()
{
    return 0;
}
void LivecutAudioProcessor::setCurrentProgram (int) {}
const juce::String LivecutAudioProcessor::getProgramName (int)
{
    return "Default";
}
void LivecutAudioProcessor::changeProgramName (int, const juce::String &) {}

void LivecutAudioProcessor::getStateInformation (juce::MemoryBlock &destData)
{
    auto state = parameters.copyState ();
    state.setProperty ("schemaVersion", 8, nullptr);
    state.setProperty ("algorithmVersion", 2, nullptr);
    if (const auto xml = state.createXml ())
        copyXmlToBinary (*xml, destData);
}

void LivecutAudioProcessor::setStateInformation (const void *data, int sizeInBytes)
{
    // State that isn't ours, or isn't readable, leaves the current settings as they are.
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || !xml->hasTagName (parameters.state.getType ()))
        return;
    auto restored = juce::ValueTree::fromXml (*xml);
    // A missing parameter means the default of that release, even when loading
    // an old set into an instance whose newer controls were already adjusted.
    for (auto *parameter : getParameters ())
    {
        const auto *ranged = dynamic_cast<juce::RangedAudioParameter *> (parameter);
        if (ranged == nullptr || restored.getChildWithProperty ("id", ranged->getParameterID ()).isValid ())
            continue;
        juce::ValueTree child ("PARAM");
        child.setProperty ("id", ranged->getParameterID (), nullptr);
        child.setProperty ("value", ranged->convertFrom0to1 (ranged->getDefaultValue ()), nullptr);
        restored.appendChild (child, nullptr);
    }
    parameters.replaceState (restored);
    resetHoldRequested.store (true, std::memory_order_release);
    applyPending.store (true, std::memory_order_release);
}

juce::String LivecutAudioProcessor::holdStatusText () const
{
    using State = livecut::engine::WetHold::State;
    switch (display.hold.load (std::memory_order_relaxed))
    {
    case State::live:
        return "Live input";
    case State::waiting:
        return "Hold waiting for a fresh complete wet phrase";
    case State::pendingEntry:
        return "Hold enters on the next bar — varispeed";
    case State::held:
        return "Audio held — varispeed follows tempo (pitch changes)";
    case State::pendingRelease:
        return "Hold releases on the next bar";
    case State::oversize:
        return "Hold unavailable: wet phrase exceeds prepared capacity";
    }
    return {};
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter ()
{
    return new LivecutAudioProcessor ();
}

juce::String LivecutAudioProcessor::modeStatusText () const
{
    const auto settings = settingsReader.read ();
    const auto requested = settings.requestedMode;
    const auto name = [] (livecut::ProcessingMode mode) { return mode == livecut::ProcessingMode::live ? "Live" : "Studio"; };
    switch (modeState ())
    {
    case ModeState::unprepared:
        return "Awaiting audio preparation";
    case ModeState::error:
        return "Cannot configure audio; check sample rate and available memory, then prepare again";
    case ModeState::pendingObservation:
        return "Apply pending: waiting for explicit stopped transport or audio preparation";
    case ModeState::pendingPlaying:
        return "Apply pending: stop host playback first";
    case ModeState::pendingHostChange:
        return "Apply pending: host audio configuration is changing";
    case ModeState::active:
        break;
    }
    const auto active = activeMode ();
    if (requested != active || settings.requestedSpectral != activeSpectral ())
        return juce::String ("Pending ") + name (requested) + (settings.requestedSpectral ? " + Spectral" : "") + "; active " +
               name (active) + (activeSpectral () ? " + Spectral" : "") + ". Apply while stopped.";
    return juce::String (name (active)) + " active; " + juce::String (publishedLatency.load ()) +
           " samples latency. Host compensation is host-controlled.";
}

juce::String LivecutAudioProcessor::transformStatusText () const
{
    using Reason = livecut::DisplayState::TransformReason;
    juce::String reason;
    switch (display.transformReason.load (std::memory_order_relaxed))
    {
    case Reason::off:
        reason = "Attack Align and Reverse are off";
        break;
    case Reason::awaitingCut:
        reason = "Waiting for the next cut";
        break;
    case Reason::searchPending:
        reason = "Attack search is still analysing; this cut keeps its planned source";
        break;
    case Reason::noAttack:
        reason = "No qualifying attack; this cut keeps its planned source";
        break;
    case Reason::sourceIncomplete:
        reason = "Complete source audio has not arrived for this cut";
        break;
    case Reason::tooLong:
        reason = "This cut needs more source audio than the available budget";
        break;
    case Reason::sourceLost:
        reason = "Selected source was lost; wet audio is gated";
        break;
    case Reason::applied:
        reason = "Selected complete source is fixed for this cut";
        break;
    case Reason::eligibleForward:
        reason = "Source eligible; reverse draw chose forward playback";
        break;
    case Reason::silentCut:
        reason = "Silent cut has no source to transform";
        break;
    case Reason::analysisError:
        reason = "Attack/source metadata unavailable for this cut";
        break;
    }
    return reason + "\nEligible " + juce::String (display.transformEligible.load ()) + " | Attack aligned " +
           juce::String (display.attackApplied.load ()) + " | Reversed " + juce::String (display.reverseApplied.load ()) +
           " | Ineligible " + juce::String (display.transformIneligible.load ());
}

juce::String LivecutAudioProcessor::spectralStatusText () const
{
    using Availability = livecut::spectral::Availability;
    if (display.spectralFault.load () != livecut::spectral::Fault::none)
        return "Spectral error: output gated. Stop the host and Apply to replace the prepared stream.";
    const auto settings = settingsReader.read ();
    if (settings.requestedSpectral != activeSpectral ())
        return "Spectral configuration pending. Apply while the host is stopped.";
    if (!activeSpectral ())
        return "Spectral off; no additional spectral delay.";
    juce::String state;
    switch (display.spectralAvailability.load ())
    {
    case Availability::waiting:
        state = "Waiting for fresh analysis; spectral wet is zero";
        break;
    case Availability::priming:
        state = "Priming spectral overlap; wet coverage is partial";
        break;
    case Availability::ready:
        state = "Spectral analysis ready";
        break;
    case Availability::disabled:
        state = "Awaiting prepared spectral output";
        break;
    }
    if (settings.spectralFreeze)
        state += "; Freeze requested at the next eligible analysis";
    return state + "\nFFT " + juce::String (display.spectralFftSize.load ()) + " / hop " + juce::String (display.spectralHop.load ()) +
           "; additional delay " + juce::String (display.spectralDelay.load ()) + " samples. Short loops may remain Waiting.";
}
