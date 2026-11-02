// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/PadPrograms.h"

#include <optional>

namespace livecut::engine
{

enum class PadQuantize { beat, bar };

class PadGate
{
public:
    struct Input
    {
        bool alwaysEngaged = false, touch = false, hold = false;
        PadProgram program = PadProgram::cut;
        PadQuantize quantize = PadQuantize::beat;
    };

    struct Step
    {
        float gain = 0.0f;
        bool idle = true;
        bool programChanged = false;
    };

    void prepare (double sampleRate);
    // The full reset at preparation: nothing latched, nothing pending, closed.
    void reset();
    // The transport moved or stopped: closes the gate and drops any release
    // or pending switch, but keeps a latched Hold and the active program, so
    // the next process() reopens at once when Hold is still on.
    void resync();
    // A switch from Classic over cuts in progress: an armed release at full
    // gain, as if a finger had just lifted, so the cuts carry on to the
    // boundary and fade rather than cutting to the input.
    void arm();
    // beatsIntoBar must be normalised to [-h, beatsPerBar - h), where h = .5 *
    // beatsPerSample, as returned by Cutter::renderPosition().
    Step process (const Input&, double beatsIntoBar, double beatsPerBar, double beatsPerSample);

    PadProgram activeProgram() const { return active; }
    std::optional<PadProgram> pendingProgram() const { return pending; }
    // Remains engaged through an armed release and all nonzero fade samples.
    bool engaged() const { return state != State::closed; }

private:
    enum class State { closed, open, armed, fading };
    State state = State::closed;
    bool latch = false;
    long fadeSamples = 1, fadePosition = 0;
    PadProgram active = PadProgram::cut;
    std::optional<PadProgram> pending;
};

} // namespace livecut::engine
