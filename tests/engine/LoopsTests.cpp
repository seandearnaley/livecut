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

namespace
{
StereoFrame tone (std::int64_t frame)
{
    const auto t = static_cast<double> (frame);
    return { static_cast<float> (.6 * std::sin (.031 * t) + .2 * std::sin (.0047 * t)),
             static_cast<float> (.5 * std::cos (.023 * t)) };
}

struct Epoch final : Cutter::SourceContext
{
    std::uint64_t epoch = 1;
    std::int64_t first = 0;
    std::optional<Cutter::TransportBounds> sourceBoundsAt (std::int64_t frame) const override
    {
        if (frame < 0)
            return std::nullopt;
        if (frame < first)
            return Cutter::TransportBounds { epoch - 1, 0, first };
        return Cutter::TransportBounds { epoch, first, std::nullopt };
    }
};

// A block as it activated: its first frame, its plan and the loop's state then.
struct Activated
{
    std::int64_t frame = 0;
    std::int64_t phraseBar = 0;
    double start = 0.0;
    SourceRegion source {};
    std::vector<Cut> cuts;
    Cutter::LoopDiagnostics loop;
    bool replay() const { return ! cuts.empty() && cuts.front().loopLevel > 0; }
};

// Loops through a Cutter, engaged from `engage` (a frame), with a host whose
// position is the integral of its tempo. Records every frame and every block.
struct LoopsHost final : Cutter::Observer
{
    Cutter cutter;
    OnsetDetector detector;
    Epoch bounds;
    double rate, bpm, meter;
    bool analysed = false;
    std::int64_t frame = 0, bufferStart = 0;
    // The host's position: counted from the last tempo change, as a host does, so
    // every buffer size sees the same position at the same frame.
    std::int64_t tempoFrame = 0;
    double tempoBeats = 0.0;
    double beatsAt (std::int64_t at) const { return tempoBeats + static_cast<double> (at - tempoFrame) * bpm / 60.0 / rate; }
    StereoFrame (*source) (std::int64_t) = tone;
    std::vector<float> inLeft, inRight, outLeft, outRight;
    std::vector<double> beatAt; // the host's position at each frame
    std::vector<Activated> blocks;
    std::int64_t silent = 0;

    LoopsHost (const CutParameters& params, double sampleRate = 48000.0, double tempo = 120.0, double beatsPerBar = 4.0, bool onsets = false)
        : rate (sampleRate), bpm (tempo), meter (beatsPerBar), analysed (onsets)
    {
        cutter.prepare (rate);
        if (analysed)
        {
            REQUIRE (detector.prepare (rate, 8192, 3.0));
            bounds.epoch = detector.epoch();
        }
        // A later epoch is begun on its first frame, in the detector as in the bounds.
        cutter.setTransformSource (detector, bounds);
        cutter.setProcedure (Procedure::loops);
        cutter.setUnitsPerBar (unitsPerBar);
        cutter.setSeed (707);
        cutter.setParameters (params);
    }

    void discontinuity (int) override {}
    void phraseCompleted (int) override {}
    void phraseStarted (int, bool) override {}
    void processOutput (int offset, float, float, float& left, float& right) override
    {
        if (! cutter.isCutting())
            return;
        const auto at = bufferStart + offset;
        const auto source = cutter.activeSourceRegion();
        if (blocks.empty() || blocks.back().start != cutter.blockStartUnits() || blocks.back().phraseBar != cutter.phraseStartBar()
            || blocks.back().source.firstFrame != source.firstFrame)
        {
            const auto cuts = cutter.currentCuts();
            blocks.push_back ({ at, cutter.phraseStartBar(), cutter.blockStartUnits(), source, { cuts.begin(), cuts.end() }, cutter.loopDiagnostics() });
        }
        (void) left;
        (void) right;
    }

