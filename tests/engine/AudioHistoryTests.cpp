// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/AudioHistory.h"
#include "AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <limits>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Audio history identifies stereo source by absolute frame and epoch", "[history]")
{
    AudioHistory history;
    CHECK_FALSE (history.append ({ 1.0f, -1.0f }));
    CHECK_FALSE (history.isAvailable (history.retainedRegion()));
    REQUIRE (history.prepare (4));
    const auto epoch = history.retainedRegion().epoch;
    REQUIRE (history.append ({ 1.0f, -10.0f }));
    REQUIRE (history.append ({ 2.0f, -20.0f }));
    const SourceRegion source { epoch, 0, 2 };
    REQUIRE (history.isAvailable (source));
    const auto first = history.read (source, 0);
    const auto last = history.read (source, 1);
    REQUIRE (first);
    REQUIRE (last);
    CHECK_THAT (first->left, WithinAbs (1.0, 0.0));
    CHECK_THAT (first->right, WithinAbs (-10.0, 0.0));
    CHECK_THAT (last->left, WithinAbs (2.0, 0.0));
    CHECK_THAT (last->right, WithinAbs (-20.0, 0.0));
    CHECK_FALSE (history.read (source, -1));
    CHECK_FALSE (history.read (source, 2));
}

TEST_CASE ("Wrapping expires source instead of assigning its identity to new audio", "[history]")
{
    AudioHistory history;
    REQUIRE (history.prepare (3));
    for (int frame = 0; frame < 3; ++frame)
        REQUIRE (history.append ({ static_cast<float> (frame), -static_cast<float> (frame) }));
    const auto original = history.retainedRegion();
    REQUIRE (history.isAvailable (original));
    REQUIRE (history.append ({ 3.0f, -3.0f }));
    CHECK_FALSE (history.isAvailable (original));
    CHECK_FALSE (history.read (original, 0));
    REQUIRE (history.read (original, 1));
    const auto retained = history.retainedRegion();
    CHECK (retained.firstFrame == 1);
    CHECK (retained.frameCount == 3);
    REQUIRE (history.isAvailable (retained));
    for (int frame = 0; frame < 3; ++frame)
    {
        const auto sample = history.read (retained, frame);
        REQUIRE (sample);
        CHECK_THAT (sample->left, WithinAbs (frame + 1, 0.0));
        CHECK_THAT (sample->right, WithinAbs (-(frame + 1), 0.0));
    }
    for (int frame = 4; frame < 40; ++frame)
        REQUIRE (history.append ({ static_cast<float> (frame), -static_cast<float> (frame) }));
    CHECK (history.retainedRegion().firstFrame == 37);
    CHECK (history.retainedRegion().frameCount == 3);
    CHECK_FALSE (history.isAvailable (retained));
}

TEST_CASE ("Reset and reprepare invalidate every region from the previous epoch", "[history]")
{
    AudioHistory history;
    REQUIRE (history.prepare (2));
    REQUIRE (history.append ({ 1.0f, 2.0f }));
    const auto old = history.retainedRegion();
    REQUIRE (history.reset());
    CHECK (history.retainedRegion().epoch == old.epoch + 1);
    CHECK (history.retainedRegion().firstFrame == 0);
    CHECK (history.retainedRegion().frameCount == 0);
    REQUIRE (history.append ({ 3.0f, 4.0f }));
    CHECK_FALSE (history.isAvailable (old));
    CHECK_FALSE (history.read (old, 0));
    const auto beforePrepare = history.retainedRegion();
    REQUIRE (history.prepare (1));
    REQUIRE (history.append ({ 5.0f, 6.0f }));
    CHECK_FALSE (history.isAvailable (beforePrepare));
    CHECK (history.retainedRegion().frameCount == 1);
}

TEST_CASE ("A progressive source permits only positions that have already arrived", "[history]")
{
    AudioHistory history;
    REQUIRE (history.prepare (4));
    const SourceRegion planned { history.retainedRegion().epoch, 0, 4 };
    CHECK_FALSE (history.isAvailable (planned));
    REQUIRE (history.append ({ 2.0f, 4.0f }));
    REQUIRE (history.read (planned, 0));
    REQUIRE (history.readFractional (planned, 0, 0.0));
    CHECK_FALSE (history.read (planned, 1));
    CHECK_FALSE (history.readFractional (planned, 0, 0.5));
    REQUIRE (history.append ({ 4.0f, 8.0f }));
    const auto sample = history.readFractional (planned, 0, 0.5);
    REQUIRE (sample);
    CHECK_THAT (sample->left, WithinAbs (3.0, 0.0));
    CHECK_THAT (sample->right, WithinAbs (6.0, 0.0));
    CHECK_FALSE (history.isAvailable (planned));
    REQUIRE (history.append ({ 6.0f, 12.0f }));
    REQUIRE (history.append ({ 8.0f, 16.0f }));
    CHECK (history.isAvailable (planned));
}

