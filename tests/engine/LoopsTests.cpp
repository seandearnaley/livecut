// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineTestHelpers.h"
#include "engine/CutProcedures.h"
#include "engine/Cutter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace livecut::engine;
using namespace livecut::engine::testing;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int unitsPerBar = 16;

CutParameters loops (double loopBars, int depth, bool straight = false)
{
    CutParameters params;
    params.loopBars = loopBars;
    params.loopDepth = depth;
    params.straight = straight;
    return params;
}

struct Laid
{
    double start = 0.0; // units into the phrase
    bool captured = false;
    std::vector<Cut> cuts;
};

// The blocks Loops lays out for a phrase started `engage` units into it, as the
// cutter asks for them: the first block captures, and the rest know it. Checks
// the procedure's contract on the way.
std::vector<Laid> layOut (const CutParameters& params, double beatsPerBar, double engage = 0.0, bool everCaptures = true)
{
    Loops procedure;
    Random random (7), untouched (7);
    BlockContext context;
    context.phraseUnits = static_cast<double> (procedure.choosePhraseBars (random, params) * unitsPerBar);
    context.unitsPerBar = unitsPerBar;
    context.beatsPerBar = beatsPerBar;
    context.unitsDone = engage;
    context.beatsPerSecond = 2.0;

    std::vector<Laid> blocks;
    std::vector<Cut> cuts;
    while (context.unitsDone < context.phraseUnits - tolerance)
    {
        procedure.chooseBlock (random, params, context, cuts);
        const auto units = totalUnits (cuts);
        REQUIRE (! cuts.empty());
        REQUIRE (cuts.size() <= maxCutsPerBlock);
        REQUIRE (units > tolerance);
        REQUIRE (context.unitsDone + units <= context.phraseUnits + tolerance);
        blocks.push_back ({ context.unitsDone, context.loopCaptured, cuts });
        context.unitsDone += units;
        context.loopCaptured = everCaptures;
        REQUIRE (blocks.size() <= 64);
    }
    REQUIRE_THAT (context.unitsDone, WithinAbs (context.phraseUnits, tolerance));
    CHECK (random.unit() == untouched.unit()); // Loops draws nothing
    return blocks;
}

// The pass table: each pass's cuts as levels, 0 the whole loop.
std::vector<int> levelsOf (int pass, int depth)
{
    switch (pass)
    {
        case 1: return { 0 };
        case 2: return depth >= 2 ? std::vector<int> { 1, 1 } : std::vector<int> { 0 };
        case 3:
            if (depth == 0)
                return { 0 };
            if (depth == 1)
                return { 1, 1 };
            if (depth == 2)
                return { 2, 2, 2, 2 };
            return { 2, 2, 3, 3, 3, 3 };
        default: return {};
    }
}

bool isTrack (const Laid& block)
{
    return block.cuts.size() == 1 && block.cuts.front().loopLevel == 0;
}

constexpr std::array meters { 4.0, 3.0, 5.0, 7.0, 2.5, 3.5, 6.5, 6.0, 1.5, 1.0, .5, .25 };
constexpr std::array eighthMeters { 4.0, 3.0, 5.0, 7.0, 2.5, 3.5, 6.5, 6.0 };
} // namespace

TEST_CASE ("Loops phrases are four passes, drawn from nothing", "[loops][layout]")
{
    Loops procedure;
    Random random (3), untouched (3);
    CutParameters params;
    params.minPhraseBars = 2;
    params.maxPhraseBars = 3;
    for (const auto [bars, expected] : { std::pair { 1.0, 4 }, { .5, 2 }, { .25, 1 }, { 0.0, 1 } })
    {
        params.loopBars = bars;
        CHECK (procedure.choosePhraseBars (random, params) == expected);
    }
    CHECK (random.unit() == untouched.unit());
}

