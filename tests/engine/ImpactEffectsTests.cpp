// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ImpactEffects.h"
#include "AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include <numbers>
#include <vector>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double pi = std::numbers::pi;
constexpr double rate = 48000.0;

void settle (ImpactEffects& effects, int count = 128)
{
    for (int i = 0; i < count; ++i)
    {
        float left = 0.0f, right = 0.0f;
        effects.process (left, right);
    }
}

double toneGain (ImpactParameters parameters, double frequency, double sampleRate = rate)
{
    ImpactEffects effects;
    effects.prepare (sampleRate);
    effects.setParameters (parameters);
    effects.startCut ({});
    const auto count = static_cast<int> (sampleRate);
    double inputEnergy = 0.0, outputEnergy = 0.0;
    for (int i = 0; i < 2 * count; ++i)
    {
        const auto input = static_cast<float> (0.05 * std::sin (2.0 * pi * frequency * i / sampleRate));
        auto left = input, right = input;
        effects.process (left, right);
        if (i >= count)
        {
            inputEnergy += input * input;
            outputEnergy += left * left;
        }
    }
    return std::sqrt (outputEnergy / inputEnergy);
}

unsigned mask (const ImpactEffects& effects)
{
    const auto& s = effects.diagnostics();
    return (s.filterActive ? 1U : 0U) | (s.driveActive ? 2U : 0U) | (s.transientActive ? 4U : 0U);
}

struct Spectrum
{
    double fundamental = 0.0, residual = 0.0;
};

Spectrum driveSpectrum (ImpactParameters::Drive::Mode mode, bool adaa, double sampleRate)
{
    // 701/4096 Fs puts the third harmonic above Nyquist. Thus the only
    // unaliased non-DC component is the fundamental; all other coherent-bin
    // energy is folded harmonic energy. Normalising by the fundamental also
    // accounts for first-order ADAA's documented high-frequency attenuation.
    constexpr int length = 4096, bin = 701;
    ImpactEffects effects;
    effects.prepare (sampleRate);
    ImpactParameters p;
    p.drive.enabled = true;
    p.drive.mode = mode;
    p.drive.driveDb = 12.0f;
    effects.setParameters (p);
    effects.startCut ({});
    double energy = 0.0, dc = 0.0, real = 0.0, imaginary = 0.0;
    for (int i = 0; i < 17 * length; ++i)
    {
        const auto phase = 2.0 * pi * bin * (i % length) / length;
        const auto input = static_cast<float> (0.8 * std::sin (phase));
        auto left = input, right = input;
        if (adaa)
            effects.process (left, right);
        else
        {
            const auto x = input * std::pow (10.0, 12.0 / 20.0);
            left = static_cast<float> (mode == ImpactParameters::Drive::Mode::drive ? std::tanh (x) : std::sin (x));
        }
        if (i >= 16 * length)
        {
            energy += left * static_cast<double> (left);
            dc += left;
            real += left * std::cos (phase);
            imaginary += left * std::sin (phase);
        }
    }
    const auto fundamental = 2.0 * std::hypot (real, imaginary) / length;
    const auto residual = std::sqrt (std::max (0.0, energy / length - std::pow (dc / length, 2) - 0.5 * fundamental * fundamental));
    return { fundamental, residual };
}

// A fixed 32-bit LCG (Numerical Recipes constants) as white noise in [-0.5, 0.5).
struct Noise
{
    std::uint32_t state = 0x1234567u;
    float next()
    {
        state = state * 1664525u + 1013904223u;
        return static_cast<float> (static_cast<double> (state >> 8) * 0x1.0p-24 - 0.5);
    }
};

// FNV-1a over the raw bytes of each float.
struct Fnv1a
{
    std::uint64_t value = UINT64_C (0xcbf29ce484222325);
    void add (float sample)
    {
        const auto bits = std::bit_cast<std::uint32_t> (sample);
        for (int shift = 0; shift < 32; shift += 8)
        {
            value ^= (bits >> shift) & 0xffu;
            value *= UINT64_C (0x100000001b3);
        }
    }
};

// Paul Kellet's refined pink filter over the local LCG.
struct Pink
{
    Noise white;
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, b4 = 0.0, b5 = 0.0, b6 = 0.0;
    float next()
    {
        const auto w = static_cast<double> (white.next());
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        const auto pink = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        return static_cast<float> (0.1 * pink);
    }
};

ImpactParameters bandParameters (float lowHz, float highHz, float q = 0.707f)
{
    ImpactParameters p;
    p.filter.enabled = true;
    p.filter.type = ImpactParameters::Filter::Type::band;
    p.filter.bandLowHz = lowHz;
    p.filter.bandHighHz = highHz;
    p.filter.q = q;
    return p;
}

double decibels (double ratio) { return 20.0 * std::log10 (ratio); }

// The pad's Suction curve: x picks the centre, y squeezes the edges toward it.
ImpactParameters suctionBand (double x, double y)
{
    const auto centre = 120.0 * std::pow (5000.0 / 120.0, x);
    const auto squeeze = 0.85 * y;
    return bandParameters (static_cast<float> (std::pow (30.0, 1.0 - squeeze) * std::pow (centre, squeeze)),
                           static_cast<float> (std::pow (18000.0, 1.0 - squeeze) * std::pow (centre, squeeze)),
                           static_cast<float> (0.707 + 1.8 * y));
}

// Suction's tightest band, breathing as the pad plays it.
ImpactParameters breathingSuction (double x)
{
    auto p = suctionBand (x, 1.0);
    p.filter.envelopeOctaves = 1.5f;
    p.attackMs = 1.0f;
    p.decayMs = 180.0f;
    return p;
}

// The Suction y whose band is this many octaves wide.
double squeezeForWidth (double octaves)
{
    return (1.0 - octaves / std::log2 (18000.0 / 30.0)) / 0.85;
}

// The band's steady-state sine gain with the level hold divided back out,
// sample by sample, using the hold gain the diagnostics report.
double preHoldGain (const ImpactParameters& parameters, double frequency)
{
    ImpactEffects effects;
    effects.prepare (rate);
    effects.setParameters (parameters);
    effects.startCut ({});
    const auto count = static_cast<int> (rate);
    double inputEnergy = 0.0, outputEnergy = 0.0;
    for (int i = 0; i < 2 * count; ++i)
    {
        const auto input = static_cast<float> (0.05 * std::sin (2.0 * pi * frequency * i / rate));
        auto left = input, right = input;
        effects.process (left, right);
        const auto filtered = left / effects.diagnostics().levelHoldGain;
        if (i >= count)
        {
            inputEnergy += input * static_cast<double> (input);
            outputEnergy += filtered * filtered;
        }
    }
    return std::sqrt (outputEnergy / inputEnergy);
}
} // namespace

