// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ImpactEffects.h"
#include "engine/PadPrograms.h"
#include "AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cmath>
#include <limits>
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

// Suction's band from the pad's own mapping, breathing as the pad plays it,
// so a retuned curve retunes these tests too. y 1 is its tightest band.
ImpactParameters suction (double x, double y = 1.0)
{
    return mapPad (PadProgram::suction, static_cast<float> (x), static_cast<float> (y), {}).impact;
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

TEST_CASE ("Disabling a Classic filter at a cut is unchanged", "[impact]")
{
    // Measured on the ImpactEffects of 069eecb, before a disabled band held its
    // shape: switched off at cut 4 with its cutoff changed too, a Classic
    // filter adopts the new parameters at once, as it always did.
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
            {
                if (i == 4 * 512)
                {
                    auto off = p;
                    off.filter.enabled = false;
                    off.filter.cutoffHz = 3000.0f;
                    effects.setParameters (off);
                }
                effects.startCut ({ 0, 0, 0.0, 0.0, static_cast<std::size_t> (i / 512), 8 });
            }
            auto left = noise.next(), right = noise.next();
            effects.process (left, right);
            hash.add (left);
            hash.add (right);
        }
        return hash.value;
    };
    CHECK (render (ImpactParameters::Filter::Type::lowPass) == UINT64_C (0xfb2a92b00ee0be24));
    CHECK (render (ImpactParameters::Filter::Type::bandPass) == UINT64_C (0x03253cb975c2908a));
    CHECK (render (ImpactParameters::Filter::Type::highPass) == UINT64_C (0x9471151b3bcd46e7));
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
            const auto p = suction (x, y);
            const auto width = std::log2 (p.filter.bandHighHz / static_cast<double> (p.filter.bandLowHz));
            const auto capped = detail::integrateBandPinkPower (detail::bandOpenOctaves, detail::bandOpenQ, detail::bandPowerStep)
                                    / detail::bandPinkPower (width, p.filter.q) >= 4.0; // the makeup's +6 dB cap
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
    const auto p = suction (0.5);
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
    const auto p = suction (0.5);
    const auto q = static_cast<double> (p.filter.q);
    const auto settledWidth = std::log2 (p.filter.bandHighHz / static_cast<double> (p.filter.bandLowHz));
    const auto exactGain = [q] (double width)
    {
        return std::min (2.0, std::sqrt (detail::integrateBandPinkPower (detail::bandOpenOctaves, detail::bandOpenQ, 1.0 / 256.0)
                                         / detail::integrateBandPinkPower (width, q, 1.0 / 256.0)));
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
    const auto open = detail::integrateBandPinkPower (detail::bandOpenOctaves, detail::bandOpenQ, 1.0 / 1024.0);
    CHECK_THAT (powerDecibels (detail::bandPinkPower (detail::bandOpenOctaves, detail::bandOpenQ) / open), WithinAbs (0.0, 0.05));
    CHECK_THAT (open, WithinRel (detail::bandOpenOctaves, 0.01)); // A flat band keeps its width in octaves.

    // Off the table's grid, everywhere it reaches.
    for (const auto q : { 0.5, 0.707, 1.3, 2.507, 5.0, 8.0 })
        for (const auto width : { 1.0 / 12.0, 0.1, 0.37, 1.385, 2.9, 4.385, 9.23, 12.49 })
        {
            CAPTURE (q, width);
            const auto direct = detail::integrateBandPinkPower (width, q, 1.0 / 1024.0);
            CHECK_THAT (powerDecibels (detail::bandPinkPower (width, q) / direct), WithinAbs (0.0, 0.05));
        }

    // Wider keeps more, up to a moderate q,
    for (const auto q : { 0.5, 0.707, 1.0, 1.4 })
    {
        CAPTURE (q);
        auto previous = 0.0;
        bool increasing = true;
        for (auto width = 1.0 / 12.0; width <= 12.5; width += 1.0 / 64.0)
        {
            const auto power = detail::bandPinkPower (width, q);
            increasing = increasing && power > previous;
            previous = power;
        }
        CHECK (increasing);
    }
    // but not above about q 2, where the two resonant edges overlap in a
    // narrow band and reinforce each other.
    CHECK (detail::bandPinkPower (1.0 / 12.0, 2.507) > detail::bandPinkPower (2.0, 2.507));
}

