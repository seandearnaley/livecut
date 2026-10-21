// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/PerformanceEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace livecut::engine
{
namespace
{
constexpr double pi = 3.14159265358979323846;
double snapInteger (double value)
{
    const auto nearest = std::round (value);
    return std::abs (value - nearest) < 1.0e-7 ? nearest : value;
}
StereoFrame mix (StereoFrame from, StereoFrame to, float amount)
{
    return { from.left + amount * (to.left - from.left), from.right + amount * (to.right - from.right) };
}
bool continuousRhythm (const PerformanceParameters& parameters)
{
    return parameters.variation <= 0.0f && std::abs (parameters.density - .5f) < 1.0e-7f && !parameters.fill;
}
}

void PerformanceEngine::prepare (double rate)
{
    if (!std::isfinite (rate) || rate < 8000.0 || rate > 384000.0)
        throw std::invalid_argument ("Unsupported performance sample rate");
    const auto capacity = static_cast<std::size_t> (std::ceil (maximumBarSeconds * rate)) + 2;
    for (auto& bank : banks)
        bank.frames.resize (capacity);
    resampler.prepare();
    sampleRate = rate;
    crossfadeFrames = std::max (1, static_cast<int> (std::lround (rate * .002)));
    attackFrames = std::max (1, static_cast<int> (std::lround (rate * .010)));
    smoothing = 1.0 - std::exp (-1.0 / (rate * .003));
    captureSmoothing = 1.0 - std::exp (-1.0 / (rate * .001));
    display = {};
    epoch = 0;
    nextGeneration = 1;
    interrupt();
    discontinuityPending = false;
}

void PerformanceEngine::interrupt()
{
    recording = captureFull = haveBar = false;
    captureRequested = true;
    scheduledStep = eventIndex = -1;
    planCount = 0;
    fading = haveVoice = barCompletedAtEnd = false;
    cachedMainRate = cachedPreviousRate = -1.0;
    grains = {};
    for (auto& bank : banks)
    {
        bank.count = 0;
        bank.generation = 0;
    }
    display.hasSource = false;
    display.captureArmed = true;
    display.sourceFrames = 0;
    display.sourceGeneration = 0;
    display.activeGrains = 0;
    display.playhead = 0.0;
    display.state = State::waitingForBar;
    discontinuityPending = true;
}

void PerformanceEngine::fail (Error error)
{
    display.error = error;
    display.state = State::error;
    recording = false;
    grains = {};
}

bool PerformanceEngine::validParameters (const PerformanceParameters& parameters) const
{
    const auto normalized = [] (float value) { return std::isfinite (value) && value >= 0.0f && value <= 1.0f; };
    return normalized (parameters.variation) && normalized (parameters.density)
        && normalized (parameters.grain) && normalized (parameters.character);
}

void PerformanceEngine::beginContext (const Timing& nextTiming, const Position& nextPosition,
                                     std::uint64_t transportEpoch, bool isRunning,
                                     const PerformanceParameters& parameters, PerformanceRequests requests)
{
    const auto interrupted = transportEpoch != epoch || running != isRunning
        || (haveBar && std::abs (timing.beatsPerBar - nextTiming.beatsPerBar) > 0.0);
    const auto validateClock = !interrupted && contextValid;
    const auto clockPosition = position;
    const auto clockFrames = contextOffset;
    const auto clockBeatsPerSample = beatsPerSample;
    const auto reanchor = interrupted || !contextValid || std::abs (timing.bpm - nextTiming.bpm) > 0.0;
    if (interrupted)
    {
        interrupt();
        epoch = transportEpoch;
    }
    running = isRunning;
    timing = nextTiming;
    if (reanchor)
    {
        position = nextPosition;
        contextOffset = 0;
    }
    // The owning transport epoch identifies seeks. Preserve one sample clock
    // across same-tempo contexts so host buffer partition cannot change phase.
    display.requested = parameters;
    if (!validParameters (parameters))
    {
        fail (Error::invalidParameters);
        return;
    }
    captureRequested |= requests.capture;
    display.captureArmed = captureRequested || recording;
    display.seedPending = parameters.seed != display.applied.seed;
    if (!running)
    {
        display.state = State::waitingForHost;
        contextValid = false;
        return;
    }
    if (transportEpoch == 0 || !std::isfinite (timing.bpm) || timing.bpm <= 0.0
        || !std::isfinite (timing.beatsPerBar) || timing.beatsPerBar <= 0.0
        || !std::isfinite (nextPosition.beatsIntoBar) || nextPosition.beatsIntoBar < 0.0
        || nextPosition.beatsIntoBar > timing.beatsPerBar)
    {
        fail (Error::invalidContext);
        contextValid = false;
        return;
    }
    beatsPerSample = timing.bpm / (60.0 * sampleRate);
    if (validateClock)
    {
        // Validate the preceding clock before adopting a new tempo: a seek and
        // a tempo change in the same callback must still invalidate the source.
        const auto expectedBeats = clockPosition.beatsIntoBar + static_cast<double> (clockFrames) * clockBeatsPerSample;
        const auto expectedOffset = std::floor (expectedBeats / timing.beatsPerBar);
        if (!std::isfinite (expectedOffset) || expectedOffset < 0.0
            || expectedOffset >= static_cast<double> (std::numeric_limits<std::int64_t>::max())
            || clockPosition.bar > std::numeric_limits<std::int64_t>::max() - static_cast<std::int64_t> (expectedOffset))
        {
            fail (Error::invalidContext);
            contextValid = false;
            return;
        }
        const auto expectedBar = clockPosition.bar + static_cast<std::int64_t> (expectedOffset);
        const auto expectedPhase = expectedBeats - expectedOffset * timing.beatsPerBar;
        auto barDifference = 0;
        auto coherent = nextPosition.bar == expectedBar;
        if (!coherent && nextPosition.bar > expectedBar && expectedBar < std::numeric_limits<std::int64_t>::max()
            && nextPosition.bar == expectedBar + 1)
        {
            barDifference = 1;
            coherent = true;
        }
        else if (!coherent && nextPosition.bar < expectedBar && expectedBar > std::numeric_limits<std::int64_t>::min()
                 && nextPosition.bar == expectedBar - 1)
        {
            barDifference = -1;
            coherent = true;
        }
        const auto difference = barDifference * timing.beatsPerBar + nextPosition.beatsIntoBar - expectedPhase;
        const auto numericScale = std::max ({1.0, std::abs (expectedBeats),
            std::abs (static_cast<double> (nextPosition.bar)) * timing.beatsPerBar});
        const auto roundoff = std::min (beatsPerSample * .125, 8.0 * std::numeric_limits<double>::epsilon() * numericScale);
        // Half a sample is the nearest-sample convention, not a per-callback
        // drift allowance: compare every host report to the original clock.
        coherent &= std::abs (difference) <= beatsPerSample * .5 + roundoff;
        if (!coherent)
        {
            interrupt();
            position = nextPosition;
            contextOffset = 0;
        }
    }
    const auto framesPerBar = timing.beatsPerBar / beatsPerSample;
    contextValid = framesPerBar <= maximumBarSeconds * sampleRate && framesPerBar >= 64.0;
    const auto completedCapture = recording && haveBar
        && (nextPosition.bar != activeBar || nextPosition.beatsIntoBar >= timing.beatsPerBar - beatsPerSample * .5);
    const auto candidateFrames = completedCapture ? banks[static_cast<std::size_t> (captureBank)].count
        : display.hasSource ? banks[static_cast<std::size_t> (sourceBank)].count : 0;
    if (candidateFrames > 0)
    {
        const auto ratio = static_cast<double> (candidateFrames) / framesPerBar;
        contextValid &= ratio >= .125 && ratio <= 8.0;
    }
    if (!contextValid)
    {
        interrupt();
        display.state = State::unsupportedTiming;
    }
}

void PerformanceEngine::finishCapture()
{
    if (!recording)
        return;
    recording = false;
    if (!captureFull)
        return;
    auto& bank = banks[static_cast<std::size_t> (captureBank)];
    if (bank.count < stepsPerBar || nextGeneration == std::numeric_limits<std::uint64_t>::max())
    {
        fail (nextGeneration == std::numeric_limits<std::uint64_t>::max()
                  ? Error::sourceIdentityExhausted : Error::sourceCapacity);
        return;
    }
    for (int i = recordedStep + 1; i <= stepsPerBar; ++i)
        bank.boundaries[static_cast<std::size_t> (i)] = bank.count;
    bank.generation = nextGeneration++;
    const auto oldSource = sourceBank;
    sourceBank = captureBank;
    captureBank = 3 - oldSource - sourceBank;
    display.hasSource = true;
    display.sourceGeneration = bank.generation;
    display.sourceFrames = bank.count;
    display.sourceBpm = 60.0 * timing.beatsPerBar * sampleRate / static_cast<double> (bank.count);
    display.captureArmed = captureRequested;
    // The next capture uses the third bank; the outgoing source stays immutable.
    grains = {};
    fading = false;
    eventIndex = scheduledStep = -1;
}

void PerformanceEngine::startBar (std::int64_t bar, double phase, Result& result)
{
    result.barCompleted = haveBar && !barCompletedAtEnd;
    barCompletedAtEnd = false;
    finishCapture();
    if (display.hasSource)
    {
        const auto ratio = static_cast<double> (banks[static_cast<std::size_t> (sourceBank)].count)
            * beatsPerSample / timing.beatsPerBar;
        if (ratio < .125 || ratio > 8.0)
        {
            interrupt();
            display.state = State::unsupportedTiming;
            contextValid = false;
            return;
        }
    }
    result.barStarted = true;
    result.fullStart = phase <= beatsPerSample / timing.beatsPerBar * .51;
    activeBar = bar;
    haveBar = true;
    scheduledStep = eventIndex = -1;
    grains = {};
    display.applied.seed = display.requested.seed;
    display.seedPending = false;
    ++display.barsRendered;
    if (captureRequested && result.fullStart)
    {
        auto& bank = banks[static_cast<std::size_t> (captureBank)];
        bank.count = 0;
        bank.boundaries = {};
        bank.attacks = {};
        bank.energy = {};
        bank.rise = {};
        recordedStep = 0;
        captureEnergy = 0.0;
        recording = captureFull = true;
        captureRequested = false;
    }
    makePlan (bar);
}

void PerformanceEngine::record (StereoFrame raw, double phase)
{
    if (!recording)
        return;
    auto& bank = banks[static_cast<std::size_t> (captureBank)];
    if (bank.count >= static_cast<std::int64_t> (bank.frames.size()))
    {
        captureFull = false;
        fail (Error::sourceCapacity);
        return;
    }
    const auto step = std::clamp (static_cast<int> (std::floor (phase * stepsPerBar + 1.0e-9)), 0, stepsPerBar - 1);
    while (recordedStep < step)
    {
        ++recordedStep;
        bank.boundaries[static_cast<std::size_t> (recordedStep)] = bank.count;
        bank.attacks[static_cast<std::size_t> (recordedStep)] = bank.count;
    }
    const auto energy = std::max (static_cast<double> (raw.left) * raw.left, static_cast<double> (raw.right) * raw.right);
    const auto next = captureEnergy + captureSmoothing * (energy - captureEnergy);
    const auto rise = next - captureEnergy;
    const auto index = static_cast<std::size_t> (step);
    bank.energy[index] += energy;
    if (rise > bank.rise[index])
    {
        bank.rise[index] = rise;
        const auto preroll = static_cast<std::int64_t> (std::lround (sampleRate * .001));
        bank.attacks[index] = std::max (bank.boundaries[index], bank.count - preroll);
    }
    captureEnergy = next;
    bank.frames[static_cast<std::size_t> (bank.count++)] = raw;
}

std::uint32_t PerformanceEngine::draw (std::uint32_t tag, std::uint32_t ordinal) const
{
    auto value = display.applied.seed ^ tag ^ (ordinal * 0x9e3779b9U);
    value ^= value >> 16;
    value *= 0x7feb352dU;
    value ^= value >> 15;
    value *= 0x846ca68bU;
    return value ^ (value >> 16);
}

void PerformanceEngine::makePlan (std::int64_t bar)
{
    display.applied.variation = display.requested.variation;
    display.applied.density = display.requested.density;
    display.applied.fill = display.requested.fill;
    planCount = 0;
    if (!display.hasSource)
        return;
    const auto& bank = banks[static_cast<std::size_t> (sourceBank)];
    const auto density = display.applied.fill ? 1.0f : display.applied.density;
    const auto stride = density < .25f ? 8 : density < .625f ? 4 : density < .875f ? 2 : 1;
    // Every fourth host bar gives Fill a different, repeatable destination.
    const auto phrasePhase = static_cast<std::uint32_t> ((bar % 4 + 4) % 4);
    for (int step = 0; step < stepsPerBar; step += stride)
    {
        auto sourceStep = stride < 4 ? step - step % 4 : step;
        auto role = step % 8 == 0 ? Role::anchor : stride < 4 ? Role::repeat : Role::original;
        const auto ordinal = static_cast<std::uint32_t> (step) + 32U * phrasePhase;
        const auto threshold = static_cast<double> (display.applied.variation) * std::numeric_limits<std::uint32_t>::max();
        if (step % 8 != 0 && draw (0x504c414eU, ordinal) < threshold)
        {
            const auto group = display.applied.variation < .5f ? (step / 8) * 8 : 0;
            const auto range = display.applied.variation < .5f ? 8U : 32U;
            // Prefer the stronger of two reproducible local source candidates.
            const auto a = group + static_cast<int> (draw (0x534f5552U, ordinal) % range);
            const auto b = group + static_cast<int> (draw (0x454e4552U, ordinal) % range);
            sourceStep = bank.energy[static_cast<std::size_t> (a)] >= bank.energy[static_cast<std::size_t> (b)] ? a : b;
            role = Role::variation;
        }
        const auto sourceIndex = static_cast<std::size_t> (sourceStep);
        const auto sourceOffset = role == Role::variation ? bank.attacks[sourceIndex] : bank.boundaries[sourceIndex];
        const auto gate = density < .5f ? .4f + density * 1.2f : 1.0f;
        const auto span = std::max (bank.count / 8, static_cast<std::int64_t> (std::ceil (static_cast<double> (bank.count) * stride * gate / stepsPerBar)));
        const auto end = std::min (bank.count, sourceOffset + std::max<std::int64_t> (1, span));
        plan[planCount++] = {step, step + stride, bank.generation, sourceOffset, end, gate, role};
    }
}

void PerformanceEngine::startEvent (int step, double phase)
{
    int next = -1;
    for (std::size_t i = 0; i < planCount; ++i)
        if (plan[i].startStep <= step && plan[i].endStep > step)
            next = static_cast<int> (i);
    if (next < 0 || next == eventIndex)
        return;
    const auto& event = plan[static_cast<std::size_t> (next)];
    const auto& bank = banks[static_cast<std::size_t> (sourceBank)];
    const auto nextPosition = continuousRhythm (display.applied) ? phase * static_cast<double> (bank.count)
        : static_cast<double> (event.sourceOffset)
            + (phase * stepsPerBar - event.startStep) * static_cast<double> (bank.count) / stepsPerBar;
    previousBank = mainBank;
    const auto oldFrames = banks[static_cast<std::size_t> (previousBank)].count;
    previousPosition = haveVoice && oldFrames > 0 ? std::fmod (mainPosition, static_cast<double> (oldFrames)) : 0.0;
    fading = haveVoice && (previousBank != sourceBank || std::abs (previousPosition - nextPosition) > 1.0e-6);
    mainBank = sourceBank;
    haveVoice = true;
    crossfadeAge = 0;
    eventIndex = next;
    eventAge = 0;
    nextGrainAge = attackFrames;
    grains = {};
}

StereoFrame PerformanceEngine::read (double coordinate, int selectedBank, double rate, GrainResampler::Kernel kernel) const
{
    const auto& bank = banks[static_cast<std::size_t> (selectedBank)];
    coordinate = snapInteger (coordinate);
    if (coordinate < 0.0 || coordinate >= static_cast<double> (bank.count))
        return {};
    const auto index = static_cast<std::int64_t> (coordinate);
    if (std::abs (rate - 1.0) < 1.0e-12 && std::abs (coordinate - static_cast<double> (index)) < 1.0e-12)
        return bank.frames[static_cast<std::size_t> (index)];
    const auto fractionalPhase = (coordinate - static_cast<double> (index)) * GrainResampler::phaseCount;
    const auto row = static_cast<int> (fractionalPhase);
    const auto blend = static_cast<float> (fractionalPhase - row);
    const auto* a = kernel.coefficients + row * kernel.taps;
    const auto* b = a + kernel.taps;
    const auto guard = static_cast<std::int64_t> (kernel.leftGuard);
    StereoFrame result {};
    for (int i = 0; i < kernel.taps; ++i)
    {
        const auto address = std::clamp (index - guard + static_cast<std::int64_t> (i), std::int64_t {0}, bank.count - 1);
        const auto sample = bank.frames[static_cast<std::size_t> (address)];
        const auto coefficient = a[i] + blend * (b[i] - a[i]);
        result.left += sample.left * coefficient;
        result.right += sample.right * coefficient;
    }
    return result;
}

void PerformanceEngine::launchGrain (const Event& event, double basePosition, int protectedAttack)
{
    auto found = std::find_if (grains.begin(), grains.end(), [] (const Voice& voice) { return !voice.active; });
    if (found == grains.end())
    {
        fail (Error::sourceCapacity);
        return;
    }
    const auto requestedLength = static_cast<int> (std::lround (sampleRate * (.012 + .078 * smoothedCharacter)));
    const auto sourceAttack = static_cast<std::int64_t> (std::ceil (protectedAttack * mainRate));
    const auto start = std::max (static_cast<double> (event.sourceOffset + sourceAttack), basePosition - requestedLength * .35 * mainRate);
    const auto available = static_cast<int> (std::max (0.0, std::floor ((static_cast<double> (event.sourceEnd) - start) / mainRate)));
    const auto length = std::min (requestedLength, available);
    if (length < 2)
        return;
    *found = {true, start, mainRate, 0, length, event.sourceEnd};
    ++display.grainsLaunched;
}

StereoFrame PerformanceEngine::render (double phase)
{
    const auto& bank = banks[static_cast<std::size_t> (sourceBank)];
    const auto nominalRate = static_cast<double> (bank.count) * beatsPerSample / timing.beatsPerBar;
    mainRate = snapInteger (nominalRate);
    if (std::abs (mainRate - cachedMainRate) > 1.0e-12)
    {
        mainKernel = resampler.kernelFor (mainRate);
        cachedMainRate = mainRate;
    }
    if (eventIndex < 0)
        return {};
    const auto& event = plan[static_cast<std::size_t> (eventIndex)];
    mainPosition = snapInteger (continuousRhythm (display.applied) ? phase * static_cast<double> (bank.count)
        : static_cast<double> (event.sourceOffset)
            + (phase * stepsPerBar - event.startStep) * static_cast<double> (bank.count) / stepsPerBar);
    const auto eventPhase = phase * stepsPerBar - event.startStep;
    const auto gateEnd = static_cast<double> (event.endStep - event.startStep) * event.gate;
    const auto gateOpen = eventPhase < gateEnd;
    auto frame = gateOpen ? read (mainPosition, sourceBank, mainRate, mainKernel) : StereoFrame {};
    if (event.gate < .999999f || event.sourceOffset + (event.endStep - event.startStep) * bank.count / stepsPerBar > bank.count)
    {
        const auto toGate = (gateEnd - eventPhase) * static_cast<double> (bank.count) / stepsPerBar / mainRate;
        const auto toSourceEnd = (static_cast<double> (event.sourceEnd) - mainPosition) / mainRate;
        const auto release = static_cast<float> (std::clamp (std::min (toGate, toSourceEnd) / crossfadeFrames, 0.0, 1.0));
        frame.left *= release;
        frame.right *= release;
    }
    if (fading)
    {
        const auto amount = static_cast<float> (crossfadeAge + 1) / static_cast<float> (crossfadeFrames);
        previousRate = snapInteger (static_cast<double> (banks[static_cast<std::size_t> (previousBank)].count)
                                    * beatsPerSample / timing.beatsPerBar);
        if (std::abs (previousRate - cachedPreviousRate) > 1.0e-12)
        {
            previousKernel = resampler.kernelFor (previousRate);
            cachedPreviousRate = previousRate;
        }
        const auto oldFrames = static_cast<double> (banks[static_cast<std::size_t> (previousBank)].count);
        previousPosition = std::fmod (previousPosition, oldFrames);
        frame = mix (read (previousPosition, previousBank, previousRate, previousKernel), frame, amount);
        previousPosition += previousRate;
        if (++crossfadeAge >= crossfadeFrames)
            fading = false;
    }
    const auto protectedAttack = static_cast<int> (std::ceil (attackFrames / std::min (1.0, mainRate)));
    if (eventAge >= protectedAttack && smoothedGrain > 1.0e-5)
    {
        if (eventAge >= nextGrainAge)
        {
            launchGrain (event, mainPosition, protectedAttack);
            // A fixed 45ms launch interval and <=90ms duration strictly bound
            // overlap to three voices even while Character moves continuously.
            nextGrainAge = eventAge + std::max (1, static_cast<int> (std::lround (sampleRate * .045)));
        }
        StereoFrame cloud {};
        auto weight = 0.0f;
        for (auto& voice : grains)
        {
            if (!voice.active)
                continue;
            const auto envelope = static_cast<float> (.5 - .5 * std::cos (2.0 * pi * voice.age / (voice.length - 1)));
            const auto grain = read (voice.position, sourceBank, mainRate, mainKernel);
            cloud.left += grain.left * envelope;
            cloud.right += grain.right * envelope;
            weight += envelope;
            voice.position += mainRate;
            voice.rate = mainRate;
            if (++voice.age >= voice.length || voice.position >= static_cast<double> (voice.end))
                voice.active = false;
        }
        if (weight > 1.0e-6f)
        {
            cloud.left /= weight;
            cloud.right /= weight;
            const auto onset = std::clamp (static_cast<double> (eventAge - protectedAttack) / std::max (1, crossfadeFrames), 0.0, 1.0);
            frame = mix (frame, cloud, static_cast<float> (smoothedGrain * onset * .85 * std::min (1.0f, weight)));
        }
    }
    mainPosition += mainRate;
    ++eventAge;
    display.activeGrains = static_cast<int> (std::count_if (grains.begin(), grains.end(), [] (const Voice& voice) { return voice.active; }));
    return frame;
}

PerformanceEngine::Result PerformanceEngine::processOne (StereoFrame raw)
{
    Result result {raw};
    result.discontinuity = std::exchange (discontinuityPending, false);
    if (!std::isfinite (raw.left) || !std::isfinite (raw.right))
        fail (Error::invalidContext);
    if (display.error != Error::none)
    {
        result.frame = {};
        return result;
    }
    if (!running || !contextValid)
        return result;
    if (contextOffset == std::numeric_limits<std::int64_t>::max())
    {
        fail (Error::invalidContext);
        result.frame = {};
        return result;
    }
    const auto beats = position.beatsIntoBar + static_cast<double> (contextOffset++) * beatsPerSample;
    const auto offset = std::floor ((beats + beatsPerSample * .5) / timing.beatsPerBar);
    if (!std::isfinite (offset) || offset < 0.0 || offset >= static_cast<double> (std::numeric_limits<std::int64_t>::max()))
    {
        fail (Error::invalidContext);
        result.frame = {};
        return result;
    }
    const auto barOffset = static_cast<std::int64_t> (offset);
    if (position.bar > std::numeric_limits<std::int64_t>::max() - barOffset)
    {
        fail (Error::invalidContext);
        result.frame = {};
        return result;
    }
    const auto bar = position.bar + barOffset;
    const auto phase = std::max (0.0, (beats - static_cast<double> (barOffset) * timing.beatsPerBar) / timing.beatsPerBar);
    if (!haveBar || bar != activeBar)
        startBar (bar, phase, result);
    if (!contextValid)
        return result;
    record (raw, phase);
    smoothedGrain += smoothing * (display.requested.grain - smoothedGrain);
    smoothedCharacter += smoothing * (display.requested.character - smoothedCharacter);
    display.applied.grain = static_cast<float> (smoothedGrain);
    display.applied.character = static_cast<float> (smoothedCharacter);
    display.playhead = phase;
    const auto step = std::clamp (static_cast<int> (std::floor (phase * stepsPerBar + 1.0e-9)), 0, stepsPerBar - 1);
    display.currentStep = step;
    if (step != scheduledStep)
    {
        // Preserve the current voice when an unrelated parameter is touched.
        if (step % 4 == 0 && (std::abs (display.applied.variation - display.requested.variation) > 1.0e-7f
            || std::abs (display.applied.density - display.requested.density) > 1.0e-7f || display.applied.fill != display.requested.fill))
        {
            makePlan (bar);
            eventIndex = -2;
        }
        scheduledStep = step;
        startEvent (step, phase);
    }
    if (display.hasSource)
        result.frame = render (phase);
    // Use the same nearest-sample boundary convention as startBar. A completed
    // capture is available at the exclusive end, even with no following sample.
    const auto exclusiveBeats = position.beatsIntoBar + static_cast<double> (contextOffset) * beatsPerSample;
    const auto exclusiveOffset = std::floor ((exclusiveBeats + beatsPerSample * .5) / timing.beatsPerBar);
    if (exclusiveOffset > offset)
    {
        result.barCompletedAfter = true;
        barCompletedAtEnd = true;
        finishCapture();
    }
    if (display.error == Error::none)
        display.state = recording ? State::capturing : display.hasSource ? State::playing : State::waitingForBar;
    display.captureArmed = captureRequested || recording;
    return result;
}
} // namespace livecut::engine
