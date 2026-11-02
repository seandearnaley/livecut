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
using namespace detail;
constexpr double pi = std::numbers::pi;
constexpr double log1000 = 6.907755278982137;
constexpr double semitone = 1.0594630943592953; // 2^(1/12)
constexpr double makeupCeiling = 2.0; // +6 dB
constexpr double makeupHighestHz = 18000.0;

// The band's pink-noise power, tabulated over its width w (octaves) and log2 q.
// The widest band the clamps allow is log2 (0.45 * 384 kHz / 30 Hz) = 12.49
// octaves; the narrowest is the semitone floor. Bilinear interpolation of
// ln P on 1/16 octave by 1/8 octave of q is within 0.05 dB of the integral
// across the whole table (worst near w 0.1, q 7); the gain halves that.
constexpr double tableWidthMin = 1.0 / 12.0, tableWidthStep = 1.0 / 16.0;
constexpr std::size_t tableWidths = 200; // 1/12 to 12.52 octaves
constexpr double tableLogQMin = -1.0, tableLogQStep = 1.0 / 8.0;
constexpr std::size_t tableQs = 33; // q 0.5 to 8

// The analogue prototypes the SVFs approximate: a high-pass at 1 and a
// low-pass at 2^width, both with this q. Each edge's tail falls 12 dB an
// octave, so 6 octaves beyond the edges leave out under 1e-6 dB; 1/16-octave
// trapezoids resolve q 8's resonance (about 0.18 octaves wide) to 0.002 dB.
double integratePinkPower (double width, double q, double step, double margin)
{
    const auto damping = 1.0 / (q * q);
    const auto scale = std::exp2 (-2.0 * width);
    const auto count = static_cast<int> (std::ceil ((width + 2.0 * margin) / step));
    auto sum = 0.0;
    for (int i = 0; i <= count; ++i)
    {
        const auto x = std::exp2 (2.0 * (i * step - margin)); // squared frequency at the low edge
        const auto y = x * scale; // and at the high edge
        const auto highPass = x * x / ((1.0 - x) * (1.0 - x) + damping * x);
        const auto lowPass = 1.0 / ((1.0 - y) * (1.0 - y) + damping * y);
        sum += (i == 0 || i == count ? 0.5 : 1.0) * highPass * lowPass;
    }
    return sum * step;
}

struct PinkPowerTable
{
    PinkPowerTable()
    {
        for (std::size_t i = 0; i < tableWidths; ++i)
            for (std::size_t j = 0; j < tableQs; ++j)
                logPower[i * tableQs + j] = std::log (integratePinkPower (tableWidthMin + static_cast<double> (i) * tableWidthStep,
                                                                          std::exp2 (tableLogQMin + static_cast<double> (j) * tableLogQStep),
                                                                          bandPowerStep, bandPowerMargin));
    }

    double interpolate (double width, double q) const
    {
        const auto u = std::clamp ((width - tableWidthMin) / tableWidthStep, 0.0, static_cast<double> (tableWidths - 1));
        const auto v = std::clamp ((std::log2 (q) - tableLogQMin) / tableLogQStep, 0.0, static_cast<double> (tableQs - 1));
        const auto i = std::min (static_cast<std::size_t> (u), tableWidths - 2);
        const auto j = std::min (static_cast<std::size_t> (v), tableQs - 2);
        const auto a = u - static_cast<double> (i), b = v - static_cast<double> (j);
        const auto at = [this] (std::size_t wi, std::size_t qi) { return logPower[wi * tableQs + qi]; };
        return std::lerp (std::lerp (at (i, j), at (i, j + 1), b), std::lerp (at (i + 1, j), at (i + 1, j + 1), b), a);
    }

    std::array<double, tableWidths * tableQs> logPower {};
    double logOpen = std::log (integratePinkPower (bandOpenOctaves, bandOpenQ, bandPowerStep, bandPowerMargin));
};

// Built once, by the first prepare(), never on the audio thread.
const PinkPowerTable& pinkPowerTable()
{
    static const PinkPowerTable table;
    return table;
}

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

