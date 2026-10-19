// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/LivePlayer.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <vector>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

namespace
{
Cut makeCut (double units, double gate = 1.0, float pan = 0.0f, float amp = 1.0f, float cents = 0.0f)
{
    Cut cut;
    cut.units = units;
    cut.gate = gate;
    cut.pan = pan;
    cut.amp = amp;
    cut.cents = cents;
    return cut;
}

// Distinct, recognisable values on each channel.
float inputLeft (int t) { return 0.001f * static_cast<float> (t + 1); }
float inputRight (int t) { return -0.002f * static_cast<float> (t + 1); }

struct Output
{
    std::vector<float> left, right;
};

Output play (LivePlayer& player, int samples)
{
    Output output;

    for (int t = 0; t < samples; ++t)
    {
        auto left = inputLeft (t);
        auto right = inputRight (t);
        player.process (left, right);
        output.left.push_back (left);
        output.right.push_back (right);
    }

    return output;
}
} // namespace

TEST_CASE ("The first cut of a block plays the input as it arrives", "[player]")
{
    LivePlayer player;
    player.prepare (1000);
    player.setFadeSamples (1);
    const std::vector cuts { makeCut (1.0) };
    player.startBlock (cuts, 100.0);

    const auto output = play (player, 100);

    for (int t = 0; t < 100; ++t)
    {
        const auto envelope = fadeEnvelope (t, 1, 100);
        REQUIRE_THAT (output.left[static_cast<std::size_t> (t)], WithinAbs (inputLeft (t) * envelope, 1.0e-6));
        REQUIRE_THAT (output.right[static_cast<std::size_t> (t)], WithinAbs (inputRight (t) * envelope, 1.0e-6));
    }
}

TEST_CASE ("Later cuts replay the block from its start", "[player]")
{
    LivePlayer player;
    player.prepare (1000);
    player.setFadeSamples (1);
    const std::vector cuts { makeCut (1.0), makeCut (1.0) };
    player.startBlock (cuts, 100.0);

    const auto output = play (player, 200);

    for (int t = 0; t < 100; ++t)
        REQUIRE_THAT (output.left[static_cast<std::size_t> (100 + t)], WithinAbs (inputLeft (t) * fadeEnvelope (t, 1, 100), 1.0e-6));
}

TEST_CASE ("Cut sizes add up exactly to the block length in samples", "[player]")
{
    LivePlayer player;
    player.prepare (1000);
    const std::vector cuts { makeCut (1.0 / 3.0), makeCut (1.0 / 3.0), makeCut (1.0 / 3.0) };
    player.startBlock (cuts, 1000.0);

    const auto playing = player.cuts();
    REQUIRE (playing.size() == 3);
    CHECK (playing[0].size + playing[1].size + playing[2].size == 1000);
    CHECK (playing[1].start == playing[0].size);
    CHECK (playing[2].start == playing[0].size + playing[1].size);
}

TEST_CASE ("The gate silences the rest of a cut", "[player]")
{
    LivePlayer player;
    player.prepare (1000);
    const std::vector cuts { makeCut (1.0), makeCut (1.0, 0.5) };
    player.startBlock (cuts, 100.0);

    const auto output = play (player, 200);

    CHECK (std::abs (output.left[100 + 49]) > 0.0f);

    for (std::size_t t = 150; t < 200; ++t)
    {
        REQUIRE_THAT (output.left[t], WithinAbs (0.0, 0.0));
        REQUIRE_THAT (output.right[t], WithinAbs (0.0, 0.0));
    }
}

TEST_CASE ("Pan keeps a centred cut's stereo image and folds hard pans to one side", "[player]")
{
    LivePlayer player;
    player.prepare (1000);
    const std::vector cuts { makeCut (1.0, 1.0, 0.0f), makeCut (1.0, 1.0, 1.0f), makeCut (1.0, 1.0, -1.0f) };
    player.startBlock (cuts, 10.0);

    const auto playing = player.cuts();
    REQUIRE (playing.size() == 3);

    CHECK_THAT (playing[0].leftToLeft, WithinAbs (1.0, 1.0e-6));
    CHECK_THAT (playing[0].leftToRight, WithinAbs (0.0, 1.0e-6));
    CHECK_THAT (playing[0].rightToLeft, WithinAbs (0.0, 1.0e-6));
    CHECK_THAT (playing[0].rightToRight, WithinAbs (1.0, 1.0e-6));

    CHECK_THAT (playing[1].leftToLeft, WithinAbs (0.0, 1.0e-6));
    CHECK_THAT (playing[1].leftToRight, WithinAbs (1.0, 1.0e-6));
    CHECK_THAT (playing[1].rightToRight, WithinAbs (1.0, 1.0e-6));

    CHECK_THAT (playing[2].leftToLeft, WithinAbs (1.0, 1.0e-6));
    CHECK_THAT (playing[2].rightToLeft, WithinAbs (1.0, 1.0e-6));
    CHECK_THAT (playing[2].rightToRight, WithinAbs (0.0, 1.0e-6));
}

