// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/PadGate.h"

#include <cassert>
#include <cmath>

namespace livecut::engine
{

void PadGate::prepare (double sampleRate)
{
    assert (std::isfinite (sampleRate) && sampleRate > 0.0);
    fadeSamples = std::lround (0.005 * sampleRate);
    assert (fadeSamples > 0);
    reset();
}

void PadGate::reset()
{
    state = State::closed;
    latch = false;
    fadePosition = 0;
    pending.reset();
}

PadGate::Step PadGate::process (const Input& input, double beatsIntoBar,
                               double /*beatsPerBar*/, double beatsPerSample)
{
    Step step;
    const bool available = isAvailable (input.program);
    latch = input.hold && (input.touch || latch);
    const bool want = available && (input.alwaysEngaged || input.touch || latch);
    const double h = .5 * beatsPerSample;
    const bool boundary = input.quantize == PadQuantize::beat
        ? std::ceil (beatsIntoBar - h) < beatsIntoBar + h
        : beatsIntoBar < h;

    // Only the current selection may be pending; reselection cancels an older request.
    pending.reset();
    if (available && input.program != active)
    {
        if (state == State::closed || boundary)
        {
            active = input.program;
            step.programChanged = true;
        }
        else
            pending = input.program;
    }

    if (state == State::closed)
    {
        if (! want)
            return step;

        state = State::open;
        fadePosition = 0;
    }

    if (want)
    {
        state = State::open;
        if (fadePosition > 0)
            --fadePosition;
    }
    else
    {
        if (state == State::open)
            state = State::armed;
        if (state == State::armed && boundary)
            state = State::fading;
        if (state == State::fading)
        {
            ++fadePosition;
            if (fadePosition == fadeSamples)
            {
                state = State::closed;
                pending.reset();
                return step;
            }
        }
    }

    step.gain = 1.f - static_cast<float> (fadePosition) / static_cast<float> (fadeSamples);
    step.idle = false;
    return step;
}

} // namespace livecut::engine
