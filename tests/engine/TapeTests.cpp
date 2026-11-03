// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Fracture.h"
#include "engine/LivePlayer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double readerRate = 48000.0;

// A causal reader rig: each frame is recorded before the reader renders it, as
// in the cutter, so a read ahead of the input fails as a lost source.
struct Rig
{
    LivePlayer player;
    FractureReader reader;
    std::int64_t recorded = 0;
    std::uint64_t epoch = 0;

    Rig()
    {
        player.prepare (96000, 400000);
        epoch = player.sourceAtFrame (0).epoch;
        reader.prepare (readerRate);
        FractureParameters p;
        p.procedure = FractureProcedure::tapeCut;
        p.tape.startSpeed = .3f; // a cycle's speeds, which a motion must not use
        p.tape.endSpeed = .9f;
        p.tape.alternate = true;
        reader.setParameters (p);
    }

    // Starts a motion on the next frame to be recorded, reading from that frame.
    void start (TapeMotion motion, int frames)
    {
        FractureCutContext context;
        context.cut.tape = motion;
        context.source = { epoch, recorded, std::numeric_limits<std::int64_t>::max() - recorded };
        context.outputFrames = frames;
        context.fadeFrames = 96; // a 2 ms fade a motion must ignore
        context.ordinal = 1;     // odd, so Alternate would swap a cycle's speeds
        reader.startCut (context);
    }

    void record (StereoFrame in)
    {
        REQUIRE (player.ingestFrame (in.left, in.right));
        ++recorded;
    }

    // Records `in` as the next frame, then renders the motion's frame `age`.
    StereoFrame play (StereoFrame in, int age, int frames)
    {
        record (in);
        StereoFrame out;
        reader.process (player, age, frames, out.left, out.right);
        REQUIRE (reader.diagnostics().state == FractureState::playing);
        return out;
    }
};

StereoFrame tone (std::int64_t frame)
{
    const auto t = static_cast<double> (frame);
    return { static_cast<float> (.6 * std::sin (.031 * t) + .2 * std::sin (.0047 * t)),
             static_cast<float> (.5 * std::cos (.023 * t)) };
}
} // namespace

TEST_CASE ("A play motion is the input, with no fade", "[tape][motion]")
{
    Rig rig;
    for (int i = 0; i < 777; ++i)
        rig.record (tone (rig.recorded)); // history before the cut
    for (int cut = 0; cut < 3; ++cut)
    {
        rig.start (TapeMotion::play, 2000);
        for (int age = 0; age < 2000; ++age)
        {
            const auto in = tone (rig.recorded);
            const auto out = rig.play (in, age, 2000);
            REQUIRE (out.left == in.left);
            REQUIRE (out.right == in.right);
            REQUIRE (rig.reader.diagnostics().tapeSpeed == 1);
        }
    }
}

TEST_CASE ("A stop starts on the track, brakes as 1 - u squared and ends in silence", "[tape][motion]")
{
    constexpr int frames = 12000;
    SECTION ("Its gain is the square root of its speed, with no fade")
    {
        Rig rig;
        rig.start (TapeMotion::stop, frames);
        for (int age = 0; age < frames; ++age)
        {
            const auto out = rig.play ({ .5f, -.25f }, age, frames);
            const auto u = static_cast<double> (age) / (frames - 1);
            REQUIRE_THAT (out.left, WithinAbs (.5 * std::sqrt (1 - u * u), 1e-6));
            REQUIRE_THAT (out.right, WithinAbs (-.25 * std::sqrt (1 - u * u), 1e-6));
        }
    }
    SECTION ("It reads from its own first frame along the brake")
    {
        // A ramp input turns each output sample into the frame it read.
        Rig rig;
        for (int i = 0; i < 5000; ++i)
            rig.record ({ static_cast<float> (rig.recorded) * 1e-4f, 0.f });
        const auto first = rig.recorded;
        rig.start (TapeMotion::stop, frames);
        TapeTrajectory brake;
        brake.start (1, 0, frames, FractureParameters::Tape::Curve::quadratic);
        for (int age = 0; age < frames - 1; ++age)
        {
            const auto out = rig.play ({ static_cast<float> (rig.recorded) * 1e-4f, 0.f }, age, frames);
            const auto gain = std::sqrt (brake.speed (age));
            if (gain >= .2) // dividing out a smaller gain magnifies the float ramp's rounding
                REQUIRE_THAT (out.left / gain * 1e4, WithinAbs (static_cast<double> (first) + brake.position (age), 0.02));
        }
        const auto last = rig.play ({ 1.f, 1.f }, frames - 1, frames);
        CHECK (last.left == 0.f);
        CHECK (last.right == 0.f);
    }
}

