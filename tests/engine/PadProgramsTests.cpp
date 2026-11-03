// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/PadPrograms.h"
#include "DrumLoop.h"
#include "EngineTestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string_view>
#include <functional>
#include <vector>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
// Zero tolerance checks exact numeric equality, including deterministic floating-point fields.
void checkCutEqual (const CutParameters& a, const CutParameters& b)
{
    CHECK (a.minPhraseBars == b.minPhraseBars);
    CHECK (a.maxPhraseBars == b.maxPhraseBars);
    CHECK_THAT (a.minAmp, WithinAbs (b.minAmp, 0.0));
    CHECK_THAT (a.maxAmp, WithinAbs (b.maxAmp, 0.0));
    CHECK_THAT (a.minPan, WithinAbs (b.minPan, 0.0));
    CHECK_THAT (a.maxPan, WithinAbs (b.maxPan, 0.0));
    CHECK_THAT (a.minCents, WithinAbs (b.minCents, 0.0));
    CHECK_THAT (a.maxCents, WithinAbs (b.maxCents, 0.0));
    CHECK_THAT (a.dutyCycle, WithinAbs (b.dutyCycle, 0.0));
    CHECK_THAT (a.fillDutyCycle, WithinAbs (b.fillDutyCycle, 0.0));
    CHECK_THAT (a.stutterChance, WithinAbs (b.stutterChance, 0.0));
    CHECK_THAT (a.stutterArea, WithinAbs (b.stutterArea, 0.0));
    CHECK (a.minRepeats == b.minRepeats);
    CHECK (a.maxRepeats == b.maxRepeats);
    CHECK_THAT (a.straightChance, WithinAbs (b.straightChance, 0.0));
    CHECK_THAT (a.regularChance, WithinAbs (b.regularChance, 0.0));
    CHECK_THAT (a.ritardChance, WithinAbs (b.ritardChance, 0.0));
    CHECK_THAT (a.accel, WithinAbs (b.accel, 0.0));
    CHECK_THAT (a.activity, WithinAbs (b.activity, 0.0));
    CHECK_THAT (a.bounceChance, WithinAbs (b.bounceChance, 0.0));
    CHECK_THAT (a.ratchetChance, WithinAbs (b.ratchetChance, 0.0));
    CHECK (a.climb == b.climb);
    CHECK_THAT (a.halftimeChance, WithinAbs (b.halftimeChance, 0.0));
}

void checkCrusherEqual (const CrusherParameters& a, const CrusherParameters& b)
{
    CHECK (a.enabled == b.enabled);
    CHECK_THAT (a.minBits, WithinAbs (b.minBits, 0.0));
    CHECK_THAT (a.maxBits, WithinAbs (b.maxBits, 0.0));
    CHECK_THAT (a.minHz, WithinAbs (b.minHz, 0.0));
    CHECK_THAT (a.maxHz, WithinAbs (b.maxHz, 0.0));
}

CutProcedure& procedureFor (const PadMapping& mapping, CutProc11& cut, Bounce& bounce, SQPusher& glitch)
{
    switch (mapping.procedure)
    {
        case Procedure::cutProc11: return cut;
        case Procedure::bounce: return bounce;
        case Procedure::sqPusher: return glitch;
        case Procedure::warpCut: break;
    }

    throw std::logic_error ("Unexpected pad procedure");
}
} // namespace

TEST_CASE ("Only Cut, Bounce, Glitch, Suction and Filter are available", "[pad]")
{
    CHECK (padProgramCount == 8);
    CHECK (isAvailable (PadProgram::cut));
    CHECK (isAvailable (PadProgram::bounce));
    CHECK (isAvailable (PadProgram::glitch));
    CHECK (isAvailable (PadProgram::suction));
    CHECK (isAvailable (PadProgram::filter));
    CHECK_FALSE (isAvailable (PadProgram::tape));
    CHECK_FALSE (isAvailable (PadProgram::loops));
    CHECK_FALSE (isAvailable (PadProgram::rush));
    CHECK (PadZones{}.x == -1);
    CHECK (PadZones{}.y == -1);
}

