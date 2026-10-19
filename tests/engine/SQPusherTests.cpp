// SPDX-License-Identifier: GPL-3.0-or-later

#include "EngineTestHelpers.h"
#include "engine/CutProcedures.h"
#include "engine/CutShaping.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <set>

using namespace livecut::engine;
using namespace livecut::engine::testing;
using Catch::Matchers::WithinAbs;

namespace
{
CutParameters phraseOfBars (int bars)
{
    CutParameters params;
    params.minPhraseBars = bars;
    params.maxPhraseBars = bars;
    return params;
}

std::vector<double> cutBeats (const std::vector<Cut>& cuts, double unitsPerBeat)
{
    std::vector<double> beats;

    for (const auto& cut : cuts)
        beats.push_back (cut.units / unitsPerBeat);

    return beats;
}

bool sameBeats (const std::vector<double>& a, const std::vector<double>& b)
{
    if (a.size() != b.size())
        return false;

    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::abs (a[i] - b[i]) > 1.0e-6)
            return false;

    return true;
}
} // namespace

TEST_CASE ("SQPusher keeps a block's first cut at its original pitch", "[sqpusher]")
{
    std::array<Cut, 4> cuts;
    CutParameters params;
    params.minCents = -1200.0f;
    params.maxCents = -100.0f;
    Random a (11);
    scatter (cuts, a, params);

    Random b (11);
    std::array<double, 4> expected;

    for (auto& cents : expected)
    {
        b.uniform (params.minPan, params.maxPan);
        b.uniform (params.minAmp, params.maxAmp);
        cents = b.uniform (params.minCents, params.maxCents);
    }

    CHECK_THAT (cuts[0].cents, WithinAbs (0.0, 0.0));

    for (std::size_t i = 1; i < cuts.size(); ++i)
        CHECK_THAT (cuts[i].cents, WithinAbs (expected[i], 1.0e-4));

    CHECK_THAT (a.unit(), WithinAbs (b.unit(), 1.0e-12));
}

// Livecut's fill table had two typos: a 2.33-beat cut in the wrong block, and
// three cuts of one block written into another, which left empty blocks.
TEST_CASE ("Every SQPusher fill lasts exactly one bar of 4/4", "[sqpusher]")
{
    const auto& fills = SQPusher::fills();
    REQUIRE (fills.size() == 13);

    for (const auto& fill : fills)
    {
        double beats = 0.0;

        for (const auto& block : fill)
        {
            REQUIRE (! block.empty());
            beats = std::accumulate (block.begin(), block.end(), beats);
        }

        CHECK_THAT (beats, WithinAbs (4.0, 1.0e-9));
    }
}

// Livecut never set the length of fill blocks, so each one reused the length of the block before it.
TEST_CASE ("SQPusher plays one complete fill across the last bar of a phrase", "[sqpusher]")
{
    SQPusher procedure;
    const auto params = phraseOfBars (2);
    constexpr int unitsPerBar = 16;
    constexpr double unitsPerBeat = 4.0;

    for (std::uint32_t seed = 1; seed <= 40; ++seed)
    {
        Random random (seed);
        std::vector<std::vector<double>> lastBar;

        for (const auto& block : playPhrase (procedure, random, params, unitsPerBar))
            if (block.startUnits >= unitsPerBar - tolerance)
                lastBar.push_back (cutBeats (block.cuts, unitsPerBeat));

        bool matchesAFill = false;

        for (const auto& fill : SQPusher::fills())
        {
            if (fill.size() != lastBar.size())
                continue;

            bool same = true;

            for (std::size_t b = 0; b < fill.size(); ++b)
                same = same && sameBeats (fill[b], lastBar[b]);

            matchesAFill = matchesAFill || same;
        }

        REQUIRE (matchesAFill);
    }
}

// Livecut inverted the block length (8 / subdiv instead of subdiv / 8 quavers) and
// misread BBCut's `2.rand + 1` as one or three quavers instead of one or two.
TEST_CASE ("SQPusher blocks are one or two quavers, or twice as many semiquavers", "[sqpusher]")
{
    const auto unitsPerBar = GENERATE (6, 8, 12, 16, 24, 32);
    const auto unitsPerBeat = unitsPerBar / 4.0;
    SQPusher procedure;
    auto params = phraseOfBars (4);
    params.activity = 0.7;
    std::set<std::size_t> quaverBlocks;
    std::set<std::size_t> semiquaverBlocks;

    for (std::uint32_t seed = 1; seed <= 40; ++seed)
    {
        Random random (seed);

        for (const auto& block : playPhrase (procedure, random, params, unitsPerBar))
        {
            if (block.startUnits >= 3 * unitsPerBar - tolerance)
                break; // the last bar is a fill

            const auto beats = cutBeats (block.cuts, unitsPerBeat);
            const auto isQuavers = std::abs (beats[0] - 0.5) < 1.0e-6;
            (isQuavers ? quaverBlocks : semiquaverBlocks).insert (beats.size());

            for (const auto beat : beats)
                REQUIRE_THAT (beat, WithinAbs (isQuavers ? 0.5 : 0.25, 1.0e-6));
        }
    }

    CHECK (quaverBlocks == std::set<std::size_t> { 1, 2 });
    CHECK (semiquaverBlocks == std::set<std::size_t> { 2, 4 });
}

