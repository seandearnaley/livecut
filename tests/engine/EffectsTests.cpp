// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Effects.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double sampleRate = 48000.0;

float signal (int t) { return 0.8f * std::sin (0.05f * static_cast<float> (t)); }
} // namespace

// Livecut swept from start to start + end instead of from start to end.
TEST_CASE ("Effect settings sweep from the block's start value to its end value", "[effects]")
{
    CHECK_THAT (sweepSetting (8.0, 16.0, 0, 4), WithinAbs (8.0, 1.0e-12));
    CHECK_THAT (sweepSetting (8.0, 16.0, 2, 4), WithinAbs (12.0, 1.0e-12));
    CHECK_THAT (sweepSetting (16.0, 8.0, 3, 4), WithinAbs (10.0, 1.0e-12));
}

TEST_CASE ("At full resolution and rate the crusher is transparent", "[crusher]")
{
    BitCrusher crusher;
    crusher.prepare (sampleRate);
    crusher.setBlock (32.0, 32.0, 192000.0, 192000.0);
    crusher.startCut (0, 1);

    for (int t = 0; t < 1000; ++t)
    {
        auto left = signal (t);
        auto right = -signal (t);
        crusher.process (left, right);
        REQUIRE_THAT (left, WithinAbs (signal (t), 0.0));
        REQUIRE_THAT (right, WithinAbs (-signal (t), 0.0));
    }
}

// Livecut floored, which offset everything by half a step, and its step was one bit finer than the setting.
TEST_CASE ("The crusher rounds to the nearest step of its bit depth", "[crusher]")
{
    BitCrusher crusher;
    crusher.prepare (sampleRate);
    crusher.setBlock (2.0, 2.0, 192000.0, 192000.0); // two bits: steps of 0.5
    crusher.startCut (0, 1);

    const std::vector<float> inputs { 0.2f, 0.3f, -0.2f, -0.3f, 0.9f };
    const std::vector<float> expected { 0.0f, 0.5f, 0.0f, -0.5f, 1.0f };

    for (std::size_t i = 0; i < inputs.size(); ++i)
    {
        auto left = inputs[i];
        auto right = inputs[i];
        crusher.process (left, right);
        CHECK_THAT (left, WithinAbs (expected[i], 1.0e-7));
    }
}

TEST_CASE ("Rate reduction holds each sample for the sample rate over the hold rate", "[crusher]")
{
    BitCrusher crusher;
    crusher.prepare (sampleRate);
    crusher.setBlock (32.0, 32.0, sampleRate / 4.0, sampleRate / 4.0);
    crusher.startCut (0, 1);

    for (int t = 0; t < 40; ++t)
    {
        auto left = signal (t);
        auto right = signal (t);
        crusher.process (left, right);
        REQUIRE_THAT (left, WithinAbs (signal (t - t % 4), 0.0));
    }
}

TEST_CASE ("The crusher sweeps bit depth and hold across a block's cuts", "[crusher]")
{
    BitCrusher crusher;
    crusher.prepare (sampleRate);
    crusher.setBlock (4.0, 12.0, sampleRate / 2.0, sampleRate / 8.0);

    crusher.startCut (0, 4);
    CHECK_THAT (crusher.bits(), WithinAbs (4.0, 1.0e-12));
    CHECK_THAT (crusher.holdSamples(), WithinAbs (2.0, 1.0e-9));

    crusher.startCut (2, 4);
    CHECK_THAT (crusher.bits(), WithinAbs (8.0, 1.0e-12));
}

TEST_CASE ("A feedforward comb adds a delayed copy at half level", "[comb]")
{
    Comb comb;
    comb.prepare (sampleRate);
    CombParameters params;
    params.type = CombParameters::Type::feedForward;
    comb.setBlock (params, 1000.0 * 10.0 / sampleRate, 1000.0 * 10.0 / sampleRate); // 10 samples
    comb.startCut (0, 1);

    std::vector<float> output;

    for (int t = 0; t < 30; ++t)
    {
        auto left = t == 0 ? 1.0f : 0.0f;
        auto right = left;
        comb.process (left, right);
        output.push_back (left);
    }

    CHECK_THAT (output[0], WithinAbs (0.5, 1.0e-6));
    CHECK_THAT (output[10], WithinAbs (0.5, 1.0e-6));
    CHECK_THAT (output[5], WithinAbs (0.0, 1.0e-6));
    CHECK_THAT (output[20], WithinAbs (0.0, 1.0e-6));
}

TEST_CASE ("A feedback comb repeats with decaying echoes at its delay", "[comb]")
{
    Comb comb;
    comb.prepare (sampleRate);
    CombParameters params;
    params.type = CombParameters::Type::feedBack;
    params.feedback = 0.5f;
    comb.setBlock (params, 1000.0 * 10.0 / sampleRate, 1000.0 * 10.0 / sampleRate);
    comb.startCut (0, 1);

    std::vector<float> output;

    for (int t = 0; t < 40; ++t)
    {
        auto left = t == 0 ? 1.0f : 0.0f;
        auto right = left;
        comb.process (left, right);
        output.push_back (left);
    }

    const auto direct = 0.99f - 0.5f;
    CHECK_THAT (output[0], WithinAbs (direct, 1.0e-6));
    CHECK_THAT (output[10], WithinAbs (0.5 * direct, 1.0e-6));
    CHECK_THAT (output[20], WithinAbs (0.25 * direct, 1.0e-6));
    CHECK_THAT (output[15], WithinAbs (0.0, 1.0e-6));
}

TEST_CASE ("The comb sweeps its delay across a block's cuts", "[comb]")
{
    Comb comb;
    comb.prepare (sampleRate);
    comb.setBlock (CombParameters {}, 10.0, 30.0);

    comb.startCut (0, 2);
    CHECK_THAT (comb.delaySamples(), WithinAbs (0.010 * sampleRate, 1.0e-6));

    comb.startCut (1, 2);
    CHECK_THAT (comb.delaySamples(), WithinAbs (0.020 * sampleRate, 1.0e-6));
}

TEST_CASE ("The feedback comb's output stays within full scale", "[comb]")
{
    Comb comb;
    comb.prepare (sampleRate);
    CombParameters params;
    params.type = CombParameters::Type::feedBack;
    params.feedback = 0.9f;
    comb.setBlock (params, 1.0, 1.0);
    comb.startCut (0, 1);

    for (int t = 0; t < 20000; ++t)
    {
        auto left = 1.0f;
        auto right = -1.0f;
        comb.process (left, right);
        REQUIRE (std::abs (left) <= 1.0f);
        REQUIRE (std::abs (right) <= 1.0f);
    }
}
