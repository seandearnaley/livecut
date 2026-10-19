// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Livecut, Copyright 2003-2004 Remy Muller.

#include "engine/Effects.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace livecut::engine
{

void BitCrusher::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    phase = 0.0;
    heldLeft = heldRight = 0.0f;
    startCut (0, 1);
}

void BitCrusher::setBlock (double newStartBits, double newEndBits, double newStartHz, double newEndHz)
{
    startBits = newStartBits;
    endBits = newEndBits;
    startHz = newStartHz;
    endHz = newEndHz;
}

void BitCrusher::startCut (std::size_t cut, std::size_t cuts)
{
    currentBits = sweepSetting (startBits, endBits, cut, cuts);

    // As BBCut's CutBit1: n bits divide full scale (-1 to 1) into steps of 2^(1 - n).
    // A float sample only carries 24 bits, so finer steps would just add rounding error.
    step = currentBits < 24.0 ? std::pow (0.5, currentBits - 1.0) : 0.0;

    hold = std::max (sampleRate / sweepSetting (startHz, endHz, cut, cuts), 1.0);
}

void BitCrusher::process (float& left, float& right)
{
    // Sample and hold: take a new sample every `hold` samples, carrying the fraction over.
    if (phase <= 0.0)
    {
        const auto quantise = [this] (float x)
        {
            return step > 0.0 ? static_cast<float> (std::round (x / step) * step) : x;
        };

        heldLeft = quantise (left);
        heldRight = quantise (right);
        phase += hold;
    }

    phase -= 1.0;
    left = heldLeft;
    right = heldRight;
}

void Comb::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;
    const auto length = static_cast<std::size_t> (std::ceil (maxDelayMs / 1000.0 * sampleRate)) + 2;
    lineLeft.assign (length, 0.0f);
    lineRight.assign (length, 0.0f);
    write = 0;

    // Livecut smooths the feedback comb's delay so it reaches 66% of a change in 40 ms.
    smoothing = std::exp (std::log (1.0 - 0.66) / (0.040 * sampleRate));
    smootherPrimed = false;
}

void Comb::setBlock (const CombParameters& params, double newStartMs, double newEndMs)
{
    type = params.type;
    feedback = params.feedback;
    startMs = newStartMs;
    endMs = newEndMs;
}

void Comb::startCut (std::size_t cut, std::size_t cuts)
{
    // The parameter range keeps delays within the line; clamping keeps reads inside it regardless.
    const auto maxDelay = static_cast<double> (lineLeft.size() - 2);
    const auto delay = sweepSetting (startMs, endMs, cut, cuts) / 1000.0 * sampleRate;
    assert (delay >= 1.0 && delay <= maxDelay);
    targetDelay = std::clamp (delay, 1.0, maxDelay);

    if (! smootherPrimed)
    {
        smoothedDelay = targetDelay;
        smootherPrimed = true;
    }
}

float Comb::read (const std::vector<float>& line, double delay) const
{
    // `write` is where the next sample goes, so a delay of d reads the sample written d samples ago.
    const auto size = static_cast<double> (line.size());
    auto position = static_cast<double> (write) - delay;

    if (position < 0.0)
        position += size;

    const auto index = static_cast<std::size_t> (position);
    const auto next = (index + 1) % line.size();
    const auto fraction = static_cast<float> (position - static_cast<double> (index));
    return line[index] + fraction * (line[next] - line[index]);
}

void Comb::process (float& left, float& right)
{
    if (type == CombParameters::Type::feedForward)
    {
        const auto delayedLeft = read (lineLeft, targetDelay);
        const auto delayedRight = read (lineRight, targetDelay);
        lineLeft[write] = left;
        lineRight[write] = right;
        left = 0.5f * (left + delayedLeft);
        right = 0.5f * (right + delayedRight);
    }
    else
    {
        smoothedDelay = smoothing * smoothedDelay + (1.0 - smoothing) * targetDelay;
        const auto delayedLeft = read (lineLeft, smoothedDelay);
        const auto delayedRight = read (lineRight, smoothedDelay);
        left = std::clamp ((0.99f - feedback) * left + feedback * delayedLeft, -1.0f, 1.0f);
        right = std::clamp ((0.99f - feedback) * right + feedback * delayedRight, -1.0f, 1.0f);
        lineLeft[write] = left;
        lineRight[write] = right;
    }

    write = (write + 1) % lineLeft.size();
}

} // namespace livecut::engine
