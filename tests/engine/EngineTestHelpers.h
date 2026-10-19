// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/CutProcedure.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace livecut::engine::testing
{

inline constexpr double tolerance = 1.0e-9;

struct PlayedBlock
{
    double startUnits = 0.0;
    std::vector<Cut> cuts;
};

// Plays one whole phrase through a procedure the way the cutter will, and
// checks the contract every block must keep along the way.
inline std::vector<PlayedBlock> playPhrase (CutProcedure& procedure,
                                            Random& random,
                                            const CutParameters& params,
                                            int unitsPerBar,
                                            double beatsPerBar = 4.0)
{
    const auto bars = procedure.choosePhraseBars (random, params);
    REQUIRE (bars >= std::min (params.minPhraseBars, params.maxPhraseBars));
    REQUIRE (bars >= 1);

    BlockContext context;
    context.phraseUnits = static_cast<double> (bars * unitsPerBar);
    context.unitsPerBar = unitsPerBar;
    context.beatsPerBar = beatsPerBar;

    std::vector<PlayedBlock> blocks;
    std::vector<Cut> cuts;

    while (context.unitsDone < context.phraseUnits - tolerance)
    {
        procedure.chooseBlock (random, params, context, cuts);

        const auto blockUnits = totalUnits (cuts);
        REQUIRE (! cuts.empty());
        REQUIRE (cuts.size() <= maxCutsPerBlock);
        REQUIRE (blockUnits > tolerance);
        REQUIRE (context.unitsDone + blockUnits <= context.phraseUnits + tolerance);

        blocks.push_back ({ context.unitsDone, cuts });
        context.unitsDone += blockUnits;
        REQUIRE (blocks.size() < 10000);
    }

    REQUIRE (std::abs (context.unitsDone - context.phraseUnits) < 1.0e-6);
    return blocks;
}

inline bool isWhole (double value)
{
    return std::abs (value - std::round (value)) < tolerance;
}

} // namespace livecut::engine::testing
