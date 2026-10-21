// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/FrequencyEffects.h"
#include "AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

using namespace livecut::engine;

namespace
{
constexpr double pi = std::numbers::pi;
struct Spectrum
{
    std::array<double, 3> amplitude {};
    double maximum = 0.0;
    bool finite = true;
};

Spectrum spectrum (FrequencyParameters parameters, double rate, double inputHz,
                   std::array<double, 3> measuredHz, double envelope = 0.0,
                   double repeat = 0.0)
{
    FrequencyEffects effects;
    effects.prepare (rate);
    effects.setParameters (parameters);
    std::array<double, 3> real {}, imag {};
    const auto count = static_cast<int> (rate);
    Spectrum result;
    for (int frame = 0; frame < 2 * count; ++frame)
    {
        const auto input = static_cast<float> (.5 * std::cos (2 * pi * inputHz * frame / rate));
        auto left = input, right = input;
        effects.process (left, right, envelope, repeat);
        result.finite &= std::isfinite (left) && left == right;
        result.maximum = std::max (result.maximum, std::abs (static_cast<double> (left)));
        if (frame < count)
            continue;
        for (std::size_t bin = 0; bin < measuredHz.size(); ++bin)
        {
            const auto phase = 2 * pi * measuredHz[bin] * frame / rate;
            real[bin] += left * std::cos (phase);
            imag[bin] += left * std::sin (phase);
        }
    }
    for (std::size_t bin = 0; bin < real.size(); ++bin)
        result.amplitude[bin] = 2 * std::hypot (real[bin], imag[bin]) / count;
    return result;
}

FrequencyParameters enabled (FrequencyParameters::Mode mode, float rate)
{
    FrequencyParameters parameters;
    parameters.enabled = true;
    parameters.mode = mode;
    parameters.rateHz = rate;
    return parameters;
}
} // namespace

TEST_CASE ("Frequency Shift translates one sideband across production sample rates", "[frequency][spectrum]")
{
    const auto rate = GENERATE (8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0);
    const auto input = GENERATE (20.0, 1000.0);
    const auto shift = GENERATE (-73.0f, 173.0f);
    const auto result = spectrum (enabled (FrequencyParameters::Mode::frequencyShift, shift), rate,
                                  input, { std::abs (input + shift), std::abs (input - shift), input });
    CHECK (result.finite);
    CHECK (std::abs (result.amplitude[0] - .5) < 1.0e-5);
    CHECK (result.amplitude[1] < result.amplitude[0] * .001); // >60 dB image rejection
    CHECK (result.amplitude[2] < 1.0e-5);
}

TEST_CASE ("Ring modulation preserves both sidebands and uses absolute rate", "[frequency][ring]")
{
    const auto sign = GENERATE (-1.0f, 1.0f);
    const auto result = spectrum (enabled (FrequencyParameters::Mode::ring, sign * 173.0f),
                                  48000.0, 1000.0, { 1173.0, 827.0, 1000.0 });
    CHECK (result.finite);
    CHECK (std::abs (result.amplitude[0] - .25) < 1.0e-5);
    CHECK (std::abs (result.amplitude[1] - .25) < 1.0e-5);
    CHECK (result.amplitude[2] < 1.0e-5);
}

TEST_CASE ("Frequency modulation uses the passed envelope and repeat context with a bounded sum", "[frequency][context]")
{
    const auto ring = GENERATE (false, true);
    const auto mode = ring ? FrequencyParameters::Mode::ring : FrequencyParameters::Mode::frequencyShift;
    auto parameters = enabled (mode, -100.0f);
    parameters.envelopeHz = 200.0f;
    parameters.repeatHz = 300.0f;
    const auto combined = spectrum (parameters, 48000.0, 1000.0, { 1100.0, 900.0, 1000.0 }, .25, .5);
    CHECK (combined.finite);
    CHECK (std::abs (combined.amplitude[0] - (ring ? .25 : .5)) < 1.0e-5);
    CHECK (std::abs (combined.amplitude[1] - (ring ? .25 : 0.0)) < 1.0e-5);
    parameters.rateHz = parameters.envelopeHz = parameters.repeatHz = 2000.0f;
    const auto clamped = spectrum (parameters, 48000.0, 1000.0, { 3000.0, 1000.0, 7000.0 }, 1, 1);
    CHECK (clamped.finite);
    CHECK (std::abs (clamped.amplitude[0] - (ring ? .25 : .5)) < 1.0e-5);
    CHECK (clamped.amplitude[2] < 1.0e-5);
}

TEST_CASE ("Zero mix, disabled modules and settled zero rate preserve exact dry samples", "[frequency][neutral]")
{
    const auto rate = GENERATE (1000.0, 44100.0, 48000.0, 192000.0);
    const auto neutral = GENERATE (0, 1, 2, 3);
    FrequencyEffects effects;
    effects.prepare (rate);
    auto parameters = enabled (FrequencyParameters::Mode::frequencyShift, 173.0f);
    if (neutral == 0) parameters.enabled = false;
    if (neutral == 1) parameters.mix = 0.0f;
    if (neutral == 2) parameters.rateHz = 0.0f;
    effects.setParameters (parameters);
    bool exact = true;
    for (int frame = 0; frame < 2000; ++frame)
    {
        auto left = static_cast<float> (std::sin (frame * .73)), right = -.7f * left;
        const auto oldLeft = left, oldRight = right;
        effects.process (left, right, 0, 0, neutral != 3);
        exact &= left == oldLeft && right == oldRight;
    }
    CHECK (exact);
    parameters.enabled = true;
    parameters.mix = 1.0f;
    parameters.rateHz = 173.0f;
    effects.setParameters (parameters);
    for (int frame = 0; frame < 2000; ++frame) { float left = .25f, right = .25f; effects.process (left, right); }
    parameters.rateHz = 0.0f;
    effects.setParameters (parameters);
    for (int frame = 0; frame < static_cast<int> (std::ceil (.002 * rate)); ++frame) { float left = .25f, right = .25f; effects.process (left, right); }
    float left = .37f, right = -.19f;
    effects.process (left, right);
    CHECK (left == .37f);
    CHECK (right == -.19f);
}

