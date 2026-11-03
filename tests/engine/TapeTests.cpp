// SPDX-License-Identifier: GPL-3.0-or-later

#include "DrumLoop.h"
#include "engine/Cutter.h"
#include "engine/Fracture.h"
#include "engine/LivePlayer.h"
#include "engine/PadPrograms.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <optional>
#include <utility>
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

// The pad's Tape through a Cutter, at 120 bpm in 4/4 unless told otherwise, fed
// a block at a time. The host's position is counted from the last tempo change.
struct TapeCutter
{
    Cutter cutter;
    OnsetDetector detector;
    Bounds bounds;
    double rate;
    std::int64_t frame = 0;
    double bpm = 120.0, beatsPerBar = 4.0;
    std::int64_t tempoFrame = 0;
    double tempoBeats = 0.0;

    explicit TapeCutter (const FractureParameters::Tape& tape, double sampleRate = 48000.0, double tempo = 120.0,
                         double meter = 4.0)
        : rate (sampleRate), bpm (tempo), beatsPerBar (meter)
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

    double beats() const
    {
        return tempoBeats + static_cast<double> (frame - tempoFrame) * bpm / 60.0 / rate;
    }

    void setTempo (double tempo)
    {
        tempoBeats = beats();
        tempoFrame = frame;
        bpm = tempo;
    }

