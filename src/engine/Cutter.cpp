// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Livecut, Copyright 2003-2004 Remy Muller.

#include "engine/Cutter.h"
#include "engine/PhraseSeed.h"
#include "engine/SwingGrid.h"
#include "engine/Accent.h"
#include <algorithm>

#include <cassert>
#include <cmath>

namespace livecut::engine
{

namespace
{
// A host position this many beats away from where the previous buffer ended
// is a jump: a loop, a relocation or a restart.
constexpr double jumpToleranceBeats = 1.0e-3;

bool hasSwingWarp (double swing, double beatsPerBar)
{
    // SwingGrid only warps complete pairs of eighths (one quarter note).
    return swing > 50.0 && beatsPerBar >= 1.0;
}
} // namespace

Cutter::Cutter()
{
    cuts.reserve (maxCutsPerBlock);
}

void Cutter::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    livePlayer.prepare (static_cast<int> (std::ceil (maxBarSeconds * sampleRate)));
    bitCrusher.prepare (sampleRate);
    combFilter.prepare (sampleRate);
    setFadeMilliseconds (fadeMilliseconds);
    reset();
}

void Cutter::setProcedure (Procedure newProcedure)
{
    pendingProcedure = newProcedure;
}

void Cutter::setUnitsPerBar (int newUnitsPerBar)
{
    assert (newUnitsPerBar > 0);
    pendingUnitsPerBar = newUnitsPerBar;
}

void Cutter::setParameters (const CutParameters& newParameters)
{
    params = newParameters;
}

void Cutter::setCrusher (const CrusherParameters& parameters)
{
    crusherParams = parameters;
}

void Cutter::setComb (const CombParameters& parameters)
{
    combParams = parameters;
}

void Cutter::setSeed (std::uint32_t newSeed)
{
    seed = newSeed;
}

void Cutter::setSwing (double percent)
{
    assert (std::isfinite (percent) && percent >= 50.0 && percent <= 75.0);
    pendingSwing = percent;
}
void Cutter::setAccent (float amount)
{
    assert (std::isfinite (amount) && amount >= 0.0f && amount <= 1.0f);
    accent = amount;
}

void Cutter::setFadeMilliseconds (double milliseconds)
{
    fadeMilliseconds = milliseconds;
    livePlayer.setFadeSamples (static_cast<int> (std::lround (milliseconds * sampleRate / 1000.0)));
}

void Cutter::reset()
{
    resetRendering();
    livePlayer.resetSource();
}

void Cutter::resetRendering()
{
    cutting = false;
    phraseCompletionNotified = false;
    havePosition = false;
    emittedBar = false;
    queueHead = queued = 0;
    hasActivePlan = false;
    processingError = Error::none;
    livePlayer.resetRendering();
}