TEST_CASE ("Interpolation requires both neighbors inside the available source region", "[history]")
{
    AudioHistory history;
    REQUIRE (history.prepare (3));
    REQUIRE (history.append ({ 0.0f, 20.0f }));
    REQUIRE (history.append ({ 4.0f, 12.0f }));
    REQUIRE (history.append ({ 8.0f, 4.0f }));
    const auto source = history.retainedRegion();
    const auto sample = history.readFractional (source, 0, 0.25);
    REQUIRE (sample);
    CHECK_THAT (sample->left, WithinAbs (1.0, 0.0));
    CHECK_THAT (sample->right, WithinAbs (18.0, 0.0));
    REQUIRE (history.readFractional (source, 2, 0.0));
    CHECK_FALSE (history.readFractional (source, 2, 0.01));
    // A retained neighbor outside this region is still not part of this voice's source.
    CHECK_FALSE (history.readFractional ({ source.epoch, 0, 1 }, 0, 0.5));
    CHECK_FALSE (history.readFractional (source, -1, 0.5));
    CHECK_FALSE (history.readFractional (source, 0, -0.1));
    CHECK_FALSE (history.readFractional (source, 0, 1.0));
    CHECK_FALSE (history.readFractional (source, 0, std::numeric_limits<double>::quiet_NaN()));
    CHECK_FALSE (history.readFractional (source, 0, std::numeric_limits<double>::infinity()));
    REQUIRE (history.append ({ 12.0f, -4.0f }));
    const auto wrapped = history.retainedRegion();
    const auto acrossWrap = history.readFractional (wrapped, 1, 0.5);
    REQUIRE (acrossWrap);
    CHECK_THAT (acrossWrap->left, WithinAbs (10.0, 0.0));
    CHECK_THAT (acrossWrap->right, WithinAbs (0.0, 0.0));
    CHECK_FALSE (history.readFractional (source, 0, 0.5));
}

TEST_CASE ("Audio history rejects malformed and unavailable regions without overflowing", "[history]")
{
    AudioHistory history;
    CHECK_FALSE (history.prepare (0));
    REQUIRE (history.prepare (2));
    REQUIRE (history.append ({ 1.0f, 2.0f }));
    const auto source = history.retainedRegion();
    const auto largest = std::numeric_limits<std::int64_t>::max();
    CHECK_FALSE (history.isAvailable ({ source.epoch, -1, 1 }));
    CHECK_FALSE (history.isAvailable ({ source.epoch, 0, 0 }));
    CHECK_FALSE (history.isAvailable ({ source.epoch, 0, -1 }));
    CHECK_FALSE (history.isAvailable ({ source.epoch, 0, 2 }));
    CHECK_FALSE (history.isAvailable ({ source.epoch, largest, largest }));
    CHECK_FALSE (history.read (source, largest));
    CHECK_FALSE (history.readFractional (source, largest, 0.5));
    CHECK_FALSE (history.prepare (std::numeric_limits<std::size_t>::max()));
    CHECK (history.isAvailable (source));
}

TEST_CASE ("Appending reading and resetting audio history allocate nothing after prepare", "[history]")
{
    AudioHistory history;
    REQUIRE (history.prepare (8));
    bool succeeded = true;
    std::size_t allocations = 0;
    {
        testing::AllocationCounter counter;
        for (int frame = 0; frame < 1000; ++frame)
        {
            succeeded &= history.append ({ static_cast<float> (frame), -static_cast<float> (frame) });
            const auto region = history.retainedRegion();
            succeeded &= history.isAvailable (region);
            succeeded &= history.read (region, region.frameCount - 1).has_value();
            if (region.frameCount > 1)
                succeeded &= history.readFractional (region, region.frameCount - 2, 0.5).has_value();
        }
        succeeded &= history.reset();
        allocations = counter.count();
    }
    CHECK (succeeded);
    CHECK (allocations == 0);
}