    void process (float* left, float* right, int count)
    {
        const auto at = beats();
        const auto bar = static_cast<std::int64_t> (std::floor (at / beatsPerBar));
        cutter.process (left, right, count, { bpm, beatsPerBar }, { bar, at - beatsPerBar * static_cast<double> (bar) });
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

namespace
{
using testing::DrumLoop;

// Tape at a pad spot: X picks the stop length in four steps, Y the density.
FractureParameters::Tape padSpot (double x, double y)
{
    constexpr double lengths[] { .5, 1.0, 2.0, 4.0 };
    return stops (lengths[std::min (static_cast<int> (4.0 * x), 3)], 4.0 * y);
}

struct DrumRun
{
    std::vector<float> in, left, right;
    std::vector<TapeMotion> motion;
    std::vector<double> speed;
};

// Eight bars of the loop through Tape engaged from the first sample, one frame at a time.
DrumRun runTape (const FractureParameters::Tape& tape, const DrumLoop& loop, int frames = 8 * 96000)
{
    TapeCutter cutter (tape);
    DrumRun run;
    for (auto* v : { &run.in, &run.left, &run.right })
        v->reserve (static_cast<std::size_t> (frames));
    while (cutter.frame < frames)
    {
        const auto in = loop.samples[static_cast<std::size_t> (cutter.frame) % loop.samples.size()];
        auto left = in, right = in;
        cutter.process (&left, &right, 1);
        run.in.push_back (in);
        run.left.push_back (left);
        run.right.push_back (right);
        run.motion.push_back (renderedMotion (cutter.cutter));
        run.speed.push_back (cutter.cutter.fractureDiagnostics().tapeSpeed);
    }
    REQUIRE (cutter.cutter.error() == Cutter::Error::none);
    return run;
}

// [first, end) of each run of `motion`.
std::vector<std::pair<std::size_t, std::size_t>> spans (const DrumRun& run, TapeMotion motion)
{
    std::vector<std::pair<std::size_t, std::size_t>> found;
    for (std::size_t i = 0; i < run.motion.size(); ++i)
        if (run.motion[i] == motion && (i == 0 || run.motion[i - 1] != motion))
        {
            auto end = i;
            while (end < run.motion.size() && run.motion[end] == motion)
                ++end;
            found.emplace_back (i, end);
        }
    return found;
}

float largestStep (const std::vector<float>& signal, std::size_t from, std::size_t to)
{
    float largest = 0.f;
    for (auto i = std::max<std::size_t> (from, 1); i < to; ++i)
        largest = std::max (largest, std::abs (signal[i] - signal[i - 1]));
    return largest;
}

double rms (const std::vector<float>& left, const std::vector<float>& right)
{
    double energy = 0.0;
    for (std::size_t i = 0; i < left.size(); ++i)
        energy += static_cast<double> (left[i]) * left[i] + static_cast<double> (right[i]) * right[i];
    return std::sqrt (energy / static_cast<double> (2 * left.size()));
}

// Cut at its middle, as the Filter's level test compares against.
double cutRms (const DrumLoop& loop, int frames = 8 * 96000)
{
    const auto mapping = mapPad (PadProgram::cut, .5f, 0.f, {});
    Cutter cutter;
    cutter.prepare (48000.0);
    cutter.setProcedure (mapping.procedure);
    cutter.setUnitsPerBar (mapping.unitsPerBar);
    cutter.setSeed (mapping.seed);
    cutter.setParameters (mapping.cut);
    std::vector<float> left, right;
    std::array<float, 512> l {}, r {};
    for (int n = 0; n < frames; n += 512)
    {
        for (int i = 0; i < 512; ++i)
            l[static_cast<std::size_t> (i)] = r[static_cast<std::size_t> (i)]
                = loop.samples[static_cast<std::size_t> (n + i) % loop.samples.size()];
        const auto beats = n / 24000.0;
        const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
        cutter.process (l.data(), r.data(), 512, { 120.0, 4.0 }, { bar, beats - 4.0 * static_cast<double> (bar) });
        left.insert (left.end(), l.begin(), l.end());
        right.insert (right.end(), r.begin(), r.end());
    }
    REQUIRE (cutter.error() == Cutter::Error::none);
    return rms (left, right);
}
} // namespace

TEST_CASE ("On drums, Tape is the track between stops, and its stops and spin-ups are clean", "[tape][drums]")
{
    const DrumLoop drums (0.0, DrumLoop::Bass::sine); // no bass: drums alone
    const auto drumStep = largestStep (drums.samples, 0, drums.samples.size());
    for (const auto x : { 0.0, .5, 1.0 })
        for (const auto y : { 0.0, .5, 1.0 })
        {
            CAPTURE (x, y);
            const auto run = runTape (padSpot (x, y), drums);
            for (std::size_t i = 0; i < run.in.size(); ++i)
                if (run.motion[i] == TapeMotion::play)
                {
                    REQUIRE (run.left[i] == run.in[i]);
                    REQUIRE (run.right[i] == run.in[i]);
                }
            const auto stopSpans = spans (run, TapeMotion::stop);
            const auto spinSpans = spans (run, TapeMotion::spinUp);
            REQUIRE (! stopSpans.empty());
            // The tail's largest step in its last 5 ms; each spin-up's largest step
            // against the hit it plays, and in its first millisecond; and how far
            // its last frame is from the input it hands back to.
            float tailStep = 0.f, spinStep = 0.f, landing = 0.f, onset = 0.f;
            double slowEnergy = 0.0, stopEnergy = 0.0;
            for (const auto& [first, end] : stopSpans)
            {
                CAPTURE (first, end);
                REQUIRE (run.left[end - 1] == 0.f); // silent on its grid point
                REQUIRE (end % 3000 == 0);         // which is a whole 32nd note
                tailStep = std::max (tailStep, largestStep (run.left, end - 240, end));
                for (auto i = first; i < end; ++i)
                {
                    const auto energy = static_cast<double> (run.left[i]) * run.left[i];
                    stopEnergy += energy;
                    slowEnergy += run.speed[i] < .25 ? energy : 0.0;
                }
            }
            for (const auto& [first, end] : spinSpans)
            {
                CAPTURE (first, end);
                REQUIRE (run.left[first] == 0.f); // from silence
                REQUIRE (end - first == 3000);   // a 32nd note
                spinStep = std::max (spinStep, largestStep (run.left, first, end) / std::max (largestStep (run.in, first, end), 1e-6f));
                onset = std::max (onset, largestStep (run.left, first, first + 48));
                if (end < run.in.size())
                    landing = std::max (landing, std::abs (run.left[end - 1] - run.in[end - 1]));
            }
            CAPTURE (drumStep, tailStep, spinStep, onset, landing, slowEnergy / stopEnergy);
            // Measured: tails 0.0041 at most (0.34% of the drums' largest step), spin-ups
            // 0.77 of the steps of the hits they play, onsets 0.016 in their first
            // millisecond, landings 3e-8 off the track, and 1.5% of a stop's energy
            // below quarter speed.
            CHECK (tailStep <= .005f * drumStep);
            CHECK (spinStep <= 1.f);
            CHECK (onset <= .02f);
            CHECK (landing <= 1e-6f);
            CHECK (slowEnergy <= .02 * stopEnergy);
        }
}

TEST_CASE ("On drums and bass, Tape is never louder than Cut or the input", "[tape][drums][level]")
{
    const auto [bassHz, bass, name] = GENERATE (table<double, DrumLoop::Bass, const char*> ({
        { 0.0, DrumLoop::Bass::sine, "drums" },
        { 55.0, DrumLoop::Bass::sine, "sub-bass" },
        { 150.0, DrumLoop::Bass::sine, "a bass line" },
    }));
    CAPTURE (name);
    const DrumLoop loop (bassHz, bass);
    const auto cut = cutRms (loop);
    const auto peak = *std::max_element (loop.samples.begin(), loop.samples.end(), [] (float a, float b) { return std::abs (a) < std::abs (b); });
    for (const auto x : { 0.0, .5, 1.0 })
        for (const auto y : { 0.0, .5, 1.0 })
        {
            const auto run = runTape (padSpot (x, y), loop);
            const auto input = rms (run.in, run.in);
            const auto tape = rms (run.left, run.right);
            const auto overCut = 20.0 * std::log10 (tape / cut);
            const auto overInput = 20.0 * std::log10 (tape / input);
            float loudest = 0.f;
            for (std::size_t i = 0; i < run.left.size(); ++i)
                loudest = std::max ({ loudest, std::abs (run.left[i]), std::abs (run.right[i]) });
            CAPTURE (x, y, overCut, overInput, loudest, peak);
            // Measured: -0.01 to -0.72 dB against the input, Cut within 0.0002 dB of it.
            CHECK (overCut <= 0.0);
            CHECK (overInput <= 0.0);
            CHECK (overInput >= -1.0);
            CHECK (loudest <= std::abs (peak));
        }
}

TEST_CASE ("A restart in the middle of a stop fades the stop out under what follows", "[tape][cutter]")
{
    // X 1 and Y 0: bar 3 is one four-beat stop. A restart lands on its third beat.
    constexpr std::int64_t restart = 3 * 96000 + 48000;
    constexpr int fade = 240; // 5 ms, the pad's release
    for (const bool toClassic : { false, true })
    {
        CAPTURE (toClassic);
        TapeCutter tape (padSpot (1.0, 0.0));
        std::vector<float> in, out;
        while (tape.frame < restart + 4 * fade)
        {
            if (tape.frame == restart - 1000)
            {
                if (toClassic)
                    tape.cutter.setFracture ({}); // a switch to a Classic-family program
                tape.cutter.restartAtNextUnit();
            }
            const auto input = tone (tape.frame);
            auto left = input.left, right = input.right;
            tape.process (&left, &right, 1);
            in.push_back (input.left);
            out.push_back (left);
            if (tape.frame == restart)
                REQUIRE (tape.cutter.fractureDiagnostics().tapeSpeed < 1.0); // mid-stop, braking
        }
        REQUIRE (tape.cutter.error() == Cutter::Error::none);
        const auto at = static_cast<std::size_t> (restart);
        // Before: the stop. From the restart, its tail fades out over 5 ms under the new cut.
        const auto stopStep = largestStep (out, at - 2000, at);
        const auto inputStep = largestStep (in, at - 2000, at + 4 * fade);
        const auto across = largestStep (out, at - 1, at + fade + 1);
        CAPTURE (stopStep, inputStep, across);
        // Measured 0.84x; spliced straight to the new cut it stepped 0.63, 32x.
        CHECK (across <= std::max (stopStep, inputStep));
        if (! toClassic)
            CHECK (std::equal (out.begin() + static_cast<std::ptrdiff_t> (at + fade), out.end(),
                               in.begin() + static_cast<std::ptrdiff_t> (at + fade))); // then the track, exactly
    }
}

// The per-cut motion sits in Cut's padding, so every block plan stays the same size.
static_assert (sizeof (Cut) == 4 * sizeof (double));

TEST_CASE ("At any tempo, Tape plays every frame of the track between its stops", "[tape][cutter][tempo]")
{
    // X 0.5 and Y 0: the first stop is beats 2 to 4 of bar 3, so bars 0 to 2 and the
    // first half of bar 3 are the track, through three bar lines. At most whole-number
    // tempos a bar isn't a whole number of samples, and the block's cuts ran out a
    // frame before the next bar's block began: that frame was silence.
    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
        for (const auto meter : { 4.0, 3.0 })
        {
            int wrong = 0, tempos = 0;
            float worst = 0.f;
            for (int bpm = 80; bpm <= 180; bpm += meter == 4.0 ? 1 : 7)
            {
                TapeCutter tape (stops (2.0, 0.0), rate, bpm, meter);
                const auto end = static_cast<std::int64_t> ((3.0 * meter + meter - 2.0) * 60.0 / bpm * rate) - 2;
                std::array<float, 512> left {}, right {}, in {};
                int here = 0;
                while (tape.frame < end)
                {
                    const auto count = static_cast<int> (std::min<std::int64_t> (512, end - tape.frame));
                    for (int i = 0; i < count; ++i)
                    {
                        const auto t = static_cast<double> (tape.frame + i) / rate;
                        in[static_cast<std::size_t> (i)] = left[static_cast<std::size_t> (i)] = right[static_cast<std::size_t> (i)]
                            = static_cast<float> (.5 * std::sin (2.0 * 3.141592653589793 * 97.0 * t) + .2);
                    }
                    tape.process (left.data(), right.data(), count);
                    for (std::size_t i = 0; i < static_cast<std::size_t> (count); ++i)
                        if (left[i] != in[i] || right[i] != in[i])
                        {
                            ++here;
                            worst = std::max (worst, std::abs (left[i] - in[i]));
                        }
                }
                REQUIRE (tape.cutter.error() == Cutter::Error::none);
                wrong += here;
                tempos += here > 0 ? 1 : 0;
            }
            CAPTURE (rate, meter, wrong, tempos, worst);
            CHECK (wrong == 0);
        }
}
