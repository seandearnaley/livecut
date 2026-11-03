// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/PadPrograms.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdlib>

namespace livecut::engine
{
namespace
{
double lerp (double a, double b, double t)
{
    return a + (b - a) * t;
}
} // namespace

bool isAvailable (PadProgram program)
{
    return program == PadProgram::cut || program == PadProgram::bounce || program == PadProgram::glitch
        || program == PadProgram::suction || program == PadProgram::filter;
}

PadCaptions padCaptions (PadProgram program)
{
    switch (program)
    {
        case PadProgram::cut: return { "Density", "Re-edit" };
        case PadProgram::bounce: return { "Bounce", "Ratchet" };
        case PadProgram::glitch: return { "Rate", "Pitch" };
        case PadProgram::suction: return { "Band", "Squeeze" };
        case PadProgram::filter: return { "Cutoff", "Ring" };
        case PadProgram::tape:
        case PadProgram::loops:
        case PadProgram::rush:
            break;
    }

    return { "", "" };
}

int padZone (float value, int zones, int previous)
{
    assert (value >= 0.f && value <= 1.f);
    assert (zones > 0);

    if (previous >= 0 && previous < zones)
    {
        const auto lower = static_cast<double> (previous) / zones - 0.03;
        const auto upper = static_cast<double> (previous + 1) / zones + 0.03;
        if (value >= lower && value <= upper)
            return previous;
    }

    return std::min (static_cast<int> (static_cast<double> (value) * zones), zones - 1);
}

PadMapping mapPad (PadProgram program, float x, float y, PadZones previous)
{
    assert (isAvailable (program));

    PadMapping mapping {};
    mapping.cut.minPhraseBars = 1;
    mapping.cut.maxPhraseBars = 2;
    mapping.zones.y = padZone (y, 1, previous.y);

    switch (program)
    {
        case PadProgram::cut:
        {
            constexpr int units[] { 8, 16, 32 };
            mapping.procedure = Procedure::cutProc11;
            mapping.seed = 101;
            mapping.zones.x = padZone (x, 3, previous.x);
            mapping.unitsPerBar = units[mapping.zones.x];
            mapping.cut.stutterChance = lerp (.05, .9, y);
            mapping.cut.stutterArea = lerp (.2, .8, y);
            mapping.cut.maxRepeats = static_cast<int> (std::lround (lerp (0, 3, y)));
            mapping.cut.minRepeats = 0;
            break;
        }
        case PadProgram::bounce:
            mapping.procedure = Procedure::bounce;
            mapping.seed = 202;
            mapping.zones.x = padZone (x, 1, previous.x);
            mapping.unitsPerBar = 16;
            mapping.cut.bounceChance = std::min (x / .85, 1.0);
            mapping.cut.halftimeChance = x >= .85 ? .5 : 0;
            mapping.cut.ratchetChance = y;
            mapping.cut.climb = static_cast<int> (std::lround (lerp (0, 12, y)));
            break;
        case PadProgram::glitch:
        {
            constexpr int units[] { 16, 32 };
            mapping.procedure = Procedure::sqPusher;
            mapping.seed = 303;
            mapping.zones.x = padZone (x, 2, previous.x);
            mapping.unitsPerBar = units[mapping.zones.x];
            mapping.cut.activity = lerp (.3, .95, x);
            mapping.crusher.enabled = true;
            mapping.crusher.minBits = mapping.crusher.maxBits = static_cast<float> (lerp (24, 6, x));
            mapping.cut.minCents = y < .5f ? -2400.f * (.5f - y) : 0.f;
            mapping.cut.maxCents = y >= .5f ? 2400.f * (y - .5f) : 0.f;
            break;
        }
        case PadProgram::suction:
        {
            // Gentle cuts underneath give the band its pulse.
            mapping.procedure = Procedure::cutProc11;
            mapping.seed = 404;
            mapping.zones.x = padZone (x, 1, previous.x);
            mapping.unitsPerBar = 16;
            mapping.cut.stutterChance = .25;
            mapping.cut.stutterArea = .5;
            mapping.cut.maxRepeats = 1;
            mapping.cut.minRepeats = 0;

            // Both edges close in on the centre X picks; each cut opens them and they suck closed.
            const auto centre = 120.0 * std::pow (5000.0 / 120.0, static_cast<double> (x));
            const auto squeeze = .85 * y;
            auto& filter = mapping.impact.filter;
            filter.enabled = true;
            filter.type = ImpactParameters::Filter::Type::band;
            filter.chance = 1.f;
            filter.mix = 1.f;
            filter.bandLowHz = static_cast<float> (std::pow (30.0, 1.0 - squeeze) * std::pow (centre, squeeze));
            filter.bandHighHz = static_cast<float> (std::pow (18000.0, 1.0 - squeeze) * std::pow (centre, squeeze));
            filter.q = static_cast<float> (.707 + 1.8 * y);
            filter.envelopeOctaves = 1.5f;
            mapping.impact.attackMs = 1.f;
            mapping.impact.decayMs = 180.f;
            break;
        }
        case PadProgram::filter:
        {
            // Rolls only: with no repeats, every multi-cut block is a stutter roll, and each strikes a tail.
            mapping.procedure = Procedure::cutProc11;
            mapping.seed = 505;
            mapping.zones.x = padZone (x, 1, previous.x);
            mapping.unitsPerBar = 16;
            mapping.cut.stutterChance = .5;
            mapping.cut.stutterArea = .5;
            mapping.cut.maxRepeats = 0;
            mapping.cut.minRepeats = 0;

            // A low-pass from dark to open; up, it resonates, easing off in the treble, and rings its rolls out.
            auto& filter = mapping.impact.filter;
            filter.enabled = true;
            filter.type = ImpactParameters::Filter::Type::sweep;
            filter.chance = 1.f;
            filter.mix = 1.f;
            filter.cutoffHz = static_cast<float> (200.0 * std::pow (90.0, static_cast<double> (x)));
            filter.q = static_cast<float> (.707 + 2.3 * y * (1.0 - .5 * x));
            filter.tailLevel = y;
            filter.tailDecayMs = static_cast<float> (200.0 + 1000.0 * y);
            break;
        }
        case PadProgram::tape:
        case PadProgram::loops:
        case PadProgram::rush:
        default:
            std::abort(); // A violated precondition must not select a fallback program.
    }

    const auto captions = padCaptions (program);
    mapping.xCaption = captions.x;
    mapping.yCaption = captions.y;
    return mapping;
}

bool sameDiscreteState (const PadMapping& a, const PadMapping& b)
{
    return a.procedure == b.procedure && a.unitsPerBar == b.unitsPerBar
        && a.zones.x == b.zones.x && a.zones.y == b.zones.y;
}

} // namespace livecut::engine
