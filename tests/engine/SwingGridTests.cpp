// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/SwingGrid.h"
#include "AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Neutral swing preserves every valid bar position bit for bit", "[swinggrid]")
{
    for (const auto bar : { 0.5, 1.0, 1.5, 2.5, 3.0, 4.0, 7.25 })
        for (const auto fraction : { -0.0, 0.0, 0.001, 0.125, 0.3333333333333333, 0.5, 0.999999, 1.0 })
        {
            const auto position = fraction * bar;
            const auto physical = SwingGrid::toPhysical (position, bar);
            const auto logical = SwingGrid::toLogical (position, bar);
            REQUIRE (physical);
            REQUIRE (logical);
            CHECK (std::bit_cast<std::uint64_t> (*physical) == std::bit_cast<std::uint64_t> (position));
            CHECK (std::bit_cast<std::uint64_t> (*logical) == std::bit_cast<std::uint64_t> (position));
        }
}

TEST_CASE ("Swing maps the middle eighth and preserves quarter pair and bar endpoints", "[swinggrid]")
{
    for (const auto amount : { 50.0, 60.0, 200.0 / 3.0, 75.0 })
        for (const auto bar : { 0.5, 1.0, 1.5, 2.5, 3.0, 4.0, 7.25 })
        {
            for (int pair = 0; pair < static_cast<int> (std::floor (bar)); ++pair)
            {
                const auto middle = SwingGrid::toPhysical (pair + 0.5, bar, amount);
                REQUIRE (middle);
                CHECK_THAT (*middle, WithinAbs (pair + amount / 100.0, 1.0e-14));
                for (const auto endpoint : { static_cast<double> (pair), pair + 1.0 })
                {
                    const auto forward = SwingGrid::toPhysical (endpoint, bar, amount);
                    const auto inverse = SwingGrid::toLogical (endpoint, bar, amount);
                    REQUIRE (forward);
                    REQUIRE (inverse);
                    CHECK (std::bit_cast<std::uint64_t> (*forward) == std::bit_cast<std::uint64_t> (endpoint));
                    CHECK (std::bit_cast<std::uint64_t> (*inverse) == std::bit_cast<std::uint64_t> (endpoint));
                }
            }
            REQUIRE (SwingGrid::toPhysical (bar, bar, amount));
            REQUIRE (SwingGrid::toLogical (bar, bar, amount));
            CHECK (std::bit_cast<std::uint64_t> (*SwingGrid::toPhysical (bar, bar, amount)) == std::bit_cast<std::uint64_t> (bar));
            CHECK (std::bit_cast<std::uint64_t> (*SwingGrid::toLogical (bar, bar, amount)) == std::bit_cast<std::uint64_t> (bar));
        }
}

TEST_CASE ("Swing and its inverse are monotonic round trips with bounded delay", "[swinggrid]")
{
    for (const auto bar : { 0.5, 1.5, 2.5, 4.0, 7.25 })
        for (const auto amount : { 50.0, 50.01, 60.0, 200.0 / 3.0, 75.0 })
        {
            double previousPhysical = -1.0, previousLogical = -1.0;
            for (int step = 0; step <= 4096; ++step)
            {
                const auto position = bar * step / 4096.0;
                const auto physical = SwingGrid::toPhysical (position, bar, amount);
                const auto logical = SwingGrid::toLogical (position, bar, amount);
                REQUIRE (physical);
                REQUIRE (logical);
                CHECK (*physical > previousPhysical);
                CHECK (*logical > previousLogical);
                CHECK (*physical >= position);
                CHECK (*physical - position <= 0.25);
                CHECK (*physical <= bar);
                const auto logicalAgain = SwingGrid::toLogical (*physical, bar, amount);
                const auto physicalAgain = SwingGrid::toPhysical (*logical, bar, amount);
                REQUIRE (logicalAgain);
                REQUIRE (physicalAgain);
                CHECK_THAT (*logicalAgain, WithinAbs (position, 2.0e-14));
                CHECK_THAT (*physicalAgain, WithinAbs (position, 2.0e-14));
                previousPhysical = *physical;
                previousLogical = *logical;
            }
        }
}

TEST_CASE ("Incomplete trailing eighth pairs stay straight in fractional meters", "[swinggrid]")
{
    for (const auto bar : { 0.5, 1.5, 2.5, 3.75, 7.25 })
        for (const auto amount : { 60.0, 75.0 })
            for (int step = 0; step <= 100; ++step)
            {
                const auto position = std::floor (bar) + (bar - std::floor (bar)) * step / 100.0;
                const auto physical = SwingGrid::toPhysical (position, bar, amount);
                const auto logical = SwingGrid::toLogical (position, bar, amount);
                REQUIRE (physical);
                REQUIRE (logical);
                CHECK (std::bit_cast<std::uint64_t> (*physical) == std::bit_cast<std::uint64_t> (position));
                CHECK (std::bit_cast<std::uint64_t> (*logical) == std::bit_cast<std::uint64_t> (position));
            }
}

TEST_CASE ("Swing changes activation positions without changing logical source offsets", "[swinggrid]")
{
    const std::array sourceOffsets { 0.0, 0.25, 0.5, 0.75, 1.0 };
    const auto original = sourceOffsets;
    for (std::size_t index = 0; index < sourceOffsets.size(); ++index)
    {
        const auto physical = SwingGrid::toPhysical (sourceOffsets[index], 4.0, 75.0);
        REQUIRE (physical);
        CHECK_THAT (*physical, WithinAbs (std::array { 0.0, 0.375, 0.75, 0.875, 1.0 }[index], 0.0));
        CHECK (std::bit_cast<std::uint64_t> (sourceOffsets[index]) == std::bit_cast<std::uint64_t> (original[index]));
    }
}

TEST_CASE ("Swing rejects invalid positions bars and percentages explicitly", "[swinggrid]")
{
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    const auto infinity = std::numeric_limits<double>::infinity();
    for (const auto values : { std::array { -0.001, 4.0, 50.0 }, { 4.001, 4.0, 75.0 },
                              { 0.0, 0.0, 50.0 }, { 0.0, -1.0, 75.0 },
                              { 0.5, 4.0, 49.999 }, { 0.5, 4.0, 75.001 },
                              { nan, 4.0, 50.0 }, { infinity, 4.0, 75.0 },
                              { 0.0, nan, 50.0 }, { 0.0, infinity, 75.0 },
                              { 0.0, 4.0, nan }, { 0.0, 4.0, infinity } })
    {
        CHECK_FALSE (SwingGrid::toPhysical (values[0], values[1], values[2]));
        CHECK_FALSE (SwingGrid::toLogical (values[0], values[1], values[2]));
    }
}

TEST_CASE ("Swing mapping allocates nothing", "[swinggrid]")
{
    bool valid = true;
    std::size_t allocations = 0;
    {
        testing::AllocationCounter counter;
        for (int i = 0; i <= 10000; ++i)
        {
            const auto position = 4.0 * i / 10000.0;
            valid &= SwingGrid::toPhysical (position, 4.0, 75.0).has_value();
            valid &= SwingGrid::toLogical (position, 4.0, 75.0).has_value();
        }
        allocations = counter.count();
    }
    CHECK (valid);
    CHECK (allocations == 0);
}
