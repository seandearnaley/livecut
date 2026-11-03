// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineTestHelpers.h"
#include "engine/CutProcedures.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace livecut::engine;
using namespace livecut::engine::testing;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr int unitsPerBar = 16;

CutParameters roll (double beats, double curve, double cents = 0.0, bool straight = false)
{
    CutParameters params;
    params.rollBeats = beats;
    params.rollCurve = curve;
    params.rollCents = cents;
    params.straight = straight;
    return params;
}

struct Laid
{
    double start = 0.0; // units into the bar
    std::vector<Cut> cuts;
};

// The blocks Rush lays out for one bar, engaged `engage` units into it, as the
// cutter asks for them, checking the procedure's contract on the way.
std::vector<Laid> layOut (const CutParameters& params, double beatsPerBar, double engage = 0.0)
{
    Rush rush;
    Random random (7), untouched (7);
    BlockContext context;
    context.phraseUnits = static_cast<double> (rush.choosePhraseBars (random, params) * unitsPerBar);
    context.unitsPerBar = unitsPerBar;
    context.beatsPerBar = beatsPerBar;
    context.unitsDone = engage;
    REQUIRE (context.phraseUnits == unitsPerBar); // phrases are one bar

    std::vector<Laid> blocks;
    std::vector<Cut> cuts;
    while (context.unitsDone < context.phraseUnits - tolerance)
    {
        rush.chooseBlock (random, params, context, cuts);
        const auto units = totalUnits (cuts);
        REQUIRE (! cuts.empty());
        REQUIRE (cuts.size() <= maxCutsPerBlock);
        REQUIRE (units > tolerance);
        REQUIRE (context.unitsDone + units <= context.phraseUnits + tolerance);
        blocks.push_back ({ context.unitsDone, cuts });
        context.unitsDone += units;
        REQUIRE (blocks.size() <= 2);
    }
    // The last block lands exactly on the bar line, and Rush draws nothing.
    REQUIRE_THAT (context.unitsDone, WithinAbs (unitsPerBar, tolerance));
    CHECK (random.unit() == untouched.unit());
    return blocks;
}

double beatsOf (const Cut& cut, double beatsPerBar)
{
    return cut.units * beatsPerBar / unitsPerBar;
}

bool isTrack (const Laid& block)
{
    return block.cuts.size() == 1;
}
} // namespace

TEST_CASE ("Rush phrases are one bar, drawn from nothing", "[rush][layout]")
{
    Rush rush;
    Random random (3), untouched (3);
    CutParameters params;
    params.minPhraseBars = 2;
    params.maxPhraseBars = 4;
    CHECK (rush.choosePhraseBars (random, params) == 1);
    CHECK (random.unit() == untouched.unit());
}

TEST_CASE ("Every Rush bar keeps the contract and lands on its bar line, engaged anywhere", "[rush][layout]")
{
    const auto meter = GENERATE (4.0, 3.0, 5.0, 7.0, 2.5, 3.5, 6.5, 1.0, .5, .25);
    const auto straight = GENERATE (false, true);
    for (const auto beats : { 0.0, 1.0, 2.0, 3.0, 4.0 })
        for (const auto curve : { -1.0, -.6, -.2, 0.0, .1, .3, .7, 1.0 })
            for (int engage = 0; engage < unitsPerBar; ++engage)
            {
                CAPTURE (meter, straight, beats, curve, engage);
                const auto blocks = layOut (roll (beats, curve, 500.0 * curve, straight), meter, engage);
                for (const auto& block : blocks)
                    for (const auto& cut : block.cuts)
                    {
                        CHECK (cut.gate == 1.0);
                        CHECK (cut.pan == 0.f);
                        CHECK_FALSE (cut.fade); // the track joins the input sample for sample
                        CHECK (cut.tape == TapeMotion::cycle);
                        // A roll's cuts never become a buzz.
                        if (! isTrack (block))
                            CHECK (beatsOf (cut, meter) >= 1.0 / 32.0 - tolerance);
                    }
                // A track cut plays the input as it is.
                for (const auto& block : blocks)
                    if (isTrack (block))
                    {
                        CHECK (block.cuts.front().amp == 1.f);
                        CHECK (block.cuts.front().cents == 0.f);
                    }
                // Only the last block can be a roll.
                if (blocks.size() == 2)
                    CHECK (isTrack (blocks.front()));
            }
}

