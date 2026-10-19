// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Livecut, Copyright 2003-2004 Remy Muller, which implements
// BBCutProc11 from Nick Collins' BBCut library for SuperCollider.

#include "engine/CutProcedures.h"
#include "engine/CutShaping.h"

#include <array>
#include <cassert>
#include <cmath>

namespace livecut::engine
{

void CutProc11::chooseBlock (Random& random,
                             const CutParameters& params,
                             const BlockContext& context,
                             std::vector<Cut>& cuts)
{
    const auto unitsLeft = static_cast<int> (std::lround (context.phraseUnits - context.unitsDone));
    assert (unitsLeft >= 1);
    cuts.clear();

    const auto inStutterArea = unitsLeft < params.stutterArea * context.unitsPerBar;

    if (inStutterArea && random.chance (params.stutterChance))
    {
        // Roll out the rest of the phrase in cuts of 1/n units. Livecut's
        // weighted choice of n replaces BBCut's fixed stutter speed.
        static constexpr std::array cutsPerUnit { 1, 2, 3, 4, 6, 8 };
        static constexpr std::array weights { 0.4, 0.3, 0.1, 0.1, 0.05, 0.05 };
        const auto perUnit = cutsPerUnit[random.weighted (weights)];

        Cut cut;
        cut.units = 1.0 / perUnit;
        cut.gate = params.fillDutyCycle;
        cuts.assign (static_cast<std::size_t> (unitsLeft * perUnit), cut);
        sweep (cuts, random, params);
        return;
    }

    // An odd number of units, 2x + 1 <= subdivision / 2 + 1.
    auto unitsInCut = 2 * random.integer (0, context.unitsPerBar / 4) + 1;

    while (unitsInCut > unitsLeft)
        unitsInCut -= 2;

    auto plays = random.integer (params.minRepeats, params.maxRepeats) + 1;

    while (plays > 1 && plays * unitsInCut > unitsLeft)
        --plays;

    for (int i = 0; i < plays; ++i)
    {
        Cut cut;
        cut.units = unitsInCut;
        cut.gate = params.dutyCycle;
        cut.amp = static_cast<float> (random.uniform (params.minAmp, params.maxAmp));
        cuts.push_back (cut);
    }
}

} // namespace livecut::engine
