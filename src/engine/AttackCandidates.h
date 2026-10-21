// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "OnsetDetector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace livecut::engine
{

// Audio-owned, per-BlockPlan cache. reset latches nominal source identity and
// search controls; update does not read current settings. A full inclusive
// +/-ceil(20ms*sr) search must finish analysis before the cache becomes ready.
// Ready (including empty), invalid and overflow are terminal until reset.
//
// Known closed epochs can discard their final partial analysis window. Only
// then may that epoch-relative complete prefix finish a shorter search. The
// true audio epoch end is NOT rounded: the owner must supply fresh exact bounds
// and arrived/retained audio to the selection predicate at EACH cut activation.
// Cache readiness does not promise source availability or pin PCM.
//
// Five markers suffice: the real detector's >=ceil(10ms*sr) refractory gap and
// this <=40ms inclusive search permit at most five. A violation is an explicit
// overflow, never a truncated candidate list. No allocation, locks or RNG.
class AttackCandidates
{
public:
    enum class State { invalid, pending, ready, overflow };
    struct Choice { OnsetMarker marker; std::int64_t sourceStart; };

    State reset (std::int64_t nominalOrigin, std::uint64_t transportEpoch,
                 std::int64_t exactEpochStart, double sampleRate, double thresholdDb) noexcept
    {
        status = State::invalid;
        size = 0;
        if (! std::isfinite (sampleRate) || sampleRate < 8000.0 || sampleRate > 384000.0
            || ! std::isfinite (thresholdDb) || thresholdDb < 3.0 || thresholdDb > 24.0
            || transportEpoch == 0 || exactEpochStart < 0 || nominalOrigin < exactEpochStart)
            return status;

        const auto radius = static_cast<std::int64_t> (std::ceil (0.020 * sampleRate));
        if (nominalOrigin > std::numeric_limits<std::int64_t>::max() - radius - 1)
            return status;
        nominal = nominalOrigin;
        epoch = transportEpoch;
        epochStart = exactEpochStart;
        first = std::max (epochStart, nominal > radius ? nominal - radius : 0);
        end = nominal + radius + 1; // Exclusive representation of inclusive endpoints.
        preroll = static_cast<std::int64_t> (std::ceil (0.002 * sampleRate));
        analysisHop = static_cast<int> (std::round (0.001 * sampleRate));
        threshold = thresholdDb;
        status = State::pending;
        return status;
    }

    // Uses OnsetDetector's read-only API. The narrow template also allows fixed
    // analysis fixtures to test the otherwise unreachable >5-marker violation.
    // No marker scan occurs until coverage is complete, and at most one scan
    // occurs for this block. A newer detector epoch alone does not close ours.
    template <typename Detector>
    State update (const Detector& detector, std::optional<std::int64_t> exactEpochEnd = std::nullopt) noexcept
    {
        if (status != State::pending)
            return status;
        const auto hop = detector.windowFrames();
        const auto written = detector.nextFrame();
        if (hop != analysisHop || written < 0 || (exactEpochEnd && *exactEpochEnd <= nominal))
            return fail (State::invalid);
        if (detector.epoch() < epoch)
            return status;
        if (detector.epoch() == epoch && detector.epochFirstFrame() != epochStart)
            return fail (State::invalid);
        if (detector.epoch() > epoch && ! exactEpochEnd)
            return status;
        if (detector.epoch() > epoch && *exactEpochEnd > detector.epochFirstFrame())
            return fail (State::invalid);
        if (written < epochStart)
            return status;

        const auto clippedEnd = exactEpochEnd ? std::min (end, *exactEpochEnd) : end;
        // Epoch-relative subtraction/division cannot overflow; all endpoints
        // are nonnegative and closed end > nominal >= epochStart.
        const auto completeEnd = exactEpochEnd
            ? epochStart + ((*exactEpochEnd - epochStart) / hop) * hop : clippedEnd;
        const auto searchEnd = std::min (clippedEnd, completeEnd);
        const auto available = detector.epoch() == epoch ? detector.analyzedFrames() : completeEnd;
        if (available < epochStart || available > written)
            return fail (State::invalid);
        if (available < searchEnd)
            return status;
        if (first >= searchEnd)
        {
            status = State::ready;
            return status;
        }

        std::array<OnsetMarker, 5> found {};
        std::size_t foundSize = 0;
        const auto markerCount = detector.markerCount();
        for (std::size_t i = 0; i < markerCount; ++i)
        {
            const auto marker = detector.marker (i);
            if (! marker)
                return fail (State::invalid);
            if (marker->epoch != epoch || marker->frame < first || marker->frame >= searchEnd)
                continue;
            if (! std::isfinite (marker->riseDb) || marker->riseDb < 0.0
                || marker->detectedAt < marker->frame || marker->detectedAt >= available)
                return fail (State::invalid);
            if (marker->riseDb < threshold)
                continue;
            if (foundSize == found.size())
                return fail (State::overflow);
            found[foundSize++] = *marker;
        }
        candidates = found;
        size = foundSize;
        status = State::ready;
        return status;
    }

    State state() const noexcept { return status; }
    std::size_t count() const noexcept { return size; }
    std::optional<OnsetMarker> candidate (std::size_t index) const noexcept
    {
        return index < size ? std::optional { candidates[index] } : std::nullopt;
    }

    // The predicate must be real-time safe. It receives the ENTIRE fixed source
    // span, and checks fresh exact epoch bounds plus physical PCM availability.
    // Invalid extents and unavailable candidates yield no choice. A returned
    // value is independent of this cache; the owner freezes it into PlayingCut.
    // Source validity may change between activations, but cached origins never do.
    template <typename Predicate>
    std::optional<Choice> nearestEligible (std::int64_t requiredFrames, Predicate&& eligible) const
    {
        if (status != State::ready || requiredFrames <= 0)
            return std::nullopt;
        std::optional<Choice> best;
        auto bestDistance = std::numeric_limits<std::int64_t>::max();
        for (std::size_t i = 0; i < size; ++i)
        {
            const auto marker = candidates[i];
            if (marker.frame < preroll)
                continue;
            const auto start = marker.frame - preroll;
            if (start < epochStart || requiredFrames > std::numeric_limits<std::int64_t>::max() - start
                || ! eligible (start, requiredFrames))
                continue;
            const auto distance = marker.frame >= nominal ? marker.frame - nominal : nominal - marker.frame;
            if (! best || distance < bestDistance || (distance == bestDistance && marker.frame < best->marker.frame))
            {
                best = Choice { marker, start };
                bestDistance = distance;
            }
        }
        return best;
    }

private:
    State fail (State failure) noexcept { size = 0; status = failure; return status; }
    std::array<OnsetMarker, 5> candidates {};
    std::size_t size = 0;
    std::int64_t nominal = 0, epochStart = 0, first = 0, end = 0, preroll = 0;
    std::uint64_t epoch = 0;
    double threshold = 3.0;
    int analysisHop = 0;
    State status = State::invalid;
};

} // namespace livecut::engine
