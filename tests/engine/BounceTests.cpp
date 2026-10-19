// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineTestHelpers.h"
#include "engine/CutProcedures.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

using namespace livecut::engine;
using namespace livecut::engine::testing;
using Catch::Matchers::WithinAbs;

namespace
{
double unitsIntoBar (double position, int unitsPerBar)
{
    return position - std::floor ((position + tolerance) / unitsPerBar) * unitsPerBar;
}

double ratchetZoneStart (double phraseUnits, int unitsPerBar, double beatsPerBar)
{
    const auto beat = unitsPerBar / beatsPerBar;
    return phraseUnits - std::min ((beatsPerBar >= 2.0 ? 2.0 : 1.0) * beat,
                                  static_cast<double> (unitsPerBar));
}

CutParameters ordinaryMoves (double bounceChance)
{
    CutParameters params;
    params.bounceChance = bounceChance;
    params.ratchetChance = 0.0;
    params.halftimeChance = 0.0;
    return params;
}
} // namespace

TEST_CASE ("Bounce parameters have their specified defaults", "[bounce]")
{
    const CutParameters params;
    CHECK_THAT (params.bounceChance, WithinAbs (0.4, tolerance));
    CHECK_THAT (params.ratchetChance, WithinAbs (0.7, tolerance));
    CHECK (params.climb == 7);
    CHECK_THAT (params.halftimeChance, WithinAbs (0.15, tolerance));
}

TEST_CASE ("Bounce blocks never cross a bar line or the ratchet zone", "[bounce]")
{
    const auto unitsPerBar = GENERATE (6, 8, 16, 32);
    const auto beatsPerBar = GENERATE (4.0, 3.0, 3.5, 1.5, 0.5);
    Bounce procedure;
    const auto params = ordinaryMoves (0.4);

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        CAPTURE (unitsPerBar, beatsPerBar, seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar, beatsPerBar);
        const auto phraseUnits = blocks.back().startUnits + totalUnits (blocks.back().cuts);
        const auto zoneStart = ratchetZoneStart (phraseUnits, unitsPerBar, beatsPerBar);

        for (const auto& block : blocks)
        {
            const auto end = block.startUnits + totalUnits (block.cuts);
            CHECK_THAT (std::floor ((block.startUnits + tolerance) / unitsPerBar),
                        WithinAbs (std::floor ((end - tolerance) / unitsPerBar), tolerance));
            CHECK_FALSE ((block.startUnits < zoneStart - tolerance && end > zoneStart + tolerance));
        }
    }
}

TEST_CASE ("At full Bounce every whole on-beat beat is a triplet", "[bounce]")
{
    const auto unitsPerBar = GENERATE (6, 8, 16, 32);
    const auto beatsPerBar = GENERATE (4.0, 3.0, 3.5);
    const auto beat = unitsPerBar / beatsPerBar;
    Bounce procedure;
    const auto params = ordinaryMoves (1.0);
    bool sawEven = false;
    bool sawShuffle = false;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        CAPTURE (unitsPerBar, beatsPerBar, seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar, beatsPerBar);
        const auto phraseUnits = blocks.back().startUnits + totalUnits (blocks.back().cuts);
        const auto zoneStart = ratchetZoneStart (phraseUnits, unitsPerBar, beatsPerBar);

        for (const auto& block : blocks)
        {
            const auto intoBar = unitsIntoBar (block.startUnits, unitsPerBar);
            const auto wholeBeatFits = unitsPerBar - intoBar >= beat - tolerance
                                    && phraseUnits - block.startUnits >= beat - tolerance
                                    && (block.startUnits >= zoneStart - tolerance
                                        || zoneStart - block.startUnits >= beat - tolerance);

            if (! isWhole (intoBar / beat) || ! wholeBeatFits)
            {
                REQUIRE (block.cuts.size() == 1);
                continue;
            }

            REQUIRE_THAT (totalUnits (block.cuts), WithinAbs (beat, tolerance));
            REQUIRE ((block.cuts.size() == 2 || block.cuts.size() == 3));
            const auto even = block.cuts.size() == 3;
            sawEven = sawEven || even;
            sawShuffle = sawShuffle || ! even;
            REQUIRE_THAT (block.cuts.front().units, WithinAbs ((even ? 1.0 : 2.0) * beat / 3.0, tolerance));

            for (std::size_t i = 1; i < block.cuts.size(); ++i)
                REQUIRE_THAT (block.cuts[i].units, WithinAbs (beat / 3.0, tolerance));
        }
    }

    CHECK (sawEven);
    CHECK (sawShuffle);
}

