// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/PadGate.h"
#include "AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double beatsPerSample = 120.0 / 60.0 / 48000.0;
constexpr double beatsPerBar = 4;
constexpr int beat = 24000;
constexpr int fade = 240;

struct Driver
{
    PadGate gate;
    PadGate::Input input;
    int sample = 0;

    Driver() { gate.prepare (48000); }

    PadGate::Step next()
    {
        auto position = std::fmod (sample * beatsPerSample, beatsPerBar);
        if (position >= beatsPerBar - .5 * beatsPerSample)
            position -= beatsPerBar;
        ++sample;
        return gate.process (input, position, beatsPerBar, beatsPerSample);
    }

    void until (int end)
    {
        while (sample < end)
            next();
    }
};

void checkStep (const PadGate::Step& step, float gain, bool idle)
{
    CHECK_THAT (step.gain, WithinAbs (gain, 0.0));
    CHECK (step.idle == idle);
}
} // namespace

TEST_CASE ("Touch opens immediately", "[padgate]")
{
    Driver d;
    CHECK (d.gate.activeProgram() == PadProgram::cut);
    CHECK_FALSE (d.gate.pendingProgram().has_value());
    CHECK_FALSE (d.gate.engaged());
    checkStep (d.next(), 0.f, true);
    d.until (2400);
    d.input.touch = true;
    checkStep (d.next(), 1.f, false);
    CHECK (d.gate.engaged());
}

TEST_CASE ("A quick tap cuts until the next beat, then fades over 5 ms", "[padgate]")
{
    Driver d;
    d.until (2400);
    d.input.touch = true;
    d.until (2880);
    d.input.touch = false;
    while (d.sample < beat)
        checkStep (d.next(), 1.f, false);
    for (int i = 0; i < fade; ++i)
    {
        CAPTURE (i);
        checkStep (d.next(), 1.f - static_cast<float> (i + 1) / 240.f, i == fade - 1);
        CHECK (d.gate.engaged() == (i != fade - 1));
    }
    for (int i = 0; i < beat; ++i)
        checkStep (d.next(), 0.f, true);
}

TEST_CASE ("Selecting an unavailable program leaves the active program alone", "[padgate]")
{
    Driver d;
    d.input.program = PadProgram::suction;
    auto step = d.next();
    checkStep (step, 0.f, true);
    CHECK_FALSE (step.programChanged);
    CHECK (d.gate.activeProgram() == PadProgram::cut);
    d.input.program = PadProgram::cut;
    d.input.touch = true;
    checkStep (d.next(), 1.f, false);
    d.input.program = PadProgram::suction;
    while (d.sample < beat + fade)
    {
        const int k = d.sample;
        step = d.next();
        CHECK (d.gate.activeProgram() == PadProgram::cut);
        CHECK_FALSE (d.gate.pendingProgram().has_value());
        CHECK_FALSE (step.programChanged);
        checkStep (step, k < beat ? 1.f : 1.f - static_cast<float> (k - beat + 1) / 240.f,
                   k == beat + fade - 1);
    }
}

TEST_CASE ("Bar quantize waits for the bar line", "[padgate]")
{
    Driver d;
    d.input.quantize = PadQuantize::bar;
    d.input.touch = true;
    d.until (2400);
    d.input.touch = false;
    while (d.sample < 4 * beat)
        checkStep (d.next(), 1.f, false);
    for (int i = 0; i < fade; ++i)
        checkStep (d.next(), 1.f - static_cast<float> (i + 1) / 240.f, i == fade - 1);
}

TEST_CASE ("A beat line exactly midway between two samples is caught once, on the later sample", "[padgate][padgate-boundary]")
{
    Driver d;
    const double h = .5 * beatsPerSample;
    // L = 0 keeps the exact half-sample tie free of subtraction rounding.
    d.input.touch = true;
    checkStep (d.gate.process (d.input, 4 - 5 * h, beatsPerBar, beatsPerSample), 1.f, false);
    d.input.touch = false;
    checkStep (d.gate.process (d.input, 4 - 3 * h, beatsPerBar, beatsPerSample), 1.f, false);
    d.input.program = PadProgram::bounce;
    const auto earlier = d.gate.process (d.input, -h, beatsPerBar, beatsPerSample);
    checkStep (earlier, 1.f, false);
    CHECK_FALSE (earlier.programChanged);
    const auto later = d.gate.process (d.input, h, beatsPerBar, beatsPerSample);
    checkStep (later, 1.f - 1.f / 240.f, false);
    CHECK (later.programChanged);
    CHECK (d.gate.activeProgram() == PadProgram::bounce);
    d.input.program = PadProgram::glitch;
    const auto following = d.gate.process (d.input, 3 * h, beatsPerBar, beatsPerSample);
    checkStep (following, 1.f - 2.f / 240.f, false);
    CHECK_FALSE (following.programChanged);
    CHECK (d.gate.pendingProgram() == PadProgram::glitch);
}