TEST_CASE ("A spin-up winds up from silence and hands back to the input exactly", "[tape][motion]")
{
    for (const int frames : { 3000, 6000, 2756 })
    {
        CAPTURE (frames);
        Rig rig;
        for (int i = 0; i < 4321; ++i)
            rig.record (tone (rig.recorded));
        rig.start (TapeMotion::spinUp, frames);
        StereoFrame previous {}, previousIn {};
        for (int age = 0; age < frames; ++age)
        {
            previousIn = tone (rig.recorded);
            previous = rig.play (previousIn, age, frames);
            if (age == 0)
            {
                REQUIRE (previous.left == 0.f); // still, so silent
                REQUIRE (previous.right == 0.f);
            }
            REQUIRE (rig.reader.diagnostics().tapeSpeed <= 1.25 + 1e-9);
        }
        // Its last frame is on the track, and the next cut continues it exactly.
        CHECK_THAT (previous.left, WithinAbs (previousIn.left, 1e-4));
        CHECK_THAT (previous.right, WithinAbs (previousIn.right, 1e-4));
        rig.start (TapeMotion::play, 500);
        for (int age = 0; age < 500; ++age)
        {
            const auto in = tone (rig.recorded);
            const auto out = rig.play (in, age, 500);
            REQUIRE (out.left == in.left);
            REQUIRE (out.right == in.right);
        }
    }
}

TEST_CASE ("A spin-up never lifts the level", "[tape][motion]")
{
    // On a steady level its gain is the square root of its speed, held at 1 through the overshoot.
    constexpr int frames = 3000;
    Rig rig;
    rig.start (TapeMotion::spinUp, frames);
    TapeTrajectory spin;
    spin.start (0, 1, frames, FractureParameters::Tape::Curve::catchUp);
    for (int age = 0; age < frames; ++age)
    {
        const auto out = rig.play ({ .5f, .5f }, age, frames);
        REQUIRE_THAT (out.left, WithinAbs (.5 * std::sqrt (std::min (spin.speed (age), 1.0)), 1e-6));
        REQUIRE (out.left <= .5f);
    }
}

TEST_CASE ("A tempo change re-times a play and a stop by their own curves, and leaves a spin-up alone", "[tape][motion]")
{
    SECTION ("A play stays on the track")
    {
        Rig rig;
        rig.start (TapeMotion::play, 2000);
        for (int age = 0; age < 3000; ++age)
        {
            if (age == 700)
                rig.reader.retimeTape (age, 2300);
            const auto in = tone (rig.recorded);
            const auto out = rig.play (in, age, 3000);
            REQUIRE (out.left == in.left);
        }
    }
    SECTION ("A stop brakes from where it is to 0 at its new end")
    {
        Rig rig;
        rig.start (TapeMotion::stop, 4000);
        double speed = 1.0;
        for (int age = 0; age < 6000; ++age)
        {
            if (age == 1000)
                rig.reader.retimeTape (age, 5000);
            rig.play ({ .5f, .5f }, age, 6000);
            const auto next = rig.reader.diagnostics().tapeSpeed;
            REQUIRE (next <= speed);
            REQUIRE (speed - next < 1e-3); // continuous through the re-time
            speed = next;
        }
        CHECK (speed == 0);
    }
    SECTION ("A spin-up still lands after its frames and plays on")
    {
        Rig rig;
        rig.start (TapeMotion::spinUp, 3000);
        StereoFrame in {}, out {};
        for (int age = 0; age < 4000; ++age)
        {
            if (age == 1000)
                rig.reader.retimeTape (age, 3000);
            in = tone (rig.recorded);
            out = rig.play (in, age, 4000);
            if (age >= 3000)
                REQUIRE (out.left == in.left);
        }
    }
}
