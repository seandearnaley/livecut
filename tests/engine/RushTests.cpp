// SPDX-License-Identifier: GPL-3.0-or-later

#include "DrumLoop.h"
#include "EngineTestHelpers.h"
#include "engine/CutProcedures.h"
#include "engine/Cutter.h"
#include "engine/PadPrograms.h"

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

namespace
{
StereoFrame tone (std::int64_t frame)
{
    const auto t = static_cast<double> (frame);
    return { static_cast<float> (.6 * std::sin (.031 * t) + .2 * std::sin (.0047 * t)),
             static_cast<float> (.5 * std::cos (.023 * t)) };
}

// The pad's Rush through a Cutter, at 120 bpm in 4/4 unless told otherwise, fed
// a frame at a time. The host's position is counted from the last tempo change.
struct RushCutter
{
    Cutter cutter;
    double rate;
    std::int64_t frame = 0;
    double bpm, beatsPerBar;
    std::int64_t tempoFrame = 0;
    double tempoBeats = 0.0;

    explicit RushCutter (const CutParameters& params, double sampleRate = 48000.0, double tempo = 120.0, double meter = 4.0)
        : rate (sampleRate), bpm (tempo), beatsPerBar (meter)
    {
        cutter.prepare (rate);
        cutter.setProcedure (Procedure::rush);
        cutter.setUnitsPerBar (unitsPerBar);
        cutter.setSeed (808);
        cutter.setParameters (params);
    }

    double beats() const
    {
        return tempoBeats + static_cast<double> (frame - tempoFrame) * bpm / 60.0 / rate;
    }

    void setTempo (double tempo)
    {
        tempoBeats = beats();
        tempoFrame = frame;
        bpm = tempo;
    }

    void process (float* left, float* right, int count)
    {
        const auto at = beats();
        const auto bar = static_cast<std::int64_t> (std::floor (at / beatsPerBar));
        cutter.process (left, right, count, { bpm, beatsPerBar }, { bar, at - beatsPerBar * static_cast<double> (bar) });
        frame += count;
    }
};

// What a run of Rush did, frame by frame and roll by roll.
struct Played
{
    std::int64_t compared = 0;  // frames that played the input (a block's first cut, or a run-on)
    std::int64_t repeats = 0;   // frames of a roll's repeats
    std::int64_t dropouts = 0;  // frames of exact silence where the input sounds
    int rolls = 0;              // rolls that reached a bar line
    int unfinished = 0;         // rolls that didn't play every cut before it
    int mistimed = 0;           // bar lines not within half a sample of the host's
    int worstRunOn = 0;         // frames of input after a roll's last cut
    int worstTruncated = 0;     // frames of a roll's last cut the bar line cut off
};

// Plays `bars` bars of `tone` through Rush from the first sample, checking every
// frame of the input bitwise. `ramp` adds bpm per beat at each `buffer` frames.
Played playRush (const CutParameters& params, double rate, double bpm, double meter, double bars, double ramp = 0.0,
                 int buffer = 512)
{
    RushCutter rush (params, rate, bpm, meter);
    Played played;
    std::int64_t phrase = -1;
    double blockStart = -1.0;
    std::size_t index = 0, count = 0, highest = 0;
    int remaining = 0, runOn = 0;
    while (rush.beats() < bars * meter)
    {
        if (ramp != 0.0 && rush.frame % buffer == 0)
            rush.setTempo (bpm + ramp * rush.beats());
        const auto at = rush.beats();
        const auto halfSample = .5 * rush.bpm / 60.0 / rate;
        const auto exhausted = index >= count && count > 0;
        const auto in = tone (rush.frame);
        auto left = in.left, right = in.right;
        rush.process (&left, &right, 1);

        const auto& cutter = rush.cutter;
        REQUIRE (cutter.error() == Cutter::Error::none);
        REQUIRE (cutter.isCutting());
        if (cutter.phraseStartBar() != phrase)
        {
            // A new bar: the roll before it landed if it played every cut, and it
            // starts within half a sample of the host's bar line.
            if (phrase >= 0 && count >= 2)
            {
                ++played.rolls;
                played.unfinished += highest + 1 < count ? 1 : 0;
                played.worstRunOn = std::max (played.worstRunOn, runOn);
                played.worstTruncated = std::max (played.worstTruncated, index + 1 == count ? remaining : 0);
            }
            const auto intoBar = at - meter * std::round (at / meter);
            played.mistimed += phrase >= 0 && std::abs (intoBar) > halfSample + 1.0e-9 ? 1 : 0;
            phrase = cutter.phraseStartBar();
            blockStart = -1.0;
        }
        const auto sameBlock = cutter.blockStartUnits() == blockStart;
        const auto& player = cutter.player();
        const auto runningOn = sameBlock && exhausted;
        if (! sameBlock)
        {
            blockStart = cutter.blockStartUnits();
            highest = 0;
            runOn = 0;
        }
        runOn += runningOn ? 1 : 0;
        count = player.cuts().size();
        index = player.currentCut();
        const auto age = player.currentCutAge();
        // At a cut's last frame the player has already moved on to the next cut, at age 0.
        const auto rendered = age == 0 && index > 0 ? index - 1 : index;
        remaining = index < count ? player.cuts()[index].size - age : 0;
        if (! runningOn)
            highest = std::max (highest, rendered);
        if (runningOn || rendered == 0)
        {
            ++played.compared;
            REQUIRE (left == in.left);
            REQUIRE (right == in.right);
        }
        else
            ++played.repeats;
        played.dropouts += left == 0.f && right == 0.f && std::abs (in.left) > .05f ? 1 : 0;
    }
    return played;
}

// At a steady tempo a roll's last cut ends within a frame of the bar line, either
// side. Through a tempo ramp each re-time rounds its cut's start, so up to two
// frames of input can come before the bar line, or four frames of the last cut
// (under 0.1 ms) fall after it (measured 2 and 4).
void checkLanded (const Played& played, int bars, int runOn = 1, int truncated = 1)
{
    CAPTURE (played.compared, played.repeats, played.rolls, played.worstRunOn, played.worstTruncated);
    CHECK (played.dropouts == 0);
    CHECK (played.rolls >= bars - 1);
    CHECK (played.unfinished == 0);
    CHECK (played.mistimed == 0);
    CHECK (played.worstRunOn <= runOn);
    CHECK (played.worstTruncated <= truncated);
    CHECK (played.repeats > 0);
    CHECK (played.compared > 0);
}

// Every whole-number tempo from 80 to 180 bpm, with the roll's length and curve
// moving with the tempo, so each spot meets bars of many sample lengths.
void checkEveryTempo (double rate)
{
    for (int bpm = 80; bpm <= 180; ++bpm)
    {
        const auto beats = 1.0 + bpm % 4;
        const auto curve = (bpm % 5 - 2) / 2.0;
        CAPTURE (rate, bpm, beats, curve);
        checkLanded (playRush (roll (beats, curve, 500.0 * curve, bpm % 3 == 0), rate, bpm, 4.0, 3.0), 3);
    }
}
} // namespace

