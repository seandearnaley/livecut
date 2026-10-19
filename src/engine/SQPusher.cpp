// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Livecut, Copyright 2003-2004 Remy Muller, which implements
// SQPusher1 from Nick Collins' BBCut library for SuperCollider.

#include "engine/CutProcedures.h"
#include "engine/CutShaping.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace livecut::engine
{

namespace
{
constexpr double epsilon = 1.0e-6;

// Squarepusher rhythms transcribed by Nick Collins. BBCut wrote the tuplets as
// rounded decimals (0.571429, 0.33, 0.167 and a fudged 1.01); exact fractions
// make every fill last exactly one bar of 4/4.
constexpr double third = 1.0 / 3.0;
constexpr double fifth = 1.0 / 5.0;
constexpr double sixth = 1.0 / 6.0;
constexpr double seventh = 1.0 / 7.0;

const std::vector<SQPusher::Fill> fillTable {
    { { 0.75, 0.75, 0.75, 0.75 }, { 1.0 } },
    { { 0.5, 1.0 }, { 1.0 }, { 1.0, 0.5 } },
    { { 0.5 }, { 1.0, 1.0, 1.0 }, { 0.5 } },
    { { 4 * seventh }, { 4 * seventh, 4 * seventh }, { 4 * seventh, 4 * seventh }, { 4 * seventh }, { 2 * seventh, 2 * seventh } },
    { { 1.0, 0.5 }, { 1.0, 0.5 }, { 0.5, 0.5 } },
    { { 0.5, 0.5 }, { 2 * third, 2 * third, 2 * third }, { 1.0 } },
    { { third }, { third, third }, { third, third }, { 7 * third } },
    { { 7 * fifth }, { 2 * fifth, 2 * fifth }, { 3 * fifth, fifth }, { 1.0 } },
    { { sixth, sixth, sixth }, { 1.0, 1.0, 1.0 }, { 0.5 } },
    { { 1.5, 0.5, 1.0 }, { 0.25, 0.25, 0.25, 0.25 } },
    { { fifth, fifth }, { 2 * fifth, 2 * fifth }, { 2 * fifth, 2 * fifth }, { 2.0 } },
    { { 0.75, 0.75, 1.0 }, { 0.25, 0.25, 0.25, 0.25, 0.25, 0.25 } },
    { { 0.5, 1.0 }, { 0.5 }, { 0.125, 0.125, 0.125, 0.125 }, { 1.0 }, { sixth, sixth, sixth } },
};

// The chance of semiquavers on each quaver of the bar, scaled by the activity setting.
constexpr std::array semiquaverWeights { 0.0, 0.3, 0.0, 0.5, 0.7, 0.8, 0.9, 0.6 };

// Shortens a block that would run past `unitsLeft`: trims the cut that
// straddles the limit and drops the cuts after it, as BBCut does.
void truncateToFit (std::vector<Cut>& cuts, double unitsLeft)
{
    auto start = 0.0;

    for (std::size_t i = 0; i < cuts.size(); ++i)
    {
        if (start + cuts[i].units > unitsLeft + epsilon)
        {
            cuts[i].units = unitsLeft - start;
            cuts.resize (cuts[i].units > epsilon ? i + 1 : i);
            return;
        }

        start += cuts[i].units;
    }
}
} // namespace

const std::vector<SQPusher::Fill>& SQPusher::fills()
{
    return fillTable;
}

int SQPusher::choosePhraseBars (Random& random, const CutParameters& params)
{
    fill = -1;
    fillBlock = 0;
    return CutProcedure::choosePhraseBars (random, params);
}

void SQPusher::chooseBlock (Random& random,
                            const CutParameters& params,
                            const BlockContext& context,
                            std::vector<Cut>& cuts)
{
    // SQPusher works one bar at a time, in quarter-note beats.
    const auto unitsPerBar = static_cast<double> (context.unitsPerBar);
    const auto unitsPerBeat = unitsPerBar / context.beatsPerBar;
    const auto bar = std::floor ((context.unitsDone + epsilon) / unitsPerBar);
    const auto unitsIntoBar = std::max (0.0, context.unitsDone - bar * unitsPerBar);
    const auto beatsIntoBar = unitsIntoBar / unitsPerBeat;
    const auto startsLastBar = unitsIntoBar < epsilon
                            && std::abs (context.phraseUnits - context.unitsDone - unitsPerBar) < epsilon;

    if (startsLastBar)
    {
        fill = random.integer (0, static_cast<int> (fillTable.size()) - 1);
        fillBlock = 0;
    }

    cuts.clear();

    if (fill >= 0 && fillBlock < fillTable[static_cast<std::size_t> (fill)].size())
    {
        for (const auto beats : fillTable[static_cast<std::size_t> (fill)][fillBlock])
        {
            Cut cut;
            cut.units = beats * unitsPerBeat;
            cut.gate = params.fillDutyCycle;
            cuts.push_back (cut);
        }

        ++fillBlock;
    }
    else
    {
        fill = -1;

        // One or two quavers, or as many pairs of semiquavers. A block starting
        // on the off-beat quaver is a single quaver, so blocks stay on the beat grid.
        const auto positionInBeat = std::fmod (std::round (beatsIntoBar * 4.0) / 4.0, 1.0);
        const auto quaver = static_cast<std::size_t> (std::lround (beatsIntoBar * 2.0)) % semiquaverWeights.size();
        auto quavers = random.integer (1, 2);

        if (std::abs (positionInBeat - 0.5) < epsilon)
            quavers = 1;

        const auto semiquavers = random.chance (semiquaverWeights[quaver] * params.activity);

        Cut cut;
        cut.units = (semiquavers ? 0.25 : 0.5) * unitsPerBeat;
        cut.gate = params.dutyCycle;
        cuts.assign (static_cast<std::size_t> (semiquavers ? 2 * quavers : quavers), cut);
    }

    scatter (cuts, random, params);
    truncateToFit (cuts, unitsPerBar - unitsIntoBar);
}

} // namespace livecut::engine