    // `count` frames in buffers from `size`, with `ramp` bpm per beat added at each buffer.
    void run (std::int64_t count, int (*size) (std::int64_t) = [] (std::int64_t) { return 512; }, double ramp = 0.0)
    {
        std::vector<float> left, right;
        for (const auto end = frame + count; frame < end;)
        {
            const auto n = static_cast<int> (std::min<std::int64_t> (size (frame), end - frame));
            if (ramp != 0.0)
            {
                tempoBeats = beatsAt (frame);
                tempoFrame = frame;
                bpm += ramp * static_cast<double> (n) * bpm / 60.0 / rate;
            }
            const auto beats = beatsAt (frame);
            left.resize (static_cast<std::size_t> (n));
            right.resize (static_cast<std::size_t> (n));
            for (int i = 0; i < n; ++i)
            {
                const auto in = source (frame + i);
                left[static_cast<std::size_t> (i)] = in.left;
                right[static_cast<std::size_t> (i)] = in.right;
                inLeft.push_back (in.left);
                inRight.push_back (in.right);
                beatAt.push_back (beatsAt (frame + i));
                if (analysed && frame + i == bounds.first && bounds.first > 0)
                    REQUIRE (detector.beginTransportEpoch (bounds.epoch));
                if (analysed)
                    detector.process (frame + i, in.left, in.right);
            }
            const auto bar = static_cast<std::int64_t> (std::floor (beats / meter));
            bufferStart = frame;
            cutter.process (left.data(), right.data(), n, { bpm, meter }, { bar, beats - meter * static_cast<double> (bar) }, this);
            REQUIRE (cutter.error() == Cutter::Error::none);
            REQUIRE_FALSE (cutter.player().hasSourceError());
            for (int i = 0; i < n; ++i)
            {
                const auto l = left[static_cast<std::size_t> (i)], r = right[static_cast<std::size_t> (i)];
                silent += l == 0.f && r == 0.f && (std::abs (inLeft[static_cast<std::size_t> (frame + i)]) > .05f) ? 1 : 0;
                outLeft.push_back (l);
                outRight.push_back (r);
            }
            frame += n;
        }
    }
};

struct Checked
{
    std::int64_t track = 0, replay = 0, skipped = 0, wrong = 0;
    int replays = 0, worstLanding = 0;
    double worstLandingSamples = 0.0;
};

// Checks every frame of a constant-tempo run against what the blocks say: a track
// block plays the input; a replay cut at age `a` plays the loop's read frame plus
// `a`, at its level's gain. The 2 ms after each join into or inside a replay are
// left to the join tests. Each block starts within half a sample of its grid point.
Checked checkPlayed (const LoopsHost& host, double samplesPerUnit)
{
    Checked checked;
    const auto join = static_cast<std::int64_t> (std::lround (.002 * host.rate));
    const auto frames = static_cast<std::int64_t> (host.outLeft.size());
    const auto beatsPerUnit = host.meter / unitsPerBar;
    for (std::size_t b = 0; b < host.blocks.size(); ++b)
    {
        const auto& block = host.blocks[b];
        const auto end = b + 1 < host.blocks.size() ? host.blocks[b + 1].frame : frames;
        const auto afterReplay = b > 0 && host.blocks[b - 1].replay();
        // The grid point: its beat over the beats per sample, from the host's own position.
        const auto gridBeat = static_cast<double> (block.phraseBar) * host.meter + block.start * beatsPerUnit;
        const auto at = host.beatAt[static_cast<std::size_t> (block.frame)];
        const auto off = (at - gridBeat) / (host.bpm / 60.0 / host.rate);
        if (b > 0)
            checked.worstLandingSamples = std::max (checked.worstLandingSamples, std::abs (off));
        checked.replays += block.replay() ? 1 : 0;
        // The cuts' sample layout, rounded cumulatively as the player does.
        std::vector<std::int64_t> starts { 0 };
        auto units = 0.0;
        for (const auto& cut : block.cuts)
        {
            units += cut.units;
            starts.push_back (std::lround (units * samplesPerUnit));
        }
        for (auto f = block.frame; f < end; ++f)
        {
            const auto i = static_cast<std::size_t> (f);
            const auto into = f - block.frame;
            if (! block.replay())
            {
                if (afterReplay && into < join)
                {
                    ++checked.skipped;
                    continue;
                }
                ++checked.track;
                checked.wrong += host.outLeft[i] != host.inLeft[i] || host.outRight[i] != host.inRight[i] ? 1 : 0;
                continue;
            }
            auto k = std::size_t { 0 };
            while (k + 1 < block.cuts.size() && into >= starts[k + 1])
                ++k;
            const auto age = into - starts[k];
            if (age < join || into >= starts.back())
            {
                ++checked.skipped;
                continue;
            }
            const auto gain = block.loop.gains[static_cast<std::size_t> (block.cuts[k].loopLevel - 1)];
            const auto read = static_cast<std::size_t> (block.source.firstFrame + age);
            ++checked.replay;
            if (gain == 1.f)
                checked.wrong += host.outLeft[i] != host.inLeft[read] || host.outRight[i] != host.inRight[read] ? 1 : 0;
            else
                checked.wrong += std::abs (host.outLeft[i] - gain * host.inLeft[read]) > 1.0e-6f
                              || std::abs (host.outRight[i] - gain * host.inRight[read]) > 1.0e-6f ? 1 : 0;
        }
    }
    // Every frame the blocks account for plays what they say.
    CHECK (checked.wrong == 0);
    return checked;
}

double samplesPerUnitOf (const LoopsHost& host)
{
    return host.rate * 60.0 / host.bpm * host.meter / unitsPerBar;
}

// Every whole-number tempo from 80 to 180 bpm at `rate`, over 8 bars, each loop
// length in turn at the top of Y, and the bar loop at the bottom.
void checkEveryTempo (double rate)
{
    for (int bpm = 80; bpm <= 180; ++bpm)
    {
        constexpr double lengths[] { 1.0, .5, .25 };
        // Each tempo plays one loop length, at the top of Y or one step down.
        for (const auto [bars, depth] : { std::pair { lengths[bpm % 3], bpm % 2 == 0 ? 3 : 1 } })
        {
            CAPTURE (rate, bpm, bars, depth);
            LoopsHost host (loops (bars, depth), rate, bpm);
            host.run (static_cast<std::int64_t> (8 * 4 * 60.0 / bpm * rate));
            const auto checked = checkPlayed (host, samplesPerUnitOf (host));
            CHECK (checked.replays >= 6);
            CHECK (checked.replay > checked.track / 2);
            CHECK (checked.worstLandingSamples <= .5 + 1.0e-6);
            CHECK (host.silent == 0);
        }
    }
}
} // namespace