TEST_CASE ("Impact bypass is exact for disabled modules, zero mixes, and neutral transient", "[impact]")
{
    for (int variant = 0; variant < 3; ++variant)
    {
        ImpactEffects effects;
        effects.prepare (rate);
        ImpactParameters p;
        if (variant == 1)
        {
            p.filter.enabled = p.drive.enabled = p.transient.enabled = true;
            p.filter.mix = p.drive.mix = p.transient.mix = 0.0f;
        }
        if (variant == 2)
            p.transient.enabled = true;
        effects.setParameters (p);
        effects.startCut ({});
        for (int i = 0; i < 4096; ++i)
        {
            const auto input = static_cast<float> (0.8 * std::sin (0.31 * i));
            auto left = input, right = -input;
            effects.process (left, right);
            REQUIRE (left == input);
            REQUIRE (right == -input);
        }
    }
}

TEST_CASE ("Impact cut envelope attacks then reaches minus 60 dB at its decay time", "[impact]")
{
    ImpactEffects effects;
    effects.prepare (rate);
    ImpactParameters p;
    p.attackMs = 1.0f;
    p.decayMs = 10.0f;
    effects.setParameters (p);
    effects.startCut ({ 9, 0, 0.0, 0.0, 0, 5 });
    settle (effects, 48);
    CHECK_THAT (effects.diagnostics().envelope, WithinAbs (1.0, 1.0e-12));
    settle (effects, 480);
    CHECK_THAT (effects.diagnostics().envelope, WithinAbs (0.001, 1.0e-12));
    effects.startCut ({ 9, 0, 0.0, 1.0, 4, 5 });
    CHECK (effects.diagnostics().repeatProgress == 1.0);
    CHECK (effects.diagnostics().cutsStarted == 2);
    effects.startCut ({ 9, 0, 0.0, 2.0, 0, 1 });
    CHECK (effects.diagnostics().repeatProgress == 0.0);
    CHECK (effects.diagnostics().envelope == 0.0);
}

TEST_CASE ("Impact probability is frozen per cut and independently tagged by effect", "[impact]")
{
    ImpactParameters p;
    p.filter.enabled = p.drive.enabled = p.transient.enabled = true;
    p.filter.chance = p.drive.chance = p.transient.chance = 0.5f;
    ImpactEffects first, second;
    first.prepare (rate);
    second.prepare (rate);
    first.setParameters (p);
    second.setParameters (p);
    std::array<int, 8> counts {};
    for (std::uint32_t seed = 0; seed < 2048; ++seed)
    {
        CutIdentity id { seed, -19, 3.25, 4.5, 2, 8 };
        first.startCut (id);
        second.startCut (id);
        const auto chosen = mask (first);
        REQUIRE (mask (second) == chosen);
        ++counts[chosen];
        settle (first, 3);
        REQUIRE (mask (first) == chosen);
    }
    for (const auto count : counts)
        CHECK (count > 180); // Every combination occurs, rather than all modules sharing one draw.

    p.filter.chance = 0.0f;
    p.drive.chance = p.transient.chance = 1.0f;
    first.setParameters (p);
    first.startCut ({});
    CHECK (mask (first) == 6);
    p.filter.enabled = false;
    first.setParameters (p);
    first.startCut ({});
    CHECK (mask (first) == 6);
}

TEST_CASE ("Impact probability is independent of module enable and other chance controls", "[impact]")
{
    ImpactEffects first, second;
    first.prepare (rate);
    second.prepare (rate);
    ImpactParameters a;
    a.filter.enabled = a.drive.enabled = a.transient.enabled = true;
    a.filter.chance = a.drive.chance = a.transient.chance = 0.37f;
    auto b = a;
    b.filter.enabled = false;
    b.transient.chance = 0.99f;
    first.setParameters (a);
    second.setParameters (b);
    int driveHits = 0;
    for (std::uint32_t i = 0; i < 100; ++i)
    {
        CutIdentity id { i, 4, 2.0, 3.0, 1, 4 };
        first.startCut (id);
        second.startCut (id);
        CHECK (first.diagnostics().driveActive == second.diagnostics().driveActive);
        driveHits += first.diagnostics().driveActive ? 1 : 0;
    }
    CHECK (driveHits > 15);
    CHECK (driveHits < 60);
}

TEST_CASE ("Impact TPT filter matches the bilinear low band and high pass responses", "[impact]")
{
    ImpactParameters p;
    p.filter.enabled = true;
    p.filter.cutoffHz = 1500.0f;
    p.filter.q = 0.70710678f;
    for (const auto type : { ImpactParameters::Filter::Type::lowPass,
                             ImpactParameters::Filter::Type::bandPass,
                             ImpactParameters::Filter::Type::highPass })
    {
        p.filter.type = type;
        for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
            for (const auto frequency : { 150.0, 1500.0, 6000.0 })
            {
                const auto r = std::tan (pi * frequency / sampleRate) / std::tan (pi * p.filter.cutoffHz / sampleRate);
                const auto denominator = std::hypot (1.0 - r * r, r / p.filter.q);
                const auto numerator = type == ImpactParameters::Filter::Type::lowPass ? 1.0
                    : type == ImpactParameters::Filter::Type::bandPass ? r / p.filter.q : r * r;
                CAPTURE (type, sampleRate, frequency);
                CHECK_THAT (toneGain (p, frequency, sampleRate), WithinAbs (numerator / denominator, 2.0e-5));
            }
    }
}

TEST_CASE ("Impact filter repeat modulation reaches the final cut and single cuts have no repeat offset", "[impact]")
{
    ImpactParameters p;
    p.filter.enabled = true;
    p.filter.cutoffHz = 1000.0f;
    p.filter.repeatOctaves = 2.0f;
    const auto output = [&p] (std::size_t index, std::size_t count)
    {
        ImpactEffects effects;
        effects.prepare (rate);
        effects.setParameters (p);
        effects.startCut ({ 0, 0, 0.0, 0.0, index, count });
        settle (effects);
        float left = 1.0f, right = 0.0f;
        effects.process (left, right);
        return left;
    };
    CHECK (output (3, 4) > 10.0f * output (0, 4));
    CHECK (output (0, 1) == output (0, 4));
}