namespace detail
{
double bandPinkPower (double widthOctaves, double q)
{
    return std::exp (pinkPowerTable().interpolate (widthOctaves, q));
}

double integrateBandPinkPower (double widthOctaves, double q, double stepOctaves)
{
    return integratePinkPower (widthOctaves, q, stepOctaves, bandPowerMargin);
}
} // namespace detail

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
    smoothingPole = std::exp (-1.0 / (0.020 * sampleRate));
    pinkPowerTable(); // Builds the band's makeup table off the audio thread.
    reset();
    setParameters (parameters);
}

void ImpactEffects::reset()
{
    state = {};
    wet = {};
    adoptFilter();
    filterState = {};
    driveState = {};
    resetBand();
    fastEnvelope = slowEnvelope = adEnvelope = 0.0;
    attackPosition = 0;
}

// Clears everything the band runs on, including the high-pass stage it shares
// with the single-SVF types.
void ImpactEffects::resetBand()
{
    filterState = {};
    bandState = {};
    bandPrimed = false;
    state.bandLowHz = state.bandHighHz = 0.0;
    state.levelHoldGain = 1.0;
}

void ImpactEffects::adoptFilter()
{
    if (requestedFilter.type == ImpactParameters::Filter::Type::band
        && parameters.filter.type != ImpactParameters::Filter::Type::band)
        resetBand(); // A band switched on never inherits classic or stale band state.
    parameters.filter = requestedFilter;
    filterHeld = filterDetached = false;
    bandLowTarget = std::log2 (static_cast<double> (requestedFilter.bandLowHz));
    bandHighTarget = std::log2 (static_cast<double> (requestedFilter.bandHighHz));
}