TEST_CASE ("A beat line a quarter sample before a sample is caught on that sample, and only once", "[padgate][padgate-boundary]")
{
    Driver d;
    const double b = beatsPerSample;
    d.input.touch = true;
    checkStep (d.gate.process (d.input, 1 - 1.75 * b, beatsPerBar, b), 1.f, false);
    d.input.touch = false;
    d.input.program = PadProgram::bounce;
    const auto earlier = d.gate.process (d.input, 1 - .75 * b, beatsPerBar, b);
    checkStep (earlier, 1.f, false);
    CHECK_FALSE (earlier.programChanged);
    const auto boundary = d.gate.process (d.input, 1 + .25 * b, beatsPerBar, b);
    checkStep (boundary, 1.f - 1.f / 240.f, false);
    CHECK (boundary.programChanged);
    CHECK (d.gate.activeProgram() == PadProgram::bounce);
    d.input.program = PadProgram::glitch;
    const auto following = d.gate.process (d.input, 1 + 1.25 * b, beatsPerBar, b);
    checkStep (following, 1.f - 2.f / 240.f, false);
    CHECK_FALSE (following.programChanged);
    CHECK (d.gate.pendingProgram() == PadProgram::glitch);
}

TEST_CASE ("A bar line reached through the normalised wrap is caught", "[padgate][padgate-boundary]")
{
    Driver d;
    const double b = beatsPerSample;
    d.input.quantize = PadQuantize::bar;
    d.input.touch = true;
    checkStep (d.gate.process (d.input, 4 - 2.25 * b, beatsPerBar, b), 1.f, false);
    d.input.touch = false;
    d.input.program = PadProgram::bounce;
    const auto earlier = d.gate.process (d.input, 4 - 1.25 * b, beatsPerBar, b);
    checkStep (earlier, 1.f, false);
    CHECK_FALSE (earlier.programChanged);
    const auto boundary = d.gate.process (d.input, -.25 * b, beatsPerBar, b);
    checkStep (boundary, 1.f - 1.f / 240.f, false);
    CHECK (boundary.programChanged);
    CHECK (d.gate.activeProgram() == PadProgram::bounce);
    d.input.program = PadProgram::glitch;
    const auto following = d.gate.process (d.input, .75 * b, beatsPerBar, b);
    checkStep (following, 1.f - 2.f / 240.f, false);
    CHECK_FALSE (following.programChanged);
    CHECK (d.gate.pendingProgram() == PadProgram::glitch);
}

TEST_CASE ("Pressing again during the fade restores full gain without going idle", "[padgate]")
{
    const int fadedSamples = GENERATE (1, 120, 239);
    Driver d;
    d.input.touch = true;
    d.until (2400);
    d.input.touch = false;
    d.until (beat + fadedSamples);
    d.input.touch = true;
    for (int remaining = fadedSamples - 1; remaining >= 0; --remaining)
    {
        checkStep (d.next(), 1.f - static_cast<float> (remaining) / 240.f, false);
        CHECK (d.gate.engaged());
    }
    while (d.sample < 2 * beat + fade)
        checkStep (d.next(), 1.f, false);
}

TEST_CASE ("Hold keeps the gate open after release, and turning Hold off releases on the next boundary", "[padgate]")
{
    Driver d;
    d.input.hold = true;
    checkStep (d.next(), 0.f, true);
    d.input.touch = true;
    d.until (2400);
    d.input.touch = false;
    while (d.sample < beat + 2400)
        checkStep (d.next(), 1.f, false);
    d.input.hold = false;
    while (d.sample < 2 * beat)
        checkStep (d.next(), 1.f, false);
    for (int i = 0; i < fade; ++i)
        checkStep (d.next(), 1.f - static_cast<float> (i + 1) / 240.f, i == fade - 1);
}

TEST_CASE ("A program switch lands on the next beat while engaged, and immediately while closed", "[padgate]")
{
    Driver d;
    d.input.program = PadProgram::bounce;
    const auto closed = d.next();
    checkStep (closed, 0.f, true);
    CHECK (closed.programChanged);
    CHECK (d.gate.activeProgram() == PadProgram::bounce);
    CHECK_FALSE (d.next().programChanged);
    d.input.touch = true;
    d.input.program = PadProgram::cut;
    const auto opened = d.next();
    checkStep (opened, 1.f, false);
    CHECK (opened.programChanged);
    d.input.program = PadProgram::glitch;
    while (d.sample < beat)
    {
        CHECK_FALSE (d.next().programChanged);
        CHECK (d.gate.activeProgram() == PadProgram::cut);
        CHECK (d.gate.pendingProgram() == PadProgram::glitch);
    }
    const auto switched = d.next();
    checkStep (switched, 1.f, false);
    CHECK (switched.programChanged);
    CHECK (d.gate.activeProgram() == PadProgram::glitch);
    CHECK_FALSE (d.gate.pendingProgram().has_value());
    CHECK_FALSE (d.next().programChanged);
}