TEST_CASE ("Impact wet transitions use two-millisecond sample-rounded ramps and return to exact bypass", "[impact]")
{
    for (const auto sampleRate : { 8000.0, 44100.0, 48000.0, 192000.0, 384000.0 })
    {
        ImpactEffects effects;
        effects.prepare (sampleRate);
        ImpactParameters p;
        p.drive.enabled = true;
        p.drive.driveDb = 30.0f;
        effects.setParameters (p);
        effects.startCut ({});
        const auto samples = static_cast<int> (std::ceil (0.002 * sampleRate));
        float first = 0.0f;
        for (int i = 0; i < samples; ++i)
        {
            float left = 0.3f, right = 0.3f;
            effects.process (left, right);
            if (i == 0)
                first = left;
        }
        CHECK (std::abs (first - 0.3f) < 1.0f / static_cast<float> (samples));
        p.drive.chance = 0.0f;
        effects.setParameters (p);
        effects.startCut ({});
        settle (effects, samples);
        float left = 0.234567f, right = -0.456789f;
        effects.process (left, right);
        CHECK (left == 0.234567f);
        CHECK (right == -0.456789f);
    }
}

TEST_CASE ("Impact enabled and chance transitions follow a linear two-millisecond wet ramp", "[impact]")
{
    for (const auto sampleRate : { 1000.0, 8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0 })
        for (const auto useChance : { false, true })
          for (const auto mix : { 0.25f, 1.0f })
        {
            ImpactEffects effects;
            effects.prepare (sampleRate);
            ImpactParameters p;
            p.transient.enabled = true;
            p.transient.mix = mix;
            p.transient.attackDb = p.transient.sustainDb = 6.0f;
            effects.setParameters (p);
            effects.startCut ({});
            const auto count = static_cast<int> (std::ceil (0.002 * sampleRate));
            const auto boost = mix * (std::pow (10.0, 6.0 / 20.0) - 1.0);
            for (int n = 1; n <= count; ++n)
            {
                float left = 0.25f, right = -0.125f;
                effects.process (left, right);
                CHECK_THAT (left, WithinAbs (0.25 * (1.0 + boost * n / count), 1.0e-7));
                CHECK_THAT (right, WithinAbs (-0.125 * (1.0 + boost * n / count), 1.0e-7));
            }
            if (useChance)
                p.transient.chance = 0.0f;
            else
                p.transient.enabled = false;
            effects.setParameters (p);
            CHECK (effects.diagnostics().transientActive); // Frozen until the next cut starts.
            effects.startCut ({});
            for (int n = 1; n <= count; ++n)
            {
                float left = 0.25f, right = -0.125f;
                effects.process (left, right);
                CHECK_THAT (left, WithinAbs (0.25 * (1.0 + boost * (count - n) / count), 1.0e-7));
                CHECK_THAT (right, WithinAbs (-0.125 * (1.0 + boost * (count - n) / count), 1.0e-7));
                if (n == count)
                {
                    CHECK (left == 0.25f);
                    CHECK (right == -0.125f);
                }
            }
        }
}

TEST_CASE ("Impact drive has the analytic constant limit and removes DC", "[impact]")
{
    for (const auto mode : { ImpactParameters::Drive::Mode::drive, ImpactParameters::Drive::Mode::wavefold })
    {
        ImpactParameters p;
        p.drive.enabled = true;
        p.drive.mode = mode;
        p.drive.driveDb = 12.0f;
        ImpactEffects effects;
        effects.prepare (rate);
        effects.setParameters (p);
        effects.startCut ({});
        settle (effects);
        float left = 0.2f, right = -0.2f;
        effects.process (left, right);
        effects.process (left = 0.2f, right = -0.2f);
        const auto x = 0.2f * std::pow (10.0, 12.0 / 20.0);
        const auto expected = mode == ImpactParameters::Drive::Mode::drive ? std::tanh (x) : std::sin (x);
        // The second sample is the equal-input ADAA limit; DC blocking attenuates it slightly.
        CHECK_THAT (left, WithinAbs (expected, 0.001));
        CHECK_THAT (right, WithinAbs (-expected, 0.001));
        for (int i = 0; i < 48000; ++i)
            effects.process (left = 0.2f, right = -0.2f);
        CHECK (std::abs (left) < 1.0e-6f);
    }
}

TEST_CASE ("Impact enabled zero dB drive is nonlinear", "[impact]")
{
    ImpactEffects effects;
    effects.prepare (rate);
    ImpactParameters p;
    p.drive.enabled = true;
    p.drive.driveDb = 0.0f;
    effects.setParameters (p);
    effects.startCut ({});
    settle (effects);
    float left = 0.8f, right = 0.8f;
    effects.process (left, right);
    effects.process (left = 0.8f, right = 0.8f);
    CHECK (left < 0.7f);
    CHECK (left > 0.6f);
}

TEST_CASE ("Impact drive matches its antiderivative definition and analytic equal-input limit", "[impact]")
{
    for (const auto mode : { ImpactParameters::Drive::Mode::drive, ImpactParameters::Drive::Mode::wavefold })
    {
        ImpactEffects effects;
        effects.prepare (rate);
        ImpactParameters p;
        p.drive.enabled = true;
        p.drive.mode = mode;
        p.drive.driveDb = 0.0f;
        effects.setParameters (p);
        effects.startCut ({});
        settle (effects);
        double previous = 0.0, dcInput = 0.0, dcOutput = 0.0;
        for (const auto input : { 0.8f, 0.8f, 0.8000001f, -0.4f, -0.4f, -0.4000001f, 0.0f })
        {
            const auto x = static_cast<double> (input);
            const auto primitive = [mode] (double v)
            {
                return mode == ImpactParameters::Drive::Mode::drive ? std::log (std::cosh (v)) : -std::cos (v);
            };
            const auto difference = x - previous;
            const auto midpoint = 0.5 * (x + previous);
            const auto shaped = std::abs (difference) < 1.0e-5
                ? (mode == ImpactParameters::Drive::Mode::drive ? std::tanh (midpoint) : std::sin (midpoint))
                : (primitive (x) - primitive (previous)) / difference;
            const auto expected = shaped - dcInput + std::exp (-2.0 * pi * 5.0 / rate) * dcOutput;
            auto left = input, right = -input;
            effects.process (left, right);
            CHECK_THAT (left, WithinAbs (expected, 1.0e-7));
            CHECK_THAT (right, WithinAbs (-expected, 1.0e-7));
            previous = x;
            dcInput = shaped;
            dcOutput = expected;
        }
    }
}

