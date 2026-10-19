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
    const auto ratchetChance = GENERATE (0.0, 0.7, 1.0);
    Bounce procedure;
    auto params = ordinaryMoves (0.4);
    params.ratchetChance = ratchetChance;
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

TEST_CASE ("At full Ratchet every phrase ends in a stepped ratchet", "[bounce][ratchet]")
{
    const auto unitsPerBar = GENERATE (6, 8, 16, 32);
    const auto beatsPerBar = GENERATE (4.0, 3.0, 3.5, 1.5, 0.5);
    const auto climb = GENERATE (7, 12);
    Bounce procedure;
    auto params = ordinaryMoves (0.4);
    params.ratchetChance = 1.0;
    params.climb = climb;
    params.dutyCycle = 0.9;
    params.fillDutyCycle = 0.37;
    params.minCents = params.maxCents = -500.0f;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        CAPTURE (unitsPerBar, beatsPerBar, climb, seed);
        Random random (seed);
        params.minPhraseBars = params.maxPhraseBars = 1 + static_cast<int> (seed % 4);
        const auto phraseUnits = static_cast<double> (params.minPhraseBars * unitsPerBar);
        const auto zoneStart = ratchetZoneStart (phraseUnits, unitsPerBar, beatsPerBar);
        const auto zone = phraseUnits - zoneStart;
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar, beatsPerBar);
        const auto& last = blocks.back();

        REQUIRE_THAT (last.startUnits, WithinAbs (zoneStart, tolerance));
        REQUIRE (last.cuts.size() == 12);
        CHECK_THAT (totalUnits (last.cuts), WithinAbs (zone, tolerance));

        for (const auto& block : blocks)
        {
            const auto end = block.startUnits + totalUnits (block.cuts);
            CHECK_THAT (std::floor ((block.startUnits + tolerance) / unitsPerBar),
                        WithinAbs (std::floor ((end - tolerance) / unitsPerBar), tolerance));
            CHECK_FALSE ((block.startUnits < zoneStart - tolerance && end > zoneStart + tolerance));

            if (block.startUnits < zoneStart - tolerance)
                CHECK (block.cuts.size() <= 3);
        }

        for (std::size_t i = 0; i < last.cuts.size(); ++i)
        {
            CAPTURE (i);
            const auto expectedUnits = zone / (i < 3 ? 6.0 : i < 6 ? 12.0 : 24.0);
            const auto expectedCents = i < 3 ? 0.0 : i < 6 ? (climb == 7 ? 400.0 : 600.0)
                                                                       : 100.0 * climb;
            CHECK_THAT (last.cuts[i].units, WithinAbs (expectedUnits, tolerance));
            CHECK_THAT (last.cuts[i].cents, WithinAbs (expectedCents, 1.0e-6));
            CHECK_THAT (last.cuts[i].gate, WithinAbs (params.fillDutyCycle, tolerance));
        }
    }
}

TEST_CASE ("With Ratchet off no phrase ends in a ratchet", "[bounce][ratchet]")
{
    const auto unitsPerBar = GENERATE (6, 8, 16, 32);
    const auto beatsPerBar = GENERATE (4.0, 3.0, 3.5, 1.5, 0.5);
    Bounce procedure;
    const auto params = ordinaryMoves (0.4);

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, unitsPerBar, beatsPerBar);

        for (const auto& block : blocks)
            CHECK (block.cuts.size() <= 3);
    }
}

TEST_CASE ("A ratchet with no climb keeps every cut at the original pitch", "[bounce][ratchet]")
{
    Bounce procedure;
    auto params = ordinaryMoves (0.4);
    params.ratchetChance = 1.0;
    params.climb = 0;
    params.minCents = params.maxCents = 1200.0f;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        Random random (seed);
        const auto blocks = playPhrase (procedure, random, params, 8);
        REQUIRE (blocks.back().cuts.size() == 12);

        for (const auto& cut : blocks.back().cuts)
            CHECK_THAT (cut.cents, WithinAbs (0.0, 1.0e-6));
    }
}

