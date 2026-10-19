// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineTestHelpers.h"
#include "engine/CutProcedures.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

using namespace livecut::engine;
using namespace livecut::engine::testing;
using Catch::Matchers::WithinAbs;

namespace
{
BlockContext longPhraseStart (int unitsPerBar = 8)
{
    BlockContext context;
    context.phraseUnits = static_cast<double> (8 * unitsPerBar);
    context.unitsPerBar = unitsPerBar;
    return context;
}
} // namespace

TEST_CASE ("WarpCut blocks are 1, 2 or 4 units, clipped to the phrase", "[warpcut]")
{
    const auto unitsPerBar = GENERATE (6, 8, 16, 32);
    WarpCut procedure;
    CutParameters params;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        Random random (seed);
        params.maxPhraseBars = 1 + static_cast<int> (seed % 4);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar);
        const auto phraseUnits = blocks.back().startUnits + totalUnits (blocks.back().cuts);

        for (const auto& block : blocks)
        {
            const auto units = totalUnits (block.cuts);
            const auto left = phraseUnits - block.startUnits;
            const auto isChoice = std::abs (units - 1.0) < 1.0e-6 || std::abs (units - 2.0) < 1.0e-6 || std::abs (units - 4.0) < 1.0e-6;
            REQUIRE ((isChoice || std::abs (units - left) < 1.0e-6));
        }
    }
}

// Livecut compared the roll length in bars against one beat, so it never chose the long rolls.
TEST_CASE ("WarpCut rolls under a beat repeat 4, 8 or 16 times, longer rolls 8, 16 or 32 times", "[warpcut]")
{
    WarpCut procedure;
    CutParameters params;
    params.straightChance = 0.0;
    Random random (31);
    std::vector<Cut> cuts;
    std::set<std::size_t> shortRolls;
    std::set<std::size_t> longRolls;

    for (int i = 0; i < 3000; ++i)
    {
        procedure.chooseBlock (random, params, longPhraseStart (8), cuts); // 2 units per beat
        const auto beats = totalUnits (cuts) / 2.0;
        (beats < 1.0 - 1.0e-6 ? shortRolls : longRolls).insert (cuts.size());
    }

    CHECK (shortRolls == std::set<std::size_t> { 4, 8, 16 });
    CHECK (longRolls == std::set<std::size_t> { 8, 16, 32 });
}

TEST_CASE ("WarpCut regular rolls split the block evenly", "[warpcut]")
{
    WarpCut procedure;
    CutParameters params;
    params.straightChance = 0.0;
    params.regularChance = 1.0;
    params.fillDutyCycle = 0.6;
    Random random (37);
    std::vector<Cut> cuts;

    for (int i = 0; i < 500; ++i)
    {
        procedure.chooseBlock (random, params, longPhraseStart(), cuts);
        REQUIRE (cuts.size() >= 4);
        const auto expected = totalUnits (cuts) / static_cast<double> (cuts.size());

        for (const auto& cut : cuts)
        {
            REQUIRE_THAT (cut.units, WithinAbs (expected, tolerance));
            REQUIRE_THAT (cut.gate, WithinAbs (0.6, tolerance));
        }
    }
}

TEST_CASE ("WarpCut accelerating rolls shrink geometrically and fill the block", "[warpcut]")
{
    WarpCut procedure;
    CutParameters params;
    params.straightChance = 0.0;
    params.regularChance = 0.0;
    params.ritardChance = 0.0;
    params.accel = 0.8;
    Random random (41);
    std::vector<Cut> cuts;

    for (int i = 0; i < 500; ++i)
    {
        procedure.chooseBlock (random, params, longPhraseStart(), cuts);
        const auto units = totalUnits (cuts);
        REQUIRE ((std::abs (units - 1.0) < 1.0e-6 || std::abs (units - 2.0) < 1.0e-6 || std::abs (units - 4.0) < 1.0e-6));

        for (std::size_t c = 1; c < cuts.size(); ++c)
            REQUIRE_THAT (cuts[c].units / cuts[c - 1].units, WithinAbs (0.8, 1.0e-9));
    }
}

TEST_CASE ("WarpCut ritards reverse the durations but still start at the original pitch", "[warpcut]")
{
    WarpCut procedure;
    CutParameters params;
    params.straightChance = 0.0;
    params.regularChance = 0.0;
    params.ritardChance = 1.0;
    params.minCents = 600.0f;
    params.maxCents = 600.0f;
    Random random (43);
    std::vector<Cut> cuts;

    procedure.chooseBlock (random, params, longPhraseStart(), cuts);

    REQUIRE (cuts.size() >= 4);
    CHECK_THAT (cuts.front().cents, WithinAbs (0.0, tolerance));

    for (std::size_t c = 1; c < cuts.size(); ++c)
    {
        CHECK (cuts[c].units > cuts[c - 1].units);
        CHECK (cuts[c].cents > cuts[c - 1].cents);
    }
}

TEST_CASE ("WarpCut straight blocks quantise the duty cycle to whole units", "[warpcut]")
{
    WarpCut procedure;
    CutParameters params;
    params.straightChance = 1.0;
    params.dutyCycle = 0.6;
    Random random (47);
    std::vector<Cut> cuts;
    std::set<int> blockSizes;

    for (int i = 0; i < 500; ++i)
    {
        procedure.chooseBlock (random, params, longPhraseStart(), cuts);
        REQUIRE (cuts.size() == 1);

        const auto units = static_cast<int> (std::round (cuts[0].units));
        const auto soundingUnits = std::max (static_cast<int> (std::floor (0.6 * units)), 1);
        REQUIRE_THAT (cuts[0].gate * cuts[0].units, WithinAbs (soundingUnits, tolerance));
        blockSizes.insert (units);
    }

    CHECK (blockSizes == std::set<int> { 1, 2, 4 });
}
