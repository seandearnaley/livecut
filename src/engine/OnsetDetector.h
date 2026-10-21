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
// Epochs/markers are local to this instance and must be coordinated with source
// history by its owner. reset is required on seeks, loops or discontinuities.
class OnsetDetector
{
public:
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
        written = analyzedEnd = 0;
        count = writeSlot = 0;
        overwritten = 0;
        inWindow = 0;
        energySum = peakEnergy = smoothedEnergy = 0.0;
        peakFrame = 0;
        previousOnset.reset();
        if (currentEpoch == maximumEpoch)
        {
            active = false;
            return false;
        }
        ++currentEpoch;
        active = ! history.empty();
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
    // Exclusive end of completed analysis; the unfinished window has no result.
    std::int64_t analyzedFrames() const noexcept { return analyzedEnd; }
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

    // Search only [first, end), intersected by the caller with retained AUDIO.
    // Reject unanalysed/future domains. No qualifying retained marker => nullopt,
    // never a grid point or unrelated attack. Equal distances choose earlier.
    // minimumRiseDb filters measured energy rise, not perceptual confidence.
    std::optional<OnsetMarker> nearest (std::uint64_t requestedEpoch, std::int64_t target,
                                       std::int64_t first, std::int64_t end,
                                       double minimumRiseDb = 0.0) const noexcept
    {
        if (! active || requestedEpoch != currentEpoch || target < 0 || first < 0
            || end <= first || end > analyzedEnd
            || ! std::isfinite (minimumRiseDb) || minimumRiseDb < 0.0)
            return std::nullopt;

        std::optional<OnsetMarker> best;
        auto bestDistance = maximumFrame;
        for (std::size_t i = 0; i < count; ++i)
        {
            const auto candidate = *marker (i);
            if (candidate.frame < first || candidate.frame >= end || candidate.riseDb < minimumRiseDb)
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
    static constexpr auto maximumFrame = std::numeric_limits<std::int64_t>::max();
    static constexpr auto maximumEpoch = std::numeric_limits<std::uint64_t>::max();
    static constexpr double energyFloor = 1.0e-8;
    std::vector<OnsetMarker> history;
    std::uint64_t currentEpoch = 0, overwritten = 0;
    std::int64_t written = 0, analyzedEnd = 0, peakFrame = 0, minimumGap = 0;
    std::optional<std::int64_t> previousOnset;
    std::size_t count = 0, writeSlot = 0;
    int hop = 0, inWindow = 0;
    double energySum = 0.0, peakEnergy = 0.0, smoothedEnergy = 0.0;
    double decay = 0.0, thresholdDb = 6.0;
    bool active = false;
};
} // namespace livecut::engine
