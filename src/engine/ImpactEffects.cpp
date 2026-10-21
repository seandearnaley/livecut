// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ImpactEffects.h"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <numbers>

namespace livecut::engine
{
namespace
{
constexpr double pi = std::numbers::pi;
constexpr double log1000 = 6.907755278982137;

// SplitMix64's finaliser, used as a stateless hash rather than an RNG stream.
std::uint64_t hash (std::uint64_t x)
{
    x = (x ^ (x >> 30)) * UINT64_C (0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)) * UINT64_C (0x94d049bb133111eb);
    return x ^ (x >> 31);
}

std::uint64_t identityHash (const CutIdentity& id)
{
    auto result = hash (id.seed + UINT64_C (0x9e3779b97f4a7c15));
    const auto add = [&result] (std::uint64_t value) { result = hash (result ^ hash (value)); };
    add (static_cast<std::uint64_t> (id.phraseBar));
    add (std::bit_cast<std::uint64_t> (id.blockStartUnits == 0.0 ? 0.0 : id.blockStartUnits));
    add (std::bit_cast<std::uint64_t> (id.cutStartUnits == 0.0 ? 0.0 : id.cutStartUnits));
    add (id.cutIndex);
    add (id.cutCount);
    return result;
}

bool choose (std::uint64_t identity, std::uint64_t tag, bool enabled, float chance)
{
    if (! enabled || chance <= 0.0f)
        return false;
    if (chance >= 1.0f)
        return true;
    const auto draw = static_cast<double> (hash (identity ^ tag) >> 11) * 0x1.0p-53;
    return draw < chance;
}

double gain (double db) { return std::exp (db * (std::numbers::ln10 / 20.0)); }

// log(cosh(x)) without overflowing at high drive, and with a well-defined F(0).
double logCosh (double x)
{
    const auto magnitude = std::abs (x);
    return magnitude + std::log1p (std::exp (-2.0 * magnitude)) - std::numbers::ln2;
}

double cleanState (double x) { return std::abs (x) < 1.0e-30 ? 0.0 : x; }
} // namespace

void ImpactEffects::Ramp::setTarget (double value, std::uint32_t samples)
{
    if (value == target)
        return;
    target = value;
    remaining = samples;
    increment = (target - current) / samples;
}

double ImpactEffects::Ramp::next()
{
    if (remaining > 0)
        current = --remaining == 0 ? target : current + increment;
    return current;
}

void ImpactEffects::prepare (double newSampleRate)
{
    assert (std::isfinite (newSampleRate) && newSampleRate >= 1000.0 && newSampleRate <= 384000.0);
    sampleRate = newSampleRate;
    rampSamples = static_cast<std::uint32_t> (std::ceil (0.002 * sampleRate));
    dcPole = std::exp (-2.0 * pi * 5.0 / sampleRate);
    reset();
    setParameters (parameters);
}

void ImpactEffects::reset()
{
    state = {};
    wet = {};
    filterState = {};
    driveState = {};
    fastEnvelope = slowEnvelope = adEnvelope = 0.0;
    attackPosition = 0;
}

