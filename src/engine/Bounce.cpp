// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bounce is new to this port, not from Nick Collins' BBCut library.

#include "engine/CutProcedures.h"
#include "engine/CutShaping.h"

#include <algorithm>
#include <cmath>

namespace livecut::engine
{

namespace
{
constexpr double epsilon = 1.0e-9;
} // namespace

void Bounce::chooseBlock (Random& random,
                          const CutParameters& params,
                          const BlockContext& context,
                          std::vector<Cut>& cuts)
{
    cuts.clear();

    const auto unitsPerBar = static_cast<double> (context.unitsPerBar);
    const auto beat = unitsPerBar / context.beatsPerBar;
    const auto intoBar = context.unitsDone - std::floor ((context.unitsDone + epsilon) / unitsPerBar) * unitsPerBar;
    const auto zone = std::min ((unitsPerBar >= 2.0 * beat - epsilon ? 2.0 : 1.0) * beat, unitsPerBar);
    const auto zoneStart = context.phraseUnits - zone;
    auto boundary = std::min (unitsPerBar - intoBar, context.phraseUnits - context.unitsDone);

    // Leave the start of the ratchet zone available for its own block decision.
    if (context.unitsDone < zoneStart - epsilon)
        boundary = std::min (boundary, zoneStart - context.unitsDone);

    // In fractional meters the zone can start off the beat, before recovery applies.
    if (std::abs (context.unitsDone - zoneStart) <= epsilon && random.chance (params.ratchetChance))
    {
        // Three pitch steps, each twice as fast as the last: 3, 3 and 6 cuts
        // at zone/6, zone/12 and zone/24, or straight 2, 2 and 4 at zone/4, zone/8 and zone/16.
        const std::size_t group = params.straight ? 2 : 3;
        const auto slowest = params.straight ? 4.0 : 6.0;
        Cut cut;
        cut.gate = params.fillDutyCycle;
        cuts.assign (4 * group, cut);
        sweep (cuts, random, params);

        for (std::size_t i = 0; i < cuts.size(); ++i)
        {
            const auto step = i < group ? 0 : i < 2 * group ? 1 : 2;
            cuts[i].units = zone / (slowest * (1 << step));
            // Keep sweep's pitch draw, but replace its ramp with whole semitone steps.
            cuts[i].cents = static_cast<float> (100.0 * std::round (params.climb * step / 2.0));
        }

        return;
    }

    const auto addCut = [&] (double units)
    {
        Cut cut;
        cut.units = units;
        cut.gate = params.dutyCycle;
        cut.amp = static_cast<float> (random.uniform (params.minAmp, params.maxAmp));
        cuts.push_back (cut);
    };

    // Leave the last bar to the ratchet or ordinary moves, even if its ratchet roll fails.
    if (std::abs (intoBar) <= epsilon
        && context.phraseUnits - context.unitsDone > unitsPerBar + epsilon
        && random.chance (params.halftimeChance))
    {
        addCut (unitsPerBar);
        cuts.back().cents = -1200.0f;
        return;
    }

    const auto beatsIntoBar = intoBar / beat;

    if (std::abs (beatsIntoBar - std::round (beatsIntoBar)) > epsilon)
    {
        // Recover the beat grid after a reset or a boundary in a fractional meter.
        const auto toBeat = (std::floor (beatsIntoBar) + 1.0) * beat - intoBar;
        addCut (std::min (toBeat, boundary));
        return;
    }

    if (boundary >= beat - epsilon && random.chance (params.bounceChance))
    {
        const auto even = random.chance (0.5);

        if (params.straight)
        {
            // A gallop where the triplet was even, a dotted pair where it shuffled.
            addCut ((even ? 1.0 : 3.0) * beat / 4.0);
            addCut (beat / 4.0);

            if (even)
                addCut (beat / 2.0);

            return;
        }

        addCut ((even ? 1.0 : 2.0) * beat / 3.0);
        addCut (beat / 3.0);

        if (even)
            addCut (beat / 3.0);

        return;
    }

    addCut (std::min ((random.chance (0.5) ? 2.0 : 1.0) * beat, boundary));
}

} // namespace livecut::engine
