// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace livecut::engine
{
struct OnsetMarker
{
    std::uint64_t epoch;
    // Earliest maximum-energy sample in the triggering analysis window.
    std::int64_t frame;
    // Inclusive input frame where the candidate became available.
    std::int64_t detectedAt;
    double riseDb;
};

// Causal, stereo-linked energy-rise candidates, not instrument classification.
// Local squared energy and its normalized rise follow the time-domain approach
// described by J. P. Bello: https://wp.nyu.edu/jpbello/wp-content/uploads/sites/1691/2018/01/3-novelty.pdf
//
// Non-overlapping ~1ms windows average max(left^2, right^2), avoiding stereo
// cancellation. A 5ms exponential smoother reduces carrier ripple. A candidate
// requires a positive inter-window smoothed energy rise above sensitivityDb,
// energy >= -80dBFS, and >=10ms since the preceding candidate.
//
// Sensitivity is the minimum rise in dB PER ANALYSIS WINDOW (3..24; lower means
// more sensitive), not a probability/confidence score. Sustained modulation can
// trigger; masked or slow/quiet attacks can be missed. No spectral analysis or
// physical-onset reconstruction is implied. The marked sample is the energy
// peak of the triggering window, not a guaranteed perceptual onset.
//
// Publication delay from marker.frame is 0..windowFrames()-1 samples. A soft
// attack may cross the threshold later than its physical beginning; that error
// is not bounded by the publication delay. This class adds no audio delay; a
// renderer needing retrospective alignment must buffer.
// Single audio thread only: prepare allocates; process/query/reset never do.
// The owner coordinates strictly increasing transport epoch IDs with its audio
// timeline. A seek/loop uses beginTransportEpoch(): future input must not erase
// markers still needed by a delayed renderer. reset/prepare start a new physical
// stream, clear all markers and advance the ID; never reuse IDs after a reset.
//
// 8192 markers cover a <=64s voice lifetime plus <=22ms source margin within
// the current render epoch (<=6403 at the 10ms minimum gap), plus <=500ms ahead
// ingestion. Epoch restarts can defeat the refractory gap, but every additional
// marker needs a complete ~1ms window: <=534 future markers over supported
// sample rates. 6403 + 534 < 8192. This is NOT a promise to retain every marker
// from 64s of rapid loops. Queries/source reads must stay in the current render
// epoch; older epochs outside it must not be referenced.
class OnsetDetector
{
public:
    struct SourceBounds
    {
        std::uint64_t epoch;
        // Exact transport-epoch start, NOT the oldest retained audio frame:
        // analysis windows are aligned to this position.
        std::int64_t firstFrame;
        // Exclusive transport-epoch end, or nextFrame() for the current epoch.
        std::int64_t endFrame;
    };

    OnsetDetector() = default;
    OnsetDetector (const OnsetDetector&) = delete;
    OnsetDetector& operator= (const OnsetDetector&) = delete;
    OnsetDetector (OnsetDetector&&) = delete;
    OnsetDetector& operator= (OnsetDetector&&) = delete;

    // Supports 8..384kHz. Invalid settings preserve existing state; allocation
    // errors propagate to the non-real-time caller. Successful prepare resets.
    bool prepare (double sampleRate, std::size_t capacity, double sensitivityDb = 6.0)
    {
        if (! std::isfinite (sampleRate) || sampleRate < 8000.0 || sampleRate > 384000.0
            || capacity == 0 || capacity > history.max_size()
            || ! std::isfinite (sensitivityDb) || sensitivityDb < 3.0 || sensitivityDb > 24.0
            || currentEpoch == maximumEpoch)
            return false;

        history.resize (capacity);
        hop = static_cast<int> (std::round (sampleRate * 0.001));
        minimumGap = static_cast<std::int64_t> (std::ceil (sampleRate * 0.010));
        decay = std::exp (-static_cast<double> (hop) / (sampleRate * 0.005));
        thresholdDb = sensitivityDb;
        return reset();
    }

    // Epoch exhaustion explicitly disables processing rather than reusing IDs.
    bool reset() noexcept
    {
        written = 0;
        count = writeSlot = 0;
        overwritten = 0;
        resetAnalysis();
        if (currentEpoch == maximumEpoch)
        {
            active = false;
            return false;
        }
        ++currentEpoch;
        active = ! history.empty();
        return true;
    }

    // Begin exactly at nextFrame(). Keep old published markers and absolute
    // input time, but never combine analysis or refractory state across epochs.
    // Invalid/old IDs leave all state intact. Zero is never a valid epoch ID.
    bool beginTransportEpoch (std::uint64_t epoch) noexcept
    {
        if (! active || epoch <= currentEpoch)
            return false;
        currentEpoch = epoch;
        resetAnalysis();
        return true;
    }

    // frame must equal nextFrame(). Invalid/nonfinite input does not advance
    // time or modify analysis. The caller must handle failure explicitly.
    // INT64_MAX is never incremented; exhaustion requires an explicit reset.
    bool process (std::int64_t frame, float left, float right) noexcept
    {
        if (! active || frame != written || written == maximumFrame
            || ! std::isfinite (left) || ! std::isfinite (right))
            return false;

        const auto energy = std::max (static_cast<double> (left) * left,
                                      static_cast<double> (right) * right);
        if (inWindow == 0 || energy > peakEnergy)
        {
            peakEnergy = energy;
            peakFrame = frame;
        }
        energySum += energy;
        ++written;
        if (++inWindow != hop)
            return true;

        const auto nextEnergy = decay * smoothedEnergy + (1.0 - decay) * energySum / hop;
        const auto rise = 10.0 * std::log10 (std::max (nextEnergy, energyFloor)
                                           / std::max (smoothedEnergy, energyFloor));
        if (nextEnergy >= energyFloor && rise >= thresholdDb
            && (! previousOnset || peakFrame - *previousOnset >= minimumGap))
        {
            history[writeSlot] = { currentEpoch, peakFrame, frame, rise };
            writeSlot = (writeSlot + 1) % history.size();
            if (count < history.size())
                ++count;
            else if (overwritten != maximumEpoch)
                ++overwritten;
            previousOnset = peakFrame;
        }
        // Below the stated floor, a silent window clears a dying envelope
        // rather than spending subsequent callbacks on denormal arithmetic.
        smoothedEnergy = energySum == 0.0 && nextEnergy < energyFloor ? 0.0 : nextEnergy;
        analyzedEnd = written;
        inWindow = 0;
        energySum = 0.0;
        return true;
    }

    std::uint64_t epoch() const noexcept { return currentEpoch; }
    std::int64_t nextFrame() const noexcept { return written; }
    // Current epoch's analysed interval is [epochFirstFrame(), analyzedFrames()).
    // This is not a continuous prefix across discarded partial epoch windows.
    std::int64_t analyzedFrames() const noexcept { return analyzedEnd; }
    std::int64_t epochFirstFrame() const noexcept { return epochFirst; }
    int windowFrames() const noexcept { return hop; }
    std::size_t markerCount() const noexcept { return count; }
    // Since reset; saturates at UINT64_MAX. Old marker loss is never hidden.
    std::uint64_t overwrittenMarkers() const noexcept { return overwritten; }

    // Oldest to newest. Marker retention does NOT guarantee audio retention.
    std::optional<OnsetMarker> marker (std::size_t index) const noexcept
    {
        if (index >= count)
            return std::nullopt;
        const auto oldest = (writeSlot + history.size() - count) % history.size();
        return history[(oldest + index) % history.size()];
    }

    // Bounds come from the owning transport timeline. The caller intersects the
    // search [first,end) with retained AUDIO and this epoch, without moving the
    // epoch's true firstFrame. Search end must not include a discarded partial analysis
    // window, even after newer epochs have advanced global input time.
    // No qualifying retained marker => nullopt, never a grid point or unrelated
    // epoch's attack. Equal distances choose earlier.
    // minimumRiseDb filters measured energy rise, not perceptual confidence.
    std::optional<OnsetMarker> nearest (SourceBounds source, std::int64_t target,
                                       std::int64_t first, std::int64_t end,
                                       double minimumRiseDb = 0.0) const noexcept
    {
        if (! active || source.epoch == 0 || source.epoch > currentEpoch
            || source.firstFrame < 0 || source.endFrame <= source.firstFrame
            || source.firstFrame > written
            || (source.epoch == currentEpoch && source.firstFrame != epochFirst)
            || target < 0 || first < source.firstFrame || end <= first
            || end > source.endFrame
            || ! std::isfinite (minimumRiseDb) || minimumRiseDb < 0.0)
            return std::nullopt;

        const auto arrived = std::min (source.endFrame, written) - source.firstFrame;
        const auto analysisEnd = source.firstFrame + (arrived / hop) * hop;
        if (end > analysisEnd)
            return std::nullopt;

        std::optional<OnsetMarker> best;
        auto bestDistance = maximumFrame;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto candidate = *marker (i);
            if (candidate.epoch != source.epoch || candidate.frame < first
                || candidate.frame >= end || candidate.riseDb < minimumRiseDb)
                continue;
            const auto distance = candidate.frame >= target ? candidate.frame - target : target - candidate.frame;
            if (! best || distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }
        return best;
    }

private:
    void resetAnalysis() noexcept
    {
        epochFirst = analyzedEnd = written;
        inWindow = 0;
        energySum = peakEnergy = smoothedEnergy = 0.0;
        peakFrame = written;
        previousOnset.reset();
    }

    static constexpr auto maximumFrame = std::numeric_limits<std::int64_t>::max();
    static constexpr auto maximumEpoch = std::numeric_limits<std::uint64_t>::max();
    static constexpr double energyFloor = 1.0e-8;
    std::vector<OnsetMarker> history;
    std::uint64_t currentEpoch = 0, overwritten = 0;
    std::int64_t written = 0, analyzedEnd = 0, epochFirst = 0, peakFrame = 0, minimumGap = 0;
    std::optional<std::int64_t> previousOnset;
    std::size_t count = 0, writeSlot = 0;
    int hop = 0, inWindow = 0;
    double energySum = 0.0, peakEnergy = 0.0, smoothedEnergy = 0.0;
    double decay = 0.0, thresholdDb = 6.0;
    bool active = false;
};
} // namespace livecut::engine