void ImpactEffects::setParameters (const ImpactParameters& next)
{
    if (next.drive.mode != parameters.drive.mode)
        for (auto& channel : driveState)
            channel.primed = false;
    parameters = next;
    auto& f = parameters.filter;
    f.chance = std::clamp (f.chance, 0.0f, 1.0f);
    f.cutoffHz = std::clamp (f.cutoffHz, 30.0f, 18000.0f);
    f.q = std::clamp (f.q, 0.5f, 8.0f);
    f.envelopeOctaves = std::clamp (f.envelopeOctaves, -6.0f, 6.0f);
    f.repeatOctaves = std::clamp (f.repeatOctaves, -4.0f, 4.0f);
    f.mix = std::clamp (f.mix, 0.0f, 1.0f);
    auto& d = parameters.drive;
    d.chance = std::clamp (d.chance, 0.0f, 1.0f);
    d.driveDb = std::clamp (d.driveDb, 0.0f, 30.0f);
    d.envelopeDb = std::clamp (d.envelopeDb, -24.0f, 24.0f);
    d.repeatDb = std::clamp (d.repeatDb, -24.0f, 24.0f);
    d.mix = std::clamp (d.mix, 0.0f, 1.0f);
    auto& t = parameters.transient;
    t.chance = std::clamp (t.chance, 0.0f, 1.0f);
    t.attackDb = std::clamp (t.attackDb, -18.0f, 18.0f);
    t.sustainDb = std::clamp (t.sustainDb, -18.0f, 18.0f);
    t.timeMs = std::clamp (t.timeMs, 10.0f, 200.0f);
    t.mix = std::clamp (t.mix, 0.0f, 1.0f);
    parameters.attackMs = std::clamp (parameters.attackMs, 0.0f, 25.0f);
    parameters.decayMs = std::clamp (parameters.decayMs, 5.0f, 1000.0f);
    attackSamples = static_cast<std::uint64_t> (std::llround (0.001 * parameters.attackMs * sampleRate));
    decayMultiplier = std::exp (-log1000 / (0.001 * parameters.decayMs * sampleRate));

    // A peak follower pair with four time constants. Both channels share the
    // larger input magnitude, so an off-centre transient cannot move the image.
    const auto time = 0.001 * t.timeMs * sampleRate;
    transientAttack = std::exp (-1.0 / (0.025 * time));
    transientRelease = std::exp (-1.0 / (0.25 * time));
    transientSlowAttack = std::exp (-1.0 / (0.5 * time));
    transientSlowRelease = std::exp (-1.0 / time);
}

void ImpactEffects::startCut (const CutIdentity& identity)
{
    ++state.cutsStarted;
    state.repeatProgress = identity.cutCount > 1
        ? static_cast<double> (std::min (identity.cutIndex, identity.cutCount - 1))
            / static_cast<double> (identity.cutCount - 1) : 0.0;
    const auto cut = identityHash (identity);
    // Fixed effect tags keep draws independent of enable order and the legacy RNG.
    state.filterActive = choose (cut, UINT64_C (0x46494c5445523036), parameters.filter.enabled, parameters.filter.chance);
    state.driveActive = choose (cut, UINT64_C (0x4452495645303036), parameters.drive.enabled, parameters.drive.chance);
    state.transientActive = choose (cut, UINT64_C (0x5452414e53493036), parameters.transient.enabled, parameters.transient.chance);
    wet[0].setTarget (state.filterActive ? parameters.filter.mix : 0.0, rampSamples);
    wet[1].setTarget (state.driveActive ? parameters.drive.mix : 0.0, rampSamples);
    wet[2].setTarget (state.transientActive ? parameters.transient.mix : 0.0, rampSamples);
    attackPosition = 0;
    adEnvelope = attackSamples == 0 ? 1.0 : 0.0;
    state.envelope = adEnvelope;
}

double ImpactEffects::nextEnvelope()
{
    if (attackPosition < attackSamples)
        adEnvelope = static_cast<double> (++attackPosition) / static_cast<double> (attackSamples);
    else if (attackSamples == 0 && attackPosition == 0)
        ++attackPosition; // An instantaneous attack emits exactly 1 on the onset sample.
    else
        adEnvelope = cleanState (adEnvelope * decayMultiplier);
    state.envelope = adEnvelope;
    return adEnvelope;
}

void ImpactEffects::processFilter (double& left, double& right, double envelope)
{
    const auto mix = wet[0].next();
    if (mix == 0.0)
    {
        filterState = {};
        return;
    }
    const auto& p = parameters.filter;
    // Octave modulation operates in log Hz; the physical coefficient is bounded
    // before tan() so extreme sweeps remain below Nyquist at every supported rate.
    const auto octaves = p.envelopeOctaves * envelope + p.repeatOctaves * state.repeatProgress;
    const auto cutoff = std::clamp (p.cutoffHz * std::exp2 (octaves), 30.0, 0.45 * sampleRate);
    const auto g = std::tan (pi * cutoff / sampleRate);
    const auto k = 1.0 / p.q;
    const auto a1 = 1.0 / (1.0 + g * (g + k));
    const auto a2 = g * a1;
    const auto a3 = g * a2;
    const auto channel = [&] (double input, FilterState& s)
    {
        // Andrew Simper's trapezoidal SVF (Cytomic, 2013, public domain).
        const auto v3 = input - s.ic2;
        const auto v1 = a1 * s.ic1 + a2 * v3;
        const auto v2 = s.ic2 + a2 * s.ic1 + a3 * v3;
        s.ic1 = cleanState (2.0 * v1 - s.ic1);
        s.ic2 = cleanState (2.0 * v2 - s.ic2);
        const auto output = p.type == ImpactParameters::Filter::Type::lowPass ? v2
            : p.type == ImpactParameters::Filter::Type::bandPass ? k * v1 : input - k * v1 - v2;
        return input + mix * (output - input);
    };
    left = channel (left, filterState[0]);
    right = channel (right, filterState[1]);
}

