// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/Resonator.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace livecut::engine
{
namespace
{
bool sameTuning (const ResonatorParameters& a, const ResonatorParameters& b)
{
    return std::array { a.rootHz, a.decayMs, a.excitationDb }
        == std::array { b.rootHz, b.decayMs, b.excitationDb }
        && a.intervals == b.intervals && a.weights == b.weights;
}
}

Resonator::State Resonator::validate (const ResonatorParameters& p) const
{
    if (! prepared)
        return State::unsupportedRate;
    if (! std::isfinite (p.rootHz) || p.rootHz < 40 || p.rootHz > 800
        || ! std::isfinite (p.decayMs) || p.decayMs < 40 || p.decayMs > 5000
        || ! std::isfinite (p.excitationDb) || p.excitationDb < -12 || p.excitationDb > 12
        || ! std::isfinite (p.mix) || p.mix < 0 || p.mix > 1)
        return State::invalidParameters;
    double sum = 0;
    for (std::size_t i = 0; i < p.weights.size(); ++i)
    {
        if (p.intervals[i] < -12 || p.intervals[i] > 24
            || ! std::isfinite (p.weights[i]) || p.weights[i] < 0 || p.weights[i] > 1
            || p.rootHz * std::exp2 (p.intervals[i] / 12.0) > 0.4 * rate)
            return State::invalidParameters;
        sum += p.weights[i];
    }
    return sum > 0 ? State::ready : State::invalidWeights;
}

void Resonator::configure (Bank& bank, const ResonatorParameters& p)
{
    bank = {};
    bank.parameters = p;
    bank.radius = std::exp (-std::log (1000.0) / (rate * .001 * p.decayMs));
    bank.excitation = std::pow (10.0, p.excitationDb / 20.0);
    double sum = 0;
    for (auto weight : p.weights)
        sum += weight;
    for (std::size_t i = 0; i < bank.voices.size(); ++i)
    {
        const auto frequency = p.rootHz * std::exp2 (p.intervals[i] / 12.0);
        const auto r2 = bank.radius * bank.radius;
        // The denominator magnitude squared is a quadratic in cos(w).
        // Its minimum occurs at c*(1+r^2)/(2*r), fixing the actual peak.
        const auto c = std::cos (2 * std::numbers::pi * frequency / rate) * 2 * bank.radius / (1 + r2);
        auto& voice = bank.voices[i];
        voice.a1 = 2 * bank.radius * c;
        voice.a2 = r2;
        voice.b0 = (1 - r2) * std::sqrt ((1 - c) * (1 + c));
        voice.weight = p.weights[i] / sum;
    }
}

bool Resonator::prepare (double sampleRate)
{
    prepared = std::isfinite (sampleRate) && sampleRate >= 1000 && sampleRate <= 384000;
    rate = sampleRate;
    if (prepared)
    {
        controlFrames = static_cast<std::uint32_t> (std::ceil (.002 * rate));
        fadeFrames = std::max (2u, static_cast<std::uint32_t> (std::ceil (.050 * rate)));
    }
    reset();
    // Parameter validity stays local to the wet path, including after a rate
    // change. It must not turn a supported-rate engine prepare into failure.
    return prepared;
}

void Resonator::reset()
{
    banks = {};
    pending.reset();
    fading = false;
    active = 0;
    fadePosition = 0;
    wet = wetTarget = wetStep = 0;
    controlRemaining = 0;
    validBank = false;
    status = {};
    setParameters (requested);
}

void Resonator::beginRetune (const ResonatorParameters& p)
{
    configure (banks[1 - active], p);
    fadePosition = 0;
    fading = true;
}

bool Resonator::setParameters (const ResonatorParameters& p)
{
    requested = p;
    status.state = validate (p);
    // Invalid mix has no usable dry proportion: selected invalid processing
    // is silent. A valid mix always retains its explicit dry proportion.
    const auto target = p.enabled ? (std::isfinite (p.mix) && p.mix >= 0 && p.mix <= 1 ? p.mix : 1.0) : 0.0;
    if (std::abs (target - wetTarget) > 0)
    {
        wetTarget = target;
        controlRemaining = controlFrames;
        wetStep = (wetTarget - wet) / controlFrames;
    }
    if (status.state != State::ready)
    {
        banks = {};
        pending.reset();
        fading = validBank = false;
        active = 0;
        updateDiagnostics();
        return false;
    }
    if (! validBank)
    {
        configure (banks[0], p);
        active = 0;
        validBank = true;
    }
    else if (fading)
    {
        if (sameTuning (p, banks[1 - active].parameters))
            pending.reset();
        else
            pending = p;
    }
    else if (! sameTuning (p, banks[active].parameters))
        beginRetune (p);
    updateDiagnostics();
    return true;
}

void Resonator::updateDiagnostics()
{
    status.retuning = fading;
    status.pending = pending.has_value();
    status.retuneFramesRemaining = fading ? fadeFrames - fadePosition : 0;
    status.wet = wet;
    status.frequencies = {};
    status.weights = {};
    status.poleRadius = 0;
    status.impulseAbsoluteSumBound = 0;
    if (! validBank)
        return;
    const auto& bank = banks[active];
    status.poleRadius = bank.radius;
    status.impulseAbsoluteSumBound = (1 + bank.radius) * bank.excitation;
    for (std::size_t i = 0; i < bank.voices.size(); ++i)
    {
        status.frequencies[i] = bank.parameters.rootHz * std::exp2 (bank.parameters.intervals[i] / 12.0);
        status.weights[i] = bank.voices[i].weight;
    }
}

std::array<double, 2> Resonator::processBank (Bank& bank, double left, double right)
{
    std::array<double, 2> out {};
    const std::array<double, 2> excitation { left * bank.excitation, right * bank.excitation };
    for (auto& voice : bank.voices)
        for (std::size_t channel = 0; channel < out.size(); ++channel)
        {
            const auto y = voice.b0 * excitation[channel] + voice.a1 * voice.y1[channel] - voice.a2 * voice.y2[channel];
            voice.y2[channel] = voice.y1[channel];
            voice.y1[channel] = std::abs (y) < 1e-30 ? 0.0 : y;
            out[channel] += voice.weight * y;
        }
    return out;
}

void Resonator::process (float& left, float& right)
{
    assert (std::isfinite (left) && std::isfinite (right));
    std::array<double, 2> filtered {};
    if (validBank)
    {
        filtered = processBank (banks[active], left, right);
        if (fading)
        {
            const auto incoming = processBank (banks[1 - active], left, right);
            const auto fraction = static_cast<double> (fadePosition) / (fadeFrames - 1);
            for (std::size_t channel = 0; channel < filtered.size(); ++channel)
                filtered[channel] += fraction * (incoming[channel] - filtered[channel]);
            ++fadePosition;
            if (fadePosition == fadeFrames)
            {
                active = 1 - active;
                fading = false;
                if (pending)
                {
                    const auto next = *pending;
                    pending.reset();
                    beginRetune (next);
                }
                updateDiagnostics();
            }
            status.retuneFramesRemaining = fading ? fadeFrames - fadePosition : 0;
        }
    }
    if (controlRemaining > 0)
        wet = --controlRemaining == 0 ? wetTarget : wet + wetStep;
    status.wet = wet;
    if (wet <= 0)
        return;
    left = static_cast<float> (static_cast<double> (left) * (1 - wet) + filtered[0] * wet);
    right = static_cast<float> (static_cast<double> (right) * (1 - wet) + filtered[1] * wet);
}
}
