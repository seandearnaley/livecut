// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/Resonator.h"
#include "engine/FrequencyEffects.h"
#include "engine/ImpactEffects.h"

#include <array>
#include <cstdint>

namespace livecut::engine
{
enum class EffectRoute { impactResonatorFrequency, resonatorImpactFrequency, impactFrequencyResonator };

struct RouteParameters
{
    ImpactParameters impact;
    FrequencyParameters frequency;
    ResonatorParameters resonator;
};

struct RouteCutContext
{
    CutIdentity identity;
    bool frequencySelected = false; // Frozen once by Cutter, never drawn by the rack.
};

// Crusher/Comb prefix and one Spectral suffix remain outside this fixed rack.
class RouteRack
{
public:
    struct Diagnostics
    {
        EffectRoute active = EffectRoute::impactResonatorFrequency;
        EffectRoute requested = EffectRoute::impactResonatorFrequency;
        bool transitioning = false;
        std::uint32_t framesRemaining = 0;
        std::uint64_t cutsStarted = 0;
    };
    bool prepare (double sampleRate);
    void reset();
    void setParameters (const RouteParameters&); // Existing next-block contract.
    void requestRoute (EffectRoute); // Structural; no switch until an eligible bar.
    void barStarted();
    void startCut (const RouteCutContext&);
    void process (float& left, float& right);
    const Diagnostics& diagnostics() const { return status; }
    const ImpactEffects& impact() const { return lanes[active].impact; }
    const Resonator& resonator() const { return lanes[active].resonator; }

private:
    struct Lane
    {
        ImpactEffects impact;
        FrequencyEffects frequency;
        Resonator resonator;
        EffectRoute route = EffectRoute::impactResonatorFrequency;
        void process (float& left, float& right, bool frequencySelected);
    };
    std::array<Lane, 2> lanes;
    RouteCutContext cut;
    Diagnostics status;
    std::size_t active = 0;
    std::uint32_t fadeFrames = 240, fadePosition = 0;
    bool fading = false;
};
} // namespace livecut::engine