TEST_CASE ("Impact ADAA reduces the measured folded harmonic residual of both drive modes", "[impact][spectral]")
{
    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (const auto mode : { ImpactParameters::Drive::Mode::drive, ImpactParameters::Drive::Mode::wavefold })
        {
            const auto direct = driveSpectrum (mode, false, sampleRate);
            const auto adaa = driveSpectrum (mode, true, sampleRate);
            CAPTURE (sampleRate, mode, direct.fundamental, direct.residual, adaa.fundamental, adaa.residual);
            CHECK (adaa.fundamental > 0.1); // A mute cannot pass the alias measurement.
            CHECK (adaa.residual / adaa.fundamental < direct.residual / direct.fundamental);
        }
}

TEST_CASE ("Impact modulation uses log Hz and dB and clamps effective settings to their physical bounds", "[impact]")
{
    for (const auto sampleRate : { 8000.0, 48000.0, 192000.0, 384000.0 })
        for (const auto sign : { -1.0f, 1.0f })
        {
            ImpactEffects effects;
            effects.prepare (sampleRate);
            ImpactParameters p;
            p.filter.enabled = true;
            p.filter.cutoffHz = 18000.0f;
            p.filter.envelopeOctaves = 6.0f * sign;
            p.filter.repeatOctaves = 4.0f * sign;
            p.attackMs = 0.0f;
            effects.setParameters (p);
            effects.startCut ({});
            settle (effects, static_cast<int> (std::ceil (0.002 * sampleRate)));
            effects.startCut ({ 1, 0, 0.0, 1.0, 3, 4 });
            float left = 1.0f, right = 0.0f;
            effects.process (left, right);
            const auto cutoff = std::clamp (18000.0 * std::exp2 (10.0 * sign), 30.0, 0.45 * sampleRate);
            const auto g = std::tan (pi * cutoff / sampleRate);
            const auto expected = g * g / (1.0 + g * (g + 1.0 / p.filter.q));
            CHECK_THAT (left, WithinAbs (expected, 1.0e-7));
            CHECK (right == 0.0f);
        }
    for (const auto base : { 0.0f, 6.0f, 30.0f })
        for (const auto modulation : { -24.0f, 0.0f, 24.0f })
            for (const auto progress : { std::size_t { 0 }, std::size_t { 3 } })
            {
                ImpactEffects effects;
                effects.prepare (rate);
                ImpactParameters p;
                p.drive.enabled = true;
                p.drive.driveDb = base;
                p.drive.envelopeDb = p.drive.repeatDb = modulation;
                p.attackMs = 0.0f;
                effects.setParameters (p);
                effects.startCut ({});
                settle (effects);
                effects.startCut ({ 1, 0, 0.0, 1.0, progress, 4 });
                float left = 0.1f, right = -0.1f;
                effects.process (left, right);
                const auto totalDb = std::clamp (base + modulation * (1.0 + static_cast<double> (progress) / 3.0), 0.0, 30.0);
                const auto x = 0.1f * std::pow (10.0, totalDb / 20.0);
                CHECK_THAT (left, WithinAbs (std::log (std::cosh (x)) / x, 1.0e-7));
            }
}

TEST_CASE ("Impact transient dynamics react to an onset within an existing cut and preserve stereo ratios", "[impact]")
{
    const auto measure = [] (float attackDb, float sustainDb)
    {
        ImpactEffects effects;
        effects.prepare (rate);
        ImpactParameters p;
        p.transient.enabled = true;
        p.transient.attackDb = attackDb;
        p.transient.sustainDb = sustainDb;
        effects.setParameters (p);
        effects.startCut ({});
        settle (effects, 4096); // The audio onset is long after the cut's AD envelope.
        double onset = 0.0, body = 0.0;
        for (int i = 0; i < 12000; ++i)
        {
            float left = 0.2f, right = 0.05f;
            effects.process (left, right);
            REQUIRE_THAT (left, WithinAbs (4.0 * right, 1.0e-7));
            if (i < 48)
                onset += left;
            if (i >= 11000)
                body += left;
        }
        return std::array<double, 2> { onset / 48, body / 1000 };
    };
    const auto attack = measure (12.0f, 0.0f);
    const auto sustain = measure (0.0f, -12.0f);
    CHECK (attack[0] > 0.5);
    CHECK_THAT (attack[1], WithinAbs (0.2, 1.0e-5));
    CHECK (sustain[0] > 0.14);
    CHECK_THAT (sustain[1], WithinAbs (0.2 * std::pow (10.0, -12.0 / 20.0), 1.0e-5));
}

TEST_CASE ("Impact transient gain stays between its attack and sustain bounds", "[impact]")
{
    for (const auto attackDb : { -18.0f, 18.0f })
        for (const auto sustainDb : { -18.0f, 18.0f })
        {
            ImpactEffects effects;
            effects.prepare (rate);
            ImpactParameters p;
            p.transient.enabled = true;
            p.transient.attackDb = attackDb;
            p.transient.sustainDb = sustainDb;
            effects.setParameters (p);
            effects.startCut ({});
            const auto minimum = std::min (1.0, std::pow (10.0, std::min (attackDb, sustainDb) / 20.0));
            const auto maximum = std::max (1.0, std::pow (10.0, std::max (attackDb, sustainDb) / 20.0));
            for (int i = 0; i < 10000; ++i)
            {
                const auto input = i % 3000 < 1000 ? 0.8f : 0.01f;
                float left = input, right = -0.25f * input;
                effects.process (left, right);
                const auto applied = left / static_cast<double> (input);
                REQUIRE (applied >= minimum - 1.0e-6);
                REQUIRE (applied <= maximum + 1.0e-6);
                REQUIRE_THAT (left, WithinAbs (-4.0 * right, 1.0e-7));
            }
        }
}

TEST_CASE ("Impact reset clears all histories and audio processing allocates no memory", "[impact]")
{
    ImpactEffects first, second;
    first.prepare (rate);
    second.prepare (rate);
    ImpactParameters p;
    p.filter.enabled = p.drive.enabled = p.transient.enabled = true;
    p.filter.envelopeOctaves = 3.0f;
    p.drive.envelopeDb = 12.0f;
    p.transient.attackDb = 9.0f;
    first.setParameters (p);
    second.setParameters (p);
    first.startCut ({});
    for (int i = 0; i < 2000; ++i)
    {
        float left = 0.6f, right = -0.3f;
        first.process (left, right);
    }
    first.reset();
    first.startCut ({});
    second.startCut ({});
    std::size_t allocations = 0;
    bool identical = true;
    {
        livecut::engine::testing::AllocationCounter counter;
        for (int i = 0; i < 10000; ++i)
        {
            auto l1 = static_cast<float> (0.7 * std::sin (0.4 * i)), r1 = -l1;
            auto l2 = l1, r2 = r1;
            first.process (l1, r1);
            second.process (l2, r2);
            identical = identical && l1 == l2 && r1 == r2;
        }
        first.setParameters (p);
        first.startCut ({});
        first.reset();
        allocations = counter.count();
    }
    CHECK (identical);
    CHECK (allocations == 0);
}

