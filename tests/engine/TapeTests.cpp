// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Cutter.h"
#include "engine/Fracture.h"
#include "engine/LivePlayer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <optional>
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

namespace
{
constexpr double spinUp = 0.125;

FractureParameters::Tape stops (double stopBeats, double density)
{
    FractureParameters::Tape tape;
    tape.layout = FractureParameters::Tape::Layout::stops;
    tape.stopBeats = static_cast<float> (stopBeats);
    tape.stopDensity = static_cast<float> (density);
    return tape;
}

// One laid-out cut, in beats from the start of its bar.
struct Placed
{
    TapeMotion motion;
    double start, end;
};

struct Bar
{
    std::vector<Placed> cuts;
    bool endsInStop = false;
};

Bar layOut (const FractureParameters::Tape& tape, std::int64_t bar, double fromBeat, bool spinUpFirst,
            double beatsPerBar = 4.0, int unitsPerBar = 16, std::uint32_t seed = 606)
{
    const auto unitsPerBeat = unitsPerBar / beatsPerBar;
    const auto blockUnits = (beatsPerBar - fromBeat) * unitsPerBeat;
    std::vector<Cut> cuts;
    cuts.reserve (maxCutsPerBlock);
    Bar result;
    result.endsInStop = layOutTapeStops (tape, seed, bar, fromBeat, beatsPerBar, unitsPerBeat, blockUnits, spinUpFirst, cuts);
    REQUIRE (! cuts.empty());
    REQUIRE (cuts.size() <= maxCutsPerBlock);
    CHECK_THAT (totalUnits (cuts), WithinAbs (blockUnits, 1e-9));
    auto at = fromBeat;
    for (const auto& cut : cuts)
    {
        REQUIRE (cut.units > 0.0);
        CHECK (cut.tape != TapeMotion::cycle);
        CHECK (cut.gate == 1.0);
        CHECK (cut.pan == 0.f);
        CHECK (cut.amp == 1.f);
        CHECK (cut.cents == 0.f);
        result.cuts.push_back ({ cut.tape, at, at + cut.units / unitsPerBeat });
        at += cut.units / unitsPerBeat;
    }
    return result;
}

std::vector<Placed> stopsIn (const Bar& bar)
{
    std::vector<Placed> found;
    for (const auto& cut : bar.cuts)
        if (cut.motion == TapeMotion::stop)
            found.push_back (cut);
    return found;
}
} // namespace

TEST_CASE ("At the bottom of Y, only the last bar of every four stops", "[tape][layout]")
{
    for (const auto length : { .5, 1.0, 2.0, 4.0 })
    {
        CAPTURE (length);
        bool spinUpFirst = false;
        for (std::int64_t bar = -8; bar < 64; ++bar)
        {
            CAPTURE (bar);
            const auto laid = layOut (stops (length, 0.0), bar, 0.0, spinUpFirst);
            const auto found = stopsIn (laid);
            const auto last = ((bar + 1) % 4 + 4) % 4 == 0;
            REQUIRE (found.size() == (last ? 1u : 0u));
            if (last)
            {
                CHECK_THAT (found[0].end, WithinAbs (4.0, 1e-12));
                CHECK_THAT (found[0].start, WithinAbs (4.0 - length, 1e-12));
            }
            // The bar after a stop opens with the spin-up, and only that bar.
            CHECK ((laid.cuts.front().motion == TapeMotion::spinUp) == spinUpFirst);
            if (spinUpFirst)
                CHECK_THAT (laid.cuts.front().end, WithinAbs (spinUp, 1e-12));
            CHECK (laid.endsInStop == last);
            spinUpFirst = laid.endsInStop;
        }
    }
}