TEST_CASE ("Captions can be looked up for any program, empty for those not yet built", "[pad]")
{
    const auto caption = [] (const char* text) { return std::string_view (text); };
    CHECK (caption (padCaptions (PadProgram::cut).x) == "Density");
    CHECK (caption (padCaptions (PadProgram::cut).y) == "Re-edit");
    CHECK (caption (padCaptions (PadProgram::bounce).x) == "Bounce");
    CHECK (caption (padCaptions (PadProgram::bounce).y) == "Ratchet");
    CHECK (caption (padCaptions (PadProgram::glitch).x) == "Rate");
    CHECK (caption (padCaptions (PadProgram::glitch).y) == "Pitch");

    for (const auto program : { PadProgram::tape, PadProgram::loops, PadProgram::rush })
    {
        CAPTURE (program);
        CHECK (caption (padCaptions (program).x).empty());
        CHECK (caption (padCaptions (program).y).empty());
    }

    // The same words the mapping carries, from one table.
    for (const auto program : { PadProgram::cut, PadProgram::bounce, PadProgram::glitch, PadProgram::suction, PadProgram::filter })
    {
        CAPTURE (program);
        const auto mapping = mapPad (program, .5f, .5f, {});
        CHECK (caption (mapping.xCaption) == caption (padCaptions (program).x));
        CHECK (caption (mapping.yCaption) == caption (padCaptions (program).y));
    }
}

TEST_CASE ("Pad zones hold inside the hysteresis band", "[pad]")
{
    CHECK (padZone (.34f, 3, 0) == 0);
    CHECK (padZone (.37f, 3, 0) == 1);
    CHECK (padZone (.31f, 3, 1) == 1);
    CHECK (padZone (.30f, 3, 1) == 0);
    CHECK (padZone (.5f, 3, -1) == 1);
    CHECK (padZone (1.f, 3, -1) == 2);
    CHECK (padZone (0.f, 3, -1) == 0);
    CHECK (padZone (.68f, 3, 1) == 1);
    CHECK (padZone (.70f, 3, 1) == 2);
    CHECK (padZone (.65f, 3, 2) == 2);
    CHECK (padZone (.63f, 3, 2) == 1);
    CHECK (padZone (.52f, 2, 0) == 0);
    CHECK (padZone (.54f, 2, 0) == 1);
    CHECK (padZone (.48f, 2, 1) == 1);
    CHECK (padZone (.46f, 2, 1) == 0);
    CHECK (padZone (1.f, 3, 0) == 2);
    CHECK (padZone (0.f, 3, 2) == 0);
    CHECK (padZone (1.f, 1, 0) == 0);
}

TEST_CASE ("Cut maps density to 8, 16 and 32 units", "[pad]")
{
    CHECK (mapPad (PadProgram::cut, 0.f, 0.f, {}).unitsPerBar == 8);
    CHECK (mapPad (PadProgram::cut, .5f, 0.f, {}).unitsPerBar == 16);
    CHECK (mapPad (PadProgram::cut, 1.f, 0.f, {}).unitsPerBar == 32);
    const auto held = mapPad (PadProgram::cut, .34f, 1.f, { 0, 0 });
    CHECK (held.unitsPerBar == 8);
    CHECK (held.zones.x == 0);
    CHECK (held.zones.y == 0);
    CHECK (mapPad (PadProgram::cut, .37f, 1.f, held.zones).unitsPerBar == 16);
    CHECK (mapPad (PadProgram::glitch, .52f, 0.f, { 0, 0 }).unitsPerBar == 16);
    CHECK (mapPad (PadProgram::glitch, .54f, 0.f, { 0, 0 }).unitsPerBar == 32);
}

