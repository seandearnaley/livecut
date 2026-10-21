// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/TransformSpan.h"
#include "AllocationCounter.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Transform spans include exactly the gated read extent and interpolation support", "[transform][span]")
{
    for (const auto rate : { 0.5, 1.0, 2.0, 4.0 })
        CHECK (requiredSourceFrames (1, rate, 1) == 1);
    CHECK (requiredSourceFrames (1, std::numeric_limits<double>::max(), 1) == 1);
    CHECK (requiredSourceFrames (5, 0.5, 3) == 3);
    CHECK (requiredSourceFrames (5, 1.0, 5) == 5);
    CHECK (requiredSourceFrames (5, 2.0, 9) == 9);
    CHECK (requiredSourceFrames (5, 4.0, 17) == 17);
    CHECK (requiredSourceFrames (4, 0.5, 3) == 3);
    CHECK_FALSE (requiredSourceFrames (5, 2.0, 8));
    CHECK_FALSE (requiredSourceFrames (5, 4.0, 16));
}

TEST_CASE ("Transform spans reject invalid rates lengths and integer conversion overflow", "[transform][span]")
{
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    for (const auto rate : { 0.0, -0.5, std::numeric_limits<double>::infinity(),
                            -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN() })
    {
        CHECK_FALSE (requiredSourceFrames (1, rate, 1));
        CHECK_FALSE (readCoordinate (0, rate, 1, false));
        CHECK_FALSE (readCoordinate (0, rate, 1, true));
    }
    CHECK_FALSE (requiredSourceFrames (0, 1.0, 1));
    CHECK_FALSE (requiredSourceFrames (-1, 1.0, 1));
    CHECK_FALSE (requiredSourceFrames (1, 1.0, 0));
    CHECK_FALSE (requiredSourceFrames (1, 1.0, -1));
    CHECK_FALSE (requiredSourceFrames (3, std::numeric_limits<double>::max(), maximum));
    CHECK_FALSE (requiredSourceFrames (2, std::ldexp (1.0, 63), maximum));
    const auto belowLimit = std::nextafter (std::ldexp (1.0, 63), 0.0);
    CHECK (requiredSourceFrames (2, belowLimit, maximum) == maximum - 1022);
    CHECK_FALSE (readCoordinate (-1, 1.0, 2, false));
    CHECK_FALSE (readCoordinate (0, 1.0, 0, false));
    CHECK_FALSE (readCoordinate (0, 1.0, -1, true));
    CHECK_FALSE (readCoordinate (2, std::numeric_limits<double>::max(), maximum, false));
    CHECK_FALSE (readCoordinate (2, std::numeric_limits<double>::max(), maximum, true));
    CHECK_FALSE (readCoordinate (1, std::ldexp (1.0, 63), maximum, false));
}

TEST_CASE ("Transform coordinate integers must survive conversion to double exactly", "[transform][span]")
{
    constexpr auto exactLimit = std::int64_t { 1 } << 53;
    CHECK_FALSE (requiredSourceFrames (exactLimit + 2, 1.0, std::numeric_limits<std::int64_t>::max()));
    // Even when the final output integer converts exactly, earlier odd frames
    // would not: a complete planned span must support every output identity.
    CHECK_FALSE (requiredSourceFrames (exactLimit + 3, 1.0 / static_cast<double> (exactLimit), 4));
    CHECK_FALSE (readCoordinate (exactLimit + 1, 0.5, exactLimit, false));
    CHECK_FALSE (readCoordinate (0, 1.0, exactLimit + 2, true));
    CHECK_FALSE (readCoordinate (0, 1.0, std::numeric_limits<std::int64_t>::max(), true));
    const auto forward = readCoordinate (exactLimit, 0.5, exactLimit, false);
    REQUIRE (forward);
    CHECK (forward->index == exactLimit / 2);
    CHECK_THAT (forward->fraction, WithinAbs (0.0, 0.0));
}

TEST_CASE ("Forward coordinates address the fixed source at the effective rate", "[transform][span]")
{
    const std::array<ReadCoordinate, 5> half {{ { 0, 0.0 }, { 0, 0.5 }, { 1, 0.0 }, { 1, 0.5 }, { 2, 0.0 } }};
    for (std::int64_t output = 0; output < 5; ++output)
    {
        const auto coordinate = readCoordinate (output, 0.5, 3, false);
        REQUIRE (coordinate);
        CHECK (coordinate->index == half[static_cast<std::size_t> (output)].index);
        CHECK_THAT (coordinate->fraction, WithinAbs (half[static_cast<std::size_t> (output)].fraction, 0.0));
        for (const auto rate : { 1.0, 2.0, 4.0 })
        {
            const auto integer = readCoordinate (output, rate, 17, false);
            REQUIRE (integer);
            CHECK (integer->index == output * static_cast<std::int64_t> (rate));
            CHECK_THAT (integer->fraction, WithinAbs (0.0, 0.0));
        }
    }
}

