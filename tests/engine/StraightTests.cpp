// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineTestHelpers.h"
#include "engine/CutProcedures.h"

#include <catch2/catch_template_test_macros.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cstdint>
#include <vector>

using namespace livecut::engine;
using namespace livecut::engine::testing;
using Catch::Matchers::WithinAbs;

namespace
{
// 1/64 of a beat is the finest even division the pad's grids reach:
// 32 units per bar at 8 cuts per unit.
bool evenDivisions (const std::vector<Cut>& cuts, int unitsPerBar, double beatsPerBar)
{
    for (const auto& cut : cuts)
    {
        const auto beats = cut.units * beatsPerBar / unitsPerBar;

        if (! isWhole (beats * 64.0))
            return false;
    }

    return true;
}

struct Meter
{
    int unitsPerBar;
    double beatsPerBar;
};

// Bit-identical: a zero-width match fails on any difference at all.
void requireIdentical (const std::vector<PlayedBlock>& a, const std::vector<PlayedBlock>& b)
{
    REQUIRE (a.size() == b.size());

    for (std::size_t i = 0; i < a.size(); ++i)
    {
        CAPTURE (i);
        REQUIRE_THAT (a[i].startUnits, WithinAbs (b[i].startUnits, 0.0));
        REQUIRE (a[i].cuts.size() == b[i].cuts.size());

        for (std::size_t j = 0; j < a[i].cuts.size(); ++j)
        {
            CAPTURE (j);
            const auto& left = a[i].cuts[j];
            const auto& right = b[i].cuts[j];
            REQUIRE_THAT (left.units, WithinAbs (right.units, 0.0));
            REQUIRE_THAT (left.gate, WithinAbs (right.gate, 0.0));
            REQUIRE_THAT (left.pan, WithinAbs (right.pan, 0.0));
            REQUIRE_THAT (left.amp, WithinAbs (right.amp, 0.0));
            REQUIRE_THAT (left.cents, WithinAbs (right.cents, 0.0));
        }
    }
}
} // namespace

TEST_CASE ("Straight Cut rolls only in even divisions", "[straight]")
{
    const auto unitsPerBar = GENERATE (8, 16, 32);
    CutProc11 procedure;
    CutParameters params;
    params.straight = true;
    params.stutterChance = 1.0;
    params.stutterArea = 1.0;

    for (std::uint32_t seed = 1; seed <= 200; ++seed)
    {
        CAPTURE (unitsPerBar, seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar);

        for (const auto& block : blocks)
            REQUIRE (evenDivisions (block.cuts, unitsPerBar, 4.0));
    }
}

TEST_CASE ("Straight Bounce bounces and ratchets in even divisions", "[straight]")
{
    const auto meter = GENERATE (values<Meter> ({ { 16, 4.0 }, { 12, 3.0 } }));
    Bounce procedure;
    CutParameters params;
    params.straight = true;
    params.bounceChance = 1.0;
    params.ratchetChance = 1.0;

    for (std::uint32_t seed = 1; seed <= 200; ++seed)
    {
        CAPTURE (meter.unitsPerBar, meter.beatsPerBar, seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, meter.unitsPerBar, meter.beatsPerBar);

        for (const auto& block : blocks)
            REQUIRE (evenDivisions (block.cuts, meter.unitsPerBar, meter.beatsPerBar));
    }
}

TEST_CASE ("A straight ratchet climbs in three steps over eight cuts", "[straight]")
{
    Bounce procedure;
    CutParameters params;
    params.straight = true;
    params.minPhraseBars = params.maxPhraseBars = 1;
    params.bounceChance = 0.0;
    params.halftimeChance = 0.0;
    params.ratchetChance = 1.0;
    params.climb = 12;
    params.fillDutyCycle = 0.37;
    constexpr int unitsPerBar = 16;
    constexpr double zone = 8.0; // two beats
    constexpr std::array expectedUnits { 2.0, 2.0, 1.0, 1.0, 0.5, 0.5, 0.5, 0.5 };
    constexpr std::array expectedCents { 0.0, 0.0, 600.0, 600.0, 1200.0, 1200.0, 1200.0, 1200.0 };

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        CAPTURE (seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar);
        const auto& last = blocks.back();

        REQUIRE_THAT (last.startUnits, WithinAbs (unitsPerBar - zone, tolerance));
        REQUIRE (last.cuts.size() == expectedUnits.size());
        CHECK_THAT (totalUnits (last.cuts), WithinAbs (zone, tolerance));

        for (std::size_t i = 0; i < last.cuts.size(); ++i)
        {
            CAPTURE (i);
            CHECK_THAT (last.cuts[i].units, WithinAbs (expectedUnits[i], tolerance));
            CHECK_THAT (last.cuts[i].cents, WithinAbs (expectedCents[i], 1.0e-6));
            CHECK_THAT (last.cuts[i].gate, WithinAbs (params.fillDutyCycle, tolerance));
        }
    }
}

TEST_CASE ("Straight Glitch plays only even fills", "[straight]")
{
    const auto unitsPerBar = GENERATE (16, 32);
    SQPusher procedure;
    CutParameters params;
    params.straight = true;
    params.activity = 0.95;

    for (std::uint32_t seed = 1; seed <= 300; ++seed)
    {
        CAPTURE (unitsPerBar, seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar);

        for (const auto& block : blocks)
            REQUIRE (evenDivisions (block.cuts, unitsPerBar, 4.0));
    }
}

TEMPLATE_TEST_CASE ("Straight off leaves every procedure's cuts unchanged", "[straight]",
                    CutProc11, Bounce, SQPusher, WarpCut)
{
    const CutParameters reference;
    auto params = reference;
    params.straight = false;
    REQUIRE_FALSE (reference.straight);

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        CAPTURE (seed);
        TestType withDefaults;
        TestType withStraightOff;
        Random first (seed);
        Random second (seed);
        const auto a = playPhrase (withDefaults, first, reference, 16);
        const auto b = playPhrase (withStraightOff, second, params, 16);
        requireIdentical (a, b);
        CHECK_THAT (first.unit(), WithinAbs (second.unit(), 0.0));
    }
}