TEST_CASE ("At the top of Y, every beat brakes for its second half", "[tape][layout]")
{
    for (const auto length : { .5, 1.0, 2.0, 4.0 })
    {
        CAPTURE (length);
        const auto laid = layOut (stops (length, 4.0), 9, 0.0, true);
        REQUIRE (laid.cuts.size() == 4 * 3);
        for (int beat = 0; beat < 4; ++beat)
        {
            CAPTURE (beat);
            const auto& spin = laid.cuts[static_cast<std::size_t> (3 * beat)];
            const auto& play = laid.cuts[static_cast<std::size_t> (3 * beat + 1)];
            const auto& stop = laid.cuts[static_cast<std::size_t> (3 * beat + 2)];
            CHECK (spin.motion == TapeMotion::spinUp);
            CHECK (play.motion == TapeMotion::play);
            CHECK (stop.motion == TapeMotion::stop);
            CHECK_THAT (spin.end, WithinAbs (beat + spinUp, 1e-12));
            CHECK_THAT (stop.start, WithinAbs (beat + .5, 1e-12));
            CHECK_THAT (stop.end, WithinAbs (beat + 1.0, 1e-12));
        }
        CHECK (laid.endsInStop);
    }
}

TEST_CASE ("A stop is X long, but never longer than its bar or half the gap before it", "[tape][layout]")
{
    for (const auto length : { .5, 1.0, 2.0, 4.0 })
    {
        CAPTURE (length);
        // Every bar: half a bar back to the last stop's end.
        for (std::int64_t bar = 0; bar < 8; ++bar)
        {
            const auto found = stopsIn (layOut (stops (length, 2.0), bar, 0.0, true));
            REQUIRE (found.size() == 1);
            CHECK_THAT (found[0].end - found[0].start, WithinAbs (std::min (length, 2.0), 1e-12));
            CHECK_THAT (tapeStopBeats (stops (length, 2.0), 606, bar, 4.0, 4.0), WithinAbs (std::min (length, 2.0), 1e-12));
        }
        // Every two bars: the gap is two bars, so only X and the bar cap it.
        for (std::int64_t bar = 1; bar < 16; bar += 2)
            CHECK_THAT (tapeStopBeats (stops (length, 1.0), 606, bar, 4.0, 4.0), WithinAbs (length, 1e-12));
        // In 3/4, a four-beat stop is a whole bar.
        CHECK_THAT (tapeStopBeats (stops (length, 0.0), 606, 3, 3.0, 3.0), WithinAbs (std::min (length, 3.0), 1e-12));
    }
}

TEST_CASE ("Every layout keeps the block, the grid and its spin-ups", "[tape][layout]")
{
    for (const auto beatsPerBar : { 4.0, 3.0, 3.5, 2.0, 7.0 })
        for (const auto density : { 0.0, .3, 1.0, 1.5, 2.0, 2.7, 3.2, 3.9, 4.0 })
            for (const auto length : { .5, 1.0, 2.0, 4.0 })
            {
                CAPTURE (beatsPerBar, density, length);
                bool spinUpFirst = false;
                for (std::int64_t bar = 0; bar < 24; ++bar)
                {
                    CAPTURE (bar);
                    const auto laid = layOut (stops (length, density), bar, 0.0, spinUpFirst, beatsPerBar);
                    for (std::size_t i = 0; i < laid.cuts.size(); ++i)
                    {
                        const auto& cut = laid.cuts[i];
                        CAPTURE (i);
                        // Straight needs nothing more: every boundary is a whole 64th of a beat.
                        CHECK_THAT (cut.start * 64.0, WithinAbs (std::round (cut.start * 64.0), 1e-9));
                        if (cut.motion == TapeMotion::stop)
                        {
                            const auto onBeat = std::abs (cut.end - std::round (cut.end)) < 1e-9;
                            const auto onBar = std::abs (cut.end - beatsPerBar) < 1e-9;
                            CHECK ((onBeat || onBar));
                            CHECK (cut.end - cut.start <= length + 1e-12);
                            CHECK (tapeStopEnds (606, density, bar, std::abs (cut.end - beatsPerBar) < 1e-9 ? beatsPerBar : std::round (cut.end), beatsPerBar));
                            if (i + 1 < laid.cuts.size())
                            {
                                CHECK (laid.cuts[i + 1].motion == TapeMotion::spinUp);
                                CHECK_THAT (laid.cuts[i + 1].end - laid.cuts[i + 1].start, WithinAbs (spinUp, 1e-9));
                            }
                            else
                                CHECK (laid.endsInStop);
                        }
                        if (cut.motion == TapeMotion::spinUp)
                            CHECK ((i == 0 ? spinUpFirst : laid.cuts[i - 1].motion == TapeMotion::stop));
                    }
                    spinUpFirst = laid.endsInStop;
                }
            }
}