TEST_CASE ("At any tempo, Loops' track pass is the input and its repeats are the capture, at 44.1 kHz", "[loops][cutter][tempo]")
{
    checkEveryTempo (44100.0);
}

TEST_CASE ("At any tempo, Loops' track pass is the input and its repeats are the capture, at 48 kHz", "[loops][cutter][tempo]")
{
    checkEveryTempo (48000.0);
}

TEST_CASE ("At any tempo, Loops' track pass is the input and its repeats are the capture, at 96 kHz", "[loops][cutter][tempo]")
{
    checkEveryTempo (96000.0);
}

namespace
{
// A bar line or a pass boundary within `bars` bars falling exactly half a sample between two frames.
bool halfSampleTie (double rate, int bpm, double meter, int bars)
{
    const auto pass = meter / 4.0 * 60.0 / bpm * rate; // a quarter bar, in samples
    for (int k = 1; k <= 4 * bars; ++k)
    {
        const auto at = k * pass;
        if (std::abs (at - std::floor (at) - .5) < 1.0e-9)
            return true;
    }
    return false;
}

// The first frame two outputs differ at, or -1.
std::int64_t firstDifference (const std::vector<float>& a, const std::vector<float>& b)
{
    for (std::size_t i = 0; i < std::min (a.size(), b.size()); ++i)
        if (a[i] != b[i])
            return static_cast<std::int64_t> (i);
    return a.size() == b.size() ? -1 : static_cast<std::int64_t> (std::min (a.size(), b.size()));
}

int randomSize (std::int64_t frame)
{
    auto h = static_cast<std::uint64_t> (frame) * 0x9e3779b97f4a7c15ULL;
    h ^= h >> 29;
    return 1 + static_cast<int> (h % 777);
}

void checkTies (double meter)
{
    int ties = 0;
    for (int bpm = 80; bpm <= 180; ++bpm)
    {
        if (! halfSampleTie (44100.0, bpm, meter, 8))
            continue;
        ++ties;
        for (const auto bars : { 1.0, .25 })
        {
            CAPTURE (meter, bpm, bars);
            const auto frames = static_cast<std::int64_t> (8 * meter * 60.0 / bpm * 44100.0);
            LoopsHost reference (loops (bars, 3), 44100.0, bpm, meter);
            reference.run (frames, [] (std::int64_t) { return 512; });
            const auto checked = checkPlayed (reference, samplesPerUnitOf (reference));
            CHECK (checked.replays >= 6);
            // One-frame buffers at the first tie tempo of each meter, the others at all of them.
            const auto single = ties == 1 && bars == 1.0;
            for (int (*size) (std::int64_t) : { +[] (std::int64_t) { return 1; }, +[] (std::int64_t) { return 64; }, randomSize })
            {
                if (size (0) == 1 && ! single)
                    continue;
                LoopsHost host (loops (bars, 3), 44100.0, bpm, meter);
                host.run (frames, size);
                CAPTURE (size (0), firstDifference (host.outLeft, reference.outLeft), firstDifference (host.outRight, reference.outRight));
                REQUIRE (host.outLeft == reference.outLeft);
                REQUIRE (host.outRight == reference.outRight);
            }
        }
    }
    CHECK (ties > 0);
}
} // namespace

