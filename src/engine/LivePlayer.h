// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/Cut.h"

#include <span>
#include <vector>

namespace livecut::engine
{

// Livecut's cut envelope at sample `t` of a cut `length` samples long: an
// exponential attack and release over `fade` samples.
float fadeEnvelope (int t, int fade, int length);

// How many samples of a pitched cut can sound before its read head overtakes
// the recording. The cut starts once `recorded` samples of its block have been
// recorded, and at time t it reads samples floor(t * rate) and floor(t * rate) + 1.
int playableSamples (int length, double rate, int recorded, int capacity);

// Plays one block of cuts on a live stereo input. The first cut plays the
// input as it arrives and each later cut replays the block from its start,
// so cutting adds no latency.
class LivePlayer
{
public:
    struct PlayingCut
    {
        int start = 0;    // samples after the block starts
        int size = 0;     // samples until the next cut starts
        int playable = 0; // samples that sound before the gate closes
        double units = 0.0;
        double gate = 1.0;
        bool pitched = false;
        double rate = 1.0;
        float leftToLeft = 1.0f, leftToRight = 0.0f, rightToLeft = 0.0f, rightToRight = 1.0f;
    };

    // Allocates room to record `capacity` samples per channel, the longest a cut can be.
    void prepare (int capacity);

    void setFadeSamples (int samples);

    // Starts a block. Sizes are rounded cumulatively, so they add up to the
    // block's length in samples exactly. The first cut ignores its pitch
    // because it plays the input live.
    void startBlock (std::span<const Cut> cuts, double samplesPerUnit);

    // Records one input frame and replaces it with the output frame.
    void process (float& left, float& right);

    // Re-times the rest of the block for a new tempo: what has already played
    // stays, and everything after it is laid out again at the new length of a unit.
    void setSamplesPerUnit (double samplesPerUnit);

    std::span<const PlayingCut> cuts() const { return playing; }

private:
    int playableFor (const PlayingCut& cut, int soundingLength) const;
    void skipFinishedCuts();

    std::vector<float> recordedLeft, recordedRight;
    std::vector<PlayingCut> playing;
    int capacity = 0;
    int recorded = 0;
    std::size_t current = 0;
    int position = 0; // samples into the current cut
    int fade = 1;
    double samplesPerUnit = 1.0;
};

} // namespace livecut::engine
