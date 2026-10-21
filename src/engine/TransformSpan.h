// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

namespace livecut::engine
{
struct ReadCoordinate
{
    std::int64_t index;
    double fraction;
};

namespace transform_span_detail
{
// INT64_MAX rounds up to 2^63 as a double. Check BEFORE converting back to an
// integer, and reject rounded input identities rather than address a neighbor.
inline std::optional<double> exactInteger (std::int64_t value) noexcept
{
    const auto converted = static_cast<double> (value);
    if (value < 0 || converted >= 0x1p63 || static_cast<std::int64_t> (converted) != value)
        return std::nullopt;
    return converted;
}
} // namespace transform_span_detail

// Minimum complete source span for N gated output frames at a fixed rate:
// ceil((N-1)*rate)+1. An exact last read needs no interpolation neighbor; a
// fractional last read needs its ceil. The owner supplies its declared source
// bound (for example, the classic eight seconds); exceeding it is ineligible,
// never a request to substitute that whole bound or shorten the voice silently.
// Every planned output integer must be exactly representable as a double, so
// N-1 cannot exceed 2^53. This excludes ambiguous enormous output identities
// without restricting practical audio spans.
inline std::optional<std::int64_t> requiredSourceFrames (std::int64_t outputFrames, double rate,
                                                        std::int64_t maxSourceFrames) noexcept
{
    if (outputFrames <= 0 || maxSourceFrames <= 0 || ! std::isfinite (rate) || rate <= 0.0)
        return std::nullopt;
    const auto lastOutput = transform_span_detail::exactInteger (outputFrames - 1);
    if (! lastOutput || *lastOutput > 0x1p53)
        return std::nullopt;
    const auto lastRead = *lastOutput * rate;
    if (! std::isfinite (lastRead))
        return std::nullopt;
    const auto neededIndex = std::ceil (lastRead);
    if (neededIndex >= 0x1p63)
        return std::nullopt;
    const auto index = static_cast<std::int64_t> (neededIndex);
    if (index >= maxSourceFrames)
        return std::nullopt;
    return index + 1;
}

// Relative address in an immutable, complete source span. Reverse starts at
// its last INTEGER frame: (fixedFrameCount-1)-outputFrame*rate. At fractional
// rates this intentionally differs from reversing a forward-resampled array.
// No clamping, looping, source selection or tempo calculation occurs here.
// A later tempo extension may ask for more output, but reads stop when this
// fixed source is consumed. The owner separately enforces its voice deadline.
// Keep fraction as double: one just below 1 can round to float 1 and still
// requires the next source frame. Exact integer reads require only that frame.
inline std::optional<ReadCoordinate> readCoordinate (std::int64_t outputFrame, double rate,
                                                    std::int64_t fixedFrameCount, bool reverse) noexcept
{
    if (fixedFrameCount <= 0 || ! std::isfinite (rate) || rate <= 0.0)
        return std::nullopt;
    const auto output = transform_span_detail::exactInteger (outputFrame);
    if (! output)
        return std::nullopt;
    const auto travelled = *output * rate;
    if (! std::isfinite (travelled))
        return std::nullopt;
    auto position = travelled;
    if (reverse)
    {
        const auto last = transform_span_detail::exactInteger (fixedFrameCount - 1);
        if (! last || travelled > *last)
            return std::nullopt;
        position = *last - travelled;
    }
    if (position >= 0x1p63)
        return std::nullopt;
    const auto index = static_cast<std::int64_t> (position);
    if (index >= fixedFrameCount)
        return std::nullopt;
    const auto fraction = position - static_cast<double> (index);
    if (fraction > 0.0 && index == fixedFrameCount - 1)
        return std::nullopt;
    return ReadCoordinate { index, fraction };
}
} // namespace livecut::engine
