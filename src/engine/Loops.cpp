// SPDX-License-Identifier: GPL-3.0-or-later
//
// Loops is new to this port, not from Nick Collins' BBCut library. A replay is
// the classic cutter's "a later cut replays the block from its start", with the
// block's start moved back to the phrase's loop by the Cutter.

#include "engine/CutProcedures.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <span>

namespace livecut::engine
{

namespace
{
constexpr double epsilon = 1.0e-9;          // units: a block that ends an ulp off its point counts as on it
constexpr double floorBeats = 1.0 / 16.0;   // no replay is shorter
constexpr int passes = 4;                   // a phrase: the track, then three repeats

// Pass `k`'s cuts, as levels: 0 the whole loop, 1 its first half, 2 its first
// quarter, 3 its first eighth. The deepest levels fill the end of the phrase.
std::span<const int> levels (int pass, int depth)
{
    static constexpr std::array<int, 1> whole { 0 };
    static constexpr std::array<int, 2> halves { 1, 1 };
    static constexpr std::array<int, 4> quarters { 2, 2, 2, 2 };
    static constexpr std::array<int, 6> eighths { 2, 2, 3, 3, 3, 3 };
    if (pass <= 1 || (pass == 2 && depth < 2) || (pass == 3 && depth == 0))
        return whole;
    if (pass == 2 || depth == 1)
        return halves;
    if (depth == 2)
        return quarters;
    return eighths;
}
} // namespace

int Loops::choosePhraseBars (Random&, const CutParameters& params)
{
    return std::max (1, static_cast<int> (std::lround (passes * params.loopBars)));
}

void Loops::chooseBlock (Random&, const CutParameters& params, const BlockContext& context, std::vector<Cut>& cuts)
{
    assert (params.loopBars == 0.0 || params.loopBars == 1.0 || params.loopBars == .5 || params.loopBars == .25);
    assert (params.loopDepth >= 0 && params.loopDepth <= 3);
    cuts.clear();

    const auto add = [&cuts] (double units, int level)
    {
        Cut cut;
        cut.units = units;
        cut.fade = false; // joins crossfade, and the track is the input itself
        cut.loopLevel = static_cast<std::uint8_t> (level);
        cuts.push_back (cut);
    };

    const auto unitsPerBar = static_cast<double> (context.unitsPerBar);
    const auto pass = unitsPerBar * params.loopBars;
    const auto beatsPerUnit = context.beatsPerBar / unitsPerBar;
    const auto left = context.phraseUnits - context.unitsDone;

    // With no loop, or one under the floor, the track to the bar line.
    if (pass <= 0.0 || pass * beatsPerUnit < floorBeats - epsilon)
    {
        const auto intoBar = context.unitsDone - std::floor ((context.unitsDone + epsilon) / unitsPerBar) * unitsPerBar;
        add (std::min (unitsPerBar - intoBar, left), 0);
        return;
    }

    const auto k = std::floor ((context.unitsDone + epsilon) / pass);
    const auto passEnd = std::min ((k + 1.0) * pass, context.phraseUnits);
    const auto onBoundary = std::abs (context.unitsDone - k * pass) < epsilon;

    // Before the capture, off a pass boundary, or past the phrase's four passes: the track to the pass's end.
    if (! context.loopCaptured || ! onBoundary || k >= passes || passEnd - context.unitsDone < pass - epsilon)
    {
        add (passEnd - context.unitsDone, 0);
        return;
    }

    // The deepest level stays at or above the floor.
    auto depth = params.loopDepth;
    while (depth > 0 && pass * beatsPerUnit / std::exp2 (depth) < floorBeats - epsilon)
        --depth;
    for (const auto level : levels (static_cast<int> (k), depth))
        add (pass / std::exp2 (level), level + 1);
}

} // namespace livecut::engine
