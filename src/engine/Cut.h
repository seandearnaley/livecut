// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

namespace livecut::engine
{

// One slice of the incoming audio, replayed from the start of its block.
// Durations are in subdivision units, so cutting is independent of tempo and
// sample rate; the player converts them to samples.
struct Cut
{
    double units = 0.0; // time until the next cut starts
    double gate = 1.0;  // fraction of `units` that sounds (the duty cycle)
    float pan = 0.0f;   // -1 is hard left, +1 hard right
    float amp = 1.0f;
    float cents = 0.0f; // pitch offset
};

// The largest block is a CutProc11 stutter: under a bar of units, at up to 8 cuts per unit.
inline constexpr std::size_t maxCutsPerBlock = 256;

struct BlockContext
{
    double unitsDone = 0.0;   // units already played in this phrase
    double phraseUnits = 0.0; // length of this phrase in units
    int unitsPerBar = 8;      // the subdivision
    double beatsPerBar = 4.0; // quarter notes per bar, e.g. 3 for 3/4 and for 6/8
};

// Every setting the cut procedures read. The original procedures keep their defaults.
struct CutParameters
{
    int minPhraseBars = 1;
    int maxPhraseBars = 4;
    float minAmp = 1.0f;
    float maxAmp = 1.0f;
    float minPan = -0.2f;
    float maxPan = 0.2f;
    float minCents = 0.0f;
    float maxCents = 0.0f;
    double dutyCycle = 1.0;     // gate of regular cuts
    double fillDutyCycle = 1.0; // gate of stutters, rolls and fills

    // CutProc11
    double stutterChance = 0.8;
    double stutterArea = 0.5;   // a stutter may start once fewer than this many bars remain
    int minRepeats = 0;         // extra plays of a cut after the first
    int maxRepeats = 1;

    // WarpCut
    double straightChance = 0.3;
    double regularChance = 0.5;
    double ritardChance = 0.5;
    double accel = 0.9;         // ratio between successive cuts of an accelerating roll

    // SQPusher
    double activity = 0.5;

    // Bounce
    double bounceChance = 0.4;
    double ratchetChance = 0.7;
    int climb = 7; // semitones
    double halftimeChance = 0.15;
};

inline double totalUnits (std::span<const Cut> cuts)
{
    double total = 0.0;

    for (const auto& cut : cuts)
        total += cut.units;

    return total;
}

} // namespace livecut::engine