void Cutter::process (float* left, float* right, int numSamples, const Timing& timing, const Position& position, Observer* observer)
{
    timingSupported = timing.bpm > 0.0
                   && timing.beatsPerBar > 0.0
                   && timing.beatsPerBar * 60.0 / timing.bpm <= maxBarSeconds;

    if (! timingSupported)
    {
        reset();
        if (observer != nullptr)
        {
            observer->discontinuity (0);
            for (int i = 0; i < numSamples; ++i)
                observer->processOutput (i, left[i], right[i], left[i], right[i]);
        }
        return; // the input passes through untouched
    }

    // Count a position outside its bar (a host's stale bar start, or a count-in
    // before bar 0) into the bar it actually falls in.
    const auto barsOver = std::floor (position.beatsIntoBar / timing.beatsPerBar);
    auto bar = position.bar + static_cast<std::int64_t> (barsOver);
    auto beats = position.beatsIntoBar - barsOver * timing.beatsPerBar;

    const auto meterChanged = havePosition && std::abs (previousBeatsPerBar - timing.beatsPerBar) > 1.0e-9;
    if (havePosition)
    {
        const auto drift = static_cast<double> (bar - expectedBar) * timing.beatsPerBar + (beats - expectedBeatsIntoBar);

        if (std::abs (drift) > jumpToleranceBeats)
            reset();
    }

    // A queued swung onset belongs to its original meter, including when the
    // new bar has no complete eighth pair. Identity-to-identity changes keep
    // the 0.3 path.
    if (meterChanged && (hasSwingWarp (swing, previousBeatsPerBar) || hasSwingWarp (swing, timing.beatsPerBar)))
        resetRendering();

    if (observer != nullptr && (! havePosition || meterChanged))
        observer->discontinuity (0);
    previousBeatsPerBar = timing.beatsPerBar;

    if (cutting && ! hasSwingWarp (swing, timing.beatsPerBar))
    {
        livePlayer.setSamplesPerUnit (samplesPerUnit (timing));

        // Re-timing can finish a cut early; set the effects up for the one now playing.
        if (livePlayer.currentCut() != effectsCut)
            startEffectsCut();
    }

    const auto beatsPerSample = timing.bpm / 60.0 / sampleRate;
    const auto halfSampleBeats = 0.5 * beatsPerSample;

    for (int i = 0; i < numSamples; ++i)
    {
        const auto dryLeft = left[i];
        const auto dryRight = right[i];
        planningSource = livePlayer.sourceForNextFrame();

        // A sample within half a sample of the bar line starts the next bar.
        if (beats >= timing.beatsPerBar - halfSampleBeats)
        {
            beats -= timing.beatsPerBar;
            ++bar;
        }

        // While nothing is being cut, structural changes take effect at once,
        // so the start position below is counted in the new subdivision.
        if (! cutting)
            applyStructuralChanges();

        const auto unitsPerBeat = unitsPerBar / timing.beatsPerBar;
        const auto unitsIntoBar = beats * unitsPerBeat;
        const auto halfSample = halfSampleBeats * unitsPerBeat;

        const auto newBar = unitsIntoBar < halfSample && (! emittedBar || lastEmittedBar != bar);
        const auto phraseEnding = cutting && (static_cast<double> (bar - phraseBar) * unitsPerBar + unitsIntoBar >= phraseBars * unitsPerBar - halfSample);
        if (phraseEnding && ! phraseCompletionNotified)
        {
            if (observer != nullptr)
                observer->phraseCompleted (i);
            phraseCompletionNotified = true;
        }
        if (newBar)
        {
            emittedBar = true;
            lastEmittedBar = bar;
            if (observer != nullptr)
                observer->barStarted (i, bar);
        }

        if (! cutting)
        {
            // Phrases may begin on any unit boundary, counted from the start of the bar.
            const auto boundary = std::ceil (unitsIntoBar - halfSample);

            if (boundary < unitsIntoBar + halfSample)
            {
                startPhrase (bar, static_cast<int> (boundary), timing);
                if (observer != nullptr)
                    observer->phraseStarted (i, boundary == 0.0);
            }
        }
        else
        {
            const auto barsIntoPhrase = static_cast<int> (bar - phraseBar);
            const auto unitsIntoPhrase = barsIntoPhrase * unitsPerBar + unitsIntoBar;
            const auto structuralChange = pendingProcedure != procedure || pendingUnitsPerBar != unitsPerBar || std::abs (pendingSwing - swing) > 1.0e-9;

            if (unitsIntoPhrase >= phraseBars * unitsPerBar - halfSample)
            {
                if (observer != nullptr && ! phraseCompletionNotified)
                    observer->phraseCompleted (i);
                startPhrase (bar, 0, timing);
                if (observer != nullptr)
                    observer->phraseStarted (i, true);
            }
            else if (structuralChange && unitsIntoBar < halfSample)
            {
                applyStructuralChanges();
                blockEnd = barsIntoPhrase * unitsPerBar;
                planBlock (timing);
            }
            else if (unitsIntoPhrase >= blockEnd - halfSample)
            {
                planBlock (timing);
            }
        }

        livePlayer.ingestFrame (dryLeft, dryRight);
        activatePlans (bar, beats, halfSampleBeats, timing);
        if (cutting && hasActivePlan && processingError == Error::none)
        {
            if (hasSwingWarp (swing, timing.beatsPerBar))
                renderSwung (left[i], right[i], bar, beats, beatsPerSample, timing);
            else
                livePlayer.renderFrame (left[i], right[i]);
            if (livePlayer.hasSourceError())
                processingError = Error::sourceUnavailable;

            // The effects always run, so switching one on picks up a current
            // delay line and hold rather than stale audio.
            auto crushedLeft = left[i];
            auto crushedRight = right[i];
            bitCrusher.process (crushedLeft, crushedRight);

            if (crusherParams.enabled)
            {
                left[i] = crushedLeft;
                right[i] = crushedRight;
            }

            auto combedLeft = left[i];
            auto combedRight = right[i];
            combFilter.process (combedLeft, combedRight);

            if (combParams.enabled)
            {
                left[i] = combedLeft;
                right[i] = combedRight;
            }

            if (livePlayer.currentCut() != effectsCut)
                startEffectsCut();
        }

        if (processingError != Error::none)
            left[i] = right[i] = 0.0f;
        if (observer != nullptr)
            observer->processOutput (i, dryLeft, dryRight, left[i], right[i]);

        beats += beatsPerSample;
    }

    if (beats >= timing.beatsPerBar - halfSampleBeats)
    {
        beats -= timing.beatsPerBar;
        ++bar;
    }

    havePosition = true;
    expectedBar = bar;
    expectedBeatsIntoBar = beats;

    if (cutting)
    {
        phrasePosition = static_cast<double> ((bar - phraseBar) * unitsPerBar) + beats * unitsPerBar / timing.beatsPerBar;
        if (! phraseCompletionNotified && phrasePosition >= phraseBars * unitsPerBar - halfSampleBeats * unitsPerBar / timing.beatsPerBar)
        {
            if (observer != nullptr)
                observer->phraseCompleted (numSamples);
            phraseCompletionNotified = true;
        }
    }
}