TEST_CASE ("Every Loops block keeps the contract and its passes tile the phrase, started anywhere", "[loops][layout]")
{
    const auto meter = GENERATE (4.0, 3.0, 5.0, 7.0, 2.5, 3.5, 6.5, 6.0, 1.5, 1.0, .5, .25);
    for (const auto bars : { 1.0, .5, .25 })
        for (int depth = 0; depth <= 3; ++depth)
            for (const auto captures : { true, false })
            {
                const auto phraseUnits = std::max (1.0, 4.0 * bars) * unitsPerBar;
                const auto pass = unitsPerBar * bars;
                for (int engage = 0; engage < static_cast<int> (phraseUnits); ++engage)
                {
                    CAPTURE (meter, bars, depth, captures, engage);
                    for (const auto& block : layOut (loops (bars, depth), meter, engage, captures))
                    {
                        // Every block ends on a pass boundary.
                        const auto end = block.start + totalUnits (block.cuts);
                        CHECK_THAT (end / pass - std::round (end / pass), WithinAbs (0.0, tolerance));
                    }
                }
            }
}

TEST_CASE ("Captured, each pass plays the table for its depth", "[loops][layout]")
{
    for (const auto bars : { 1.0, .5, .25 })
        for (int depth = 0; depth <= 3; ++depth)
        {
            CAPTURE (bars, depth);
            const auto pass = unitsPerBar * bars;
            const auto blocks = layOut (loops (bars, depth), 4.0);
            REQUIRE (blocks.size() == 4);
            CHECK (isTrack (blocks[0]));
            CHECK_THAT (totalUnits (blocks[0].cuts), WithinAbs (pass, 0.0));
            for (int k = 1; k < 4; ++k)
            {
                CAPTURE (k);
                const auto expected = levelsOf (k, depth);
                const auto& cuts = blocks[static_cast<std::size_t> (k)].cuts;
                REQUIRE (cuts.size() == expected.size());
                for (std::size_t i = 0; i < cuts.size(); ++i)
                {
                    CHECK (cuts[i].loopLevel == expected[i] + 1);
                    CHECK (cuts[i].units == pass / std::exp2 (expected[i])); // exact: powers of two
                }
            }
        }
}

TEST_CASE ("Before the capture, or off a pass boundary, a block is one track cut to the end of its pass", "[loops][layout]")
{
    for (const auto bars : { 1.0, .5, .25 })
    {
        const auto pass = unitsPerBar * bars;
        const auto phraseUnits = 4.0 * pass;
        for (int engage = 0; engage < static_cast<int> (phraseUnits); ++engage)
        {
            CAPTURE (bars, engage);
            const auto blocks = layOut (loops (bars, 3), 4.0, engage);
            REQUIRE (isTrack (blocks.front()));
            CHECK_THAT (blocks.front().start + totalUnits (blocks.front().cuts), WithinAbs ((std::floor (engage / pass) + 1.0) * pass, tolerance));
            // The passes after the capture play their table rows, by position.
            for (std::size_t b = 1; b < blocks.size(); ++b)
            {
                const auto k = static_cast<int> (std::lround (blocks[b].start / pass));
                const auto expected = levelsOf (k, 3);
                REQUIRE (blocks[b].cuts.size() == expected.size());
                for (std::size_t i = 0; i < expected.size(); ++i)
                    CHECK (blocks[b].cuts[i].loopLevel == expected[i] + 1);
            }
        }
        // Never captured: every pass is the track.
        for (const auto& block : layOut (loops (bars, 3), 4.0, 0.0, false))
            CHECK (isTrack (block));
    }
}

TEST_CASE ("loopBars 0 plays the track to the bar line", "[loops][layout]")
{
    for (const auto meter : meters)
        for (int engage = 0; engage < unitsPerBar; ++engage)
        {
            CAPTURE (meter, engage);
            const auto blocks = layOut (loops (0.0, 3), meter, engage);
            REQUIRE (blocks.size() == 1);
            CHECK (isTrack (blocks.front()));
        }
}

