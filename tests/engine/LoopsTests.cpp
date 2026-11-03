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

TEST_CASE ("Through tempo automation, Loops' passes land within 2 frames", "[loops][cutter][tempo]")
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
                CHECK (worst <= 2.0); // measured 1
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

namespace
{
// A held bass: 55 Hz (or `hz`) at 0.6 with its octave at 0.15.
template <int Hz = 55>
StereoFrame heldBass (std::int64_t frame)
{
    const auto t = static_cast<double> (frame) / 48000.0;
    const auto v = static_cast<float> (.6 * std::sin (2.0 * 3.141592653589793 * Hz * t + .7) + .15 * std::sin (2.0 * 3.141592653589793 * 2 * Hz * t + .2));
    return { v, v };
}

float largestStep (const std::vector<float>& samples, std::size_t from, std::size_t to)
{
    float step = 0.f;
    for (auto i = std::max<std::size_t> (from, 1); i < std::min (to, samples.size()); ++i)
        step = std::max (step, std::abs (samples[i] - samples[i - 1]));
    return step;
}

// Where each cut of a run reads: from `start` on, frame f reads `read + (f - start)`,
// at `gain`, and runs on past its end until the next.
struct Segment
{
    std::int64_t start = 0, read = 0;
    float gain = 1.f;
    bool replay = false;
    bool blockStart = false;
};

std::vector<Segment> segmentsOf (const LoopsHost& host, double samplesPerUnit)
{
    std::vector<Segment> segments;
    for (const auto& block : host.blocks)
    {
        auto units = 0.0;
        std::int64_t start = 0;
        for (std::size_t k = 0; k < block.cuts.size(); ++k)
        {
            const auto& cut = block.cuts[k];
            const auto replay = cut.loopLevel > 0;
            const auto read = replay || block.source.firstFrame != block.frame ? block.source.firstFrame : block.frame + start;
            segments.push_back ({ block.frame + start, replay ? read : block.frame + start,
                                  replay ? block.loop.gains[static_cast<std::size_t> (cut.loopLevel - 1)] : 1.f, replay, k == 0 });
            units += cut.units;
            start = std::lround (units * samplesPerUnit);
        }
    }
    return segments;
}

float sampleOf (const LoopsHost& host, const Segment& segment, std::int64_t frame)
{
    return segment.gain * host.inLeft[static_cast<std::size_t> (segment.read + (frame - segment.start))];
}

// The largest difference from what the joins should make: a replay's cut, and the
// cut a replay block ends on, runs on under what follows for 2 ms, fading linearly.
// A replay block that runs out early begins that fade where it runs out.
float joinError (const LoopsHost& host, double samplesPerUnit, std::int64_t& joins)
{
    const auto segments = segmentsOf (host, samplesPerUnit);
    const auto fade = static_cast<std::int64_t> (std::lround (.002 * host.rate));
    float worst = 0.f;
    joins = 0;
    for (std::size_t s = 1; s < segments.size(); ++s)
    {
        const auto& before = segments[s - 1];
        const auto& now = segments[s];
        if (! before.replay && ! now.replay)
            continue;
        ++joins;
        // Where the outgoing cut's layout ended: an early run-out begins the fade there.
        auto from = now.start;
        if (now.blockStart && before.replay)
        {
            const auto& block = *std::find_if (host.blocks.rbegin(), host.blocks.rend(), [&] (const Activated& b) { return b.frame < now.start; });
            auto units = 0.0;
            for (const auto& cut : block.cuts)
                units += cut.units;
            from = std::min (now.start, block.frame + std::lround (units * samplesPerUnit));
        }
        for (auto f = from; f < from + fade && f < static_cast<std::int64_t> (host.outLeft.size()); ++f)
        {
            const auto weight = static_cast<float> (fade - (f - from)) / static_cast<float> (fade);
            const auto under = f < now.start ? sampleOf (host, before, f) : sampleOf (host, now, f);
            const auto expected = weight * sampleOf (host, before, f) + (1.f - weight) * under;
            worst = std::max (worst, std::abs (host.outLeft[static_cast<std::size_t> (f)] - expected));
        }
    }
    return worst;
}
} // namespace

