// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Random.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <set>

using livecut::engine::pickWeighted;
using livecut::engine::Random;

TEST_CASE ("Random repeats its sequence for the same seed", "[random]")
{
    Random a (42);
    Random b (42);
    std::set<int> seen;

    for (int i = 0; i < 1000; ++i)
    {
        const auto value = a.integer (0, 1000);
        REQUIRE (b.integer (0, 1000) == value);
        seen.insert (value);
    }

    CHECK (seen.size() > 500);
}

TEST_CASE ("Different seeds give different sequences", "[random]")
{
    Random a (1);
    Random b (2);
    int matches = 0;

    for (int i = 0; i < 1000; ++i)
        matches += a.integer (0, 1000) == b.integer (0, 1000) ? 1 : 0;

    CHECK (matches < 20);
}

TEST_CASE ("Random reseeding restarts the sequence", "[random]")
{
    Random random (7);
    std::array<int, 16> first {};

    for (auto& value : first)
        value = random.integer (0, 1 << 20);

    REQUIRE (std::set<int> (first.begin(), first.end()).size() == first.size());
    random.reseed (7);

    for (const auto value : first)
        REQUIRE (random.integer (0, 1 << 20) == value);
}

// Livecut rounded a uniform float, which gave each endpoint half the weight of the values between.
TEST_CASE ("Random integers include both endpoints with equal weight", "[random]")
{
    Random random (1);
    std::array<int, 4> counts {};

    for (int i = 0; i < 40000; ++i)
    {
        const auto value = random.integer (1, 4);
        REQUIRE (value >= 1);
        REQUIRE (value <= 4);
        ++counts[static_cast<std::size_t> (value - 1)];
    }

    for (const auto count : counts)
    {
        CHECK (count > 9500);
        CHECK (count < 10500);
    }
}

TEST_CASE ("Random integers accept their bounds in either order", "[random]")
{
    Random random (3);

    for (int i = 0; i < 1000; ++i)
    {
        const auto value = random.integer (5, 2);
        REQUIRE (value >= 2);
        REQUIRE (value <= 5);
    }
}

TEST_CASE ("Random uniform values never reach the upper bound", "[random]")
{
    Random random (9);

    for (int i = 0; i < 100000; ++i)
    {
        const auto value = random.uniform (-1.0, 1.0);
        REQUIRE (value >= -1.0);
        REQUIRE (value < 1.0);
    }
}

// Livecut's wchoose read one past the end of its table when rand() returned RAND_MAX.
TEST_CASE ("Weighted picks map the whole draw range onto valid indices", "[random]")
{
    const std::array weights { 0.5, 0.4, 0.1 };

    CHECK (pickWeighted (weights, 0.0) == 0);
    CHECK (pickWeighted (weights, 0.48) == 0);
    CHECK (pickWeighted (weights, 0.52) == 1);
    CHECK (pickWeighted (weights, 0.88) == 1);
    CHECK (pickWeighted (weights, 0.92) == 2);
    CHECK (pickWeighted (weights, std::nextafter (1.0, 0.0)) == 2);
}

TEST_CASE ("Weighted draws follow their weights", "[random]")
{
    Random random (5);
    const std::array weights { 0.5, 0.4, 0.1 };
    std::array<int, 3> counts {};

    for (int i = 0; i < 100000; ++i)
    {
        const auto index = random.weighted (weights);
        REQUIRE (index < weights.size());
        ++counts[index];
    }

    CHECK (counts[0] > 49000);
    CHECK (counts[0] < 51000);
    CHECK (counts[1] > 39000);
    CHECK (counts[1] < 41000);
    CHECK (counts[2] > 9400);
    CHECK (counts[2] < 10600);
}