TEST_CASE ("Raising Y only adds stops, on fixed bars", "[tape][layout]")
{
    const auto ends = [] (double density, std::int64_t bar)
    {
        std::vector<double> found;
        for (int beat = 1; beat <= 4; ++beat)
            if (tapeStopEnds (606, density, bar, beat, 4.0))
                found.push_back (beat);
        return found;
    };
    int added = 0;
    for (std::int64_t bar = 0; bar < 64; ++bar)
        for (double density = 0.0; density < 4.0; density += .25)
        {
            const auto lower = ends (density, bar), higher = ends (density + .25, bar);
            CHECK (std::includes (higher.begin(), higher.end(), lower.begin(), lower.end()));
            added += static_cast<int> (higher.size() - lower.size());
        }
    CHECK (added == 64 * 4 - 16); // every beat of 64 bars, less the 16 ends that always stop
    // Halfway to every bar, the 32 odd bar lines come in on some bars and not others.
    int some = 0;
    for (std::int64_t bar = 0; bar < 64; ++bar)
        some += tapeStopEnds (606, 1.5, bar, 4.0, 4.0) ? 1 : 0;
    CAPTURE (some);
    CHECK (some > 32 + 4);
    CHECK (some < 64 - 4);
}

TEST_CASE ("Engaging mid-bar gives the bar's own stops from the first one that fits", "[tape][layout]")
{
    for (const auto density : { 2.5, 3.5, 4.0 })
        for (const auto length : { .5, 1.0, 2.0 })
            for (int sixteenth = 1; sixteenth < 16; ++sixteenth)
            {
                const auto from = sixteenth / 4.0;
                CAPTURE (density, length, from);
                const auto whole = stopsIn (layOut (stops (length, density), 5, 0.0, true));
                const auto late = layOut (stops (length, density), 5, from, false);
                CHECK (late.cuts.front().motion != TapeMotion::spinUp); // an engage starts on the track
                std::vector<Placed> expected;
                for (const auto& stop : whole)
                    if (stop.start >= from - 1e-12)
                        expected.push_back (stop);
                const auto found = stopsIn (late);
                REQUIRE (found.size() == expected.size());
                for (std::size_t i = 0; i < found.size(); ++i)
                {
                    CHECK_THAT (found[i].start, WithinAbs (expected[i].start, 1e-9));
                    CHECK_THAT (found[i].end, WithinAbs (expected[i].end, 1e-9));
                }
            }
}

TEST_CASE ("Stop levels follow the host's bars and beats", "[tape][layout]")
{
    CHECK (tapeStopLevel (3, 4.0, 4.0) == 0);
    CHECK (tapeStopLevel (7, 4.0, 4.0) == 0);
    CHECK (tapeStopLevel (-1, 4.0, 4.0) == 0); // a count-in bar before bar 1
    CHECK (tapeStopLevel (1, 4.0, 4.0) == 1);
    CHECK (tapeStopLevel (-3, 4.0, 4.0) == 1);
    CHECK (tapeStopLevel (0, 4.0, 4.0) == 2);
    CHECK (tapeStopLevel (2, 4.0, 4.0) == 2);
    CHECK (tapeStopLevel (2, 2.0, 4.0) == 3);
    CHECK (tapeStopLevel (2, 1.0, 4.0) == 4);
    CHECK (tapeStopLevel (2, 3.0, 4.0) == 4);
    CHECK (tapeStopLevel (3, 3.5, 3.5) == 0); // 7/8's bar line
    CHECK (tapeStopLevel (3, 3.0, 3.5) == 4);
}

namespace
{
struct Bounds final : Cutter::SourceContext
{
    std::optional<Cutter::TransportBounds> sourceBoundsAt (std::int64_t frame) const override
    {
        if (frame < 0)
            return std::nullopt;
        return Cutter::TransportBounds { 7, 0, std::nullopt };
    }
};

// The pad's Tape through a Cutter at 120 bpm in 4/4, fed a block at a time.
struct TapeCutter
{
    Cutter cutter;
    OnsetDetector detector;
    Bounds bounds;
    double rate;
    std::int64_t frame = 0;