TEST_CASE ("Every Loops join crossfades over 2 ms", "[loops][joins]")
{
    for (const auto rate : { 48000.0, 44100.0 })
        for (const auto bars : { 1.0, .5, .25 })
            for (int depth = 0; depth <= 3; ++depth)
            {
                CAPTURE (rate, bars, depth);
                LoopsHost host (loops (bars, depth), rate, 120.0, 4.0, true);
                host.source = drums<55>;
                host.run (static_cast<std::int64_t> (8 * 2.0 * rate));
                std::int64_t joins = 0;
                const auto error = joinError (host, samplesPerUnitOf (host), joins);
                CHECK (joins > 6);
                CHECK (error <= 1.0e-6f);
            }
}

TEST_CASE ("On a held bass, no Loops join steps more than 0.025", "[loops][joins]")
{
    // Prototype: 0.016 with the joins crossfaded, 0.598 spliced.
    float worst = 0.f, material = 0.f;
    for (const auto bars : { 1.0, .5, .25 })
        for (int depth = 0; depth <= 3; ++depth)
        {
            CAPTURE (bars, depth);
            LoopsHost host (loops (bars, depth), 48000.0, 120.0, 4.0, true);
            host.source = heldBass<>;
            host.run (16 * 96000);
            worst = std::max (worst, largestStep (host.outLeft, 0, host.outLeft.size()));
            material = std::max (material, largestStep (host.inLeft, 0, host.inLeft.size()));
        }
    CAPTURE (worst, material);
    CHECK (worst <= .025f);
}

TEST_CASE ("A join where the track continues the track leaves the input untouched", "[loops][joins]")
{
    // The epoch starts mid-pass, so a track pass runs into another before the first repeat.
    LoopsHost host (loops (.25, 3));
    host.bounds.epoch = 5;
    host.bounds.first = 96000 + 5 * 6000;
    host.cutter.setIdle (true);
    host.run (96000 + 5 * 6000 + 10);
    host.cutter.setIdle (false);
    host.run (96000);
    REQUIRE (host.blocks.size() >= 3);
    REQUIRE_FALSE (host.blocks[1].replay());
    for (auto f = host.blocks[0].frame; f < host.blocks[2].frame; ++f)
        REQUIRE (host.outLeft[static_cast<std::size_t> (f)] == host.inLeft[static_cast<std::size_t> (f)]);
}

namespace
{
// The restart's tail: the cut playing at `restart`, run on, fading over 5 ms under `incoming`.
float restartError (const LoopsHost& host, std::int64_t restart, StereoFrame (*incoming) (const LoopsHost&, std::int64_t))
{
    const auto segments = segmentsOf (host, samplesPerUnitOf (host));
    const auto fade = static_cast<std::int64_t> (std::lround (.005 * host.rate));
    const auto before = *std::find_if (segments.rbegin(), segments.rend(), [&] (const Segment& s) { return s.start < restart; });
    float worst = 0.f;
    for (auto f = restart; f < restart + fade; ++f)
    {
        const auto weight = static_cast<float> (fade - (f - restart)) / static_cast<float> (fade);
        const auto expected = weight * sampleOf (host, before, f) + (1.f - weight) * incoming (host, f).left;
        worst = std::max (worst, std::abs (host.outLeft[static_cast<std::size_t> (f)] - expected));
    }
    return worst;
}

StereoFrame inputOf (const LoopsHost& host, std::int64_t frame)
{
    return { host.inLeft[static_cast<std::size_t> (frame)], host.inRight[static_cast<std::size_t> (frame)] };
}
} // namespace