void ImpactEffects::processDrive (double& left, double& right, double envelope)
{
    const auto mix = wet[1].next();
    if (mix == 0.0)
    {
        driveState = {};
        return;
    }
    const auto& p = parameters.drive;
    const auto db = std::clamp (p.driveDb + p.envelopeDb * envelope + p.repeatDb * state.repeatProgress, 0.0, 30.0);
    const auto inputGain = gain (db);
    const auto channel = [&] (double input, DriveState& s)
    {
        const auto x = input * inputGain;
        double shaped;
        if (p.mode == ImpactParameters::Drive::Mode::wavefold)
        {
            // F(x)=-cos(x). Its divided difference is evaluated with the exact
            // trig identity sin(midpoint)*sinc(halfDifference), avoiding cancellation.
            const auto previous = s.primed ? s.previous : x;
            const auto halfDifference = 0.5 * (x - previous);
            const auto sinc = std::abs (halfDifference) < 1.0e-4
                ? 1.0 - halfDifference * halfDifference / 6.0 : std::sin (halfDifference) / halfDifference;
            shaped = std::sin (0.5 * (x + previous)) * sinc;
        }
        else
        {
            // First-order ADAA: [F(x[n])-F(x[n-1])]/[x[n]-x[n-1]].
            // The analytic midpoint limit avoids a nearly zero denominator.
            const auto primitive = logCosh (x);
            const auto difference = x - s.previous;
            shaped = ! s.primed || std::abs (difference) < 1.0e-4
                ? std::tanh (s.primed ? 0.5 * (x + s.previous) : x)
                : (primitive - s.previousPrimitive) / difference;
            s.previousPrimitive = primitive;
        }
        s.previous = x;
        s.primed = true;
        const auto blocked = cleanState (shaped - s.dcInput + dcPole * s.dcOutput);
        s.dcInput = shaped;
        s.dcOutput = blocked;
        return input + mix * (blocked - input);
    };
    left = channel (left, driveState[0]);
    right = channel (right, driveState[1]);
}

void ImpactEffects::processTransient (double& left, double& right)
{
    const auto mix = wet[2].next();
    if (mix == 0.0)
    {
        fastEnvelope = slowEnvelope = 0.0;
        return;
    }
    const auto detector = std::max (std::abs (left), std::abs (right));
    const auto fast = detector > fastEnvelope ? transientAttack : transientRelease;
    const auto slow = detector > slowEnvelope ? transientSlowAttack : transientSlowRelease;
    fastEnvelope = cleanState (detector + fast * (fastEnvelope - detector));
    slowEnvelope = cleanState (detector + slow * (slowEnvelope - detector));
    const auto& p = parameters.transient;
    if ((p.attackDb == 0.0f && p.sustainDb == 0.0f) || std::max (fastEnvelope, slowEnvelope) < 1.0e-15)
        return;
    const auto attack = std::clamp ((fastEnvelope - slowEnvelope) / std::max (fastEnvelope, slowEnvelope), 0.0, 1.0);
    // A convex dB blend bounds gain by the two controls (at most +/-18 dB).
    const auto db = p.attackDb * attack + p.sustainDb * (1.0 - attack);
    const auto amount = 1.0 + mix * (gain (db) - 1.0);
    left *= amount;
    right *= amount;
}

void ImpactEffects::process (float& left, float& right)
{
    const auto envelope = nextEnvelope();
    if (wet[0].current == 0.0 && wet[0].target == 0.0
        && wet[1].current == 0.0 && wet[1].target == 0.0
        && wet[2].current == 0.0 && wet[2].target == 0.0)
        return;
    auto l = static_cast<double> (left), r = static_cast<double> (right);
    processFilter (l, r, envelope);
    processDrive (l, r, envelope);
    processTransient (l, r);
    left = static_cast<float> (l);
    right = static_cast<float> (r);
}
} // namespace livecut::engine