TEST_CASE ("Impact states are invariant to host partition boundaries", "[impact]")
{
    ImpactParameters p;
    p.filter.enabled = p.drive.enabled = p.transient.enabled = true;
    p.filter.cutoffHz = 1700.0f;
    p.filter.envelopeOctaves = -3.0f;
    p.drive.envelopeDb = 15.0f;
    p.transient.attackDb = -9.0f;
    const auto render = [&p] (int partition)
    {
        ImpactEffects effects;
        effects.prepare (rate);
        effects.setParameters (p);
        std::vector<float> output (9000);
        for (int block = 0; block < 9000; block += partition)
            for (int i = block; i < std::min (block + partition, 9000); ++i)
            {
                if (i % 1000 == 0)
                    effects.startCut ({ 7, 0, 0.0, i / 1000.0, static_cast<std::size_t> (i / 1000), 9 });
                float left = static_cast<float> (0.4 * std::sin (0.3 * i)), right = 0.5f * left;
                effects.process (left, right);
                output[static_cast<std::size_t> (i)] = left;
            }
        return output;
    };
    CHECK (render (1) == render (17));
    CHECK (render (1) == render (512));
}

TEST_CASE ("Impact extremes remain finite across supported rates and silence remains silent", "[impact]")
{
    for (const auto sampleRate : { 1000.0, 8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0 })
        for (const auto high : { false, true })
        {
            ImpactEffects effects;
            effects.prepare (sampleRate);
            ImpactParameters p;
            p.filter.enabled = p.drive.enabled = p.transient.enabled = true;
            p.filter.cutoffHz = high ? 18000.0f : 30.0f;
            p.filter.q = 8.0f;
            p.filter.envelopeOctaves = high ? 6.0f : -6.0f;
            p.filter.repeatOctaves = high ? 4.0f : -4.0f;
            p.drive.driveDb = 30.0f;
            p.drive.envelopeDb = p.drive.repeatDb = high ? 24.0f : -24.0f;
            p.drive.mode = high ? ImpactParameters::Drive::Mode::wavefold : ImpactParameters::Drive::Mode::drive;
            p.transient.attackDb = p.transient.sustainDb = high ? 18.0f : -18.0f;
            p.transient.timeMs = high ? 200.0f : 10.0f;
            p.attackMs = high ? 25.0f : 0.0f;
            p.decayMs = high ? 1000.0f : 5.0f;
            effects.setParameters (p);
            effects.startCut ({ 2, 4, 0.0, 0.0, 7, 8 });
            for (int i = 0; i < 1000; ++i)
            {
                float left = 0.0f, right = 0.0f;
                effects.process (left, right);
                REQUIRE (left == 0.0f);
                REQUIRE (right == 0.0f);
            }
            for (int i = 0; i < 50000; ++i)
            {
                float left = i % 2 == 0 ? 16.0f : -16.0f;
                float right = static_cast<float> (8.0 * std::sin (0.51 * i));
                effects.process (left, right);
                REQUIRE (std::isfinite (left));
                REQUIRE (std::isfinite (right));
            }
        }
}

TEST_CASE ("Classic filter types are bit-identical", "[impact]")
{
    // Measured on the single-SVF filter before the band type existed.
    const auto render = [] (ImpactParameters::Filter::Type type)
    {
        ImpactEffects effects;
        effects.prepare (rate);
        ImpactParameters p;
        p.filter.enabled = true;
        p.filter.type = type;
        p.filter.cutoffHz = 900.0f;
        p.filter.q = 2.0f;
        p.filter.envelopeOctaves = 2.0f;
        p.filter.repeatOctaves = 1.0f;
        effects.setParameters (p);
        Noise noise;
        Fnv1a hash;
        for (int i = 0; i < 4096; ++i)
        {
            if (i % 512 == 0)
                effects.startCut ({ 0, 0, 0.0, 0.0, static_cast<std::size_t> (i / 512), 8 });
            auto left = noise.next(), right = noise.next();
            effects.process (left, right);
            hash.add (left);
            hash.add (right);
        }
        return hash.value;
    };
    CHECK (render (ImpactParameters::Filter::Type::lowPass) == UINT64_C (0xca23b283d2a00333));
    CHECK (render (ImpactParameters::Filter::Type::bandPass) == UINT64_C (0x0c90f4acbe734b6d));
    CHECK (render (ImpactParameters::Filter::Type::highPass) == UINT64_C (0x7217203d7ef5f2b1));
}

TEST_CASE ("A band filter's edges are where it says", "[impact]")
{
    const auto p = bandParameters (200.0f, 2000.0f);
    for (const auto edge : { 200.0, 2000.0 })
    {
        CAPTURE (edge);
        CHECK_THAT (decibels (preHoldGain (p, edge)), WithinAbs (-3.0, 1.0));
    }
    for (const auto outside : { 50.0, 8000.0 })
    {
        CAPTURE (outside);
        CHECK (decibels (preHoldGain (p, outside)) < -10.0);
    }
}

TEST_CASE ("A band breathes open at each cut", "[impact]")
{
    auto p = bandParameters (300.0f, 3000.0f);
    p.filter.envelopeOctaves = 1.5f;
    p.attackMs = 1.0f;
    p.decayMs = 180.0f;
    ImpactEffects effects;
    effects.prepare (rate);
    effects.setParameters (p);
    effects.startCut ({});
    settle (effects, 48); // The end of the 1 ms attack.
    const auto openLow = effects.diagnostics().bandLowHz;
    const auto openHigh = effects.diagnostics().bandHighHz;
    settle (effects, static_cast<int> (0.18 * rate));
    const auto laterLow = effects.diagnostics().bandLowHz;
    const auto laterHigh = effects.diagnostics().bandHighHz;
    CHECK (openLow < laterLow);
    CHECK (openHigh > laterHigh);
    CHECK_THAT (openLow, WithinAbs (300.0 * std::exp2 (-1.5), 1.0));
    CHECK_THAT (openHigh, WithinAbs (3000.0 * std::exp2 (1.5), 10.0));
}

