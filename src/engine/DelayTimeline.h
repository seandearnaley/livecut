// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace livecut::engine
{

// Metadata only: input clock t renders r=t-L. submittedEnd() describes announced
// callback contexts, NEVER audio availability. A whole callback may be announced
// before its PCM is ingested sample by sample. The owner separately tracks PCM,
// uses the looked-up original transport/settings/bypass, and clamps reads to
// both retained audio and sourceEpoch(r). Physical audio-storage generations
// are distinct from these owner-supplied transport epoch IDs.
//
// Two fixed L+2-entry rings cover one-frame callbacks. The owner must retire
// consumed contexts after rendering; capacity exhaustion rejects append without
// overwriting. Lookups are logarithmic and return COPIES valid after retirement,
// reset or ring reuse. Context must be a self-contained value; any pointers in
// it remain the owner's lifetime responsibility. Single audio thread only.
template <typename Context>
class DelayTimeline
{
    static_assert (std::is_trivially_copyable_v<Context>
                   && std::is_trivially_copy_constructible_v<Context>
                   && std::is_trivially_copy_assignable_v<Context>);

public:
    struct Segment
    {
        std::int64_t firstFrame, frameCount;
        std::uint64_t transportEpoch;
        std::int64_t epochStart;
        Context context;
    };
    struct Lookup { Segment segment; std::int64_t offset; };
    struct Epoch { std::uint64_t id; std::int64_t start; std::optional<std::int64_t> end; };

    DelayTimeline() = default;
    DelayTimeline (const DelayTimeline&) = delete;
    DelayTimeline& operator= (const DelayTimeline&) = delete;
    DelayTimeline (DelayTimeline&&) = delete;
    DelayTimeline& operator= (DelayTimeline&&) = delete;

    // Only allocating operation. Invalid sizes preserve state; allocation
    // failures propagate to the non-real-time caller with the old rings intact.
    bool prepare (std::int64_t latencyFrames)
    {
        if (latencyFrames < 0 || latencyFrames > maximumFrame - 2
            || std::cmp_greater (latencyFrames, std::numeric_limits<std::size_t>::max() - 2))
            return false;
        const auto size = static_cast<std::size_t> (latencyFrames) + 2;
        if (size > segments.max_size() || size > boundaries.max_size())
            return false;

        std::vector<std::optional<Segment>> newSegments (size);
        std::vector<Boundary> newBoundaries (size);
        segments.swap (newSegments);
        boundaries.swap (newBoundaries);
        latency = latencyFrames;
        return reset();
    }

    // Clear timelines, retaining the highest accepted transport ID so stale
    // identities cannot alias after physical reset/reprepare. The owner supplies
    // a newer ID for the next callback. No internal transport ID is generated.
    bool reset (std::int64_t firstInputFrame = 0) noexcept
    {
        if (segments.empty() || firstInputFrame < 0)
            return false;
        // Trivial payloads need no destruction; stale slots are inaccessible.
        segmentHead = segmentSize = boundaryHead = boundarySize = 0;
        endSubmitted = retired = firstInputFrame;
        latestEpoch.reset();
        renderEpoch.reset();
        return true;
    }

    // Positive segments must be contiguous. Same-epoch starts are immutable;
    // new nonzero IDs strictly increase and start exactly at this callback.
    // The initial epoch may have begun before firstInputFrame. Zero frames are
    // ignored. All rejection paths leave accepted metadata and counters intact.
    bool append (std::int64_t first, std::int64_t count, std::uint64_t epoch,
                 std::int64_t epochStart, Context context) noexcept
    {
        if (segments.empty())
            return false;
        if (count == 0)
            return true;
        if (first < 0 || first != endSubmitted || count < 0 || count > maximumFrame - first
            || epoch == 0 || epochStart < 0 || epochStart > first || segmentSize == capacity())
            return false;

        const auto newEpoch = ! latestEpoch || epoch != latestEpoch->id;
        if (newEpoch)
        {
            if (epoch <= highestEpoch || (latestEpoch && epochStart != first) || boundarySize == capacity())
                return false;
        }
        else if (epochStart != latestEpoch->start)
            return false;

        segments[(segmentHead + segmentSize) % capacity()] = Segment { first, count, epoch, epochStart, context };
        ++segmentSize;
        if (newEpoch)
        {
            const Boundary boundary { epoch, epochStart };
            boundaries[(boundaryHead + boundarySize) % capacity()] = boundary;
            ++boundarySize;
            latestEpoch = boundary;
            highestEpoch = epoch;
        }
        endSubmitted = first + count;
        return true;
    }

    // cursor is the next frame the renderer will consume. It may advance only
    // through submitted metadata; it is NOT evidence that audio was rendered.
    // Keep the current render-epoch start separately when its marker retires.
    bool retireBefore (std::int64_t cursor) noexcept
    {
        if (segments.empty() || cursor < retired || cursor > endSubmitted)
            return false;
        while (segmentSize > 0 && segmentAt (0).firstFrame + segmentAt (0).frameCount <= cursor)
        {
            segmentHead = (segmentHead + 1) % capacity();
            --segmentSize;
        }
        while (boundarySize > 0 && boundaryAt (0).start <= cursor)
        {
            renderEpoch = boundaryAt (0);
            boundaryHead = (boundaryHead + 1) % capacity();
            --boundarySize;
        }
        retired = cursor;
        return true;
    }

    std::optional<Lookup> lookup (std::int64_t frame) const noexcept
    {
        if (frame < retired || frame >= endSubmitted || segmentSize == 0)
            return std::nullopt;
        std::size_t low = 0, high = segmentSize;
        while (low < high)
        {
            const auto middle = low + (high - low) / 2;
            if (segmentAt (middle).firstFrame <= frame)
                low = middle + 1;
            else
                high = middle;
        }
        if (low == 0)
            return std::nullopt;
        const auto& segment = segmentAt (low - 1);
        return Lookup { segment, frame - segment.firstFrame };
    }

    // Exclusive epoch end is known only once its successor was ingested. This
    // survives context retirement and does not substitute submittedEnd for an
    // unknown future boundary. Frames outside retained metadata are rejected.
    std::optional<Epoch> sourceEpoch (std::int64_t frame) const noexcept
    {
        if (segments.empty() || frame < retired || frame >= endSubmitted)
            return std::nullopt;
        std::size_t low = 0, high = boundarySize;
        while (low < high)
        {
            const auto middle = low + (high - low) / 2;
            if (boundaryAt (middle).start <= frame)
                low = middle + 1;
            else
                high = middle;
        }
        const auto current = low > 0 ? std::optional { boundaryAt (low - 1) } : renderEpoch;
        if (! current)
            return std::nullopt;
        const auto end = low < boundarySize ? std::optional { boundaryAt (low).start } : std::nullopt;
        return Epoch { current->id, current->start, end };
    }

    // After retireBefore, available even when no contexts remain. A later
    // append can supply this epoch's previously unknown next boundary.
    std::optional<Epoch> currentRenderEpoch() const noexcept
    {
        if (! renderEpoch)
            return std::nullopt;
        const auto end = boundarySize > 0 ? std::optional { boundaryAt (0).start } : std::nullopt;
        return Epoch { renderEpoch->id, renderEpoch->start, end };
    }

    // Arithmetic clock conversion only; no metadata/PCM availability claim.
    std::optional<std::int64_t> renderFrame (std::int64_t input) const noexcept
    {
        if (segments.empty() || input < latency)
            return std::nullopt;
        return input - latency;
    }
    std::int64_t submittedEnd() const noexcept { return endSubmitted; }
    std::size_t segmentCount() const noexcept { return segmentSize; }
    std::size_t boundaryCount() const noexcept { return boundarySize; }
    std::size_t capacity() const noexcept { return segments.size(); }

private:
    struct Boundary { std::uint64_t id; std::int64_t start; };
    static constexpr auto maximumFrame = std::numeric_limits<std::int64_t>::max();
    const Segment& segmentAt (std::size_t index) const noexcept { return *segments[(segmentHead + index) % capacity()]; }
    const Boundary& boundaryAt (std::size_t index) const noexcept { return boundaries[(boundaryHead + index) % capacity()]; }
    std::vector<std::optional<Segment>> segments;
    std::vector<Boundary> boundaries;
    std::optional<Boundary> latestEpoch, renderEpoch;
    std::uint64_t highestEpoch = 0;
    std::int64_t latency = 0, endSubmitted = 0, retired = 0;
    std::size_t segmentHead = 0, segmentSize = 0, boundaryHead = 0, boundarySize = 0;
};

} // namespace livecut::engine