double Cutter::samplesPerUnit (const Timing& timing) const
{
    return sampleRate * 60.0 / timing.bpm * timing.beatsPerBar / unitsPerBar;
}

CutProcedure& Cutter::procedureFor (Procedure p)
{
    switch (p)
    {
        case Procedure::cutProc11: return cutProc11;
        case Procedure::warpCut:   return warpCut;
        case Procedure::sqPusher:  return sqPusher;
        case Procedure::bounce:    return bounce;
    }

    assert (false);
    return cutProc11;
}

void Cutter::applyStructuralChanges()
{
    if (pendingProcedure != procedure)
    {
        procedureFor (pendingProcedure).reset();
        procedure = pendingProcedure;
    }

    unitsPerBar = pendingUnitsPerBar;
    swing = pendingSwing;
}

void Cutter::startPhrase (std::int64_t bar, int unitsIntoBar, const Timing& timing)
{
    applyStructuralChanges();

    // Reseeding on every phrase makes the cuts depend only on the seed and the position.
    random.reseed (phraseSeed (seed, bar));
    phraseBar = bar;
    phraseBars = procedureFor (procedure).choosePhraseBars (random, params);
    cutOrdinal = 0;
    assert (phraseBars >= 1);

    cutting = true;
    phraseCompletionNotified = false;
    blockEnd = unitsIntoBar;
    planBlock (timing);
}

void Cutter::planBlock (const Timing& timing)
{
    blockStart = blockEnd;

    BlockContext context;
    context.unitsDone = blockStart;
    context.phraseUnits = static_cast<double> (phraseBars * unitsPerBar);
    context.unitsPerBar = unitsPerBar;
    context.beatsPerBar = timing.beatsPerBar;

    procedureFor (procedure).chooseBlock (random, params, context, cuts);
    blockEnd = blockStart + totalUnits (cuts);

    if (queued == plans.size())
    {
        processingError = Error::planOverflow;
        return;
    }
    auto& plan = plans[(queueHead + queued) % plans.size()];
    plan.count = cuts.size();
    plan.start = blockStart;
    plan.end = blockEnd;
    plan.source = planningSource;
    plan.phraseBar = phraseBar;
    plan.unitsPerBar = unitsPerBar;
    const auto intoBar = blockStart - std::floor (blockStart / unitsPerBar) * unitsPerBar;
    plan.activationBar = phraseBar + static_cast<std::int64_t> (std::floor (blockStart / unitsPerBar));
    plan.activationBeat = *SwingGrid::toPhysical (std::clamp (intoBar * timing.beatsPerBar / unitsPerBar, 0.0, timing.beatsPerBar), timing.beatsPerBar, swing);
    for (std::size_t i = 0; i < cuts.size(); ++i)
    {
        plan.cuts[i] = cuts[i];
        if (accent > 0.0f)
            plan.cuts[i].amp *= accentGain (accent, seed, phraseBar, cutOrdinal);
        ++cutOrdinal;
    }

    // The effects get their settings before the block's first cut starts. They
    // are drawn whether or not an effect is on, so toggling one never changes the cuts.
    const auto startBits = random.uniform (crusherParams.minBits, crusherParams.maxBits);
    const auto endBits = random.uniform (crusherParams.minBits, crusherParams.maxBits);
    const auto startHz = random.uniform (crusherParams.minHz, crusherParams.maxHz);
    const auto endHz = random.uniform (crusherParams.minHz, crusherParams.maxHz);
    const auto startMs = random.uniform (combParams.minDelayMs, combParams.maxDelayMs);
    const auto endMs = random.uniform (combParams.minDelayMs, combParams.maxDelayMs);
    plan.effects = { startBits, endBits, startHz, endHz, startMs, endMs };
    plan.comb = combParams;
    ++queued;
    maximumQueued = std::max (maximumQueued, queued);
}

