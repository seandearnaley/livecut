// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/ImpactEffects.h"
#include "AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

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