TEST_CASE ("A band holds its level along Suction's path", "[impact]")
{
    // Pink noise in, measured against the open band's output, so the synthetic
    // noise's energy outside 30 Hz-18 kHz, which every band removes, cancels.
    const auto measure = [] (const ImpactParameters& first, const ImpactParameters& second)
    {
        ImpactEffects effects;
        effects.prepare (rate);
        effects.setParameters (first);
        effects.startCut ({});
        Pink pinkLeft, pinkRight;
        pinkRight.white.state = 0x89abcdefu;
        const auto half = static_cast<int> (0.5 * rate);
        double inputEnergy = 0.0, outputEnergy = 0.0;
        for (int i = 0; i < 6 * half; ++i)
        {
            if (i == half)
                effects.setParameters (second);
            auto left = pinkLeft.next(), right = pinkRight.next();
            const auto inLeft = static_cast<double> (left), inRight = static_cast<double> (right);
            effects.process (left, right);
            if (i >= 2 * half) // 500 ms after the change
            {
                inputEnergy += inLeft * inLeft + inRight * inRight;
                outputEnergy += left * static_cast<double> (left) + right * static_cast<double> (right);
            }
        }
        return decibels (std::sqrt (outputEnergy / inputEnergy));
    };
    const auto open = bandParameters (30.0f, 18000.0f);
    const auto reference = measure (open, open);
    for (const auto x : { 0.0, 0.5, 1.0 })
    {
        for (int step = 0; step <= 10; ++step)
        {
            const auto y = step / 10.0;
            const auto p = suctionBand (x, y);
            const auto width = std::log2 (p.filter.bandHighHz / static_cast<double> (p.filter.bandLowHz));
            const auto capped = std::log2 (18000.0 / 30.0) / width * (0.707 / p.filter.q) >= 4.0;
            const auto steady = measure (p, p) - reference;
            const auto squeezing = measure (open, p) - reference;
            const auto opening = measure (p, open) - reference;
            CAPTURE (x, y, capped, steady, squeezing, opening);
            if (capped) // The band may thin, but by no more than 6 dB.
            {
                CHECK (steady >= -6.0);
                CHECK (steady <= 1.5);
                CHECK (squeezing >= -6.0);
            }
            else
            {
                CHECK_THAT (steady, WithinAbs (0.0, 1.5));
                CHECK_THAT (squeezing, WithinAbs (0.0, 1.5));
            }
            CHECK_THAT (opening, WithinAbs (0.0, 1.5));
        }
    }
}

TEST_CASE ("A band keeps silence silent", "[impact]")
{
    const auto p = breathingSuction (0.5);
    ImpactEffects effects;
    effects.prepare (rate);
    effects.setParameters (p);
    for (int i = 0; i < static_cast<int> (rate); ++i)
    {
        if (i % 6000 == 0)
            effects.startCut ({ 404, 0, 0.0, 0.0, static_cast<std::size_t> (i / 6000), 8 });
        float left = 0.0f, right = 0.0f;
        effects.process (left, right);
        REQUIRE (left == 0.0f);
        REQUIRE (right == 0.0f);
    }
    CHECK (effects.diagnostics().filterActive);
}

TEST_CASE ("A band's makeup never steps", "[impact]")
{
    // The makeup is g = min (2, sqrt (P_open / P (w, q))), so where g < 2,
    // |dg/dw| = g / 2 * |d ln P / dw|, and it is 0 where capped. With q and the
    // targets settled, only the envelope moves the edges, and only outward, so
    // w stays in [w0, w0 + 3]. Each edge opens by envelopeOctaves times the
    // envelope's change, and the clamps and the semitone floor only shrink
    // that, so w moves by at most 2 * 1.5 * |d env| a sample. The attack rises
    // by 1 / 48 a sample at 48 kHz; the decay, and a new cut's restart from
    // env (6000 samples) = 0.0083, move it less. The table's slope in each
    // 1/16-octave cell is a secant of ln P, so it stays within the exact
    // slope's range one cell either side; 5% covers the table's error.
    const auto p = breathingSuction (0.5);
    const auto q = static_cast<double> (p.filter.q);
    const auto settledWidth = std::log2 (p.filter.bandHighHz / static_cast<double> (p.filter.bandLowHz));
    const auto exactGain = [q] (double width)
    {
        return std::min (2.0, std::sqrt (integrateBandPinkPower (bandOpenOctaves, bandOpenQ, 1.0 / 256.0)
                                         / integrateBandPinkPower (width, q, 1.0 / 256.0)));
    };
    constexpr double h = 1.0 / 256.0;
    double steepest = 0.0, exactLowest = 2.0, exactHighest = 0.0;
    for (auto width = settledWidth - 1.0 / 16.0; width <= settledWidth + 3.0 + 1.0 / 16.0; width += h)
    {
        const auto g = exactGain (width);
        steepest = std::max (steepest, std::abs (exactGain (width + h) - g) / h);
        if (width >= settledWidth && width <= settledWidth + 3.0)
        {
            exactLowest = std::min (exactLowest, g);
            exactHighest = std::max (exactHighest, g);
        }
    }
    const auto bound = 1.05 * steepest * 2.0 * 1.5 / 48.0;

    ImpactEffects effects;
    effects.prepare (rate);
    effects.setParameters (p);
    auto previous = 0.0, largest = 0.0, lowest = 2.0, highest = 0.0;
    for (int i = 0; i < static_cast<int> (rate); ++i)
    {
        if (i % 6000 == 0)
            effects.startCut ({ 404, 0, 0.0, 0.0, static_cast<std::size_t> (i / 6000), 8 });
        auto left = static_cast<float> (0.5 * std::sin (2.0 * pi * 99.0 * i / rate)), right = left;
        effects.process (left, right);
        const auto gain = effects.diagnostics().levelHoldGain;
        if (i > 0)
            largest = std::max (largest, std::abs (gain - previous));
        lowest = std::min (lowest, gain);
        highest = std::max (highest, gain);
        previous = gain;
    }
    CAPTURE (largest, bound, lowest, highest, exactLowest, exactHighest);
    // The breathing sweeps the makeup across exactly the range its widths give,
    CHECK_THAT (lowest, WithinRel (exactLowest, 0.01));
    CHECK_THAT (highest, WithinRel (exactHighest, 0.01));
    // and never faster than the bound.
    CHECK (largest <= bound);
}