TEST_CASE ("Amplitude scales a cut", "[player]")
{
    LivePlayer player;
    player.prepare (1000);
    player.setFadeSamples (1);
    const std::vector cuts { makeCut (1.0, 1.0, 0.0f, 0.25f) };
    player.startBlock (cuts, 100.0);

    const auto output = play (player, 100);

    CHECK_THAT (output.left[50], WithinAbs (0.25 * inputLeft (50) * fadeEnvelope (50, 1, 100), 1.0e-6));
}

TEST_CASE ("A pitched cut reads the block faster", "[player]")
{
    LivePlayer player;
    player.prepare (1000);
    player.setFadeSamples (1);
    const std::vector cuts { makeCut (1.0), makeCut (1.0, 1.0, 0.0f, 1.0f, 1200.0f) };
    player.startBlock (cuts, 100.0);

    REQUIRE (player.cuts()[1].playable == 100);
    const auto output = play (player, 200);

    for (int t = 0; t < 100; ++t)
        REQUIRE_THAT (output.left[static_cast<std::size_t> (100 + t)], WithinAbs (inputLeft (2 * t) * fadeEnvelope (t, 1, 100), 1.0e-5));
}

// Livecut resampled a pitched cut from a buffer that was still mostly unrecorded,
// so pitched-up repeats went silent part way through.
TEST_CASE ("A pitched cut ends, faded, before it reads unrecorded audio", "[player]")
{
    CHECK (playableSamples (100, 2.0, 100, 1000) == 100);
    CHECK (playableSamples (100, 2.0, 100, 150) == 75);
    CHECK (playableSamples (100, 4.0, 100, 1000) == 34);
    CHECK (playableSamples (100, 0.5, 1, 1000) == 100);

    LivePlayer player;
    player.prepare (150);
    player.setFadeSamples (1);
    const std::vector cuts { makeCut (1.0), makeCut (1.0, 1.0, 0.0f, 1.0f, 1200.0f) };
    player.startBlock (cuts, 100.0);

    const auto playable = player.cuts()[1].playable;
    REQUIRE (playable == 75);
    const auto output = play (player, 200);

    for (int t = 0; t < 100; ++t)
    {
        const auto expected = t < playable ? inputLeft (2 * t) * fadeEnvelope (t, 1, playable) : 0.0f;
        REQUIRE_THAT (output.left[static_cast<std::size_t> (100 + t)], WithinAbs (expected, 1.0e-5));
    }
}

TEST_CASE ("The first cut of a block ignores its pitch", "[player]")
{
    LivePlayer player;
    player.prepare (1000);
    player.setFadeSamples (1);
    const std::vector cuts { makeCut (1.0, 1.0, 0.0f, 1.0f, 1200.0f) };
    player.startBlock (cuts, 100.0);

    CHECK_FALSE (player.cuts()[0].pitched);
    const auto output = play (player, 100);
    CHECK_THAT (output.left[60], WithinAbs (inputLeft (60) * fadeEnvelope (60, 1, 100), 1.0e-6));
}

TEST_CASE ("A block is silent once its cuts run out", "[player]")
{
    LivePlayer player;
    player.prepare (1000);
    const std::vector cuts { makeCut (0.5) };
    player.startBlock (cuts, 100.0);

    const auto output = play (player, 60);

    for (std::size_t t = 50; t < 60; ++t)
        REQUIRE_THAT (output.left[t], WithinAbs (0.0, 0.0));
}

TEST_CASE ("The fade envelope rises from silence and falls towards it", "[player]")
{
    CHECK_THAT (fadeEnvelope (0, 10, 100), WithinAbs (0.0, 1.0e-6));
    CHECK (fadeEnvelope (50, 10, 100) > 0.99f);
    CHECK (fadeEnvelope (99, 10, 100) < 0.4f);
    CHECK (fadeEnvelope (5, 10, 100) < fadeEnvelope (10, 10, 100));
}