TEST_CASE ("At every half-sample tie tempo, Loops' track pass is the input, for every buffer size", "[loops][cutter][tempo][tie]")
{
    // 128 bpm in 4/4, 160 in 3/4, 96 in 5/4 and 7/8, 80 in 13/8, among others.
    REQUIRE (halfSampleTie (44100.0, 128, 4.0, 8));
    const auto meter = GENERATE (4.0, 3.0, 5.0, 3.5, 2.5, 6.5);
    checkTies (meter);
}

TEST_CASE ("In 3/4, 5/4, 7/4, 5/8, 7/8 and 13/8, Loops' passes land", "[loops][cutter][meter]")
{
    const auto meter = GENERATE (3.0, 5.0, 7.0, 2.5, 3.5, 6.5);
    for (const auto rate : { 44100.0, 48000.0 })
        for (const auto bpm : { 93.0, 120.0, 140.0 })
            for (const auto bars : { 1.0, .5, .25 })
            {
                CAPTURE (meter, rate, bpm, bars);
                LoopsHost host (loops (bars, 3), rate, bpm, meter);
                host.run (static_cast<std::int64_t> (8 * meter * 60.0 / bpm * rate));
                const auto checked = checkPlayed (host, samplesPerUnitOf (host));
                CHECK (checked.replays >= 6);
                CHECK (checked.worstLandingSamples <= .5 + 1.0e-6);
                CHECK (host.silent == 0);
            }
}

TEST_CASE ("Through tempo automation, Loops' passes land within 4 frames", "[loops][cutter][tempo]")
{
    for (const auto ramp : { .5, -.5, 3.0, -3.0 })
        for (int (*size) (std::int64_t) : { +[] (std::int64_t) { return 1; }, +[] (std::int64_t) { return 64; },
                                           +[] (std::int64_t) { return 512; }, +[] (std::int64_t) { return 2048; } })
            for (const auto bars : { 1.0, .25 })
            {
                CAPTURE (ramp, size (0), bars);
                LoopsHost host (loops (bars, 3), 48000.0, 120.0);
                while (host.beatsAt (host.frame) < 6 * 4.0) // six bars, however long they take
                    host.run (4800, size, ramp);
                double worst = 0.0;
                int replays = 0;
                for (std::size_t b = 1; b < host.blocks.size(); ++b)
                {
                    const auto& block = host.blocks[b];
                    const auto grid = static_cast<double> (block.phraseBar) * host.meter + block.start * host.meter / unitsPerBar;
                    // The frame nearest the grid point, by the host's own position.
                    auto nearest = block.frame;
                    while (nearest > 0 && host.beatAt[static_cast<std::size_t> (nearest)] > grid)
                        --nearest;
                    while (host.beatAt[static_cast<std::size_t> (nearest)] < grid)
                        ++nearest;
                    worst = std::max (worst, static_cast<double> (std::llabs (block.frame - nearest)));
                    replays += block.replay() ? 1 : 0;
                }
                CHECK (replays >= 4);
                CHECK (worst <= 4.0);
                CHECK (host.silent == 0);
            }
}