TEST_CASE ("Every Loops cut is unpitched, centred, at unity, with gate 1 and no fade", "[loops][layout]")
{
    for (const auto meter : meters)
        for (const auto bars : { 1.0, .5, .25 })
            for (int depth = 0; depth <= 3; ++depth)
                for (const auto& block : layOut (loops (bars, depth), meter))
                    for (const auto& cut : block.cuts)
                    {
                        CHECK (cut.gate == 1.0);
                        CHECK (cut.pan == 0.f);
                        CHECK (cut.amp == 1.f);
                        CHECK (cut.cents == 0.f);
                        CHECK_FALSE (cut.fade);
                        CHECK (cut.tape == TapeMotion::cycle);
                    }
}

TEST_CASE ("No Loops cut is shorter than 1/16 beat, and in 4/4 the shortest is an eighth of a beat", "[loops][layout]")
{
    for (int eighths = 1; eighths <= 64; ++eighths)
    {
        const auto meter = eighths / 2.0;
        double shortest = 1.0e9;
        int deepest = 0;
        for (const auto bars : { 1.0, .5, .25 })
            for (const auto& block : layOut (loops (bars, 3), meter))
                for (const auto& cut : block.cuts)
                {
                    const auto beats = cut.units * meter / unitsPerBar;
                    if (cut.loopLevel > 1)
                        CHECK (beats >= 1.0 / 16.0 - tolerance);
                    shortest = std::min (shortest, beats);
                    deepest = std::max (deepest, static_cast<int> (cut.loopLevel));
                }
        CAPTURE (meter, shortest, deepest);
        // The depth stops at the last level at or above the floor: one level deeper would be under it.
        if (deepest < 4 && deepest > 1)
            CHECK (shortest / 2.0 < 1.0 / 16.0 - tolerance);
        if (meter == 4.0)
        {
            CHECK (shortest == .125);
            CHECK (deepest == 4);
        }
    }
}

TEST_CASE ("Every Loops cut is a whole number of 64ths of a beat, with Straight on or off", "[loops][layout][straight]")
{
    for (const auto meter : eighthMeters)
        for (const auto bars : { 1.0, .5, .25 })
            for (int depth = 0; depth <= 3; ++depth)
                for (int engage = 0; engage < 16; engage += 3)
                {
                    CAPTURE (meter, bars, depth, engage);
                    const auto off = layOut (loops (bars, depth, false), meter, engage);
                    const auto on = layOut (loops (bars, depth, true), meter, engage);
                    REQUIRE (off.size() == on.size());
                    for (std::size_t b = 0; b < off.size(); ++b)
                    {
                        REQUIRE (off[b].cuts.size() == on[b].cuts.size());
                        for (std::size_t i = 0; i < off[b].cuts.size(); ++i)
                        {
                            const auto& cut = on[b].cuts[i];
                            CHECK (cut.units == off[b].cuts[i].units);
                            CHECK (cut.loopLevel == off[b].cuts[i].loopLevel);
                            const auto sixtyFourths = cut.units * meter / unitsPerBar * 64.0;
                            if (cut.loopLevel > 0)
                                CHECK_THAT (sixtyFourths - std::round (sixtyFourths), WithinAbs (0.0, tolerance));
                        }
                    }
                }
}

TEST_CASE ("Only Loops sets a loop level", "[loops][layout]")
{
    STATIC_REQUIRE (sizeof (Cut) == 32);
    CutProc11 cutProc11;
    WarpCut warpCut;
    SQPusher sqPusher;
    Bounce bounce;
    Rush rush;
    CutParameters params;
    params.rollBeats = 2.0;
    params.rollCurve = .5;
    for (CutProcedure* procedure : std::initializer_list<CutProcedure*> { &cutProc11, &warpCut, &sqPusher, &bounce, &rush })
    {
        Random random (11);
        for (int phrase = 0; phrase < 200; ++phrase)
            for (const auto& block : playPhrase (*procedure, random, params, unitsPerBar))
                for (const auto& cut : block.cuts)
                    CHECK (cut.loopLevel == 0);
    }
}