TEST_CASE ("Engaged on the bar line, the roll fills the last rollBeats of the bar in round (4T) cuts", "[rush][layout]")
{
    struct Case
    {
        double meter, beats, rolled;
    };
    for (const auto [meter, beats, rolled] : { Case { 4.0, 1.0, 1.0 }, Case { 4.0, 2.0, 2.0 }, Case { 4.0, 3.0, 3.0 },
                                               Case { 4.0, 4.0, 4.0 }, Case { 3.0, 4.0, 3.0 }, Case { 3.0, 3.0, 3.0 },
                                               Case { 7.0, 4.0, 4.0 }, Case { 2.5, 3.0, 2.5 }, Case { 2.5, 1.0, 1.0 },
                                               Case { 6.5, 2.0, 2.0 }, Case { .5, 1.0, .5 } })
        for (const auto curve : { -1.0, 0.0, .5 })
        {
            CAPTURE (meter, beats, curve);
            const auto blocks = layOut (roll (beats, curve), meter);
            const auto rollStart = (meter - rolled) * unitsPerBar / meter;
            REQUIRE (blocks.size() == (rollStart > 0.0 ? 2u : 1u));
            if (rollStart > 0.0)
            {
                REQUIRE (isTrack (blocks.front()));
                CHECK_THAT (blocks.front().cuts.front().units, WithinAbs (rollStart, tolerance));
            }
            CHECK_THAT (blocks.back().start, WithinAbs (rollStart, tolerance));
            CHECK (blocks.back().cuts.size() == static_cast<std::size_t> (std::lround (4.0 * rolled)));
        }
}

TEST_CASE ("rollBeats 0 plays the track to the bar line", "[rush][layout]")
{
    const auto meter = GENERATE (4.0, 3.0, 2.5);
    for (int engage = 0; engage < unitsPerBar; ++engage)
    {
        CAPTURE (meter, engage);
        const auto blocks = layOut (CutParameters {}, meter, engage);
        REQUIRE (blocks.size() == 1);
        REQUIRE (isTrack (blocks.front()));
        CHECK_THAT (blocks.front().cuts.front().units, WithinAbs (unitsPerBar - engage, tolerance));
    }
}

TEST_CASE ("X rushes or drags a roll by up to eight times, geometrically", "[rush][layout]")
{
    const auto beats = GENERATE (1.0, 2.0, 3.0, 4.0);
    for (const auto curve : { -1.0, -.75, -.3, 0.0, .3, .75, 1.0 })
    {
        CAPTURE (beats, curve);
        const auto blocks = layOut (roll (beats, curve), 4.0);
        const auto& cuts = blocks.back().cuts;
        REQUIRE (cuts.size() == static_cast<std::size_t> (4.0 * beats));
        const auto first = beatsOf (cuts.front(), 4.0), last = beatsOf (cuts.back(), 4.0);
        CHECK_THAT (first / last, WithinRel (std::pow (8.0, curve), 1.0e-9));
        for (std::size_t k = 1; k < cuts.size(); ++k)
        {
            const auto ratio = beatsOf (cuts[k], 4.0) / beatsOf (cuts[k - 1], 4.0);
            // Every cut is the same ratio shorter (rush) or longer (drag) than the one before.
            CHECK_THAT (ratio, WithinRel (std::pow (8.0, -curve / static_cast<double> (cuts.size() - 1)), 1.0e-9));
        }
        if (curve == 0.0)
            for (const auto& cut : cuts)
                CHECK_THAT (beatsOf (cut, 4.0), WithinAbs (.25, tolerance)); // an even roll is 16ths
    }
    // At the right edge a beat-long roll runs 8/15, 4/15, 2/15 and 1/15 of a beat.
    const auto edge = layOut (roll (1.0, 1.0), 4.0).back().cuts;
    for (std::size_t k = 0; k < 4; ++k)
        CHECK_THAT (beatsOf (edge[k], 4.0), WithinAbs ((8.0 / 15.0) / std::exp2 (static_cast<double> (k)), tolerance));
}

TEST_CASE ("The shortest roll cut is never under 1/24 of a beat", "[rush][layout]")
{
    // round (4T) cuts at a ratio of at most 8 keep the floor of 1/32 with room to
    // spare, down to the shortest bar that still rolls (two cuts, 3/8 of a beat).
    double shortest = 1.0;
    for (int eighths = 1; eighths <= 64; ++eighths)
    {
        const auto meter = eighths / 8.0;
        for (const auto beats : { 1.0, 2.0, 3.0, 4.0 })
            for (const auto curve : { -1.0, 1.0 })
                for (const auto& block : layOut (roll (beats, curve), meter))
                    if (! isTrack (block))
                        for (const auto& cut : block.cuts)
                            shortest = std::min (shortest, beatsOf (cut, meter));
    }
    CHECK (shortest >= 1.0 / 24.0);
    CHECK_THAT (shortest, WithinAbs ((3.0 / 8.0) / 9.0, 1.0e-12)); // 3/8 of a beat, 8:1 in two cuts
}