TEST_CASE ("Engaged at any 16th, the capture is the pass pressed in, from its start", "[loops][cutter][late]")
{
    // At 120 bpm and 48 kHz a 16th is 6,000 frames, so every pass starts on a whole frame.
    for (const auto bars : { 1.0, .5, .25 })
    {
        // A phrase starts in the bar pressed in, so a press is somewhere in its first bar.
        const auto pass = static_cast<int> (unitsPerBar * bars);
        for (int engage = 0; engage < unitsPerBar; ++engage)
        {
            CAPTURE (bars, engage);
            LoopsHost host (loops (bars, 3));
            host.cutter.setIdle (true);
            host.run (96000 + engage * 6000 - 100);
            host.cutter.setIdle (false);
            host.run (4 * 96000);
            REQUIRE_FALSE (host.blocks.empty());
            const auto& first = host.blocks.front();
            CHECK (first.frame == 96000 + engage * 6000);
            const auto passStart = 96000 + (engage / pass) * pass * 6000;
            // The block after the track pass knows the capture: the pressed pass's first
            // frame. Pressed in the phrase's last pass, the next phrase captures its own.
            REQUIRE (host.blocks.size() >= 2);
            if (bars == .25 && engage / pass == 3)
            {
                CHECK (host.blocks[1].phraseBar != first.phraseBar);
                CHECK_FALSE (host.blocks[1].replay());
                CHECK (host.blocks[1].loop.captureFrame == host.blocks[1].frame);
            }
            else
                CHECK (host.blocks[1].loop.captureFrame == passStart);
            // The passes after it play their rows of the table.
            for (std::size_t b = 1; b < host.blocks.size() && host.blocks[b].phraseBar == first.phraseBar; ++b)
            {
                const auto k = static_cast<int> (std::lround (host.blocks[b].start / pass));
                const auto expected = levelsOf (k, 3);
                REQUIRE (host.blocks[b].cuts.size() == expected.size());
                for (std::size_t i = 0; i < expected.size(); ++i)
                    CHECK (host.blocks[b].cuts[i].loopLevel == expected[i] + 1);
            }
            checkPlayed (host, 6000.0);
        }
    }
}

TEST_CASE ("A capture that would start before the transport epoch moves to the next pass", "[loops][cutter][epoch]")
{
    // The epoch starts five 16ths into the first one-beat pass of bar 2; the cutter engages a 16th later.
    LoopsHost host (loops (.25, 0));
    host.bounds.epoch = 5;
    host.bounds.first = 96000 + 5 * 6000;
    host.cutter.setIdle (true);
    host.run (96000 + 5 * 6000 + 10);
    host.cutter.setIdle (false);
    host.run (2 * 96000);
    REQUIRE (host.blocks.size() >= 3);
    CHECK_FALSE (host.blocks[0].replay());
    CHECK_FALSE (host.blocks[1].replay()); // the beat it began in can't capture: the next one does
    CHECK (host.blocks[1].frame == 96000 + 8 * 6000);
    CHECK (host.blocks[2].replay());
    CHECK (host.blocks[2].loop.captureFrame == 96000 + 8 * 6000);
    checkPlayed (host, 6000.0);
}

TEST_CASE ("Loops plans take no accent and no transform", "[loops][cutter]")
{
    const auto play = [] (bool classic)
    {
        LoopsHost host (loops (.5, 3), 48000.0, 120.0, 4.0, true);
        if (classic)
        {
            host.cutter.setAccent (1.0f);
            host.cutter.setTransforms ({ true, 6.0, 1.0 });
        }
        host.run (4 * 96000);
        return host.outLeft;
    };
    CHECK (play (true) == play (false));
}