TEST_CASE ("Mappings are deterministic", "[pad]")
{
    const auto program = GENERATE (PadProgram::cut, PadProgram::bounce, PadProgram::glitch, PadProgram::suction, PadProgram::filter);
    const auto x = GENERATE (0.f, .34f, .5f, .85f, 1.f);
    const auto y = GENERATE (0.f, .25f, .5f, .75f, 1.f);
    const auto usePrevious = GENERATE (false, true);
    const auto previous = usePrevious ? mapPad (program, .5f, .5f, {}).zones : PadZones{};

    const auto a = mapPad (program, x, y, previous);
    const auto b = mapPad (program, x, y, previous);
    CHECK (a.procedure == b.procedure);
    CHECK (a.unitsPerBar == b.unitsPerBar);
    CHECK (a.seed == b.seed);
    checkCutEqual (a.cut, b.cut);
    checkCrusherEqual (a.crusher, b.crusher);
    CHECK_THAT (a.impact.filter.bandLowHz, WithinAbs (b.impact.filter.bandLowHz, 0.0));
    CHECK_THAT (a.impact.filter.bandHighHz, WithinAbs (b.impact.filter.bandHighHz, 0.0));
    CHECK_THAT (a.impact.filter.q, WithinAbs (b.impact.filter.q, 0.0));
    CHECK_THAT (a.impact.filter.cutoffHz, WithinAbs (b.impact.filter.cutoffHz, 0.0));
    CHECK_THAT (a.impact.filter.tailLevel, WithinAbs (b.impact.filter.tailLevel, 0.0));
    CHECK_THAT (a.impact.filter.tailDecayMs, WithinAbs (b.impact.filter.tailDecayMs, 0.0));
    CHECK (a.zones.x == b.zones.x);
    CHECK (a.zones.y == b.zones.y);
    CHECK (std::string_view (a.xCaption) == std::string_view (b.xCaption));
    CHECK (std::string_view (a.yCaption) == std::string_view (b.yCaption));
}

TEST_CASE ("Every mapping stays inside the parameter ranges", "[pad]")
{
    const auto program = GENERATE (PadProgram::cut, PadProgram::bounce, PadProgram::glitch, PadProgram::suction, PadProgram::filter);
    const auto seed = program == PadProgram::cut ? 101u : program == PadProgram::bounce ? 202u
                    : program == PadProgram::glitch ? 303u : program == PadProgram::suction ? 404u : 505u;
    constexpr std::array allowedUnits { 6, 8, 12, 16, 18, 24, 32 };

    for (int xi = 0; xi <= 20; ++xi)
    {
        for (int yi = 0; yi <= 20; ++yi)
        {
            CAPTURE (program, xi, yi);
            const auto mapping = mapPad (program, static_cast<float> (xi) / 20.f,
                                         static_cast<float> (yi) / 20.f, {});
            const auto& cut = mapping.cut;
            for (const auto chance : { cut.stutterChance, cut.straightChance, cut.regularChance,
                                       cut.ritardChance, cut.activity, cut.bounceChance,
                                       cut.ratchetChance, cut.halftimeChance })
            {
                CHECK (chance >= 0.0);
                CHECK (chance <= 1.0);
            }
            CHECK (cut.minRepeats >= 0);
            CHECK (cut.maxRepeats <= 4);
            CHECK (cut.minRepeats <= cut.maxRepeats);
            CHECK (cut.minCents >= -2400.f);
            CHECK (cut.maxCents <= 2400.f);
            CHECK (cut.minCents <= cut.maxCents);
            CHECK (mapping.crusher.minBits >= 1.f);
            CHECK (mapping.crusher.maxBits <= 32.f);
            CHECK (mapping.crusher.minBits <= mapping.crusher.maxBits);
            CHECK (std::ranges::find (allowedUnits, mapping.unitsPerBar) != allowedUnits.end());
            CHECK (mapping.seed == seed);
            CHECK (cut.minPhraseBars == 1);
            CHECK (cut.maxPhraseBars == 2);
            const auto& filter = mapping.impact.filter;
            CHECK (filter.bandLowHz >= 30.f);
            CHECK (filter.bandHighHz <= 18000.f);
            CHECK (filter.bandLowHz <= filter.bandHighHz);
            CHECK (filter.q >= .5f);
            CHECK (filter.q <= 8.f);
            CHECK (filter.cutoffHz >= 30.f);
            CHECK (filter.cutoffHz <= 18000.f);
            CHECK (filter.tailLevel >= 0.f);
            CHECK (filter.tailLevel <= 1.f);
            CHECK (filter.tailDecayMs >= 50.f);
            CHECK (filter.tailDecayMs <= 2000.f);
        }
    }
}

TEST_CASE ("Glitch gets crunchier as it gets faster", "[pad]")
{
    auto previous = mapPad (PadProgram::glitch, 0.f, .5f, {});
    CHECK (mapPad (PadProgram::glitch, 1.f, .5f, {}).crusher.minBits < previous.crusher.minBits);
    CHECK (mapPad (PadProgram::glitch, 1.f, .5f, {}).crusher.maxBits < previous.crusher.maxBits);
    for (int i = 1; i <= 20; ++i)
    {
        const auto next = mapPad (PadProgram::glitch, static_cast<float> (i) / 20.f, .5f, previous.zones);
        CHECK (next.cut.activity > previous.cut.activity);
        previous = next;
    }
}