TEST_CASE ("A restart in the middle of a Loops cut fades it out under what follows over 5 ms", "[loops][joins]")
{
    // A one-bar loop: bar 2 repeats bar 1. A restart asked for in the middle of it
    // lands on the next 16th; what follows is a new phrase's track.
    for (const auto rate : { 48000.0, 44100.0 })
    {
        LoopsHost host (loops (1.0, 0), rate, 120.0, 4.0, true);
        host.source = heldBass<57>;
        const auto bar = static_cast<std::int64_t> (2.0 * rate);
        host.run (bar + bar / 3, [] (std::int64_t) { return 1; });
        host.cutter.restartAtNextUnit();
        host.run (bar / 2, [] (std::int64_t) { return 1; });
        const auto restart = host.blocks.back().frame;
        CAPTURE (rate, restart);
        REQUIRE_FALSE (host.blocks.back().replay());
        REQUIRE (host.blocks[host.blocks.size() - 2].replay());
        CHECK (restartError (host, restart, inputOf) <= 1.0e-6f);
    }
}

TEST_CASE ("A restart on a Loops pass boundary fades the last cut out too, with no zero sample", "[loops][joins]")
{
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (int beat = 1; beat < 4; ++beat) // beat 1 ends the track pass; 2 and 3 end repeats
        {
            CAPTURE (rate, beat);
            LoopsHost host (loops (.25, 3), rate, 120.0, 4.0, true);
            host.source = heldBass<57>;
            const auto beatFrames = .5 * rate;
            // Asked for a frame before the beat, so the restart lands on it, a pass boundary.
            host.run (static_cast<std::int64_t> (4 * beatFrames + beat * beatFrames) - 1, [] (std::int64_t) { return 1; });
            host.cutter.restartAtNextUnit();
            host.run (static_cast<std::int64_t> (beatFrames), [] (std::int64_t) { return 1; });
            const auto restart = host.blocks.back().frame;
            CAPTURE (restart);
            CHECK (host.blocks[host.blocks.size() - 2].replay() == (beat > 1));
            CHECK (restartError (host, restart, inputOf) <= 1.0e-6f);
            for (auto f = restart - 10; f < restart + 300; ++f)
                CHECK ((host.outLeft[static_cast<std::size_t> (f)] != 0.f || host.inLeft[static_cast<std::size_t> (f)] == 0.f));
        }
}

TEST_CASE ("A restart into Loops fades the outgoing cut out", "[loops][joins]")
{
    // CutProc11 with no pan, pitch or level changes, then Loops from a restart asked
    // for at each 16th of bar 1; only restarts that cut off a repeat count.
    CutParameters classic;
    classic.minPan = classic.maxPan = 0.f;
    classic.minRepeats = 1;
    classic.maxRepeats = 3;
    int repeats = 0;
    for (int sixteenth = 0; sixteenth < 16; ++sixteenth)
    {
        CAPTURE (sixteenth);
        LoopsHost host (classic, 48000.0, 120.0, 4.0, true);
        host.source = heldBass<57>;
        host.cutter.setProcedure (Procedure::cutProc11);
        host.cutter.setSeed (5);
        host.run (96000 + sixteenth * 6000 + 1000, [] (std::int64_t) { return 1; });
        host.cutter.setProcedure (Procedure::loops);
        host.cutter.setParameters (loops (.5, 3));
        host.cutter.restartAtNextUnit();
        const auto asked = host.frame;
        host.run (12000, [] (std::int64_t) { return 1; });
        REQUIRE (host.cutter.activeProcedure() == Procedure::loops);
        // The restart lands on the next 16th, where Loops' first block, a track, starts.
        const auto first = std::find_if (host.blocks.begin(), host.blocks.end(), [&] (const Activated& b) { return b.frame >= asked; });
        REQUIRE (first != host.blocks.end());
        REQUIRE (first != host.blocks.begin());
        const auto restart = first->frame;
        CHECK (restart - asked == 5000);
        CHECK_FALSE (first->replay());
        // The outgoing classic cut that played the frame before, read on from its block's source.
        const auto& out = *(first - 1);
        std::vector<std::int64_t> starts { 0 };
        auto units = 0.0;
        for (const auto& cut : out.cuts)
        {
            units += cut.units;
            starts.push_back (std::lround (units * samplesPerUnitOf (host)));
        }
        auto k = std::size_t { 0 };
        while (k + 2 < starts.size() && restart - out.frame > starts[k + 1])
            ++k;
        if (k == 0)
            continue; // the live cut: its tail is the input
        ++repeats;
        const auto read = out.source.firstFrame - out.frame - starts[k]; // input frame = frame + read
        float worst = 0.f;
        for (auto f = restart; f < restart + 240; ++f)
        {
            const auto weight = static_cast<float> (240 - (f - restart)) / 240.f;
            const auto expected = weight * host.inLeft[static_cast<std::size_t> (f + read)] + (1.f - weight) * host.inLeft[static_cast<std::size_t> (f)];
            worst = std::max (worst, std::abs (host.outLeft[static_cast<std::size_t> (f)] - expected));
        }
        CHECK (worst <= 1.0e-6f);
        // Then the track, exactly.
        for (auto f = restart + 240; f < restart + 480; ++f)
            CHECK (host.outLeft[static_cast<std::size_t> (f)] == host.inLeft[static_cast<std::size_t> (f)]);
    }
    CHECK (repeats >= 3);
}