namespace
{
// Clicks: one sample of 0.8 on silence, `offset` frames from each pass's start.
std::int64_t clickOffset = 0, clickPass = 24000;
std::uint64_t clickJitter = 0; // if not 0, a seeded offset in +/- this many frames per pass

std::int64_t clickAt (std::int64_t pass)
{
    if (clickJitter == 0)
        return pass * clickPass + clickOffset;
    auto h = static_cast<std::uint64_t> (pass + 1) * 0x9e3779b97f4a7c15ULL;
    h ^= h >> 31;
    return pass * clickPass + static_cast<std::int64_t> (h % (2 * clickJitter + 1)) - static_cast<std::int64_t> (clickJitter);
}

StereoFrame clicks (std::int64_t frame)
{
    const auto pass = (frame + clickPass / 2) / clickPass;
    const auto v = frame == clickAt (pass) ? .8f : 0.f;
    return { v, v };
}

StereoFrame sine (std::int64_t frame)
{
    const auto v = static_cast<float> (.5 * std::sin (2.0 * 3.141592653589793 * 220.0 * static_cast<double> (frame) / 48000.0));
    return { v, v };
}

} // namespace

TEST_CASE ("Repeats read from 2 ms before the nearest click within 20 ms of the loop's start", "[loops][snap]")
{
    for (const auto ms : { -15, -5, 5, 15 })
    {
        CAPTURE (ms);
        clickOffset = ms * 48;
        clickPass = 24000;
        clickJitter = 0;
        LoopsHost host (loops (.25, 0), 48000.0, 120.0, 4.0, true); // a fed detector
        host.source = clicks;
        host.cutter.setIdle (true);
        host.run (96000);
        host.cutter.setIdle (false);
        host.run (4 * 96000);
        int replays = 0;
        for (const auto& block : host.blocks)
        {
            if (! block.replay())
                continue;
            ++replays;
            CHECK (block.loop.snapped);
            CHECK (block.loop.readFrame == block.loop.captureFrame + clickOffset - 96);
            // The click sounds 2 ms after the pass starts.
            CHECK (host.outLeft[static_cast<std::size_t> (block.frame + 96)] == .8f);
        }
        CHECK (replays == 12);
    }
}

TEST_CASE ("Without a hit within the tolerance, repeats read from 2 ms before the grid", "[loops][snap]")
{
    for (const auto ms : { -25, 25 })
    {
        CAPTURE (ms);
        clickOffset = ms * 48;
        clickPass = 24000;
        clickJitter = 0;
        LoopsHost host (loops (.25, 0), 48000.0, 120.0, 4.0, true); // a fed detector
        host.source = clicks;
        host.cutter.setIdle (true);
        host.run (96000);
        host.cutter.setIdle (false);
        host.run (2 * 96000);
        for (const auto& block : host.blocks)
            if (block.replay())
            {
                CHECK_FALSE (block.loop.snapped);
                CHECK (block.loop.readFrame == block.loop.captureFrame - 96);
            }
    }
    LoopsHost host (loops (.25, 0), 48000.0, 120.0, 4.0, true); // a fed detector
        host.source = sine;
    host.cutter.setIdle (true);
    host.run (96000);
    host.cutter.setIdle (false);
    host.run (2 * 96000);
    for (const auto& block : host.blocks)
        if (block.replay())
        {
            CHECK_FALSE (block.loop.snapped);
            CHECK (block.loop.readFrame == block.loop.captureFrame - 96);
        }
}

TEST_CASE ("The tolerance is a quarter of the loop when that is under 20 ms", "[loops][snap]")
{
    // A quarter of a 1/8 bar at 180 bpm is 2,000 frames (41.7 ms), so the tolerance is 500 frames.
    for (const auto [offset, snapped] : { std::pair { 576, false }, { 432, true }, { -576, false }, { -432, true } })
    {
        CAPTURE (offset);
        clickOffset = offset;
        clickPass = 2000;
        clickJitter = 0;
        LoopsHost host (loops (.25, 0), 48000.0, 180.0, .5, true); // a fed detector
        host.source = clicks;
        host.run (4 * 8000);
        int replays = 0;
        for (const auto& block : host.blocks)
            if (block.replay() && block.loop.captureFrame > 0)
            {
                ++replays;
                CHECK (block.loop.snapped == snapped);
            }
        CHECK (replays > 0);
    }
}

