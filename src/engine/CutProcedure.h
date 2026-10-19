// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/Cut.h"
#include "engine/Random.h"

#include <vector>

namespace livecut::engine
{

// A strategy that decides how long each phrase lasts and how each block in it is cut.
class CutProcedure
{
public:
    virtual ~CutProcedure() = default;

    // Called at the start of every phrase.
    virtual int choosePhraseBars (Random& random, const CutParameters& params)
    {
        return random.integer (params.minPhraseBars, params.maxPhraseBars);
    }

    // Replaces `cuts` with the next block. The block lasts totalUnits (cuts),
    // which is never zero and never runs past the end of the phrase.
    virtual void chooseBlock (Random& random,
                              const CutParameters& params,
                              const BlockContext& context,
                              std::vector<Cut>& cuts) = 0;
};

} // namespace livecut::engine