TEST_CASE ("A phrase that starts inside the ratchet zone never ratchets", "[bounce][ratchet]")
{
    const auto unitsPerBar = GENERATE (6, 8, 16, 32);
    const auto beatsPerBar = GENERATE (4.0, 3.0, 3.5, 1.5, 0.5);
    Bounce procedure;
    auto params = ordinaryMoves (0.4);
    params.ratchetChance = 1.0;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        Random random (seed);
        BlockContext context;
        context.unitsPerBar = unitsPerBar;
        context.beatsPerBar = beatsPerBar;
        context.phraseUnits = 2.0 * unitsPerBar;
        context.unitsDone = ratchetZoneStart (context.phraseUnits, unitsPerBar, beatsPerBar) + 0.5;
        std::vector<Cut> cuts;

        while (context.unitsDone < context.phraseUnits - tolerance)
        {
            procedure.chooseBlock (random, params, context, cuts);
            REQUIRE (! cuts.empty());
            CHECK (cuts.size() <= 3);
            const auto units = totalUnits (cuts);
            REQUIRE (units > tolerance);
            context.unitsDone += units;
            REQUIRE (context.unitsDone <= context.phraseUnits + tolerance);
        }

        CHECK_THAT (context.unitsDone, WithinAbs (context.phraseUnits, tolerance));
    }
}

TEST_CASE ("Ratchet uses the shared pan and amplitude sweep and preserves its draws", "[bounce][ratchet]")
{
    Bounce procedure;
    auto params = ordinaryMoves (0.4);
    params.ratchetChance = 0.7;
    params.minPan = -0.8f;
    params.maxPan = 0.6f;
    params.minAmp = 0.2f;
    params.maxAmp = 0.9f;
    params.minCents = -1200.0f;
    params.maxCents = 1200.0f;
    BlockContext context;
    context.phraseUnits = 16.0;
    context.unitsDone = 12.0;
    bool sawRatchet = false;
    bool sawOrdinary = false;

    for (std::uint32_t seed = 1; seed <= 50; ++seed)
    {
        Random random (seed);
        Random expected (seed);
        std::vector<Cut> cuts;
        procedure.chooseBlock (random, params, context, cuts);

        if (! expected.chance (params.ratchetChance))
        {
            sawOrdinary = true;
            CHECK (cuts.size() <= 3);
            continue;
        }

        sawRatchet = true;
        REQUIRE (cuts.size() == 12);
        const auto startPan = expected.uniform (params.minPan, params.maxPan);
        const auto endPan = expected.uniform (params.minPan, params.maxPan);
        const auto startAmp = expected.uniform (params.minAmp, params.maxAmp);
        const auto endAmp = expected.uniform (params.minAmp, params.maxAmp);
        expected.uniform (params.minCents, params.maxCents);

        for (std::size_t i = 0; i < cuts.size(); ++i)
        {
            const auto phase = static_cast<double> (i) / 12.0;
            CHECK_THAT (cuts[i].pan, WithinAbs (startPan + (endPan - startPan) * phase, 1.0e-6));
            CHECK_THAT (cuts[i].amp, WithinAbs (startAmp + (endAmp - startAmp) * phase, 1.0e-6));
        }

        CHECK_THAT (random.unit(), WithinAbs (expected.unit(), tolerance));
    }

    CHECK (sawRatchet);
    CHECK (sawOrdinary);
}

TEST_CASE ("Ratchet is eligible only within epsilon of the zone start", "[bounce][ratchet]")
{
    const auto offset = GENERATE (-4.0e-9, -0.5e-9, 0.0, 0.5e-9, 4.0e-9, 0.5);
    Bounce procedure;
    auto params = ordinaryMoves (0.4);
    params.ratchetChance = 1.0;
    BlockContext context;
    context.phraseUnits = 16.0;
    context.unitsDone = 12.0 + offset;
    Random random (31);
    std::vector<Cut> cuts;
    procedure.chooseBlock (random, params, context, cuts);

    if (std::abs (offset) < tolerance)
    {
        REQUIRE (cuts.size() == 12);
        return;
    }

    REQUIRE (cuts.size() <= 3);
    // These off-beat positions draw only the recovery cut's amplitude.
    Random expected (31);
    expected.uniform (params.minAmp, params.maxAmp);
    CHECK_THAT (random.unit(), WithinAbs (expected.unit(), tolerance));
}