TEST_CASE ("A band's peak inside it is its resonance times its makeup", "[impact]")
{
    // The pad's tightest band, breathing with cuts at Suction's 16ths, on a
    // full-scale sine inside it. Its peak is at most the band's own gain at
    // that frequency with no makeup, the largest over the breathing, times the
    // largest makeup the breathing gives, both computed here rather than read
    // from the band. The 10% is the sine's onset ringing at the resonant edges
    // (measured up to 5%). At the top of the pad the overlapping edges keep
    // more pink power than the open band, so the makeup cuts, to about 0.86.
    // The Simper SVF is the bilinear transform of the analogue prototype, so
    // its steady gain is the prototype's at the prewarped frequency.
    const auto bandGain = [] (double low, double high, double q, double frequency)
    {
        const auto t = std::tan (pi * frequency / rate), k = 1.0 / q;
        const auto w1 = t / std::tan (pi * low / rate), w2 = t / std::tan (pi * high / rate);
        return w1 * w1 / std::hypot (1.0 - w1 * w1, k * w1) / std::hypot (1.0 - w2 * w2, k * w2);
    };
    for (const auto x : { 0.0, 0.5, 1.0 })
    {
        CAPTURE (x);
        const auto p = suction (x);
        const auto low = static_cast<double> (p.filter.bandLowHz), high = static_cast<double> (p.filter.bandHighHz);
        const auto q = static_cast<double> (p.filter.q), opening = static_cast<double> (p.filter.envelopeOctaves);
        REQUIRE (low * std::exp2 (-opening) >= 30.0); // so the breathing opens each edge in full
        REQUIRE (high * std::exp2 (opening) <= 18000.0);
        const auto open = detail::integrateBandPinkPower (detail::bandOpenOctaves, detail::bandOpenQ, 1.0 / 64.0);
        double makeup = 0.0;
        for (auto width = std::log2 (high / low); width <= std::log2 (high / low) + 2.0 * opening; width += 1.0 / 32.0)
            makeup = std::max (makeup, std::min (2.0, std::sqrt (open / detail::integrateBandPinkPower (width, q, 1.0 / 64.0))));
        for (int n = 0; n <= 8; ++n)
        {
            const auto frequency = low * std::pow (high / low, n / 8.0);
            double gain = 0.0;
            for (int step = 0; step <= 150; ++step) // the breathing, 1/100 octave at a time
            {
                const auto o = opening * step / 150.0;
                gain = std::max (gain, bandGain (low * std::exp2 (-o), high * std::exp2 (o), q, frequency));
            }
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
            }
            CAPTURE (frequency, peak, gain, makeup);
            CHECK (peak <= 1.1 * makeup * gain);
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
    // Switched to a Classic type, the band runs on until it fades at the next
    // cut, and then it is cleared for good.
    auto classic = p;
    classic.filter.type = ImpactParameters::Filter::Type::highPass;
    first.setParameters (classic);
    settle (first, 100);
    CHECK_FALSE (clearedDiagnostics());
    first.startCut ({});
    settle (first, 96);
    CHECK (clearedDiagnostics());
    first.startCut ({});
    settle (first, 200);
    CHECK (first.diagnostics().filterActive);
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

TEST_CASE ("A band switched on starts fresh", "[impact]")
{
    // Switched on while another filter sounds, the band waits for it to fade
    // at the next cut, then comes in at the cut after that. The fresh instance
    // runs a disabled band on silence until that cut, so its wet mix and
    // envelope match while its band state and glide stay clean. The stale band
    // fades out at cut 1, leaving the low-pass to sound from cut 2.
    constexpr int cut = 2000;
    const auto band = bandParameters (300.0f, 3000.0f, 2.0f);
    auto classic = band;
    classic.filter.type = ImpactParameters::Filter::Type::lowPass;
    classic.filter.cutoffHz = 500.0f;
    auto disabled = band;
    disabled.filter.enabled = false;
    const auto signal = [] (int i) { return static_cast<float> (0.5 * std::sin (0.07 * i) + 0.2 * std::sin (0.9 * i)); };
    for (const auto staleBand : { false, true })
    {
        CAPTURE (staleBand);
        ImpactEffects switched, fresh;
        switched.prepare (rate);
        fresh.prepare (rate);
        switched.setParameters (staleBand ? bandParameters (100.0f, 8000.0f, 4.0f) : classic);
        fresh.setParameters (disabled);
        bool identical = true;
        for (int i = 0; i < 6 * cut; ++i)
        {
            const auto index = i / cut;
            if (i % cut == 0)
            {
                if (staleBand && index == 1)
                    switched.setParameters (classic);
                if (index == 3)
                    switched.setParameters (band);
                if (index == 4)
                    fresh.setParameters (band);
                const CutIdentity identity { 404, 0, 0.0, 0.0, static_cast<std::size_t> (index), 8 };
                switched.startCut (identity);
                fresh.startCut (identity);
            }
            if (i == 3 * cut - 1) // the low-pass sounds just before the switch
            {
                REQUIRE (switched.diagnostics().filterActive);
                REQUIRE (switched.diagnostics().bandLowHz == 0.0);
            }
            auto l1 = signal (i), r1 = -0.5f * l1;
            auto l2 = index < 4 ? 0.0f : l1, r2 = index < 4 ? 0.0f : r1;
            switched.process (l1, r1);
            fresh.process (l2, r2);
            if (index >= 4)
                identical = identical && l1 == l2 && r1 == r2;
        }
        CHECK (identical);
        CHECK (switched.diagnostics().bandLowHz > 0.0);
    }
}

TEST_CASE ("A band switched off keeps its shape while it fades", "[impact]")
{
    // At the next cut the wet ramps out over 2 ms. Meanwhile the band runs on
    // exactly as if nothing had changed, not even that cut, which would open
    // it: the output is the input plus the ramp times the unchanged band's wet
    // difference. Then the new parameters take over.
    const auto band = suction (0.5);
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

namespace
{
struct TypeSwitch
{
    double largestError = 0.0; // against the unswitched twin times the ramp
    bool sameShape = true;
    float switchedStep = 0.0f, fromStep = 0.0f, toStep = 0.0f;
};

// Switches `from` to `to`, both enabled, at cut 1, mid-sound at full wet, with
// cuts every 6000 samples as at Suction's 16ths. The largest steps are over a
// window of 2400 samples either side of cuts 1 and 2.
TypeSwitch switchType (const ImpactParameters& from, const ImpactParameters& to)
{
    constexpr int cut = 6000, window = 2400;
    const auto signal = [] (int i)
    {
        return static_cast<float> (0.5 * std::sin (2.0 * pi * 700.0 * i / rate) + 0.3 * std::sin (2.0 * pi * 99.0 * i / rate));
    };
    const auto identity = [] (int i) { return CutIdentity { 404, 0, 0.0, 0.0, static_cast<std::size_t> (i / cut), 8 }; };
    const auto largestStep = [&] (const ImpactParameters& first, const ImpactParameters* second)
    {
        ImpactEffects effects;
        effects.prepare (rate);
        effects.setParameters (first);
        float previous = 0.0f, largest = 0.0f;
        for (int i = 0; i < 3 * cut; ++i)
        {
            if (i % cut == 0)
            {
                if (second != nullptr && i == cut)
                    effects.setParameters (*second);
                effects.startCut (identity (i));
            }
            auto left = signal (i), right = -0.5f * left;
            effects.process (left, right);
            if (i > cut - window && i < 2 * cut + window)
                largest = std::max (largest, std::abs (left - previous));
            previous = left;
        }
        return largest;
    };
    TypeSwitch result;
    result.switchedStep = largestStep (from, &to);
    result.fromStep = largestStep (from, nullptr);
    result.toStep = largestStep (to, nullptr);

    // While its wet ramps out, the sounding filter runs on as if nothing had
    // changed, not even the cut, which would restart its envelope.
    ImpactEffects switched, unchanged;
    for (auto* effects : { &switched, &unchanged })
    {
        effects->prepare (rate);
        effects->setParameters (from);
        effects->startCut (identity (0));
    }
    int i = 0;
    for (; i < cut; ++i)
    {
        auto l1 = signal (i), r1 = -0.5f * l1, l2 = l1, r2 = r1;
        switched.process (l1, r1);
        unchanged.process (l2, r2);
    }
    switched.setParameters (to);
    switched.startCut (identity (i));
    result.sameShape = ! switched.diagnostics().filterActive;
    for (int k = 0; k < 96; ++k, ++i)
    {
        const auto input = signal (i);
        auto l1 = input, r1 = -0.5f * input, l2 = l1, r2 = r1;
        switched.process (l1, r1);
        unchanged.process (l2, r2);
        const auto ramp = 1.0 - (k + 1) / 96.0;
        result.largestError = std::max (result.largestError, std::abs (l1 - (input + ramp * (static_cast<double> (l2) - input))));
        if (k < 95)
            result.sameShape = result.sameShape && switched.diagnostics().bandLowHz == unchanged.diagnostics().bandLowHz;
    }
    return result;
}

ImpactParameters suctionLowPass()
{
    // A Classic low-pass, sharing Suction's attack and decay.
    auto p = suction (0.5);
    p.filter = ImpactParameters::Filter {};
    p.filter.enabled = true;
    p.filter.cutoffHz = 2000.0f;
    p.filter.q = 2.0f;
    p.filter.envelopeOctaves = 2.0f;
    return p;
}

// After the switch, the new type comes in at the next cut.
void checkTakesOver (const ImpactParameters& from, const ImpactParameters& to, bool band)
{
    ImpactEffects effects;
    effects.prepare (rate);
    effects.setParameters (from);
    effects.startCut ({ 404, 0, 0.0, 0.0, 0, 8 });
    settle (effects, 6000);
    effects.setParameters (to);
    effects.startCut ({ 404, 0, 0.0, 0.0, 1, 8 });
    settle (effects, 6000);
    CHECK_FALSE (effects.diagnostics().filterActive);
    effects.startCut ({ 404, 0, 0.0, 0.0, 2, 8 });
    settle (effects, 200);
    CHECK (effects.diagnostics().filterActive);
    CHECK ((effects.diagnostics().bandLowHz > 0.0) == band);
}
} // namespace

TEST_CASE ("A low-pass switched to a band keeps its shape while it fades", "[impact]")
{
    // The low-pass fades out whole at the next cut, and the band comes in at
    // the cut after. Adopted at once, the switch cleared the high-pass state
    // the two share, mid-sound.
    const auto lowPass = suctionLowPass();
    const auto band = suction (0.5);
    const auto result = switchType (lowPass, band);
    CAPTURE (result.largestError, result.switchedStep, result.fromStep, result.toStep);
    CHECK (result.largestError < 1.0e-6);
    CHECK (result.sameShape);
    CHECK (result.switchedStep <= 3.0f * std::max (result.fromStep, result.toStep));
    checkTakesOver (lowPass, band, true);
}

TEST_CASE ("A band switched to a low-pass keeps its shape while it fades", "[impact]")
{
    // A Pad to Classic switch: the band fades out whole, makeup included.
    // Adopted at once, the low-pass ran on the band's high-pass state, without
    // the makeup.
    const auto lowPass = suctionLowPass();
    const auto band = suction (0.5);
    const auto result = switchType (band, lowPass);
    CAPTURE (result.largestError, result.switchedStep, result.fromStep, result.toStep);
    CHECK (result.largestError < 1.0e-6);
    CHECK (result.sameShape);
    CHECK (result.switchedStep <= 3.0f * std::max (result.fromStep, result.toStep));
    checkTakesOver (band, lowPass, false);
}

TEST_CASE ("An open band's breathing doesn't dip the level at high rates", "[impact]")
{
    // At 96 kHz each cut opens the bottom of the pad's high edge to 43 kHz,
    // where material has no power; counted in the makeup's width, those
    // octaves dipped the level about 0.5 dB at every cut. Pink noise below
    // 20 kHz, through the breathing band and the same band held still.
    constexpr double highRate = 96000.0;
    constexpr int cut = 12000; // 16ths at 120 bpm
    // An eighth-order Butterworth low-pass at 19 kHz, from four RBJ biquads.
    struct Biquad
    {
        double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0, z1 = 0.0, z2 = 0.0;
        Biquad (double frequency, double q)
        {
            const auto w = 2.0 * pi * frequency / highRate, alpha = std::sin (w) / (2.0 * q), a0 = 1.0 + alpha;
            b0 = b2 = (1.0 - std::cos (w)) / (2.0 * a0);
            b1 = (1.0 - std::cos (w)) / a0;
            a1 = -2.0 * std::cos (w) / a0;
            a2 = (1.0 - alpha) / a0;
        }
        double process (double x)
        {
            const auto y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    std::vector<Biquad> limit;
    for (const auto q : { 0.5098, 0.6013, 0.9000, 2.5629 })
        limit.emplace_back (19000.0, q);
    Pink pink;

    const auto breathing = suction (0.5, 0.0);
    auto still = breathing;
    still.filter.envelopeOctaves = 0.0f;
    ImpactEffects moving, held;
    moving.prepare (highRate);
    held.prepare (highRate);
    moving.setParameters (breathing);
    held.setParameters (still);
    auto deepest = std::numeric_limits<double>::infinity(), movingEnergy = 0.0, heldEnergy = 0.0;
    for (int i = 0; i < 24 * cut; ++i)
    {
        if (i % cut == 0)
        {
            if (i > 0) // the level over each cut's first 20 ms, where it breathes widest
                deepest = std::min (deepest, decibels (std::sqrt (movingEnergy / heldEnergy)));
            movingEnergy = heldEnergy = 0.0;
            const CutIdentity identity { 404, 0, 0.0, 0.0, static_cast<std::size_t> (i / cut), 16 };
            moving.startCut (identity);
            held.startCut (identity);
        }
        auto x = static_cast<double> (pink.next());
        for (auto& stage : limit)
            x = stage.process (x);
        auto l1 = static_cast<float> (x), r1 = l1, l2 = l1, r2 = l1;
        moving.process (l1, r1);
        held.process (l2, r2);
        if (i % cut < static_cast<int> (0.02 * highRate))
        {
            movingEnergy += l1 * static_cast<double> (l1);
            heldEnergy += l2 * static_cast<double> (l2);
        }
    }
    CAPTURE (deepest);
    CHECK (deepest > -0.15);
}
