// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Livecut, Copyright 2003-2004 Remy Muller.

#include "engine/Cutter.h"
#include "engine/PhraseSeed.h"

#include <cassert>
#include <cmath>

namespace livecut::engine
{

namespace
{
// A host position this many beats away from where the previous buffer ended
// is a jump: a loop, a relocation or a restart.
constexpr double jumpToleranceBeats = 1.0e-3;
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

void Cutter::setFadeMilliseconds (double milliseconds)
{
    fadeMilliseconds = milliseconds;
    livePlayer.setFadeSamples (static_cast<int> (std::lround (milliseconds * sampleRate / 1000.0)));
}

void Cutter::reset()
{
    cutting = false;
    havePosition = false;
}

void Cutter::process (float* left, float* right, int numSamples, const Timing& timing, const Position& position)
{
    timingSupported = timing.bpm > 0.0
                   && timing.beatsPerBar > 0.0
                   && timing.beatsPerBar * 60.0 / timing.bpm <= maxBarSeconds;

    if (! timingSupported)
    {
        reset();
        return; // the input passes through untouched
    }

    // Count a position outside its bar (a host's stale bar start, or a count-in
    // before bar 0) into the bar it actually falls in.
    const auto barsOver = std::floor (position.beatsIntoBar / timing.beatsPerBar);
    auto bar = position.bar + static_cast<std::int64_t> (barsOver);
    auto beats = position.beatsIntoBar - barsOver * timing.beatsPerBar;

    if (havePosition)
    {
        const auto drift = static_cast<double> (bar - expectedBar) * timing.beatsPerBar + (beats - expectedBeatsIntoBar);

        if (std::abs (drift) > jumpToleranceBeats)
            reset();
    }

    if (cutting)
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

        if (! cutting)
        {
            // Phrases may begin on any unit boundary, counted from the start of the bar.
            const auto boundary = std::ceil (unitsIntoBar - halfSample);

            if (boundary < unitsIntoBar + halfSample)
                startPhrase (bar, static_cast<int> (boundary), timing);
        }
        else
        {
            const auto barsIntoPhrase = static_cast<int> (bar - phraseBar);
            const auto unitsIntoPhrase = barsIntoPhrase * unitsPerBar + unitsIntoBar;
            const auto structuralChange = pendingProcedure != procedure || pendingUnitsPerBar != unitsPerBar;

            if (unitsIntoPhrase >= phraseBars * unitsPerBar - halfSample)
            {
                startPhrase (bar, 0, timing);
            }
            else if (structuralChange && unitsIntoBar < halfSample)
            {
                applyStructuralChanges();
                blockEnd = barsIntoPhrase * unitsPerBar;
                startBlock (timing);
            }
            else if (unitsIntoPhrase >= blockEnd - halfSample)
            {
                startBlock (timing);
            }
        }

        if (cutting)
        {
            livePlayer.process (left[i], right[i]);

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
        phrasePosition = static_cast<double> ((bar - phraseBar) * unitsPerBar) + beats * unitsPerBar / timing.beatsPerBar;
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
}

void Cutter::startPhrase (std::int64_t bar, int unitsIntoBar, const Timing& timing)
{
    applyStructuralChanges();

    // Reseeding on every phrase makes the cuts depend only on the seed and the position.
    random.reseed (phraseSeed (seed, bar));
    phraseBar = bar;
    phraseBars = procedureFor (procedure).choosePhraseBars (random, params);
    assert (phraseBars >= 1);

    cutting = true;
    blockEnd = unitsIntoBar;
    startBlock (timing);
}

void Cutter::startBlock (const Timing& timing)
{
    blockStart = blockEnd;

    BlockContext context;
    context.unitsDone = blockStart;
    context.phraseUnits = static_cast<double> (phraseBars * unitsPerBar);
    context.unitsPerBar = unitsPerBar;
    context.beatsPerBar = timing.beatsPerBar;

    procedureFor (procedure).chooseBlock (random, params, context, cuts);
    blockEnd = blockStart + totalUnits (cuts);

    livePlayer.startBlock (cuts, samplesPerUnit (timing));

    // The effects get their settings before the block's first cut starts. They
    // are drawn whether or not an effect is on, so toggling one never changes the cuts.
    const auto startBits = random.uniform (crusherParams.minBits, crusherParams.maxBits);
    const auto endBits = random.uniform (crusherParams.minBits, crusherParams.maxBits);
    const auto startHz = random.uniform (crusherParams.minHz, crusherParams.maxHz);
    const auto endHz = random.uniform (crusherParams.minHz, crusherParams.maxHz);
    const auto startMs = random.uniform (combParams.minDelayMs, combParams.maxDelayMs);
    const auto endMs = random.uniform (combParams.minDelayMs, combParams.maxDelayMs);
    bitCrusher.setBlock (startBits, endBits, startHz, endHz);
    combFilter.setBlock (combParams, startMs, endMs);
    startEffectsCut();
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
