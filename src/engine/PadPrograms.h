// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/Cutter.h"
#include "engine/ImpactEffects.h"

#include <cstddef>
#include <cstdint>

namespace livecut::engine
{

enum class PadProgram { cut, bounce, glitch, suction, filter, tape, loops, rush };
inline constexpr std::size_t padProgramCount = 8;

bool isAvailable (PadProgram program);

struct PadZones
{
    int x = -1, y = -1;
};

struct PadMapping
{
    Procedure procedure;
    int unitsPerBar;
    std::uint32_t seed;
    CutParameters cut;
    CrusherParameters crusher;
    ImpactParameters impact; // off unless the program shapes its cuts
    PadZones zones;
    const char* xCaption;
    const char* yCaption;
};

struct PadCaptions
{
    const char* x;
    const char* y;
};

// What the pad's axes do for a program. Empty for programs not yet built.
PadCaptions padCaptions (PadProgram program);

// Normalized value in [0, 1], with 0.03 hysteresis around the previous zone.
int padZone (float value, int zones, int previous);

// Requires an available program and normalized coordinates in [0, 1].
PadMapping mapPad (PadProgram program, float x, float y, PadZones previous);
bool sameDiscreteState (const PadMapping& a, const PadMapping& b);

} // namespace livecut::engine
