// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bounce is new to this port, not from Nick Collins' BBCut library.

#include "engine/CutProcedures.h"

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

    const auto addCut = [&] (double units)
    {
        Cut cut;
        cut.units = units;
        cut.gate = params.dutyCycle;
        cut.amp = static_cast<float> (random.uniform (params.minAmp, params.maxAmp));
        cuts.push_back (cut);
    };

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
        addCut ((even ? 1.0 : 2.0) * beat / 3.0);
        addCut (beat / 3.0);

        if (even)
            addCut (beat / 3.0);

        return;
    }

    addCut (std::min ((random.chance (0.5) ? 2.0 : 1.0) * beat, boundary));
}

} // namespace livecut::engine
