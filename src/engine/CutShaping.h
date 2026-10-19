// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/Cut.h"
#include "engine/Random.h"

#include <span>

namespace livecut::engine
{

// Sweeps pan and amplitude between two random settings across a roll, and
// pitch from the original up to a random offset. The first cut keeps the
// original pitch because it plays the input live.
inline void sweep (std::span<Cut> cuts, Random& random, const CutParameters& params)
{
    const auto startPan = random.uniform (params.minPan, params.maxPan);
    const auto endPan = random.uniform (params.minPan, params.maxPan);
    const auto startAmp = random.uniform (params.minAmp, params.maxAmp);
    const auto endAmp = random.uniform (params.minAmp, params.maxAmp);
    const auto endCents = random.uniform (params.minCents, params.maxCents);

    for (std::size_t i = 0; i < cuts.size(); ++i)
    {
        const auto phase = static_cast<double> (i) / static_cast<double> (cuts.size());
        cuts[i].pan = static_cast<float> (startPan + (endPan - startPan) * phase);
        cuts[i].amp = static_cast<float> (startAmp + (endAmp - startAmp) * phase);
        cuts[i].cents = static_cast<float> (endCents * phase);
    }
}

// Gives every cut its own random pan, amplitude and pitch. The first cut
// plays the input live and keeps its original pitch, but still draws its
// random pitch so existing seeds keep the same pattern.
inline void scatter (std::span<Cut> cuts, Random& random, const CutParameters& params)
{
    for (auto& cut : cuts)
    {
        cut.pan = static_cast<float> (random.uniform (params.minPan, params.maxPan));
        cut.amp = static_cast<float> (random.uniform (params.minAmp, params.maxAmp));
        cut.cents = static_cast<float> (random.uniform (params.minCents, params.maxCents));
    }

    if (! cuts.empty())
        cuts.front().cents = 0.0f;
}

} // namespace livecut::engine