TEST_CASE ("SQPusher blocks on the off-beat quaver last a single quaver", "[sqpusher]")
{
    SQPusher procedure;
    auto params = phraseOfBars (4);
    params.activity = 1.0;
    Random random (53);
    std::vector<Cut> cuts;

    BlockContext context;
    context.phraseUnits = 64.0;
    context.unitsPerBar = 16;
    context.unitsDone = 2.0; // half way through the first beat

    procedure.choosePhraseBars (random, params);

    for (int i = 0; i < 500; ++i)
    {
        procedure.chooseBlock (random, params, context, cuts);
        REQUIRE_THAT (totalUnits (cuts), WithinAbs (2.0, 1.0e-6));
    }
}

TEST_CASE ("SQPusher with no activity plays only quavers", "[sqpusher]")
{
    SQPusher procedure;
    auto params = phraseOfBars (4);
    params.activity = 0.0;
    Random random (59);

    for (const auto& block : playPhrase (procedure, random, params, 16))
    {
        if (block.startUnits >= 48.0 - tolerance)
            break;

        for (const auto& cut : block.cuts)
            REQUIRE_THAT (cut.units, WithinAbs (2.0, 1.0e-6));
    }
}

TEST_CASE ("SQPusher blocks never cross a bar line", "[sqpusher]")
{
    const auto meter = GENERATE (std::pair { 12, 3.0 }, std::pair { 16, 5.0 }, std::pair { 12, 6.0 });
    const auto unitsPerBar = meter.first;
    SQPusher procedure;
    const auto params = phraseOfBars (3);

    for (std::uint32_t seed = 1; seed <= 40; ++seed)
    {
        Random random (seed);

        for (const auto& block : playPhrase (procedure, random, params, unitsPerBar, meter.second))
        {
            const auto end = block.startUnits + totalUnits (block.cuts);
            const auto bar = std::floor ((block.startUnits + tolerance) / unitsPerBar);
            REQUIRE (end <= (bar + 1.0) * unitsPerBar + 1.0e-6);
        }
    }
}

TEST_CASE ("Resetting SQPusher abandons a fill in progress", "[sqpusher]")
{
    const auto params = phraseOfBars (2);
    SQPusher used;
    SQPusher fresh;
    std::vector<Cut> cuts;

    BlockContext lastBar;
    lastBar.phraseUnits = 32.0;
    lastBar.unitsPerBar = 16;
    lastBar.unitsDone = 16.0;

    Random warmup (5);
    used.choosePhraseBars (warmup, params);
    used.chooseBlock (warmup, params, lastBar, cuts); // starts a fill
    used.reset();

    auto midBar = lastBar;
    midBar.unitsDone = 20.0;
    Random a (9);
    Random b (9);
    std::vector<Cut> fromUsed;
    std::vector<Cut> fromFresh;
    used.chooseBlock (a, params, midBar, fromUsed);
    fresh.chooseBlock (b, params, midBar, fromFresh);

    REQUIRE (fromUsed.size() == fromFresh.size());

    for (std::size_t i = 0; i < fromUsed.size(); ++i)
    {
        CHECK_THAT (fromUsed[i].units, WithinAbs (fromFresh[i].units, 1.0e-12));
        CHECK_THAT (fromUsed[i].pan, WithinAbs (fromFresh[i].pan, 0.0));
    }
}

// Livecut only panned the semiquaver cuts; its quaver cuts stayed in the centre.
TEST_CASE ("SQPusher pans every cut within the pan range", "[sqpusher]")
{
    SQPusher procedure;
    auto params = phraseOfBars (4);
    params.minPan = 0.5f;
    params.maxPan = 0.9f;
    Random random (61);

    for (const auto& block : playPhrase (procedure, random, params, 16))
    {
        for (const auto& cut : block.cuts)
        {
            REQUIRE (cut.pan >= 0.5f);
            REQUIRE (cut.pan <= 0.9f);
        }
    }
}