namespace
{
// An arrangement loop of `bars` bars, wrapping `early` 16ths before its end, on
// `material` addressed by the host's position, as a host replays its audio. Each
// wrap steps no more than the input's own step there, or three times the material's
// largest step: a 2 ms crossfade between two phases of a held bass is a little
// steeper than the bass itself, as the joins' bar allows.
struct Wrapped
{
    float worstRatio = 0.f;
    int wraps = 0;
};

// `jumps` lists frames where the host jumps back `jumpBeats`, as a seek does; with
// none, the host wraps an arrangement loop of `bars` bars on the frame nearest its
// end (the later frame where the end falls half a sample between two).
Wrapped wrapLoop (StereoFrame (*material) (std::int64_t), int bars, double meter, double bpm, int early, double rate = 48000.0,
                  const CutParameters& params = loops (.25, 3), Procedure procedure = Procedure::loops,
                  std::vector<std::int64_t> jumps = {}, double jumpBeats = 0.0)
{
    Cutter cutter;
    OnsetDetector detector;
    Epoch bounds;
    REQUIRE (detector.prepare (rate, 8192, 3.0));
    bounds.epoch = detector.epoch();
    cutter.prepare (rate);
    cutter.setTransformSource (detector, bounds);
    cutter.setProcedure (procedure);
    cutter.setUnitsPerBar (unitsPerBar);
    cutter.setParameters (params);
    const auto perBeat = rate * 60.0 / bpm;
    const auto loopEnd = bars * meter - early * meter / unitsPerBar;
    // The host wraps on the frame nearest the loop's end.
    const auto loopFrames = static_cast<std::int64_t> (std::llround (loopEnd * perBeat));
    std::int64_t position = 0;
    std::vector<float> in, out;
    std::vector<std::size_t> wraps;
    float material_ = 0.f;
    const auto length = jumps.empty() ? 3 * loopFrames : jumps.back() + static_cast<std::int64_t> (rate);
    for (std::int64_t frame = 0; frame < length; ++frame)
    {
        const auto jump = std::find (jumps.begin(), jumps.end(), frame) != jumps.end();
        if (jump || (jumps.empty() && position == loopFrames))
        {
            position = jump ? position - static_cast<std::int64_t> (std::llround (jumpBeats * perBeat)) : 0;
            ++bounds.epoch;
            bounds.first = frame;
            REQUIRE (detector.beginTransportEpoch (bounds.epoch));
            wraps.push_back (static_cast<std::size_t> (frame));
        }
        const auto beats = static_cast<double> (position) / perBeat;
        const auto at = material (position);
        auto left = at.left, right = at.right;
        detector.process (frame, left, right);
        const auto bar = static_cast<std::int64_t> (std::floor (beats / meter));
        cutter.process (&left, &right, 1, { bpm, meter }, { bar, beats - meter * static_cast<double> (bar) });
        REQUIRE (cutter.error() == Cutter::Error::none);
        in.push_back (at.left);
        out.push_back (left);
        ++position;
    }
    material_ = largestStep (in, 0, in.size());
    Wrapped wrapped;
    for (const auto w : wraps)
    {
        ++wrapped.wraps;
        const auto own = std::abs (in[w] - in[w - 1]);
        const auto ours = largestStep (out, w, w + 200);
        wrapped.worstRatio = std::max (wrapped.worstRatio, ours / std::max (own, 3.f * material_));
    }
    return wrapped;
}
} // namespace