TEST_CASE ("Each available program plays legal phrases at its corners", "[pad]")
{
    const auto program = GENERATE (PadProgram::cut, PadProgram::bounce, PadProgram::glitch, PadProgram::suction, PadProgram::filter);
    const auto x = GENERATE (0.f, 1.f);
    const auto y = GENERATE (0.f, 1.f);
    const auto mapping = mapPad (program, x, y, {});
    CutProc11 cut;
    Bounce bounce;
    SQPusher glitch;
    Random random { mapping.seed };
    testing::playPhrase (procedureFor (mapping, cut, bounce, glitch), random, mapping.cut, mapping.unitsPerBar);
}

TEST_CASE ("Pad programs use the pinned curves and preserve other defaults", "[pad]")
{
    const auto x = GENERATE (0.f, .25f, .5f, .75f, .85f, 1.f);
    const auto y = GENERATE (0.f, .25f, .5f, .75f, 1.f);
    CutParameters expected;
    expected.minPhraseBars = 1;
    expected.maxPhraseBars = 2;

    SECTION ("Cut")
    {
        const auto mapping = mapPad (PadProgram::cut, x, y, {});
        expected.stutterChance = .05 + (.9 - .05) * y;
        expected.stutterArea = .2 + (.8 - .2) * y;
        expected.minRepeats = 0;
        expected.maxRepeats = static_cast<int> (std::lround (3.0 * y));
        checkCutEqual (mapping.cut, expected);
        checkCrusherEqual (mapping.crusher, CrusherParameters{});
        CHECK (mapping.procedure == Procedure::cutProc11);
        CHECK (std::string_view (mapping.xCaption) == "Density");
        CHECK (std::string_view (mapping.yCaption) == "Re-edit");
    }

    SECTION ("Bounce")
    {
        const auto mapping = mapPad (PadProgram::bounce, x, y, {});
        expected.bounceChance = std::min (x / .85, 1.0);
        expected.halftimeChance = x >= .85 ? .5 : 0.0;
        expected.ratchetChance = y;
        expected.climb = static_cast<int> (std::lround (12.0 * y));
        checkCutEqual (mapping.cut, expected);
        checkCrusherEqual (mapping.crusher, CrusherParameters{});
        CHECK (mapping.procedure == Procedure::bounce);
        CHECK (mapping.unitsPerBar == 16);
        CHECK (mapping.zones.x == 0);
        CHECK (std::string_view (mapping.xCaption) == "Bounce");
        CHECK (std::string_view (mapping.yCaption) == "Ratchet");
    }

    SECTION ("Glitch")
    {
        const auto mapping = mapPad (PadProgram::glitch, x, y, {});
        expected.activity = .3 + (.95 - .3) * x;
        expected.minCents = y < .5f ? -2400.f * (.5f - y) : 0.f;
        expected.maxCents = y >= .5f ? 2400.f * (y - .5f) : 0.f;
        checkCutEqual (mapping.cut, expected);
        auto crusher = CrusherParameters{};
        crusher.enabled = true;
        crusher.minBits = crusher.maxBits = 24.f - 18.f * x;
        checkCrusherEqual (mapping.crusher, crusher);
        CHECK (mapping.procedure == Procedure::sqPusher);
        CHECK (mapping.unitsPerBar == (x < .5f ? 16 : 32));
        CHECK (std::string_view (mapping.xCaption) == "Rate");
        CHECK (std::string_view (mapping.yCaption) == "Pitch");
    }
}

TEST_CASE ("Suction is available, with Band and Squeeze", "[pad]")
{
    CHECK (isAvailable (PadProgram::suction));
    CHECK (std::string_view (padCaptions (PadProgram::suction).x) == "Band");
    CHECK (std::string_view (padCaptions (PadProgram::suction).y) == "Squeeze");
    const auto mapping = mapPad (PadProgram::suction, .5f, .5f, {});
    CHECK (std::string_view (mapping.xCaption) == "Band");
    CHECK (std::string_view (mapping.yCaption) == "Squeeze");
}