TEST_CASE ("A capture at an epoch's first frame reads from that frame", "[loops][snap]")
{
    clickOffset = 0;
    clickPass = 24000;
    clickJitter = 0;
    LoopsHost host (loops (.25, 0), 48000.0, 120.0, 4.0, true); // a fed detector
        host.source = clicks;
    host.bounds.epoch = host.detector.epoch() + 1;
    host.bounds.first = 96000;
    host.cutter.setIdle (true);
    host.run (96000);
    host.cutter.setIdle (false);
    host.run (96000);
    REQUIRE (host.blocks.size() >= 2);
    CHECK (host.blocks[1].loop.captureFrame == 96000);
    CHECK (host.blocks[1].loop.readFrame == 96000);
}

TEST_CASE ("The snap is the same for every buffer partition", "[loops][snap]")
{
    clickOffset = 0;
    clickPass = 24000;
    clickJitter = 18 * 48;
    std::vector<float> reference;
    for (int (*size) (std::int64_t) : { +[] (std::int64_t) { return 1; }, +[] (std::int64_t) { return 37; }, +[] (std::int64_t) { return 512; }, randomSize })
    {
        LoopsHost host (loops (.5, 0), 48000.0, 120.0, 4.0, true); // a fed detector
        host.source = clicks;
        host.run (6 * 96000, size);
        int snapped = 0;
        for (const auto& block : host.blocks)
            snapped += block.replay() && block.loop.snapped ? 1 : 0;
        CHECK (snapped > 0);
        if (reference.empty())
            reference = host.outLeft;
        CHECK (host.outLeft == reference);
    }
    clickJitter = 0;
}

namespace
{
const DrumLoop& drumLoop (double bassHz)
{
    static const DrumLoop drums (0.0, DrumLoop::Bass::sine), sub (55.0, DrumLoop::Bass::sine), bass (150.0, DrumLoop::Bass::sine);
    return bassHz == 0.0 ? drums : bassHz == 55.0 ? sub : bass;
}

template <int BassHz>
StereoFrame drums (std::int64_t frame)
{
    const auto& loop = drumLoop (BassHz);
    const auto v = loop.samples[static_cast<std::size_t> (frame) % loop.samples.size()];
    return { v, v };
}

// The review's pad chord, with a slow swell.
StereoFrame chord (std::int64_t frame)
{
    const auto t = static_cast<double> (frame) / 48000.0;
    auto v = 0.0;
    for (const auto hz : { 130.8, 261.6, 329.6, 392.0 })
        v += .2 * std::sin (2.0 * 3.141592653589793 * hz * t);
    v *= .75 + .25 * std::sin (2.0 * 3.141592653589793 * .3 * t);
    return { static_cast<float> (v), static_cast<float> (v) };
}

double rmsOf (const std::vector<float>& left, const std::vector<float>& right)
{
    double energy = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i)
        energy += static_cast<double> (left[i]) * left[i] + static_cast<double> (right[i]) * right[i];
    return std::sqrt (energy / static_cast<double> (2 * left.size()));
}

float peakOf (const std::vector<float>& samples)
{
    float peak = 0.f;
    for (const auto v : samples)
        peak = std::max (peak, std::abs (v));
    return peak;
}

// Cut at its busiest of nine spots on the same material, over 16 bars.
double busiestCut (StereoFrame (*source) (std::int64_t))
{
    double busiest = 0.0;
    for (const auto x : { 0.f, .5f, 1.f })
        for (const auto y : { 0.f, .5f, 1.f })
        {
            const auto m = mapPad (PadProgram::cut, x, y, {});
            LoopsHost host (m.cut, 48000.0, 120.0);
            host.cutter.setProcedure (m.procedure);
            host.cutter.setUnitsPerBar (m.unitsPerBar);
            host.cutter.setSeed (m.seed);
            host.source = source;
            host.run (16 * 96000);
            busiest = std::max (busiest, rmsOf (host.outLeft, host.outRight));
        }
    return busiest;
}
} // namespace