TEST_CASE ("A band's pink power table matches its integral", "[impact]")
{
    ImpactEffects effects;
    effects.prepare (rate); // builds the table
    const auto powerDecibels = [] (double ratio) { return 10.0 * std::log10 (ratio); };

    // The open band, against a direct integration at 64 times the resolution.
    const auto open = integrateBandPinkPower (bandOpenOctaves, bandOpenQ, 1.0 / 1024.0);
    CHECK_THAT (powerDecibels (bandPinkPower (bandOpenOctaves, bandOpenQ) / open), WithinAbs (0.0, 0.05));
    CHECK_THAT (open, WithinRel (bandOpenOctaves, 0.01)); // A flat band keeps its width in octaves.

    // Off the table's grid, everywhere it reaches.
    for (const auto q : { 0.5, 0.707, 1.3, 2.507, 5.0, 8.0 })
        for (const auto width : { 1.0 / 12.0, 0.1, 0.37, 1.385, 2.9, 4.385, 9.23, 12.49 })
        {
            CAPTURE (q, width);
            const auto direct = integrateBandPinkPower (width, q, 1.0 / 1024.0);
            CHECK_THAT (powerDecibels (bandPinkPower (width, q) / direct), WithinAbs (0.0, 0.05));
        }

    // Wider keeps more, up to a moderate q,
    for (const auto q : { 0.5, 0.707, 1.0, 1.4 })
    {
        CAPTURE (q);
        auto previous = 0.0;
        bool increasing = true;
        for (auto width = 1.0 / 12.0; width <= 12.5; width += 1.0 / 64.0)
        {
            const auto power = bandPinkPower (width, q);
            increasing = increasing && power > previous;
            previous = power;
        }
        CHECK (increasing);
    }
    // but not above about q 2, where the two resonant edges overlap in a
    // narrow band and reinforce each other.
    CHECK (bandPinkPower (1.0 / 12.0, 2.507) > bandPinkPower (2.0, 2.507));
}

TEST_CASE ("A band's peak stays within twice its resonance", "[impact]")
{
    // The pad's tightest band, breathing with cuts at Suction's 16ths. Its
    // resonant peak with no makeup is the largest band output over the sweep.
    for (const auto x : { 0.0, 0.5, 1.0 })
    {
        CAPTURE (x);
        const auto p = breathingSuction (x);
        std::vector<double> frequencies { 30.0, 18000.0, p.filter.bandLowHz, p.filter.bandHighHz, 99.0 };
        for (int i = 0; i <= 24; ++i)
            frequencies.push_back (30.0 * std::pow (600.0, i / 24.0));
        double resonance = 0.0;
        std::vector<double> peaks;
        for (const auto frequency : frequencies)
        {
            ImpactEffects effects;
            effects.prepare (rate);
            effects.setParameters (p);
            double peak = 0.0;
            for (int i = 0; i < 24000; ++i)
            {
                if (i % 6000 == 0)
                    effects.startCut ({ 404, 0, 0.0, 0.0, static_cast<std::size_t> (i / 6000), 8 });
                auto left = static_cast<float> (std::sin (2.0 * pi * frequency * i / rate)), right = left;
                effects.process (left, right);
                REQUIRE (std::isfinite (left));
                peak = std::max (peak, static_cast<double> (std::abs (left)));
                resonance = std::max (resonance, std::abs (left / effects.diagnostics().levelHoldGain));
            }
            peaks.push_back (peak);
        }
        for (std::size_t i = 0; i < frequencies.size(); ++i)
        {
            CAPTURE (frequencies[i], peaks[i], resonance);
            CHECK (peaks[i] <= 2.0 * resonance);
        }
    }
}

TEST_CASE ("A band at extreme rates stays finite", "[impact]")
{
    for (const auto sampleRate : { 8000.0, 384000.0 })
        for (const auto high : { false, true })
        {
            CAPTURE (sampleRate, high);
            ImpactEffects effects;
            effects.prepare (sampleRate);
            auto p = high ? bandParameters (18000.0f, 18000.0f, 8.0f) : bandParameters (30.0f, 30.0f, 8.0f);
            p.filter.envelopeOctaves = high ? 6.0f : -6.0f;
            p.filter.repeatOctaves = high ? 4.0f : -4.0f;
            p.attackMs = 0.0f;
            effects.setParameters (p);
            effects.startCut ({ 2, 4, 0.0, 0.0, 7, 8 });
            for (int i = 0; i < 50000; ++i)
            {
                float left = i % 2 == 0 ? 16.0f : -16.0f;
                float right = static_cast<float> (8.0 * std::sin (0.51 * i));
                effects.process (left, right);
                REQUIRE (std::isfinite (left));
                REQUIRE (std::isfinite (right));
                const auto& d = effects.diagnostics();
                REQUIRE (d.bandLowHz >= 30.0);
                REQUIRE (d.bandHighHz <= 0.45 * sampleRate);
                REQUIRE (d.bandLowHz < d.bandHighHz);
            }
        }
}

TEST_CASE ("A band glides when its edges jump", "[impact]")
{
    const auto largestStep = [] (bool jump)
    {
        ImpactEffects effects;
        effects.prepare (rate);
        effects.setParameters (bandParameters (200.0f, 2000.0f));
        effects.startCut ({});
        const auto change = static_cast<int> (0.5 * rate);
        float previous = 0.0f, largest = 0.0f;
        for (int i = 0; i < 2 * change; ++i)
        {
            if (jump && i == change)
                effects.setParameters (bandParameters (1000.0f, 1200.0f));
            // A loud tone the new band rejects: without the glide, the jump
            // steps the high-pass states' output by several times its slew.
            const auto input = static_cast<float> (0.8 * std::sin (2.0 * pi * 100.0 * i / rate));
            auto left = input, right = input;
            effects.process (left, right);
            if (i >= change)
                largest = std::max (largest, std::abs (left - previous));
            previous = left;
        }
        return largest;
    };
    CHECK (largestStep (true) <= 1.5f * largestStep (false));
}

TEST_CASE ("Crossed band edges hold a semitone apart", "[impact]")
{
    ImpactEffects effects;
    effects.prepare (rate);
    effects.setParameters (bandParameters (1000.0f, 900.0f));
    effects.startCut ({});
    settle (effects, 256);
    CHECK_THAT (effects.diagnostics().bandLowHz, WithinAbs (1000.0, 1.0e-6));
    CHECK_THAT (effects.diagnostics().bandHighHz, WithinAbs (1000.0 * std::exp2 (1.0 / 12.0), 1.0e-6));
}