TEST_CASE ("Frequency Shift documents DC carrier gain and Nyquist folding", "[frequency][edges]")
{
    const auto rate = GENERATE (8000.0, 48000.0, 192000.0);
    const auto mode = GENERATE (FrequencyParameters::Mode::ring, FrequencyParameters::Mode::frequencyShift);
    const auto ring = mode == FrequencyParameters::Mode::ring;
    const auto dc = spectrum (enabled (mode, 173), rate, 0, { 173, 0, 346 });
    CHECK (dc.finite);
    CHECK (std::abs (dc.amplitude[0] - (ring ? .5 : std::sqrt (.5))) < 1.0e-5);
    const auto alias = spectrum (enabled (mode, 2000), rate, rate * .5 - 100,
                                 { rate * .5 - 1900, rate * .5 - 2100, 100 });
    CHECK (alias.finite);
    CHECK (std::abs (alias.amplitude[0] - (ring ? .25 : .5)) < 1.0e-4);
}

TEST_CASE ("Frequency reset reproduces the complete stereo sequence without callback allocations", "[frequency][reset][allocation]")
{
    const auto rate = GENERATE (1000.0, 48000.0, 384000.0);
    FrequencyEffects effects;
    effects.prepare (rate);
    effects.setParameters (enabled (FrequencyParameters::Mode::frequencyShift, -173));
    std::vector<std::array<float, 2>> first (12000);
    for (int frame = 0; frame < 12000; ++frame)
    {
        auto left = static_cast<float> (.7 * std::sin (frame * .37)), right = -.4f * left;
        effects.process (left, right);
        first[static_cast<std::size_t> (frame)] = { left, right };
    }
    effects.reset();
    bool exact = true, finite = true;
    std::size_t count = 0;
    {
        testing::AllocationCounter allocations;
        for (int frame = 0; frame < 12000; ++frame)
        {
            auto left = static_cast<float> (.7 * std::sin (frame * .37)), right = -.4f * left;
            effects.process (left, right);
            const auto& expected = first[static_cast<std::size_t> (frame)];
            exact &= left == expected[0] && right == expected[1];
            finite &= std::isfinite (left) && std::isfinite (right);
        }
        count = allocations.count();
    }
    CHECK (exact);
    CHECK (finite);
    CHECK (count == 0);
}

TEST_CASE ("Frequency carrier and quadrature remain coherent across chance and disabled intervals", "[frequency][continuity]")
{
    const auto rate = GENERATE (8000.0, 48000.0, 192000.0);
    const auto gate = GENERATE (0, 1, 2, 3);
    FrequencyEffects reference, gated;
    reference.prepare (rate);
    gated.prepare (rate);
    const auto parameters = enabled (FrequencyParameters::Mode::frequencyShift, 173);
    reference.setParameters (parameters);
    gated.setParameters (parameters);
    const auto ramp = static_cast<int> (std::ceil (.002 * rate));
    bool exactAfterReentry = true;
    for (int frame = 0; frame < 7000 + ramp; ++frame)
    {
        if (frame == 1000)
        {
            auto off = parameters;
            if (gate == 1) off.enabled = false;
            if (gate >= 2) off.mix = 0.0f;
            if (gate == 3) off.mode = FrequencyParameters::Mode::ring;
            gated.setParameters (off);
        }
        if (frame == 4000)
            gated.setParameters (parameters);
        const auto input = static_cast<float> (.4 * std::sin (frame * .47) + .1 * std::cos (frame * .13));
        auto left = input, right = -.3f * input;
        auto expectedLeft = left, expectedRight = right;
        reference.process (expectedLeft, expectedRight);
        gated.process (left, right, 0, 0, gate != 0 || frame < 1000 || frame >= 4000);
        if (frame >= 4000 + ramp)
            exactAfterReentry &= left == expectedLeft && right == expectedRight;
    }
    CHECK (exactAfterReentry);
}

TEST_CASE ("Frequency enable transitions start with a bounded two millisecond wet step", "[frequency][ramp]")
{
    const auto rate = GENERATE (8000.0, 44100.0, 48000.0, 192000.0);
    FrequencyEffects effects;
    effects.prepare (rate);
    auto parameters = enabled (FrequencyParameters::Mode::ring, 173);
    parameters.enabled = false;
    effects.setParameters (parameters);
    for (int frame = 0; frame < 1237; ++frame)
    {
        float left = .3f, right = -.3f;
        effects.process (left, right);
    }
    parameters.enabled = true;
    effects.setParameters (parameters);
    float left = .3f, right = -.3f;
    effects.process (left, right);
    const auto samples = std::ceil (.002 * rate);
    CHECK (std::abs (left - .3f) <= .6 / samples + 1e-7);
    CHECK (left == -right);
}