TEST_CASE ("Programs not yet built never engage", "[padgate]")
{
    Driver d;
    d.input.program = GENERATE (PadProgram::suction, PadProgram::filter, PadProgram::tape,
                               PadProgram::loops, PadProgram::rush);
    d.input.touch = true;
    d.input.hold = true;
    d.input.alwaysEngaged = GENERATE (false, true);
    for (int k = 0; k < 4 * beat + fade; ++k)
    {
        const auto step = d.next();
        checkStep (step, 0.f, true);
        CHECK_FALSE (step.programChanged);
        CHECK_FALSE (d.gate.engaged());
        CHECK (d.gate.activeProgram() == PadProgram::cut);
    }
}

TEST_CASE ("Always engaged is open at every sample", "[padgate]")
{
    Driver d;
    d.input.alwaysEngaged = true;
    for (int k = 0; k < 4 * beat + fade; ++k)
    {
        d.input.touch = k % 2 == 0;
        d.input.hold = k % 3 == 0;
        checkStep (d.next(), 1.f, false);
    }
}

TEST_CASE ("A returning touch cancels an armed release including on the boundary", "[padgate]")
{
    Driver d;
    d.input.touch = true;
    d.until (2400);
    d.input.touch = false;
    d.until (GENERATE (12000, beat));
    d.input.touch = true;
    while (d.sample < beat + fade)
        checkStep (d.next(), 1.f, false);
}

TEST_CASE ("Release and program switching include the current boundary sample", "[padgate]")
{
    Driver d;
    d.input.quantize = GENERATE (PadQuantize::beat, PadQuantize::bar);
    d.input.touch = true;
    d.until (4 * beat);
    d.input.program = PadProgram::bounce;
    d.input.touch = false;
    const auto step = d.next();
    checkStep (step, 1.f - 1.f / 240.f, false);
    CHECK (step.programChanged);
    CHECK (d.gate.activeProgram() == PadProgram::bounce);
}

TEST_CASE ("Pending programs follow the current selection and bar quantize", "[padgate]")
{
    Driver d;
    d.input.touch = true;
    d.input.quantize = PadQuantize::bar;
    d.until (2400);
    d.input.program = PadProgram::bounce;
    d.next();
    CHECK (d.gate.pendingProgram() == PadProgram::bounce);
    d.input.program = PadProgram::cut;
    d.next();
    CHECK_FALSE (d.gate.pendingProgram().has_value());
    d.input.program = PadProgram::bounce;
    d.next();
    d.input.program = PadProgram::suction;
    d.next();
    CHECK_FALSE (d.gate.pendingProgram().has_value());
    d.input.program = PadProgram::glitch;
    while (d.sample < 4 * beat)
    {
        CHECK_FALSE (d.next().programChanged);
        CHECK (d.gate.activeProgram() == PadProgram::cut);
        CHECK (d.gate.pendingProgram() == PadProgram::glitch);
    }
    CHECK (d.next().programChanged);
    CHECK (d.gate.activeProgram() == PadProgram::glitch);
}

TEST_CASE ("Reset clears latch, release and pending switch while preserving the active program", "[padgate]")
{
    Driver d;
    d.input.touch = true;
    d.input.hold = true;
    d.input.program = PadProgram::bounce;
    d.until (2400);
    d.input.program = PadProgram::glitch;
    d.input.touch = false;
    d.next();
    SECTION ("Latched with a pending program") {}
    SECTION ("Armed")
    {
        d.input.hold = false;
        d.next();
    }
    SECTION ("Fading")
    {
        d.input.hold = false;
        d.until (beat + 120);
    }
    const auto active = d.gate.activeProgram();
    d.gate.reset();
    CHECK (d.gate.activeProgram() == active);
    CHECK_FALSE (d.gate.engaged());
    CHECK_FALSE (d.gate.pendingProgram().has_value());
    d.input.program = active;
    d.input.hold = true;
    checkStep (d.next(), 0.f, true);
    d.input.touch = true;
    checkStep (d.next(), 1.f, false);
}

TEST_CASE ("Pad gate lifecycle and processing allocate nothing", "[padgate]")
{
    std::size_t allocations = 0;
    {
        testing::AllocationCounter counter;
        Driver d;
        d.input.touch = true;
        d.until (2400);
        d.input.program = PadProgram::bounce;
        d.until (beat + 1);
        d.input.touch = false;
        d.until (2 * beat + 120);
        d.input.touch = true;
        d.until (2 * beat + fade);
        d.input.touch = false;
        d.until (3 * beat + fade);
        d.gate.reset();
        allocations = counter.count();
    }
    CHECK (allocations == 0);
}