TEST_CASE ("Reverse begins at the last integer source frame rather than a reversed resampled array", "[transform][span]")
{
    const std::array<ReadCoordinate, 5> half {{ { 4, 0.0 }, { 3, 0.5 }, { 3, 0.0 }, { 2, 0.5 }, { 2, 0.0 } }};
    for (std::int64_t output = 0; output < 5; ++output)
    {
        const auto coordinate = readCoordinate (output, 0.5, 5, true);
        REQUIRE (coordinate);
        CHECK (coordinate->index == half[static_cast<std::size_t> (output)].index);
        CHECK_THAT (coordinate->fraction, WithinAbs (half[static_cast<std::size_t> (output)].fraction, 0.0));
    }
    const auto final = readCoordinate (3, 0.5, 3, true);
    REQUIRE (final);
    CHECK (final->index == 0);
    CHECK_THAT (final->fraction, WithinAbs (0.5, 0.0)); // the forward final coordinate is 1.5
}

TEST_CASE ("Integer endpoints need no neighbor but fractional endpoints must retain both frames", "[transform][span]")
{
    for (const auto reverse : { false, true })
    {
        const auto only = readCoordinate (0, std::numeric_limits<double>::max(), 1, reverse);
        REQUIRE (only);
        CHECK (only->index == 0);
        CHECK_THAT (only->fraction, WithinAbs (0.0, 0.0));
        CHECK_FALSE (readCoordinate (1, 1.0, 1, reverse));
    }
    const auto belowOne = std::nextafter (1.0, 0.0);
    const auto coordinate = readCoordinate (1, belowOne, 2, false);
    REQUIRE (coordinate);
    CHECK (coordinate->index == 0);
    CHECK_THAT (coordinate->fraction, WithinAbs (belowOne, 0.0));
    CHECK_THAT (static_cast<float> (coordinate->fraction), WithinAbs (1.0, 0.0));
    CHECK_FALSE (readCoordinate (1, belowOne, 1, false));
    const auto last = readCoordinate (1, 1.0, 2, false);
    REQUIRE (last);
    CHECK (last->index == 1);
    CHECK_THAT (last->fraction, WithinAbs (0.0, 0.0));
    CHECK_FALSE (readCoordinate (1, std::nextafter (1.0, 2.0), 2, false));
}

TEST_CASE ("A tempo extension consumes the fixed span then stops without looping or clamping", "[transform][span]")
{
    for (const auto rate : { 0.5, 1.0, 2.0, 4.0 })
    {
        const auto count = requiredSourceFrames (5, rate, 100);
        REQUIRE (count);
        for (const auto reverse : { false, true })
        {
            for (std::int64_t output = 0; output < 5; ++output)
                CHECK (readCoordinate (output, rate, *count, reverse));
            CHECK_FALSE (readCoordinate (5, rate, *count, reverse));
            CHECK_FALSE (readCoordinate (5000, rate, *count, reverse));
        }
    }
}

TEST_CASE ("The minimum transform span accepts every planned read and one fewer frame cannot", "[transform][span]")
{
    for (const auto rate : { 0.125, 0.5, std::nextafter (1.0, 0.0), 1.0, 1.5, 2.0, 4.0 })
        for (std::int64_t length = 1; length <= 64; ++length)
        {
            const auto count = requiredSourceFrames (length, rate, 256);
            REQUIRE (count);
            for (const auto reverse : { false, true })
            {
                for (std::int64_t output = 0; output < length; ++output)
                    REQUIRE (readCoordinate (output, rate, *count, reverse));
                if (*count > 1)
                    CHECK_FALSE (readCoordinate (length - 1, rate, *count - 1, reverse));
            }
        }
}

TEST_CASE ("Transform span and coordinate queries allocate no memory", "[transform][span][allocations]")
{
    bool valid = true;
    std::size_t count = 0;
    {
        const testing::AllocationCounter allocations;
        for (int query = 0; query < 4096; ++query)
        {
            const auto frames = requiredSourceFrames (17, 4.0, 65);
            const auto coordinate = readCoordinate (query % 17, 4.0, 65, (query & 1) != 0);
            valid &= frames.has_value() && coordinate.has_value();
        }
        count = allocations.count();
    }
    CHECK (valid);
    CHECK (count == 0);
}