    explicit TapeCutter (const FractureParameters::Tape& tape, double sampleRate = 48000.0)
        : rate (sampleRate)
    {
        cutter.prepare (rate);
        cutter.setUnitsPerBar (16);
        cutter.setSeed (606);
        CutParameters cut;
        cut.minPhraseBars = 1;
        cut.maxPhraseBars = 2;
        cutter.setParameters (cut);
        FractureParameters fracture;
        fracture.procedure = FractureProcedure::tapeCut;
        fracture.tape = tape;
        cutter.setFracture (fracture);
        cutter.setTransformSource (detector, bounds);
    }

    void process (float* left, float* right, int count)
    {
        const auto beats = static_cast<double> (frame) * 2.0 / rate;
        const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
        cutter.process (left, right, count, { 120.0, 4.0 }, { bar, beats - 4.0 * static_cast<double> (bar) });
        frame += count;
    }

    // Runs silence up to `until`, one frame at a time.
    void runTo (std::int64_t until)
    {
        while (frame < until)
        {
            float left = 0.f, right = 0.f;
            process (&left, &right, 1);
        }
    }
};
} // namespace

namespace
{
// The motion of the cut that rendered the last frame. At a cut's last frame the
// player has already moved on to the next cut, at age 0.
TapeMotion renderedMotion (const Cutter& cutter)
{
    const auto cut = cutter.player().currentCut();
    const auto cuts = cutter.currentCuts();
    if (cutter.player().currentCutAge() == 0 && cut > 0)
        return cuts[cut - 1].tape;
    return cut < cuts.size() ? cuts[cut].tape : TapeMotion::play;
}
} // namespace

TEST_CASE ("The bar after a stop opens with its spin-up, and a restart forgets it", "[tape][cutter]")
{
    constexpr std::int64_t bar = 96000;
    TapeCutter played (stops (1.0, 0.0));
    played.runTo (4 * bar + 1); // through bar 3's stop onto bar 4's first sample
    REQUIRE (played.cutter.isCutting());
    REQUIRE (played.cutter.currentCuts().front().tape == TapeMotion::spinUp);
    CHECK (played.cutter.error() == Cutter::Error::none);

    // Going idle and back drops the phrase: the next start is on the track.
    TapeCutter restarted (stops (1.0, 0.0));
    restarted.runTo (4 * bar - 10);
    restarted.cutter.setIdle (true);
    restarted.runTo (4 * bar - 5);
    restarted.cutter.setIdle (false);
    restarted.runTo (4 * bar + 1);
    REQUIRE (restarted.cutter.isCutting());
    CHECK (restarted.cutter.currentCuts().front().tape == TapeMotion::play);

    // A Classic Tape page never gets one.
    FractureParameters::Tape classic;
    classic.stopDensity = 4.f;
    TapeCutter page (classic);
    page.runTo (4 * bar + 1);
    for (const auto& cut : page.cutter.currentCuts())
        CHECK (cut.tape == TapeMotion::cycle);
}

TEST_CASE ("Tape plays the input exactly outside its stops and spin-ups", "[tape][cutter]")
{
    for (const auto density : { 0.0, 2.0, 4.0 })
    {
        CAPTURE (density);
        TapeCutter tape (stops (1.0, density));
        int stopped = 0, compared = 0;
        // One frame at a time, so each output knows the cut it came from.
        while (tape.frame < 8 * 96000)
        {
            const auto in = tone (tape.frame);
            auto left = in.left, right = in.right;
            tape.process (&left, &right, 1);
            const auto motion = renderedMotion (tape.cutter);
            if (motion == TapeMotion::play && tape.cutter.isCutting())
            {
                ++compared;
                REQUIRE (left == in.left);
                REQUIRE (right == in.right);
            }
            stopped += motion == TapeMotion::stop ? 1 : 0;
            REQUIRE (tape.cutter.fractureDiagnostics().state != FractureState::sourceLost);
        }
        CHECK (tape.cutter.error() == Cutter::Error::none);
        CHECK (compared > 3 * 96000);
        CHECK (stopped > 0);
    }
}