TEST_CASE ("An arrangement loop that wraps on or off a pass boundary crossfades from the loop", "[loops][joins][loop]")
{
    for (auto* material : { heldBass<57>, drums<0> })
        for (const auto meter : { 4.0, 3.0 })
            for (const auto bpm : { 93.0, 120.0 })
                for (const auto bars : { 2, 4 })
                    for (const auto early : { 0, 2 })
                    {
                        CAPTURE (meter, bpm, bars, early);
                        const auto wrapped = wrapLoop (material, bars, meter, bpm, early);
                        CAPTURE (wrapped.worstRatio);
                        CHECK (wrapped.wraps == 2);
                        CHECK (wrapped.worstRatio <= 1.f);
                    }
}

namespace
{
// One random Loops scenario, rendered in buffers from `size`, split at every event
// so each event lands on the same frame whatever the partition.
struct Scenario
{
    std::vector<float> output;
    float inputPeak = 0.f;
    bool finite = true;
    std::int64_t replayed = 0;
};

Scenario playScenario (std::uint64_t seed, int (*size) (std::int64_t))
{
    std::uint64_t state = seed * 0x9e3779b97f4a7c15ULL + 1;
    const auto next = [&state]
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return static_cast<double> (state >> 11) * 0x1p-53;
    };
    const double rates[] { 44100.0, 48000.0, 96000.0 };
    const double meters[] { 4.0, 3.0, 5.0, 7.0, 2.5, 3.5, 6.5, 6.0 };
    const auto rate = rates[static_cast<std::size_t> (next() * 3)];
    const auto meter = meters[static_cast<std::size_t> (next() * 8)];
    auto bpm = 80.0 + 100.0 * next();
    Cutter cutter;
    OnsetDetector detector;
    Epoch bounds;
    REQUIRE (detector.prepare (rate, 8192, 3.0));
    bounds.epoch = detector.epoch();
    cutter.prepare (rate);
    cutter.setTransformSource (detector, bounds);
    cutter.setProcedure (Procedure::loops);
    cutter.setUnitsPerBar (unitsPerBar);
    const double lengths[] { 1.0, .5, .25 };
    auto params = loops (lengths[static_cast<std::size_t> (next() * 3)], static_cast<int> (next() * 4), next() < .3);
    cutter.setParameters (params);

