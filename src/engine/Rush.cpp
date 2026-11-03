// SPDX-License-Identifier: GPL-3.0-or-later
//
// Rush is new to this port, not from Nick Collins' BBCut library. Its roll is
// CutProc11's stutter (one block whose cuts replay the block's first frame) with
// an exact length that ends on the bar line, and WarpCut's geometric curve.

#include "engine/CutProcedures.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace livecut::engine
{

namespace
{
constexpr double epsilon = 1.0e-9;     // beats: a block that ends an ulp off its point counts as on it
constexpr double swellDb = -12.0;      // where a roll's repeats start, swelling back to full level
constexpr double rushRatio = 8.0;      // first cut to last at the edges of X
constexpr double cutsPerBeat = 4.0;    // an even roll is 16ths
constexpr double floorBeats = 1.0 / 32.0;

// Appends a roll's cut lengths, in beats, first to last.
void rollLengths (double beats, double curve, bool straight, std::vector<Cut>& cuts)
{
    const auto add = [&cuts] (double length)
    {
        Cut cut;
        cut.units = length;
        cuts.push_back (cut);
    };

    if (! straight)
    {
        // A geometric series from first to last, `rushRatio ^ curve` apart.
        const auto n = static_cast<int> (std::lround (cutsPerBeat * beats));
        const auto a = curve * std::log (rushRatio) / std::max (n - 1, 1);
        const auto first = a == 0.0 ? beats / n : beats * std::expm1 (-a) / std::expm1 (-n * a);
        for (int k = 0; k < n; ++k)
            add (first * std::exp (-k * a));
        return;
    }

    // Steps of halvings: m of them, the continuous ratio rounded to an octave. Each
    // step j plays cuts of 1/2^(j+1) beats until the roll is 1 - 2^-(j+1) through,
    // and the rest is the fastest. With no steps, an even roll of 16ths.
    const auto steps = static_cast<int> (std::lround (std::log2 (rushRatio) * std::abs (curve)));
    auto done = 0.0;
    const auto fill = [&] (double length, double until)
    {
        while (done + length <= until + epsilon)
        {
            add (length);
            done += length;
        }
    };
    if (steps == 0)
        fill (1.0 / cutsPerBeat, beats);
    else
    {
        for (int j = 0; j < steps; ++j)
            fill (.5 / std::exp2 (j), beats * (1.0 - std::exp2 (-(j + 1))));
        fill (.5 / std::exp2 (steps), beats);
    }
    // Only a bar off the 64th grid leaves a remainder; one under the floor joins the cut before.
    const auto remainder = beats - done;
    if (remainder > epsilon)
    {
        if (remainder < floorBeats && ! cuts.empty())
            cuts.back().units += remainder;
        else
            add (remainder);
    }
    if (curve < 0.0)
        std::reverse (cuts.begin(), cuts.end());
}
} // namespace

void Rush::chooseBlock (Random&, const CutParameters& params, const BlockContext& context, std::vector<Cut>& cuts)
{
    assert (params.rollBeats >= 0.0 && params.rollBeats <= 4.0);
    assert (params.rollCurve >= -1.0 && params.rollCurve <= 1.0);
    cuts.clear();

    const auto unitsPerBar = static_cast<double> (context.unitsPerBar);
    const auto bar = context.beatsPerBar;
    const auto unitsPerBeat = unitsPerBar / bar;
    const auto intoBar = context.unitsDone - std::floor ((context.unitsDone + epsilon * unitsPerBeat) / unitsPerBar) * unitsPerBar;
    const auto barUnits = std::min (unitsPerBar - intoBar, context.phraseUnits - context.unitsDone);
    const auto start = intoBar / unitsPerBeat; // beats into the bar

    // The track: one cut, unpitched at unity, played live as its block's first.
    const auto track = [&] (double units)
    {
        Cut cut;
        cut.units = units;
        cut.fade = false;
        cuts.push_back (cut);
    };

    // The roll starts a whole number of beats before the bar line, or at the bar's
    // start. A late engage gets a shorter roll from the next such point, or none.
    const auto rolled = std::min (params.rollBeats, bar);
    const auto rolls = [] (double beats) { return std::lround (cutsPerBeat * beats) >= 2; }; // a hit and a repeat
    auto rollStart = bar - rolled;
    if (! rolls (rolled))
        rollStart = bar;
    else if (start > rollStart + epsilon)
        rollStart = bar - std::floor (bar - start + epsilon);

    if (rollStart >= bar - epsilon || ! rolls (bar - rollStart))
    {
        track (barUnits);
        return;
    }
    if (start < rollStart - epsilon)
    {
        track ((rollStart - start) * unitsPerBeat);
        return;
    }

    rollLengths (bar - rollStart, params.rollCurve, params.straight, cuts);
    const auto n = cuts.size();
    assert (n >= 2 && n <= maxCutsPerBlock);
    auto done = 0.0;
    for (std::size_t k = 0; k < n; ++k)
    {
        auto& cut = cuts[k];
        assert (cut.units >= floorBeats - epsilon);
        // The last cut takes the exact remainder of the bar, so the roll lands on the bar line.
        cut.units = k + 1 < n ? cut.units * unitsPerBeat : barUnits - done;
        done += cut.units;
        cut.gate = 1.0;
        cut.fade = false;
        cut.cents = static_cast<float> (params.rollCents * static_cast<double> (k) / static_cast<double> (n - 1));
        // The hit at full level; its repeats swell from swellDb back to full, even in decibels.
        const auto db = k == 0 || n == 2 ? 0.0 : swellDb * (1.0 - static_cast<double> (k - 1) / static_cast<double> (n - 2));
        cut.amp = static_cast<float> (std::pow (10.0, db / 20.0));
    }
}

} // namespace livecut::engine