TEST_CASE ("A band resets cleanly and allocates no memory", "[impact]")
{
    auto p = bandParameters (400.0f, 2500.0f, 2.0f);
    p.filter.envelopeOctaves = 1.5f;
    ImpactEffects first, second;
    first.prepare (rate);
    second.prepare (rate);
    first.setParameters (p);
    second.setParameters (p);
    first.startCut ({});
    first.setParameters (bandParameters (900.0f, 1000.0f, 3.0f));
    settle (first, 100);
    for (int i = 0; i < 4000; ++i)
    {
        auto left = static_cast<float> (0.6 * std::sin (0.2 * i)), right = -left;
        first.process (left, right);
    }
    const auto clearedDiagnostics = [&first]
    {
        const auto& d = first.diagnostics();
        return d.levelHoldGain == 1.0 && d.bandLowHz == 0.0 && d.bandHighHz == 0.0;
    };
    REQUIRE (first.diagnostics().levelHoldGain != 1.0);
    REQUIRE (! clearedDiagnostics());
    auto silenced = bandParameters (900.0f, 1000.0f, 3.0f);
    silenced.filter.chance = 0.0f;
    first.setParameters (silenced);
    first.startCut ({});
    settle (first, 96); // The 2 ms wet ramp reaches 0.
    CHECK (clearedDiagnostics());
    first.setParameters (bandParameters (900.0f, 1000.0f, 3.0f));
    first.startCut ({});
    settle (first, 200);
    auto classic = p;
    classic.filter.type = ImpactParameters::Filter::Type::highPass;
    first.setParameters (classic);
    first.setParameters (p);
    CHECK (clearedDiagnostics());
    first.reset();
    CHECK (clearedDiagnostics());
    first.setParameters (p);
    first.startCut ({});
    second.startCut ({});
    bool identical = true;
    std::size_t allocations = 0;
    {
        livecut::engine::testing::AllocationCounter counter;
        for (int i = 0; i < 10000; ++i)
        {
            auto l1 = static_cast<float> (0.7 * std::sin (0.04 * i)), r1 = 0.5f * l1;
            auto l2 = l1, r2 = r1;
            first.process (l1, r1);
            second.process (l2, r2);
            identical = identical && l1 == l2 && r1 == r2;
        }
        allocations = counter.count();
    }
    CHECK (identical);
    CHECK (allocations == 0);
}

TEST_CASE ("A band switched on mid-cut starts fresh", "[impact]")
{
    // The fresh instance runs band on silence up to the switch, so its wet mix
    // and envelope match while its band state, followers and glide stay clean.
    const auto band = bandParameters (300.0f, 3000.0f, 2.0f);
    auto classic = band;
    classic.filter.type = ImpactParameters::Filter::Type::lowPass;
    classic.filter.cutoffHz = 500.0f;
    for (const auto staleBand : { false, true })
    {
        CAPTURE (staleBand);
        ImpactEffects switched, fresh;
        switched.prepare (rate);
        fresh.prepare (rate);
        switched.setParameters (staleBand ? bandParameters (100.0f, 8000.0f, 4.0f) : classic);
        fresh.setParameters (band);
        switched.startCut ({});
        fresh.startCut ({});
        const auto signal = [] (int i) { return static_cast<float> (0.5 * std::sin (0.07 * i) + 0.2 * std::sin (0.9 * i)); };
        for (int i = 0; i < 4000; ++i)
        {
            if (staleBand && i == 2000)
                switched.setParameters (classic);
            auto left = signal (i), right = -0.5f * left;
            switched.process (left, right);
            float silentLeft = 0.0f, silentRight = 0.0f;
            fresh.process (silentLeft, silentRight);
        }
        switched.setParameters (band);
        bool identical = true;
        for (int i = 4000; i < 8000; ++i)
        {
            auto l1 = signal (i), r1 = -0.5f * l1;
            auto l2 = l1, r2 = r1;
            switched.process (l1, r1);
            fresh.process (l2, r2);
            identical = identical && l1 == l2 && r1 == r2;
        }
        CHECK (identical);
    }
}

TEST_CASE ("A filter switched off keeps its shape while it fades", "[impact]")
{
    // At the next cut the wet ramps out over 2 ms. Meanwhile the band runs on
    // exactly as if nothing had changed, not even that cut, which would open
    // it: the output is the input plus the ramp times the unchanged band's wet
    // difference. Then the new parameters take over.
    const auto band = breathingSuction (0.5);
    auto off = band; // Cut's Impact: off, with every other filter setting changed too
    off.filter = ImpactParameters::Filter {};
    ImpactEffects switched, unchanged;
    for (auto* effects : { &switched, &unchanged })
    {
        effects->prepare (rate);
        effects->setParameters (band);
        effects->startCut ({ 404, 0, 0.0, 0.0, 0, 8 });
    }
    const auto signal = [] (int i)
    {
        return static_cast<float> (0.6 * std::sin (2.0 * pi * 99.0 * i / rate) + 0.2 * std::sin (0.9 * i));
    };
    int i = 0;
    for (; i < 6000; ++i)
    {
        auto l1 = signal (i), r1 = -0.5f * l1, l2 = l1, r2 = r1;
        switched.process (l1, r1);
        unchanged.process (l2, r2);
        REQUIRE (l1 == l2);
    }
    switched.setParameters (off);
    switched.startCut ({ 404, 0, 0.0, 0.0, 1, 8 });
    CHECK_FALSE (switched.diagnostics().filterActive);
    double largestError = 0.0;
    bool sameEdges = true;
    for (int k = 0; k < 96; ++k, ++i)
    {
        const auto input = signal (i);
        auto l1 = input, r1 = -0.5f * input, l2 = l1, r2 = r1;
        switched.process (l1, r1);
        unchanged.process (l2, r2);
        const auto ramp = 1.0 - (k + 1) / 96.0;
        const auto expected = input + ramp * (static_cast<double> (l2) - input);
        largestError = std::max (largestError, std::abs (l1 - expected));
        if (k < 95)
            sameEdges = sameEdges && switched.diagnostics().bandLowHz == unchanged.diagnostics().bandLowHz;
    }
    CHECK (largestError < 1.0e-6);
    CHECK (sameEdges);

    // Faded out, it is exactly bypassed, and the new shape is what runs next.
    for (int k = 0; k < 100; ++k, ++i)
    {
        auto left = signal (i), right = -0.5f * left;
        const auto inLeft = left, inRight = right;
        switched.process (left, right);
        REQUIRE (left == inLeft);
        REQUIRE (right == inRight);
    }
    auto lowPass = off;
    lowPass.filter.enabled = true;
    switched.setParameters (lowPass);
    switched.startCut ({ 404, 0, 0.0, 0.0, 2, 8 });
    settle (switched, 200);
    CHECK (switched.diagnostics().filterActive);
    CHECK (switched.diagnostics().bandLowHz == 0.0); // a low-pass now, not the band
}