TEST_CASE ("Suction's bottom edge is open at every X", "[pad]")
{
    for (int xi = 0; xi <= 20; ++xi)
    {
        CAPTURE (xi);
        const auto& filter = mapPad (PadProgram::suction, static_cast<float> (xi) / 20.f, 0.f, {}).impact.filter;
        CHECK_THAT (filter.bandLowHz, WithinAbs (30.0, .5));
        CHECK_THAT (filter.bandHighHz, WithinAbs (18000.0, .5));
    }
}

TEST_CASE ("Suction squeezes toward the centre X picks", "[pad]")
{
    const auto [x, centre] = GENERATE (table<float, double> ({ { 0.f, 120.0 },
                                                               { .5f, std::sqrt (120.0 * 5000.0) },
                                                               { 1.f, 5000.0 } }));
    CAPTURE (x);
    const auto mapping = mapPad (PadProgram::suction, x, 1.f, {});
    const auto& filter = mapping.impact.filter;
    CHECK_THAT (filter.bandLowHz, WithinRel (std::pow (30.0, .15) * std::pow (centre, .85), .005));
    CHECK_THAT (filter.bandHighHz, WithinRel (std::pow (18000.0, .15) * std::pow (centre, .85), .005));
    CHECK_THAT (filter.q, WithinAbs (2.507, 1.0e-5));
    CHECK_THAT (filter.envelopeOctaves, WithinAbs (1.5, 0.0));
    CHECK_THAT (mapping.impact.decayMs, WithinAbs (180.0, 0.0));
    CHECK_THAT (mapping.impact.attackMs, WithinAbs (1.0, 0.0));
    CHECK (filter.enabled);
    CHECK (filter.type == ImpactParameters::Filter::Type::band);
    CHECK_THAT (filter.chance, WithinAbs (1.0, 0.0));
    CHECK_THAT (filter.mix, WithinAbs (1.0, 0.0));
    CHECK_FALSE (mapping.impact.drive.enabled);
    CHECK_FALSE (mapping.impact.transient.enabled);
}

TEST_CASE ("Suction cuts gently underneath", "[pad]")
{
    const auto x = GENERATE (0.f, .5f, 1.f);
    const auto y = GENERATE (0.f, .5f, 1.f);
    const auto mapping = mapPad (PadProgram::suction, x, y, {});
    CutParameters expected;
    expected.minPhraseBars = 1;
    expected.maxPhraseBars = 2;
    expected.stutterChance = .25;
    expected.stutterArea = .5;
    expected.minRepeats = 0;
    expected.maxRepeats = 1;
    checkCutEqual (mapping.cut, expected);
    checkCrusherEqual (mapping.crusher, CrusherParameters{});
    CHECK (mapping.procedure == Procedure::cutProc11);
    CHECK (mapping.unitsPerBar == 16);
    CHECK (mapping.seed == 404);
}

TEST_CASE ("Suction never restarts the cutting while you move", "[pad]")
{
    const auto first = mapPad (PadProgram::suction, 0.f, 0.f, {});
    auto previous = first;
    for (int xi = 0; xi <= 20; ++xi)
    {
        for (int yi = 0; yi <= 20; ++yi)
        {
            CAPTURE (xi, yi);
            const auto x = static_cast<float> (xi) / 20.f;
            const auto y = static_cast<float> (xi % 2 == 0 ? yi : 20 - yi) / 20.f; // a boustrophedon sweep
            const auto next = mapPad (PadProgram::suction, x, y, previous.zones);
            CHECK (sameDiscreteState (previous, next));
            CHECK (sameDiscreteState (first, mapPad (PadProgram::suction, x, y, {})));
            previous = next;
        }
    }
}

TEST_CASE ("Cut, Bounce and Glitch keep Impact off", "[pad]")
{
    const auto program = GENERATE (PadProgram::cut, PadProgram::bounce, PadProgram::glitch);
    const auto x = GENERATE (0.f, .5f, 1.f);
    const auto y = GENERATE (0.f, .5f, 1.f);
    const auto mapping = mapPad (program, x, y, {});
    const ImpactParameters off;
    CHECK_FALSE (mapping.impact.filter.enabled);
    CHECK_FALSE (mapping.impact.drive.enabled);
    CHECK_FALSE (mapping.impact.transient.enabled);
    CHECK (mapping.impact.filter.type == off.filter.type);
    CHECK_THAT (mapping.impact.filter.envelopeOctaves, WithinAbs (off.filter.envelopeOctaves, 0.0));
    CHECK_THAT (mapping.impact.attackMs, WithinAbs (off.attackMs, 0.0));
    CHECK_THAT (mapping.impact.decayMs, WithinAbs (off.decayMs, 0.0));
}