    Scenario scenario;
    const auto frames = static_cast<std::int64_t> (12 * meter * 60.0 / bpm * rate);
    auto beats = 0.0;
    std::int64_t frame = 0, nextEvent = static_cast<std::int64_t> (next() * rate);
    std::vector<float> left, right;
    while (frame < frames)
    {
        if (frame == nextEvent)
        {
            nextEvent = frame + 1 + static_cast<std::int64_t> (next() * .8 * rate);
            const auto e = next();
            if (e < .2)
            {
                params.loopBars = lengths[static_cast<std::size_t> (next() * 3)]; // an X zone change restarts
                cutter.setParameters (params);
                cutter.restartAtNextUnit();
            }
            else if (e < .4)
            {
                params.loopDepth = static_cast<int> (next() * 4); // Y takes effect from the next pass
                params.straight = next() < .3;
                cutter.setParameters (params);
            }
            else if (e < .5)
                cutter.restartAtNextUnit();
            else if (e < .6)
                cutter.setIdle (next() < .3);
            else if (e < .7)
                bpm = std::clamp (bpm * (.85 + .3 * next()), 60.0, 200.0);
            else if (e < .8)
            {
                // A seek or an arrangement loop's wrap: a new epoch from this frame.
                beats = std::max (0.0, beats + (next() < .5 ? -8.0 * next() : 8.0 * next()));
                ++bounds.epoch;
                bounds.first = frame;
                REQUIRE (detector.beginTransportEpoch (bounds.epoch));
            }
        }
        const auto n = static_cast<int> (std::min ({ static_cast<std::int64_t> (size (frame)), frames - frame, nextEvent - frame }));
        left.resize (static_cast<std::size_t> (n));
        right.resize (static_cast<std::size_t> (n));
        for (int i = 0; i < n; ++i)
        {
            // The tone with a click on every 8th of a second, for the snap to find.
            auto in = tone (frame + i);
            if ((frame + i) % static_cast<std::int64_t> (rate / 8.0) == 0)
                in.left = in.right = .8f;
            left[static_cast<std::size_t> (i)] = in.left;
            right[static_cast<std::size_t> (i)] = in.right;
            scenario.inputPeak = std::max ({ scenario.inputPeak, std::abs (in.left), std::abs (in.right) });
            detector.process (frame + i, in.left, in.right);
        }
        const auto bar = static_cast<std::int64_t> (std::floor (beats / meter));
        cutter.process (left.data(), right.data(), n, { bpm, meter }, { bar, beats - meter * static_cast<double> (bar) });
        REQUIRE (cutter.error() == Cutter::Error::none);
        REQUIRE_FALSE (cutter.player().hasSourceError());
        if (cutter.isCutting() && ! cutter.currentCuts().empty() && cutter.currentCuts().front().loopLevel > 0)
            scenario.replayed += n;
        for (int i = 0; i < n; ++i)
        {
            scenario.finite = scenario.finite && std::isfinite (left[static_cast<std::size_t> (i)]) && std::isfinite (right[static_cast<std::size_t> (i)]);
            scenario.output.push_back (left[static_cast<std::size_t> (i)]);
            scenario.output.push_back (right[static_cast<std::size_t> (i)]);
        }
        frame += n;
        beats += static_cast<double> (n) * bpm / 60.0 / rate;
    }
    return scenario;
}
} // namespace

TEST_CASE ("Random Loops scenarios are partition-independent, finite and never louder than the input", "[loops][fuzz]")
{
    std::int64_t replayed = 0;
    for (std::uint64_t seed = 1; seed <= 8; ++seed)
    {
        CAPTURE (seed);
        const auto reference = playScenario (seed, [] (std::int64_t) { return 1; });
        CHECK (reference.finite);
        // A crossfade of two samples can round a float's last bit past the larger.
        CHECK (peakOf (reference.output) <= reference.inputPeak * (1.f + 1.0e-6f));
        replayed += reference.replayed;
        for (int (*size) (std::int64_t) : { randomSize, +[] (std::int64_t) { return 1024; } })
        {
            const auto other = playScenario (seed, size);
            CAPTURE (firstDifference (other.output, reference.output));
            CHECK (other.output == reference.output);
        }
    }
    CHECK (replayed > 0);
}

TEST_CASE ("An arrangement loop that wraps a frame into a pass boundary's join keeps the join's fade", "[loops][joins][loop]")
{
    // At 44.1 kHz and 128 bpm bar 3's line falls at 248062.5: the earlier frame
    // takes the pass boundary and its join, and a host wrapping on the later frame
    // reset the rendering one frame into the join, cutting its fade to the
    // incoming repeat off at once. Spliced, it stepped 2.05 times the bound.
    for (const auto [bars, depth] : { std::pair { 1.0, 0 }, { .5, 2 }, { .25, 3 } })
    {
        CAPTURE (bars, depth);
        const auto wrapped = wrapLoop (heldBass<55>, 3, 4.0, 128.0, 0, 44100.0, loops (bars, depth));
        CAPTURE (wrapped.worstRatio);
        CHECK (wrapped.wraps == 2);
        CHECK (wrapped.worstRatio <= 1.f);
    }
}

