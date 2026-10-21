// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cmath>
#include <optional>

namespace livecut::engine
{

// Maps positions within ONE BAR, measured in quarter-note beats. The caller
// keeps the bar ordinal separately and retains original logical source offsets;
// this helper only maps activation time, never source audio or its duration.
//
// Each complete quarter note contains a pair of eighths. Swing 50..75 percent
// places the middle eighth at 0.50..0.75 of the pair. Quarter endpoints and the
// bar end stay fixed. Any incomplete final quarter stays entirely straight
// (for example, the last eighth of 3/8, or the whole bar of 1/8).
//
// Invalid/nonfinite positions, bar lengths or percentages return nullopt; no
// clamping or implicit meter conversion. Both endpoints 0 and beatsPerBar are
// valid. Neutral 50 returns the input directly, preserving its bits. Functions
// are stateless, allocation-free and monotonic, subject to double precision.
class SwingGrid
{
public:
    static std::optional<double> toPhysical (double logical, double beatsPerBar,
                                            double swingPercent = 50.0) noexcept
    {
        if (! valid (logical, beatsPerBar, swingPercent))
            return std::nullopt;
        if (swingPercent <= 50.0 || logical >= std::floor (beatsPerBar))
            return logical;

        const auto fraction = logical - std::floor (logical);
        if (fraction <= 0.0)
            return logical;

        const auto amount = 2.0 * (swingPercent / 100.0) - 1.0;
        const auto distance = fraction <= 0.5 ? fraction : 1.0 - fraction;
        // Adding a nonnegative delay also avoids roundoff creating an advance.
        return logical + amount * distance;
    }

    static std::optional<double> toLogical (double physical, double beatsPerBar,
                                           double swingPercent = 50.0) noexcept
    {
        if (! valid (physical, beatsPerBar, swingPercent))
            return std::nullopt;
        if (swingPercent <= 50.0 || physical >= std::floor (beatsPerBar))
            return physical;

        const auto start = std::floor (physical);
        const auto fraction = physical - start;
        if (fraction <= 0.0)
            return physical;

        const auto ratio = swingPercent / 100.0;
        const auto logical = fraction <= ratio ? 0.5 * fraction / ratio
                                               : 0.5 + 0.5 * (fraction - ratio) / (1.0 - ratio);
        return start + logical;
    }

private:
    static bool valid (double position, double beatsPerBar, double swingPercent) noexcept
    {
        return std::isfinite (position) && std::isfinite (beatsPerBar)
            && std::isfinite (swingPercent) && beatsPerBar > 0.0
            && position >= 0.0 && position <= beatsPerBar
            && swingPercent >= 50.0 && swingPercent <= 75.0;
    }
};

} // namespace livecut::engine
