// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineTestHelpers.h"
#include "engine/CutProcedures.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <set>

using namespace livecut::engine;
using namespace livecut::engine::testing;
using Catch::Matchers::WithinAbs;

namespace
{
BlockContext phraseStart (int bars, int unitsPerBar)
{
    BlockContext context;
    context.phraseUnits = static_cast<double> (bars * unitsPerBar);
    context.unitsPerBar = unitsPerBar;
    return context;
}
} // namespace

TEST_CASE ("CutProc11 phrases end exactly on the phrase boundary", "[cutproc11]")
{
    const auto unitsPerBar = GENERATE (6, 8, 12, 16, 18, 24, 32);
    CutProc11 procedure;
    CutParameters params;
    params.maxPhraseBars = 8;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar);

        for (const auto& block : blocks)
            for (const auto& cut : block.cuts)
                REQUIRE (cut.units > 0.0);
    }
}

// Livecut's own comment documents the bound: units = 2x + 1 <= subdiv / 2 + 1.
// Its rounding broke that bound at subdivisions 6 and 18.
TEST_CASE ("CutProc11 regular cuts are odd and at most half a bar plus one unit", "[cutproc11]")
{
    const auto unitsPerBar = GENERATE (6, 8, 12, 16, 18, 24, 32);
    CutProc11 procedure;
    CutParameters params;
    params.stutterChance = 0.0;
    params.maxPhraseBars = 8;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        Random random (seed);

        for (const auto& block : playPhrase (procedure, random, params, unitsPerBar))
        {
            for (const auto& cut : block.cuts)
            {
                REQUIRE (isWhole (cut.units));
                const auto units = static_cast<int> (std::round (cut.units));
                REQUIRE (units % 2 == 1);
                REQUIRE (units <= unitsPerBar / 2 + 1);
            }
        }
    }
}

TEST_CASE ("CutProc11 picks regular cut lengths uniformly", "[cutproc11]")
{
    CutProc11 procedure;
    CutParameters params;
    params.stutterChance = 0.0;
    params.minRepeats = 0;
    params.maxRepeats = 0;
    Random random (11);
    std::vector<Cut> cuts;
    std::array<int, 3> counts {}; // 1, 3 and 5 units

    for (int i = 0; i < 30000; ++i)
    {
        procedure.chooseBlock (random, params, phraseStart (8, 8), cuts);
        REQUIRE (cuts.size() == 1);
        ++counts[static_cast<std::size_t> (std::round (cuts[0].units)) / 2];
    }

    for (const auto count : counts)
    {
        CHECK (count > 9500);
        CHECK (count < 10500);
    }
}

TEST_CASE ("CutProc11 plays a cut once plus its chosen number of repeats", "[cutproc11]")
{
    CutProc11 procedure;
    CutParameters params;
    params.stutterChance = 0.0;
    params.minRepeats = 1;
    params.maxRepeats = 3;
    params.dutyCycle = 0.5;
    Random random (13);
    std::vector<Cut> cuts;
    std::set<std::size_t> plays;

    for (int i = 0; i < 1000; ++i)
    {
        procedure.chooseBlock (random, params, phraseStart (8, 8), cuts);
        REQUIRE (cuts.size() >= 2);
        REQUIRE (cuts.size() <= 4);
        plays.insert (cuts.size());

        for (const auto& cut : cuts)
        {
            REQUIRE_THAT (cut.units, WithinAbs (cuts[0].units, tolerance));
            REQUIRE_THAT (cut.gate, WithinAbs (0.5, tolerance));
        }
    }

    CHECK (plays == std::set<std::size_t> { 2, 3, 4 });
}

TEST_CASE ("CutProc11 stutters fill the rest of the phrase", "[cutproc11]")
{
    CutProc11 procedure;
    CutParameters params;
    params.stutterChance = 1.0;
    params.stutterArea = 0.5;
    params.fillDutyCycle = 0.75;
    Random random (17);
    std::vector<Cut> cuts;
    std::set<std::size_t> cutsPerUnit;

    auto context = phraseStart (1, 8);
    context.unitsDone = 5.0; // 3 units left, inside the last half bar

    for (int i = 0; i < 500; ++i)
    {
        procedure.chooseBlock (random, params, context, cuts);
        REQUIRE_THAT (totalUnits (cuts), WithinAbs (3.0, 1.0e-6));
        REQUIRE (cuts.size() % 3 == 0);

        const auto perUnit = cuts.size() / 3;
        cutsPerUnit.insert (perUnit);

        for (const auto& cut : cuts)
        {
            REQUIRE_THAT (cut.units, WithinAbs (1.0 / static_cast<double> (perUnit), tolerance));
            REQUIRE_THAT (cut.gate, WithinAbs (0.75, tolerance));
        }
    }

    CHECK (cutsPerUnit == std::set<std::size_t> { 1, 2, 3, 4, 6, 8 });
}

TEST_CASE ("CutProc11 never stutters outside the stutter area", "[cutproc11]")
{
    CutProc11 procedure;
    CutParameters params;
    params.stutterChance = 1.0;
    params.stutterArea = 0.5;
    params.minRepeats = 0;
    params.maxRepeats = 0;
    Random random (19);
    std::vector<Cut> cuts;

    auto context = phraseStart (1, 8);
    context.unitsDone = 4.0; // exactly half a bar left is not inside the area

    for (int i = 0; i < 500; ++i)
    {
        procedure.chooseBlock (random, params, context, cuts);
        REQUIRE (cuts.size() == 1);
        REQUIRE (isWhole (cuts[0].units));
    }
}

TEST_CASE ("CutProc11 stutters sweep pitch up from the original", "[cutproc11]")
{
    CutProc11 procedure;
    CutParameters params;
    params.stutterChance = 1.0;
    params.minCents = 1200.0f;
    params.maxCents = 1200.0f;
    Random random (23);
    std::vector<Cut> cuts;

    auto context = phraseStart (1, 8);
    context.unitsDone = 6.0;
    procedure.chooseBlock (random, params, context, cuts);

    REQUIRE (cuts.size() >= 2);
    CHECK_THAT (cuts.front().cents, WithinAbs (0.0, tolerance));

    for (std::size_t i = 1; i < cuts.size(); ++i)
    {
        CHECK (cuts[i].cents > cuts[i - 1].cents);
        CHECK (cuts[i].cents < 1200.0f);
    }
}

TEST_CASE ("CutProc11's largest stutter fits the cut capacity", "[cutproc11]")
{
    CutProc11 procedure;
    CutParameters params;
    params.stutterChance = 1.0;
    params.stutterArea = 1.0;
    Random random (29);
    std::vector<Cut> cuts;

    auto context = phraseStart (1, 32);
    context.unitsDone = 1.0; // 31 units left

    std::size_t largest = 0;

    for (int i = 0; i < 2000; ++i)
    {
        procedure.chooseBlock (random, params, context, cuts);
        REQUIRE (cuts.size() <= maxCutsPerBlock);
        largest = std::max (largest, cuts.size());
    }

    CHECK (largest == 31 * 8);
}