TEST_CASE ("Discrete state compares only procedure, subdivision and zones", "[pad]")
{
    const auto original = mapPad (PadProgram::cut, 0.f, 0.f, {});
    auto changed = original;
    CHECK (sameDiscreteState (original, changed));

    SECTION ("Procedure") { changed.procedure = Procedure::bounce; }
    SECTION ("Subdivision") { changed.unitsPerBar = 16; }
    SECTION ("X zone") { changed.zones.x = 1; }
    SECTION ("Y zone") { changed.zones.y = 1; }
    SECTION ("Other fields")
    {
        changed.seed = 999;
        changed.cut = mapPad (PadProgram::cut, 0.f, 1.f, {}).cut;
        changed.crusher.enabled = true;
        changed.impact.filter.enabled = true;
        changed.xCaption = "Other X";
        changed.yCaption = "Other Y";
        CHECK (sameDiscreteState (original, changed));
        return;
    }

    CHECK_FALSE (sameDiscreteState (original, changed));
    CHECK_FALSE (sameDiscreteState (changed, original));
}

TEST_CASE ("Filter is available, with Cutoff and Ring", "[pad]")
{
    CHECK (isAvailable (PadProgram::filter));
    CHECK (std::string_view (padCaptions (PadProgram::filter).x) == "Cutoff");
    CHECK (std::string_view (padCaptions (PadProgram::filter).y) == "Ring");
    const auto mapping = mapPad (PadProgram::filter, .5f, .5f, {});
    CHECK (std::string_view (mapping.xCaption) == "Cutoff");
    CHECK (std::string_view (mapping.yCaption) == "Ring");
}

TEST_CASE ("Filter's bottom-right is the open filter", "[pad]")
{
    const auto& filter = mapPad (PadProgram::filter, 1.f, 0.f, {}).impact.filter;
    CHECK_THAT (filter.cutoffHz, WithinAbs (18000.0, .5));
    CHECK_THAT (filter.q, WithinAbs (.707, 1.0e-6));
    CHECK_THAT (filter.tailLevel, WithinAbs (0.0, 0.0));
}

TEST_CASE ("Filter sweeps and rings as the spec maps it", "[pad]")
{
    const auto x = GENERATE (0.f, .5f, 1.f);
    const auto y = GENERATE (0.f, .5f, 1.f);
    CAPTURE (x, y);
    const auto mapping = mapPad (PadProgram::filter, x, y, {});
    const auto& filter = mapping.impact.filter;
    CHECK_THAT (filter.cutoffHz, WithinRel (200.0 * std::pow (90.0, static_cast<double> (x)), 1.0e-5));
    // Resonance eases off in the treble, and in the bass to 15% at 200 Hz.
    CHECK_THAT (filter.q, WithinRel (.707 + 2.3 * y * (1.0 - .5 * x) * std::min (1.0, .15 + .85 * x / .4), 1.0e-5));
    CHECK_THAT (filter.tailLevel, WithinAbs (static_cast<double> (y), 1.0e-6));
    CHECK_THAT (filter.tailDecayMs, WithinRel (200.0 + 1000.0 * y, 1.0e-5));
    CHECK (filter.type == ImpactParameters::Filter::Type::sweep);
    CHECK (filter.enabled);
    CHECK_THAT (filter.chance, WithinAbs (1.0, 0.0));
    CHECK_THAT (filter.mix, WithinAbs (1.0, 0.0));
    CHECK_THAT (filter.envelopeOctaves, WithinAbs (0.0, 0.0));
    CHECK_THAT (filter.repeatOctaves, WithinAbs (0.0, 0.0));
    CHECK_FALSE (mapping.impact.drive.enabled);
    CHECK_FALSE (mapping.impact.transient.enabled);
}