void ImpactEffects::setParameters (const ImpactParameters& next)
{
    if (next.drive.mode != parameters.drive.mode)
        for (auto& channel : driveState)
            channel.primed = false;
    requestedFilter = next.filter;
    auto& f = requestedFilter;
    f.chance = std::clamp (f.chance, 0.0f, 1.0f);
    f.cutoffHz = std::clamp (f.cutoffHz, 30.0f, 18000.0f);
    f.q = std::clamp (f.q, 0.5f, 8.0f);
    f.envelopeOctaves = std::clamp (f.envelopeOctaves, -6.0f, 6.0f);
    f.repeatOctaves = std::clamp (f.repeatOctaves, -4.0f, 4.0f);
    f.bandLowHz = std::clamp (f.bandLowHz, 30.0f, 18000.0f);
    f.bandHighHz = std::clamp (f.bandHighHz, 30.0f, 18000.0f);
    f.mix = std::clamp (f.mix, 0.0f, 1.0f);
    const auto filter = parameters.filter;
    parameters = next;
    parameters.filter = filter;
    // A band switched off, or switched to or from another type, while it still
    // sounds keeps its last shape, its breathing included, until its wet ramps
    // out at the next cut; it never changes shape while audible. The new filter
    // comes in on a later cut. Classic types, which never involve a band, adopt
    // the new parameters at once, as they always have.
    const auto band = ImpactParameters::Filter::Type::band;
    const auto sounding = parameters.filter.type;
    const auto audible = wet[0].current != 0.0 || wet[0].target != 0.0;
    if (audible && (sounding == band || f.type == band) && (! f.enabled || f.type != sounding))
    {
        if (! filterHeld)
            heldDecayMultiplier = decayMultiplier;
        filterHeld = true;
        parameters.filter.enabled = false;
    }
    else
        adoptFilter();
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
    if (filterHeld && ! filterDetached)
    {
        // A band fading out shouldn't breathe: this cut would open a held band
        // 1.5 octaves while it fades, an audible change of shape. So it fades on
        // its own envelope, decaying on from where it was.
        filterDetached = true;
        filterEnvelope = adEnvelope;
        filterRepeatProgress = state.repeatProgress;
    }
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

void ImpactEffects::processFilter (double& left, double& right, double envelope, double repeatProgress)
{
    const auto mix = wet[0].next();
    if (mix == 0.0)
    {
        resetBand();
        if (filterHeld) // Only a held filter differs from the requested one.
            adoptFilter();
        return;
    }
    if (parameters.filter.type == ImpactParameters::Filter::Type::band)
    {
        processBand (left, right, envelope, repeatProgress, mix);
        return;
    }
    const auto& p = parameters.filter;
    // Octave modulation operates in log Hz; the physical coefficient is bounded
    // before tan() so extreme sweeps remain below Nyquist at every supported rate.
    const auto octaves = p.envelopeOctaves * envelope + p.repeatOctaves * repeatProgress;
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

void ImpactEffects::processBand (double& left, double& right, double envelope, double repeatProgress, double mix)
{
    const auto& p = parameters.filter;
    if (! bandPrimed)
    {
        bandLow = bandLowTarget;
        bandHigh = bandHighTarget;
        bandQ = p.q;
        bandPrimed = true;
    }
    else
    {
        // 20 ms one-pole glides, so block-rate parameter steps cannot zipper.
        bandLow = bandLowTarget + smoothingPole * (bandLow - bandLowTarget);
        bandHigh = bandHighTarget + smoothingPole * (bandHigh - bandHighTarget);
        bandQ = p.q + smoothingPole * (bandQ - p.q);
    }
    // The envelope opens the edges outward and repeat progress shifts both.
    // The high edge stays at least a semitone above the low edge, so crossed
    // edges hold a semitone apart, and both stay inside [30, 0.45 * rate].
    const auto opening = p.envelopeOctaves * envelope;
    const auto shift = p.repeatOctaves * repeatProgress;
    const auto limit = 0.45 * sampleRate;
    auto low = std::clamp (std::exp2 (bandLow - opening + shift), 30.0, limit);
    auto high = std::clamp (std::exp2 (bandHigh + opening + shift), 30.0, limit);
    high = std::min (std::max (high, low * semitone), limit);
    low = std::min (low, high / semitone);
    state.bandLowHz = low;
    state.bandHighHz = high;

    struct Stage { double a1, a2, a3; };
    const auto k = 1.0 / bandQ;
    const auto stage = [this, k] (double cutoff)
    {
        const auto g = std::tan (pi * cutoff / sampleRate);
        const auto a1 = 1.0 / (1.0 + g * (g + k));
        return Stage { a1, g * a1, g * (g * a1) };
    };
    const auto tick = [] (const Stage& c, double input, FilterState& s)
    {
        // The same Simper SVF as the single-stage types.
        const auto v3 = input - s.ic2;
        const auto v1 = c.a1 * s.ic1 + c.a2 * v3;
        const auto v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
        s.ic1 = cleanState (2.0 * v1 - s.ic1);
        s.ic2 = cleanState (2.0 * v2 - s.ic2);
        return std::array<double, 2> { v1, v2 };
    };
    const auto highPass = stage (low), lowPass = stage (high);
    const auto channel = [&] (double input, FilterState& highState, FilterState& lowState)
    {
        const auto [v1, v2] = tick (highPass, input, highState);
        return tick (lowPass, input - k * v1 - v2, lowState)[1];
    };
    const auto bandLeft = channel (left, filterState[0], bandState[0]);
    const auto bandRight = channel (right, filterState[1], bandState[1]);

    // Level hold: makeup from the band's shape this sample, so pink material
    // keeps the open band's power, including where the resonant edges overlap.
    // It is at most +6 dB. It follows the breathing edges exactly, so it never
    // lags or overshoots.
    // Octaves above 18 kHz don't count: at high rates the breathing pushes the
    // high edge there, where material has little power, and counting them
    // would dip the level at every cut. The table clamps a band that sits
    // wholly above 18 kHz to its narrowest width.
    const auto& table = pinkPowerTable();
    const auto width = std::log2 (std::min (high, makeupHighestHz) / low);
    const auto hold = std::min (makeupCeiling, std::exp (0.5 * (table.logOpen - table.interpolate (width, bandQ))));
    state.levelHoldGain = hold;
    left += mix * (hold * bandLeft - left);
    right += mix * (hold * bandRight - right);
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
    if (filterDetached)
    {
        filterEnvelope = cleanState (filterEnvelope * heldDecayMultiplier);
        processFilter (l, r, filterEnvelope, filterRepeatProgress);
    }
    else
        processFilter (l, r, envelope, state.repeatProgress);
    processDrive (l, r, envelope);
    processTransient (l, r);
    left = static_cast<float> (l);
    right = static_cast<float> (r);
}
} // namespace livecut::engine
