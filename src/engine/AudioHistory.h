// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace livecut::engine
{

struct SourceRegion
{
    std::uint64_t epoch;
    std::int64_t firstFrame;
    std::int64_t frameCount;
};

struct StereoFrame
{
    float left;
    float right;
};

// Single-threaded stereo source storage, shared by the producer and renderer on
// the audio thread. Regions identify this instance's audio, never ring slots.
// A region must only be passed back to the instance that created it; epochs do
// not identify different AudioHistory instances.
// Availability does not pin a region: a renderer must retain its source for the
// entire voice lifetime, or use an immutable captured snapshot outside this class.
class AudioHistory
{
public:
    AudioHistory() = default;
    AudioHistory (const AudioHistory&) = delete;
    AudioHistory& operator= (const AudioHistory&) = delete;
    AudioHistory (AudioHistory&&) = delete;
    AudioHistory& operator= (AudioHistory&&) = delete;

    // The only allocating operation. Invalid capacities leave existing audio
    // intact; allocation failures propagate to the non-real-time caller.
    // Successful preparation starts a new epoch, even at the same capacity.
    bool prepare (std::size_t capacity)
    {
        if (capacity == 0 || std::cmp_greater (capacity, maximumFrame)
            || capacity > frames.max_size() || epoch == maximumEpoch)
            return false;

        frames.resize (capacity);
        return reset();
    }

    // Never wrap an epoch back onto an old source identity. Exhausting uint64_t
    // invalidates all audio and requires a new instance; false reports this.
    bool reset() noexcept
    {
        nextWritten = 0;
        if (epoch == maximumEpoch)
        {
            active = false;
            return false;
        }

        ++epoch;
        active = ! frames.empty();
        return true;
    }

    // Absolute frame positions never wrap. Once INT64_MAX frames have arrived,
    // appends fail without changing audio; the caller must explicitly reset.
    bool append (StereoFrame frame) noexcept
    {
        if (! active || nextWritten == maximumFrame)
            return false;

        frames[index (nextWritten)] = frame;
        ++nextWritten;
        return true;
    }

    // The half-open available interval is [firstFrame, firstFrame + frameCount).
    SourceRegion retainedRegion() const noexcept
    {
        const auto first = oldestRetained();
        return { epoch, first, nextWritten - first };
    }

    // Empty, malformed, partially arrived and partially expired regions fail.
    bool isAvailable (SourceRegion region) const noexcept
    {
        return validRegion (region) && region.firstFrame >= oldestRetained()
            && region.firstFrame <= nextWritten
            && region.frameCount <= nextWritten - region.firstFrame;
    }

    // A planned region may still be arriving or partly expired. Only this
    // position must be retained; check isAvailable separately when full source
    // completion is required (for example, before starting a reverse voice).
    std::optional<StereoFrame> read (SourceRegion region, std::int64_t offset) const noexcept
    {
        if (! validRegion (region) || offset < 0 || offset >= region.frameCount)
            return std::nullopt;

        const auto absolute = region.firstFrame + offset;
        if (absolute < oldestRetained() || absolute >= nextWritten)
            return std::nullopt;

        return frames[index (absolute)];
    }

    // offset is the integer source position; fraction must be finite in [0, 1).
    // Keeping the integer separate avoids double's loss of absolute-frame
    // precision. Direction and rate belong to the caller's playback transform.
    // An exact integer needs no next frame; fractional reads require both
    // neighbors inside the declared region AND the retained source interval.
    std::optional<StereoFrame> readFractional (SourceRegion region, std::int64_t offset,
                                              double fraction) const noexcept
    {
        if (! std::isfinite (fraction) || fraction < 0.0 || fraction >= 1.0)
            return std::nullopt;

        const auto first = read (region, offset);
        if (! first || fraction == 0.0)
            return first;

        // A successful read implies offset < frameCount <= INT64_MAX, so +1 is safe.
        const auto second = read (region, offset + 1);
        if (! second)
            return std::nullopt;

        const auto blend = static_cast<float> (fraction);
        return StereoFrame { first->left + blend * (second->left - first->left),
                             first->right + blend * (second->right - first->right) };
    }

private:
    static constexpr auto maximumFrame = std::numeric_limits<std::int64_t>::max();
    static constexpr auto maximumEpoch = std::numeric_limits<std::uint64_t>::max();

    bool validRegion (SourceRegion region) const noexcept
    {
        return active && region.epoch == epoch && region.firstFrame >= 0
            && region.frameCount > 0 && region.frameCount <= maximumFrame - region.firstFrame;
    }

    std::int64_t oldestRetained() const noexcept
    {
        const auto capacity = static_cast<std::int64_t> (frames.size());
        return nextWritten > capacity ? nextWritten - capacity : 0;
    }

    std::size_t index (std::int64_t absolute) const noexcept
    {
        return static_cast<std::size_t> (absolute % static_cast<std::int64_t> (frames.size()));
    }

    std::vector<StereoFrame> frames;
    std::uint64_t epoch = 0;
    std::int64_t nextWritten = 0;
    bool active = false;
};

} // namespace livecut::engine
