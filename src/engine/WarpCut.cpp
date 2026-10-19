// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Livecut, Copyright 2003-2004 Remy Muller, which implements
// WarpCutProc1 from Nick Collins' BBCut library for SuperCollider.

#include "engine/CutProcedures.h"
#include "engine/CutShaping.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

namespace livecut::engine
{

void WarpCut::chooseBlock (Random& random,
                           const CutParameters& params,
                           const BlockContext& context,
                           std::vector<Cut>& cuts)
{
    cuts.clear();

    static constexpr std::array blockSizes { 1.0, 2.0, 4.0 };
    static constexpr std::array blockWeights { 0.5, 0.4, 0.1 };
    const auto unitsLeft = context.phraseUnits - context.unitsDone;
    const auto blockUnits = std::min (blockSizes[random.weighted (blockWeights)], unitsLeft);

    if (random.chance (params.straightChance))
    {
        // Livecut quantises a straight block's duty cycle to whole units.
        const auto soundingUnits = std::max (std::floor (params.dutyCycle * blockUnits), 1.0);

        Cut cut;
        cut.units = blockUnits;
        cut.gate = std::min (soundingUnits / blockUnits, 1.0);
        cut.amp = static_cast<float> (random.uniform (params.minAmp, params.maxAmp));
        cuts.push_back (cut);
        return;
    }

    static constexpr std::array shortRolls { 4, 8, 16 };
    static constexpr std::array longRolls { 8, 16, 32 };
    const auto blockBeats = blockUnits * context.beatsPerBar / context.unitsPerBar;
    const auto& rolls = blockBeats < 1.0 ? shortRolls : longRolls;
    const auto repeats = rolls[static_cast<std::size_t> (random.integer (0, static_cast<int> (rolls.size()) - 1))];

    Cut cut;
    cut.gate = params.fillDutyCycle;
    cuts.assign (static_cast<std::size_t> (repeats), cut);

    if (random.chance (params.regularChance))
    {
        for (auto& c : cuts)
            c.units = blockUnits / repeats;
    }
    else
    {
        // Cut lengths form a geometric series with ratio `accel` that sums to the block.
        const auto accel = params.accel;
        assert (accel > 0.0 && accel < 1.0);
        const auto first = blockUnits * (1.0 - accel) / (1.0 - std::pow (accel, repeats));

        for (std::size_t i = 0; i < cuts.size(); ++i)
            cuts[i].units = first * std::pow (accel, static_cast<double> (i));

        // A ritard reverses the durations only, so the sweep still starts at the original pitch.
        if (random.chance (params.ritardChance))
            std::reverse (cuts.begin(), cuts.end());
    }

    sweep (cuts, random, params);
}

} // namespace livecut::engine