TEST_CASE ("At any tempo, Rush is the input outside its rolls' repeats, and its rolls land, at 44.1 kHz", "[rush][cutter][tempo]")
{
    checkEveryTempo (44100.0);
}

TEST_CASE ("At any tempo, Rush is the input outside its rolls' repeats, and its rolls land, at 48 kHz", "[rush][cutter][tempo]")
{
    checkEveryTempo (48000.0);
}

TEST_CASE ("At any tempo, Rush is the input outside its rolls' repeats, and its rolls land, at 96 kHz", "[rush][cutter][tempo]")
{
    checkEveryTempo (96000.0);
}

namespace
{
void checkMeter (double meter)
{
    for (const auto bpm : { 93.0, 120.0, 140.0 })
        for (const auto beats : { 1.0, 4.0 })
            for (const auto curve : { -1.0, 0.0, 1.0 })
            {
                CAPTURE (meter, bpm, beats, curve);
                checkLanded (playRush (roll (beats, curve, 500.0 * curve), 48000.0, bpm, meter, 4.0), 4);
            }
}
} // namespace

TEST_CASE ("In 3/4, Rush's rolls land on the bar line", "[rush][cutter][meter]")
{
    checkMeter (3.0);
}

TEST_CASE ("In 5/4, Rush's rolls land on the bar line", "[rush][cutter][meter]")
{
    checkMeter (5.0);
}

TEST_CASE ("In 7/4, Rush's rolls land on the bar line", "[rush][cutter][meter]")
{
    checkMeter (7.0);
}

TEST_CASE ("In 5/8, Rush's rolls land on the bar line", "[rush][cutter][meter]")
{
    checkMeter (2.5);
}

TEST_CASE ("In 13/8, Rush's rolls land on the bar line", "[rush][cutter][meter]")
{
    checkMeter (6.5);
}

TEST_CASE ("Through tempo automation, Rush's rolls land on the bar line", "[rush][cutter][tempo]")
{
    const auto ramp = GENERATE (-3.0, -.5, .5, 3.0);
    const auto buffer = GENERATE (64, 512);
    for (const auto beats : { 1.0, 4.0 })
        for (const auto curve : { -1.0, 1.0 })
        {
            CAPTURE (ramp, buffer, beats, curve);
            checkLanded (playRush (roll (beats, curve, 500.0 * curve), 48000.0, 120.0, 4.0, 6.0, ramp, buffer), 6, 2, 4);
        }
}