TEST_CASE ("On drums and bass, Loops stays near the track's level and never peaks above it", "[loops][level][drums]")
{
    // Measured over 16 bars at 12 spots, against the input (prototype: +1.20, +0.45 and +0.33 dB at most).
    // Without the hold, a one-beat loop at the top of Y lifts drums alone by 3.6 dB.
    const auto [source, most, name] = GENERATE (table<StereoFrame (*) (std::int64_t), double, const char*> ({
        { drums<0>, 1.5, "drums" },
        { drums<55>, .6, "a 55 Hz sub" },
        { drums<150>, .5, "a 150 Hz bass" },
    }));
    CAPTURE (name);
    const auto busiest = busiestCut (source);
    for (const auto bars : { 1.0, .5, .25 })
        for (int depth = 0; depth <= 3; ++depth)
        {
            CAPTURE (bars, depth);
            LoopsHost host (loops (bars, depth), 48000.0, 120.0, 4.0, true);
            host.source = source;
            host.run (16 * 96000);
            const auto input = rmsOf (host.inLeft, host.inRight);
            const auto output = rmsOf (host.outLeft, host.outRight);
            const auto overInput = 20.0 * std::log10 (output / input);
            CAPTURE (overInput, 20.0 * std::log10 (output / busiest));
            CHECK (peakOf (host.outLeft) <= peakOf (host.inLeft));
            CHECK (overInput <= most);
            CHECK (overInput >= -.5);
            CHECK (output <= busiest);
        }
}

TEST_CASE ("The hold never lifts a level, never cuts one below its bound, and leaves sustained material alone", "[loops][level]")
{
    const auto gainsOf = [] (StereoFrame (*source) (std::int64_t), double bars)
    {
        std::vector<std::array<float, 4>> found;
        LoopsHost host (loops (bars, 3), 48000.0, 120.0, 4.0, true);
        host.source = source;
        host.cutter.setIdle (true);
        host.run (96000);
        host.cutter.setIdle (false);
        host.run (8 * 96000);
        for (const auto& block : host.blocks)
            if (block.replay())
                found.push_back (block.loop.gains);
        REQUIRE_FALSE (found.empty());
        return found;
    };
    for (const auto bars : { 1.0, .5, .25 })
    {
        CAPTURE (bars);
        for (auto* source : { drums<0>, drums<55>, drums<150>, tone })
            for (const auto& gains : gainsOf (source, bars))
            {
                CHECK (gains[0] == 1.f);
                for (std::size_t level = 0; level < 4; ++level)
                {
                    CHECK (gains[level] <= 1.f);
                    CHECK (gains[level] >= std::exp2 (-.5 * static_cast<double> (level)) - 1.0e-3);
                }
            }
        // Sustained material is held only as far as its short spans' beating lifts them:
        // a swelling chord's eighth of a beat is up to 1.1 dB over its beat (measured -1.12 dB).
        for (const auto& gains : gainsOf (chord, bars))
            for (const auto gain : gains)
                CHECK (20.0 * std::log10 (gain) >= -1.25);
        // A click at the loop's start in a silent bar: the bound, reached.
        clickOffset = 0;
        clickPass = static_cast<std::int64_t> (96000 * bars);
        clickJitter = 0;
        for (const auto& gains : gainsOf (clicks, bars))
            for (std::size_t level = 0; level < 4; ++level)
                CHECK_THAT (gains[level], WithinAbs (std::exp2 (-.5 * static_cast<double> (level)), 1.0e-6));
        clickPass = 24000;
        for (const auto& gains : gainsOf ([] (std::int64_t) { return StereoFrame { 0.f, 0.f }; }, bars))
            for (const auto gain : gains)
                CHECK (gain == 1.f);
    }
}
