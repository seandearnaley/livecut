// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/Cut.h"
#include "engine/AudioHistory.h"

#include <span>
#include <optional>
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
        int soundingFrames = 0; // physical musical gate, independent of classic availability
        bool transformDecided = false;
        double units = 0.0;
        double logicalStart = 0.0;
        double gate = 1.0;
        bool pitched = false;
        double rate = 1.0;
        float leftToLeft = 1.0f, leftToRight = 0.0f, rightToLeft = 0.0f, rightToRight = 1.0f;
        Cut requested;
    };

    // Declares the classic source extent; retains eight such extents plus two guards.
    void prepare (int capacity, std::size_t retainedFrames = 0);

    void setFadeSamples (int samples);

    // Starts a block. Sizes are rounded cumulatively, so they add up to the
    // block's length in samples exactly. The first cut plays the input live,
    // so it honours only a pitch below the original.
    void startBlock (std::span<const Cut> cuts, double samplesPerUnit);
    void startBlock (std::span<const Cut> cuts, double samplesPerUnit, SourceRegion source);

    // Raw ingestion and rendering are separate: a future delayed renderer can
    // address an explicit older frame without ingesting that audio twice.
    SourceRegion sourceAtFrame (std::int64_t frame) const;
    SourceRegion sourceForNextFrame() const;
    bool ingestFrame (float left, float right);
    void renderFrame (float& left, float& right, bool advanceBySamples = true);
    void advanceFrame (bool advanceBySamples = true);
    int currentCutAge() const { return position; }
    void resetSource();
    void resetRendering();
    std::optional<StereoFrame> readSourceFrame (double relativeFrame) const;
    std::optional<StereoFrame> readSourceFrame (SourceRegion region, double relativeFrame) const;
    std::optional<AudioHistory::Window> readSourceWindow (SourceRegion region, std::int64_t offset, std::size_t count) const
    { return history.readWindow (region, offset, count); }
    std::optional<StereoFrame> rawFrame (std::int64_t frame) const
    {
        return history.read ({ history.retainedRegion().epoch, frame, 1 }, 0);
    }
    bool hasSourceError() const { return sourceError; }
    // Activation only: use the physical gate, not classic live-read availability.
    int currentSoundingFrames() const;
    void markCurrentTransformDecided();
    // Only at the first sample of an undecided cut. The complete fixed source
    // must cover its entire planned physical gate and fit the classic extent.
    // Success freezes source/direction/rate AND the original output-frame budget.
    bool setCurrentTransform (SourceRegion completeFixedSpan, bool reverse);
    bool sourceAvailable (SourceRegion completeFixedSpan) const;
    SourceRegion retainedSource() const;
    void advanceToUnits (double logicalUnits);
    void retimeMusicalCut (int samplesToEnd, int samplesToGate, std::int64_t renderedExclusiveEnd);

    // Records one input frame and replaces it with the output frame.
    void process (float& left, float& right);

    // Re-times the rest of the block for a new tempo: what has already played
    // stays, and everything after it is laid out again at the new length of a unit.
    void setSamplesPerUnit (double samplesPerUnit);

    std::span<const PlayingCut> cuts() const { return playing; }

    // Index of the cut playing next; cuts().size() once the block has run out.
    std::size_t currentCut() const { return current; }

private:
    int playableFor (const PlayingCut& cut, int soundingLength) const;
    void skipFinishedCuts();

    struct Transform
    {
        SourceRegion source;
        double rate;
        int outputFrames;
        bool reverse;
    };
    std::optional<Transform> transform;

    AudioHistory history;
    SourceRegion source {};
    bool sourceError = false;
    std::vector<PlayingCut> playing;
    int capacity = 0;
    std::size_t current = 0;
    int position = 0; // samples into the current cut
    int fade = 1;
    double samplesPerUnit = 1.0;
};

} // namespace livecut::engine