void Cutter::activatePlans (std::int64_t bar, double beats, double halfSample, const Timing& timing)
{
    while (queued > 0)
    {
        const auto& next = plans[queueHead];
        if (next.activationBar > bar || (next.activationBar == bar && next.activationBeat > beats + halfSample))
            return;
        activePlan = next;
        queueHead = (queueHead + 1) % plans.size();
        --queued;
        hasActivePlan = true;
        livePlayer.startBlock ({ activePlan.cuts.data(), activePlan.count }, samplesPerUnit (timing), activePlan.source);
        const auto& fx = activePlan.effects;
        bitCrusher.setBlock (fx[0], fx[1], fx[2], fx[3]);
        combFilter.setBlock (activePlan.comb, fx[4], fx[5]);
        startEffectsCut();
    }
}

double Cutter::physicalDistance (double phraseUnits, std::int64_t bar, double beats, const Timing& timing) const
{
    const auto bars = std::floor (phraseUnits / activePlan.unitsPerBar);
    const auto logicalBeat = (phraseUnits - bars * activePlan.unitsPerBar) * timing.beatsPerBar / activePlan.unitsPerBar;
    const auto physical = *SwingGrid::toPhysical (std::clamp (logicalBeat, 0.0, timing.beatsPerBar), timing.beatsPerBar, swing);
    return static_cast<double> (activePlan.phraseBar + static_cast<std::int64_t> (bars) - bar) * timing.beatsPerBar + physical - beats;
}

void Cutter::renderSwung (float& left, float& right, std::int64_t bar, double beats, double beatsPerSample, const Timing& timing)
{
    // Inverse-map the half-sample lookahead too: the decision follows musical
    // progress, not accumulated samples divided by a later tempo.
    const auto near = *SwingGrid::toLogical (std::clamp (beats + 0.5 * beatsPerSample, 0.0, timing.beatsPerBar), timing.beatsPerBar, swing);
    const auto units = static_cast<double> (bar - activePlan.phraseBar) * activePlan.unitsPerBar + near * activePlan.unitsPerBar / timing.beatsPerBar;
    livePlayer.advanceToUnits (units - activePlan.start);
    if (livePlayer.currentCut() != effectsCut)
        startEffectsCut();
    if (livePlayer.currentCut() < livePlayer.cuts().size())
    {
        const auto& cut = livePlayer.cuts()[livePlayer.currentCut()];
        const auto start = activePlan.start + cut.logicalStart;
        const auto toEnd = physicalDistance (start + cut.units, bar, beats, timing);
        const auto toGate = physicalDistance (start + cut.units * cut.gate, bar, beats, timing);
        livePlayer.retimeMusicalCut (static_cast<int> (std::lround (toEnd / beatsPerSample)), static_cast<int> (std::lround (toGate / beatsPerSample)));
    }
    livePlayer.renderFrame (left, right, false);
}

void Cutter::startEffectsCut()
{
    // Set up the effects for the cut that plays next, so their state always
    // matches it. Once a block's cuts run out they keep the last cut's settings.
    effectsCut = livePlayer.currentCut();

    if (effectsCut < livePlayer.cuts().size())
    {
        bitCrusher.startCut (effectsCut, livePlayer.cuts().size());
        combFilter.startCut (effectsCut, livePlayer.cuts().size());
    }
}

} // namespace livecut::engine