TEST_CASE ("With Bounce off every block is a single straight cut", "[bounce]")
{
    const auto unitsPerBar = GENERATE (6, 8, 16, 32);
    const auto beatsPerBar = GENERATE (4.0, 3.0, 3.5);
    const auto beat = unitsPerBar / beatsPerBar;
    Bounce procedure;
    const auto params = ordinaryMoves (0.0);

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        CAPTURE (unitsPerBar, beatsPerBar, seed);
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar, beatsPerBar);
        const auto phraseUnits = blocks.back().startUnits + totalUnits (blocks.back().cuts);
        const auto zoneStart = ratchetZoneStart (phraseUnits, unitsPerBar, beatsPerBar);

        for (const auto& block : blocks)
        {
            REQUIRE (block.cuts.size() == 1);
            const auto& cut = block.cuts.front();
            const auto end = block.startUnits + cut.units;
            const auto intoBar = unitsIntoBar (block.startUnits, unitsPerBar);
            const auto atBoundary = isWhole (end / unitsPerBar)
                                 || std::abs (end - zoneStart) < tolerance
                                 || std::abs (end - phraseUnits) < tolerance;
            REQUIRE_THAT (cut.cents, WithinAbs (0.0, tolerance));
            REQUIRE (cut.units <= 2.0 * beat + tolerance);

            if (! isWhole (intoBar / beat))
            {
                const auto toBeat = (std::floor (intoBar / beat) + 1.0) * beat - intoBar;
                REQUIRE (cut.units <= toBeat + tolerance);
                REQUIRE ((std::abs (cut.units - toBeat) < tolerance || atBoundary));
                continue;
            }

            REQUIRE ((std::abs (cut.units - beat) < tolerance
                      || std::abs (cut.units - 2.0 * beat) < tolerance || atBoundary));
        }
    }
}

TEST_CASE ("A phrase that starts off the beat gets back onto it", "[bounce]")
{
    Bounce procedure;
    const auto params = ordinaryMoves (1.0);
    BlockContext context;
    context.unitsDone = 1.0;
    context.unitsPerBar = 6;
    context.phraseUnits = 12.0;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        Random random (seed);
        std::vector<Cut> cuts;
        procedure.chooseBlock (random, params, context, cuts);
        REQUIRE (cuts.size() == 1);
        CHECK_THAT (totalUnits (cuts), WithinAbs (0.5, tolerance));
        CHECK (isWhole (unitsIntoBar (context.unitsDone + totalUnits (cuts), 6) / 1.5));
    }
}

TEST_CASE ("Bounce groove and triplet cuts use Duty and independent amplitudes", "[bounce]")
{
    const auto bounceChance = GENERATE (0.0, 1.0);
    Bounce procedure;
    auto params = ordinaryMoves (bounceChance);
    params.dutyCycle = 0.63;
    params.fillDutyCycle = 0.12;
    params.minAmp = 0.2f;
    params.maxAmp = 0.8f;
    params.minPan = params.maxPan = 0.7f;
    params.minCents = params.maxCents = 700.0f;
    BlockContext context;
    context.phraseUnits = 32.0;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        Random random (seed);
        Random expected (seed);
        std::vector<Cut> cuts;
        procedure.chooseBlock (random, params, context, cuts);
        expected.chance (bounceChance);
        expected.chance (0.5); // both moves choose a shape before drawing amplitudes

        REQUIRE (! cuts.empty());

        for (const auto& cut : cuts)
        {
            CHECK_THAT (cut.gate, WithinAbs (params.dutyCycle, tolerance));
            CHECK_THAT (cut.pan, WithinAbs (0.0, tolerance));
            CHECK_THAT (cut.cents, WithinAbs (0.0, tolerance));
            const auto amp = static_cast<float> (expected.uniform (params.minAmp, params.maxAmp));
            CHECK_THAT (cut.amp, WithinAbs (amp, tolerance));
        }
    }
}

TEST_CASE ("Bounce is deterministic", "[bounce]")
{
    const auto unitsPerBar = GENERATE (6, 8, 16, 32);
    const auto beatsPerBar = GENERATE (4.0, 3.0, 3.5);
    Bounce procedure;
    auto params = ordinaryMoves (0.4);
    params.minAmp = 0.2f;
    params.maxAmp = 0.8f;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        Random first (seed);
        Random second (seed);
        const auto a = playPhrase (procedure, first, params, unitsPerBar, beatsPerBar);
        const auto b = playPhrase (procedure, second, params, unitsPerBar, beatsPerBar);
        REQUIRE (a.size() == b.size());

        for (std::size_t i = 0; i < a.size(); ++i)
        {
            REQUIRE_THAT (a[i].startUnits, WithinAbs (b[i].startUnits, tolerance));
            REQUIRE (a[i].cuts.size() == b[i].cuts.size());

            for (std::size_t j = 0; j < a[i].cuts.size(); ++j)
            {
                const auto& left = a[i].cuts[j];
                const auto& right = b[i].cuts[j];
                CHECK_THAT (left.units, WithinAbs (right.units, tolerance));
                CHECK_THAT (left.gate, WithinAbs (right.gate, tolerance));
                CHECK_THAT (left.amp, WithinAbs (right.amp, tolerance));
                CHECK_THAT (left.pan, WithinAbs (right.pan, tolerance));
                CHECK_THAT (left.cents, WithinAbs (right.cents, tolerance));
            }
        }
    }
}