TEST_CASE ("A seek within 2 ms after a Loops join keeps the join's fade", "[loops][joins][loop]")
{
    // A one-bar loop at the top of Y: bar 4's last pass starts its eighths at 348000.
    // A seek back two bars 1, 8, 40 or 90 frames later used to splice, up to 0.62.
    for (const auto after : { 1, 8, 40, 90 })
    {
        CAPTURE (after);
        const auto wrapped = wrapLoop (heldBass<55>, 0, 4.0, 120.0, 0, 48000.0, loops (1.0, 3), Procedure::loops, { 348000 + after }, -8.0);
        CAPTURE (wrapped.worstRatio);
        CHECK (wrapped.wraps == 1);
        CHECK (wrapped.worstRatio <= 1.f);
    }
}

TEST_CASE ("A seek within 2 ms after a Rush join keeps the join's fade", "[loops][joins][loop][rush]")
{
    // Rush shares the join: a whole-bar even roll joins every 16th, and bar 3's
    // sixth repeat starts at 222000. A seek just after it spliced the same way.
    const auto roll = mapPad (PadProgram::rush, .5f, .9f, {}).cut;
    for (const auto after : { 1, 8, 40, 90 })
    {
        CAPTURE (after);
        const auto wrapped = wrapLoop (heldBass<55>, 0, 4.0, 120.0, 0, 48000.0, roll, Procedure::rush, { 222000 + after }, -8.0);
        CAPTURE (wrapped.worstRatio);
        CHECK (wrapped.wraps == 1);
        CHECK (wrapped.worstRatio <= 1.f);
    }
}

TEST_CASE ("A press on a half-sample 16th captures its pass from its start, for every buffer size", "[loops][cutter][late][tie]")
{
    // At 44.1 kHz and 120 bpm a 16th is 5512.5 samples, so every odd 16th ties half
    // a sample between two frames. The earlier frame takes it, as Rush's tie rule
    // says; the capture used to round the other way, start a frame before the
    // epoch, and be refused: the first repeat came a bar late, by buffer size.
    for (const auto bars : { 1.0, .5, .25 })
        for (const auto pressedBar : { 0, 3 })
        {
            const auto barFrames = 88200;
            const auto pressAt = pressedBar * barFrames + 11264; // in the 16th before the third
            const auto pass = static_cast<std::int64_t> (barFrames * bars);
            const auto expected = pressedBar * barFrames + (11264 + 5513) / pass * pass; // the pass holding 16th 3
            std::vector<float> reference;
            for (int (*size) (std::int64_t) : { +[] (std::int64_t) { return 1; }, +[] (std::int64_t) { return 2; },
                                               +[] (std::int64_t) { return 7; }, +[] (std::int64_t) { return 64; },
                                               +[] (std::int64_t) { return 100; }, +[] (std::int64_t) { return 128; },
                                               +[] (std::int64_t) { return 256; }, +[] (std::int64_t) { return 512; },
                                               +[] (std::int64_t) { return 1000; }, +[] (std::int64_t) { return 1024; }, randomSize })
            {
                CAPTURE (bars, pressedBar, size (0));
                LoopsHost host (loops (bars, 0), 44100.0, 120.0);
                host.cutter.setIdle (true);
                host.run (pressAt, size);
                host.cutter.setIdle (false);
                host.run (3 * barFrames, size);
                REQUIRE (host.blocks.size() >= 2);
                CHECK (host.blocks[1].loop.captureFrame == expected);
                CHECK (host.blocks[1].replay()); // the first repeat, a pass after the press
                CHECK (host.blocks[1].frame - expected <= pass + 1);
                if (reference.empty())
                    reference = host.outLeft;
                CHECK (host.outLeft == reference);
            }
        }
}

