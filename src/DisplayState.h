// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include "engine/WetHold.h"

namespace livecut
{

// What the editor shows about the cutter. The audio thread writes it at the end
// of every block and the editor reads it on a timer. Each field is atomic on its
// own, so a read may mix neighbouring blocks, which a display can tolerate.
struct DisplayState
{
    enum class Status
    {
        waitingForHost, // the host is stopped or doesn't report its transport
        barTooLong,     // bars last longer than the cutter can record
        startingSoon,   // playing, waiting for the next unit to start cutting
        cutting,
        planOverflow,
        sourceUnavailable,
        filling,
        preparationError,
        streamError
    };

    std::atomic<Status> status { Status::waitingForHost };
    std::atomic<engine::WetHold::State> hold { engine::WetHold::State::live };
    std::atomic<int> phraseBars { 0 };
    std::atomic<int> unitsPerBar { 8 };
    std::atomic<double> blockStart { 0.0 }; // units into the phrase
    std::atomic<double> blockEnd { 0.0 };
    std::atomic<double> position { 0.0 };

    static_assert (std::atomic<Status>::is_always_lock_free);
    static_assert (std::atomic<double>::is_always_lock_free);
};

} // namespace livecut