TEST_CASE ("Filter cuts in rolls", "[pad]")
{
    const auto x = GENERATE (0.f, .5f, 1.f);
    const auto y = GENERATE (0.f, .5f, 1.f);
    const auto mapping = mapPad (PadProgram::filter, x, y, {});
    CutParameters expected;
    expected.minPhraseBars = 1;
    expected.maxPhraseBars = 2;
    expected.stutterChance = .5;
    expected.stutterArea = .5;
    expected.minRepeats = 0;
    expected.maxRepeats = 0; // so every multi-cut block is a stutter roll
    checkCutEqual (mapping.cut, expected);
    checkCrusherEqual (mapping.crusher, CrusherParameters{});
    CHECK (mapping.procedure == Procedure::cutProc11);
    CHECK (mapping.unitsPerBar == 16);
    CHECK (mapping.seed == 505);
}

TEST_CASE ("Filter never restarts the cutting while you move", "[pad]")
{
    const auto first = mapPad (PadProgram::filter, 0.f, 0.f, {});
    auto previous = first;
    for (int xi = 0; xi <= 20; ++xi)
    {
        for (int yi = 0; yi <= 20; ++yi)
        {
            CAPTURE (xi, yi);
            const auto x = static_cast<float> (xi) / 20.f;
            const auto y = static_cast<float> (xi % 2 == 0 ? yi : 20 - yi) / 20.f; // a boustrophedon sweep
            const auto next = mapPad (PadProgram::filter, x, y, previous.zones);
            CHECK (sameDiscreteState (previous, next));
            CHECK (sameDiscreteState (first, mapPad (PadProgram::filter, x, y, {})));
            previous = next;
        }
    }
}

namespace
{
using Loop = testing::DrumLoop;

// A program's RMS through the Cutter over bars 2 to 8 of the loop.
double programRms (const PadMapping& mapping, const Loop& loop)
{
    constexpr double rate = 48000.0;
    constexpr int block = 512, total = 8 * 96000, from = 96000;
    Cutter cutter;
    cutter.prepare (rate);
    cutter.setProcedure (mapping.procedure);
    cutter.setUnitsPerBar (mapping.unitsPerBar);
    cutter.setSeed (mapping.seed);
    cutter.setParameters (mapping.cut);
    cutter.setCrusher (mapping.crusher);
    cutter.setImpact (mapping.impact);
    std::array<float, block> left {}, right {};
    double energy = 0.0;
    for (int n = 0; n < total; n += block)
    {
        for (int i = 0; i < block; ++i)
            left[static_cast<std::size_t> (i)] = right[static_cast<std::size_t> (i)]
                = loop.samples[static_cast<std::size_t> ((n + i) % static_cast<int> (loop.samples.size()))];
        const auto beats = n / 24000.0;
        const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
        cutter.process (left.data(), right.data(), block, { 120.0, 4.0 }, { bar, beats - 4.0 * static_cast<double> (bar) });
        if (n >= from)
            for (const auto sample : left)
                energy += static_cast<double> (sample) * sample;
    }
    REQUIRE (cutter.error() == Cutter::Error::none);
    return std::sqrt (energy / (total - from));
}
} // namespace

TEST_CASE ("Filter is never louder than Cut on bass-heavy material", "[pad][level]")
{
    // Sub-bass under drums, a square bass, and a bass note on the left edge's
    // 200 Hz cutoff. Engaging Filter anywhere must not jump the level: at most
    // 1.5 dB over Cut playing the same material. The tail is included.
    const auto [bassHz, bass, name] = GENERATE (table<double, Loop::Bass, const char*> ({
        { 55.0, Loop::Bass::sine, "sub-bass" },
        { 55.0, Loop::Bass::square, "square bass" },
        { 150.0, Loop::Bass::sine, "a note on 200 Hz" },
    }));
    CAPTURE (name);
    const Loop loop (bassHz, bass);
    const auto cut = programRms (mapPad (PadProgram::cut, .5f, 0.f, {}), loop);
    for (const auto x : { 0.f, .5f, 1.f })
        for (const auto y : { 0.f, .5f, 1.f })
        {
            const auto lift = 20.0 * std::log10 (programRms (mapPad (PadProgram::filter, x, y, {}), loop) / cut);
            CAPTURE (x, y, lift);
            CHECK (lift <= 1.5);
        }
}