TEST_CASE ("The level hold reads a bounded number of frames for each frame it renders", "[loops][level][cpu]")
{
    // It used to read the whole loop on the sample of the phrase's first repeat: a
    // bar of 768,000 frames at 96 kHz and 30 bpm, 0.5 to 2 ms on one sample. Now it
    // reads at most 32 frames of history a sample while the track pass plays.
    struct Config
    {
        double rate, bpm, meter;
        int buffer;
    };
    for (const auto [rate, bpm, meter, buffer] : { Config { 96000.0, 60.0, 4.0, 32 }, Config { 96000.0, 70.0, 7.0, 64 }, Config { 192000.0, 30.0, 4.0, 128 } })
    {
        CAPTURE (rate, bpm, meter, buffer);
        LoopsHost host (loops (1.0, 3), rate, bpm, meter, true);
        host.source = drums<0>;
        static int size = 0;
        size = buffer;
        host.run (static_cast<std::int64_t> (2.25 * meter * 60.0 / bpm * rate), [] (std::int64_t) { return size; });
        int replays = 0;
        for (const auto& block : host.blocks)
            replays += block.replay() ? 1 : 0;
        CHECK (replays >= 1);
        CHECK (host.cutter.loopHoldMostReads() > 0);
        CHECK (host.cutter.loopHoldMostReads() <= 32);
    }
}

TEST_CASE ("The level hold sums the snapped loop exactly", "[loops][level]")
{
    // Spread over the track pass or not, the gains are the same sum, in the same
    // order, of the same frames: from the read frame for a loop's length, up to the
    // frame of the first repeat.
    for (const auto [rate, bpm] : { std::pair { 48000.0, 120.0 }, { 44100.0, 128.0 }, { 96000.0, 93.0 } })
        for (const auto bars : { 1.0, .5, .25 })
        {
            CAPTURE (rate, bpm, bars);
            LoopsHost host (loops (bars, 3), rate, bpm, 4.0, true);
            host.source = drums<0>;
            host.cutter.setIdle (true);
            host.run (static_cast<std::int64_t> (rate) / 3);
            host.cutter.setIdle (false);
            host.run (static_cast<std::int64_t> (6 * 4 * 60.0 / bpm * rate));
            int checked = 0;
            for (std::size_t b = 1; b < host.blocks.size(); ++b)
            {
                const auto& block = host.blocks[b];
                if (! block.replay() || host.blocks[b - 1].replay())
                    continue; // a phrase's first repeat
                const auto read = block.loop.readFrame;
                const auto loopFrames = unitsPerBar * bars * samplesPerUnitOf (host);
                std::array<std::int64_t, 4> lengths {};
                for (std::size_t level = 0; level < 4; ++level)
                    lengths[level] = std::max<std::int64_t> (1, std::lround (loopFrames / std::exp2 (static_cast<double> (level))));
                const auto recorded = std::min (lengths[0], block.frame + 1 - read);
                std::array<double, 4> energy {};
                auto sum = 0.0;
                for (std::int64_t i = 0; i < recorded; ++i)
                {
                    const auto l = host.inLeft[static_cast<std::size_t> (read + i)], r = host.inRight[static_cast<std::size_t> (read + i)];
                    sum += static_cast<double> (l) * l + static_cast<double> (r) * r;
                    for (std::size_t level = 1; level < 4; ++level)
                        if (i + 1 == lengths[level])
                            energy[level] = sum;
                }
                const auto whole = sum / static_cast<double> (std::max<std::int64_t> (recorded, 1));
                for (std::size_t level = 1; level < 4; ++level)
                {
                    const auto part = energy[level] / static_cast<double> (lengths[level]);
                    const auto gain = part > whole && part > 0.0 ? static_cast<float> (std::sqrt (whole / part)) : 1.0f;
                    CHECK (block.loop.gains[level] == gain);
                }
                ++checked;
            }
            CHECK (checked >= 2);
        }
}
