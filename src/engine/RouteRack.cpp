// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/RouteRack.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <type_traits>

namespace livecut::engine
{
bool RouteRack::prepare (double rate)
{
    if (! std::isfinite (rate) || rate < 1000 || rate > 384000)
        return false;
    fadeFrames = std::max (2u, static_cast<std::uint32_t> (std::ceil (.005 * rate)));
    for (auto& lane : lanes)
    {
        lane.impact.prepare (rate);
        lane.frequency.prepare (rate);
        if (! lane.resonator.prepare (rate))
            return false;
    }
    reset();
    return true;
}

void RouteRack::reset()
{
    fading = false;
    fadePosition = 0;
    cut = {};
    status.active = lanes[active].route;
    status.transitioning = false;
    status.framesRemaining = 0;
    status.cutsStarted = 0;
    lanes[active].impact.reset();
    lanes[active].frequency.reset();
    lanes[active].resonator.reset();
    lanes[1 - active] = lanes[active];
}

void RouteRack::setParameters (const RouteParameters& p)
{
    for (auto& lane : lanes)
    {
        lane.impact.setParameters (p.impact);
        lane.frequency.setParameters (p.frequency);
        lane.resonator.setParameters (p.resonator);
    }
}

void RouteRack::requestRoute (EffectRoute route)
{
    assert (route == EffectRoute::impactResonatorFrequency || route == EffectRoute::resonatorImpactFrequency
         || route == EffectRoute::impactFrequencyResonator);
    status.requested = route;
}

void RouteRack::barStarted()
{
    if (fading || status.requested == lanes[active].route)
        return;
    static_assert (std::is_trivially_copy_assignable_v<Lane>);
    // Copy only fixed DSP state. The copied carrier, cut selections and AD
    // already describe this exact source frame; no state warm-up/RNG draw.
    lanes[1 - active] = lanes[active];
    lanes[1 - active].route = status.requested;
    fading = true;
    fadePosition = 0;
    status.transitioning = true;
    status.framesRemaining = fadeFrames;
}

void RouteRack::startCut (const RouteCutContext& context)
{
    cut = context;
    lanes[active].impact.startCut (context.identity);
    if (fading)
        lanes[1 - active].impact.startCut (context.identity);
    ++status.cutsStarted;
}

void RouteRack::Lane::process (float& left, float& right, bool selected)
{
    const auto processFrequency = [&]
    {
        const auto& ad = impact.diagnostics();
        frequency.process (left, right, ad.envelope, ad.repeatProgress, selected);
    };
    switch (route)
    {
        case EffectRoute::impactResonatorFrequency:
            impact.process (left, right);
            resonator.process (left, right);
            processFrequency();
            return;
        case EffectRoute::resonatorImpactFrequency:
            resonator.process (left, right);
            impact.process (left, right);
            processFrequency();
            return;
        case EffectRoute::impactFrequencyResonator:
            impact.process (left, right);
            processFrequency();
            resonator.process (left, right);
            return;
    }
    assert (false);
}

void RouteRack::process (float& left, float& right)
{
    auto outgoingLeft = left, outgoingRight = right;
    lanes[active].process (outgoingLeft, outgoingRight, cut.frequencySelected);
    if (! fading)
    {
        left = outgoingLeft;
        right = outgoingRight;
        return;
    }
    auto incomingLeft = left, incomingRight = right;
    lanes[1 - active].process (incomingLeft, incomingRight, cut.frequencySelected);
    const auto fraction = static_cast<double> (fadePosition) / (fadeFrames - 1);
    left = static_cast<float> (outgoingLeft + fraction * (static_cast<double> (incomingLeft) - outgoingLeft));
    right = static_cast<float> (outgoingRight + fraction * (static_cast<double> (incomingRight) - outgoingRight));
    ++fadePosition;
    if (fadePosition == fadeFrames)
    {
        active = 1 - active;
        fading = false;
        status.active = lanes[active].route;
        status.transitioning = false;
    }
    status.framesRemaining = fading ? fadeFrames - fadePosition : 0;
}
}