namespace
{
using testing::DrumLoop;

// Rush at a pad spot: X drags or rushes and bends the pitch, Y picks the roll's length.
CutParameters padSpot (double x, double y)
{
    constexpr double lengths[] { 1.0, 2.0, 3.0, 4.0 };
    return roll (lengths[std::min (static_cast<int> (4.0 * y), 3)], 2.0 * x - 1.0, 500.0 * (2.0 * x - 1.0));
}

struct Level
{
    double rms = 0.0;
    float peak = 0.f;
};

Level level (const std::vector<float>& left, const std::vector<float>& right)
{
    Level found;
    double energy = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i)
    {
        energy += static_cast<double> (left[i]) * left[i] + static_cast<double> (right[i]) * right[i];
        found.peak = std::max ({ found.peak, std::abs (left[i]), std::abs (right[i]) });
    }
    found.rms = std::sqrt (energy / static_cast<double> (2 * left.size()));
    return found;
}

// Eight bars of the loop through a Cutter engaged from the first sample, in
// 512-frame buffers. `check` sees the cutter after each buffer.
template <typename Check>
Level runLoop (const DrumLoop& loop, Procedure procedure, int units, std::uint32_t seed, const CutParameters& params, Check&& check)
{
    constexpr int frames = 8 * 96000;
    Cutter cutter;
    cutter.prepare (48000.0);
    cutter.setProcedure (procedure);
    cutter.setUnitsPerBar (units);
    cutter.setSeed (seed);
    cutter.setParameters (params);
    std::vector<float> left, right;
    std::array<float, 512> l {}, r {};
    for (int n = 0; n < frames; n += 512)
    {
        for (int i = 0; i < 512; ++i)
            l[static_cast<std::size_t> (i)] = r[static_cast<std::size_t> (i)]
                = loop.samples[static_cast<std::size_t> (n + i) % loop.samples.size()];
        const auto beats = n / 24000.0;
        const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
        cutter.process (l.data(), r.data(), 512, { 120.0, 4.0 }, { bar, beats - 4.0 * static_cast<double> (bar) });
        left.insert (left.end(), l.begin(), l.end());
        right.insert (right.end(), r.begin(), r.end());
        REQUIRE (cutter.error() == Cutter::Error::none);
        check (cutter);
    }
    return level (left, right);
}

double decibels (double a, double b)
{
    return 20.0 * std::log10 (a / b);
}
} // namespace

TEST_CASE ("On drums and bass, Rush stays near the track's level and never peaks above it", "[rush][drums][level]")
{
    // Measured over eight bars, against the input:
    // - drums alone: -3.3 to +2.4 dB, Cut's busiest spot +1.6 dB;
    // - with a 55 Hz sub: -3.3 to +0.05 dB; with a 150 Hz bass line: -2.8 to -0.1 dB.
    // A roll repeats the start of a hit, its loudest part; the swell from -12 dB
    // holds a whole bar of a rolled kick to the drums' +2.4 dB, from +5.8 dB at unity.
    const auto [bassHz, name] = GENERATE (table<double, const char*> ({
        { 0.0, "drums" },
        { 55.0, "sub-bass" },
        { 150.0, "a bass line" },
    }));
    CAPTURE (name);
    const DrumLoop loop (bassHz, DrumLoop::Bass::sine);
    const auto input = level (loop.samples, loop.samples);
    // Cut at its busiest spot on the same loop.
    double busiest = 0.0;
    for (const auto x : { 0.f, .5f, 1.f })
        for (const auto y : { 0.f, .5f, 1.f })
        {
            const auto mapping = mapPad (PadProgram::cut, x, y, {});
            busiest = std::max (busiest, runLoop (loop, mapping.procedure, mapping.unitsPerBar, mapping.seed, mapping.cut, [] (const Cutter&) {}).rms);
        }
    for (const auto x : { 0.0, .25, .5, .75, 1.0 })
        for (const auto y : { .1, .35, .6, .85 }) // rolls of one, two, three and four beats
        {
            CAPTURE (x, y);
            int pitched = 0;
            const auto rush = runLoop (loop, Procedure::rush, unitsPerBar, 808, padSpot (x, y), [&pitched] (const Cutter& cutter)
            {
                // Every repeat, pitched up or down, plays to its end before it reads unrecorded audio.
                for (const auto& cut : cutter.player().cuts())
                {
                    REQUIRE (cut.playable == cut.soundingFrames);
                    pitched += cut.pitched ? 1 : 0;
                }
            });
            if (x != .5)
                CHECK (pitched > 0);
            const auto overInput = decibels (rush.rms, input.rms);
            const auto overCut = decibels (rush.rms, busiest);
            CAPTURE (overInput, overCut, rush.peak, input.peak);
            CHECK (rush.peak <= input.peak);
            CHECK (overInput >= -3.5);
            if (bassHz > 0.0)
                CHECK (overInput <= .25);
            else
            {
                CHECK (overInput <= 2.5);
                CHECK (overCut <= 1.0);
            }
        }
}