TEST_CASE ("Engaged late, the roll starts on the next whole beat before the bar line", "[rush][layout]")
{
    const auto meter = GENERATE (4.0, 3.0, 2.5, 6.5);
    const auto beats = GENERATE (1.0, 2.0, 4.0);
    const auto rolled = std::min (beats, meter);
    const auto rollStart = meter - rolled;
    for (int engage = 0; engage < unitsPerBar; ++engage)
    {
        const auto s = engage * meter / unitsPerBar;
        CAPTURE (meter, beats, engage, s);
        const auto blocks = layOut (roll (beats, .5), meter, engage);
        const auto& last = blocks.back();
        if (s <= rollStart + tolerance)
        {
            CHECK_THAT (last.start * meter / unitsPerBar, WithinAbs (rollStart, tolerance));
            continue;
        }
        // Late: the first point a whole number of beats before the bar line at or after s.
        const auto expected = meter - std::floor (meter - s + tolerance);
        if (expected >= meter - tolerance)
        {
            REQUIRE (blocks.size() == 1);
            CHECK (isTrack (last)); // the bar's last beat has begun: no roll
            continue;
        }
        REQUIRE_FALSE (isTrack (last));
        CHECK_THAT (last.start * meter / unitsPerBar, WithinAbs (expected, tolerance));
        CHECK (last.cuts.size() == static_cast<std::size_t> (std::lround (4.0 * (meter - expected))));
        CHECK (blocks.size() == (expected > s + tolerance ? 2u : 1u));
    }
}

TEST_CASE ("A roll keeps the hit, then swells back from -12 dB, its pitch moving to rollCents", "[rush][layout]")
{
    const auto straight = GENERATE (false, true);
    const auto beats = GENERATE (1.0, 2.0, 4.0);
    const auto curve = GENERATE (-1.0, -.4, 0.0, .6, 1.0);
    CAPTURE (straight, beats, curve);
    const auto cents = 500.0 * curve;
    const auto cuts = layOut (roll (beats, curve, cents, straight), 4.0).back().cuts;
    const auto n = cuts.size();
    REQUIRE (n >= 3);
    CHECK (cuts.front().amp == 1.f);
    CHECK (cuts.front().cents == 0.f);
    CHECK (cuts.back().amp == 1.f);
    CHECK_THAT (cuts.back().cents, WithinAbs (cents, 1.0e-3));
    for (std::size_t k = 1; k < n; ++k)
    {
        CAPTURE (k);
        const auto db = -12.0 * (1.0 - static_cast<double> (k - 1) / static_cast<double> (n - 2));
        CHECK_THAT (cuts[k].amp, WithinRel (std::pow (10.0, db / 20.0), 1.0e-6));
        CHECK_THAT (cuts[k].cents, WithinAbs (cents * static_cast<double> (k) / static_cast<double> (n - 1), 1.0e-3));
    }
}

TEST_CASE ("A single repeat plays at full level", "[rush][layout]")
{
    // Half a beat in 1/8: two cuts, the hit and one repeat.
    const auto cuts = layOut (roll (1.0, .5, 300.0), .5).back().cuts;
    REQUIRE (cuts.size() == 2);
    CHECK (cuts[0].amp == 1.f);
    CHECK (cuts[1].amp == 1.f);
    CHECK_THAT (cuts[1].cents, WithinAbs (300.0, 1.0e-3));
}

TEST_CASE ("Straight Rush steps a roll in halvings, rushing or dragging", "[rush][layout][straight]")
{
    const auto lengths = [] (double beats, double curve)
    {
        std::vector<double> found;
        const auto blocks = layOut (roll (beats, curve, 0.0, true), 4.0);
        for (const auto& cut : blocks.back().cuts)
            found.push_back (beatsOf (cut, 4.0));
        return found;
    };
    const auto near = [] (const std::vector<double>& a, const std::vector<double>& b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::abs (a[i] - b[i]) > tolerance)
                return false;
        return true;
    };
    CHECK (near (lengths (1.0, 0.0), { .25, .25, .25, .25 }));
    CHECK (near (lengths (1.0, .1), { .25, .25, .25, .25 }));               // m = round (.3) = 0
    CHECK (near (lengths (1.0, .34), { .5, .25, .25 }));                    // m = 1
    CHECK (near (lengths (1.0, .67), { .5, .25, .125, .125 }));             // m = 2
    CHECK (near (lengths (1.0, 1.0), { .5, .25, .125, .0625, .0625 }));     // m = 3
    CHECK (near (lengths (1.0, -1.0), { .0625, .0625, .125, .25, .5 }));    // a drag is the rush reversed
    CHECK (near (lengths (2.0, 1.0), { .5, .5, .25, .25, .125, .125, .0625, .0625, .0625, .0625 }));
    auto bar = std::vector<double> (4, .5);
    bar.insert (bar.end(), 4, .25);
    bar.insert (bar.end(), 4, .125);
    bar.insert (bar.end(), 8, .0625);
    CHECK (near (lengths (4.0, 1.0), bar));
}
