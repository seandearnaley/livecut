// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/DelayTimeline.h"
#include "AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <limits>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

namespace
{
struct Context { double bpm; int numerator, denominator; bool bypass; int setting; };
using Timeline = DelayTimeline<Context>;
constexpr Context firstContext { 120.0, 4, 4, false, 10 };
constexpr Context secondContext { 93.0, 3, 8, true, 99 };
} // namespace

TEST_CASE ("Delayed timeline retrieves original callback context at the render clock", "[delaytimeline]")
{
    for (const auto latency : { 0, 1, 17 })
    {
        Timeline timeline;
        REQUIRE (timeline.prepare (latency));
        CHECK (timeline.capacity() == static_cast<std::size_t> (latency + 2));
        std::int64_t input = 0;
        int callback = 0;
        for (const auto length : { 1, 23, 2, 7, 31, 1 })
        {
            const auto context = callback++ % 2 == 0 ? firstContext : secondContext;
            REQUIRE (timeline.append (input, length, 1, 0, context));
            for (int offset = 0; offset < length; ++offset)
            {
                const auto rendered = timeline.renderFrame (input + offset);
                if (input + offset < latency)
                {
                    CHECK_FALSE (rendered);
                    continue;
                }
                REQUIRE (rendered);
                CHECK (*rendered == input + offset - latency);
                const auto found = timeline.lookup (*rendered);
                REQUIRE (found);
                CHECK (found->segment.firstFrame + found->offset == *rendered);
                const auto expected = found->segment.firstFrame == 0 || found->segment.firstFrame == 24
                                      || found->segment.firstFrame == 33 ? firstContext : secondContext;
                CHECK_THAT (found->segment.context.bpm, WithinAbs (expected.bpm, 0.0));
                CHECK (found->segment.context.numerator == expected.numerator);
                CHECK (found->segment.context.denominator == expected.denominator);
                CHECK (found->segment.context.bypass == expected.bypass);
                CHECK (found->segment.context.setting == expected.setting);
                REQUIRE (timeline.retireBefore (*rendered + 1));
            }
            input += length;
        }
    }
}

TEST_CASE ("Timeline reserves the worst case one-frame callback occupancy without overwriting", "[delaytimeline]")
{
    Timeline timeline;
    REQUIRE (timeline.prepare (8));
    for (std::int64_t i = 0; i < 10; ++i)
        REQUIRE (timeline.append (i, 1, static_cast<std::uint64_t> (i + 1), i, firstContext));
    CHECK (timeline.segmentCount() == 10);
    CHECK (timeline.boundaryCount() == 10);
    CHECK_FALSE (timeline.append (10, 1, 11, 10, secondContext));
    CHECK (timeline.submittedEnd() == 10);
    REQUIRE (timeline.lookup (0));
    REQUIRE (timeline.retireBefore (1));
    REQUIRE (timeline.append (10, 1, 11, 10, secondContext));
    CHECK_FALSE (timeline.lookup (0));
    REQUIRE (timeline.lookup (10));
    CHECK (timeline.segmentCount() == 10);
}

TEST_CASE ("Short transport loops retain multiple pending epoch boundaries inside latency", "[delaytimeline]")
{
    Timeline timeline;
    REQUIRE (timeline.prepare (12));
    for (std::int64_t i = 0; i < 12; ++i)
        REQUIRE (timeline.append (i, 1, static_cast<std::uint64_t> (i / 3 + 1), i / 3 * 3, firstContext));
    for (std::int64_t frame = 0; frame < 12; ++frame)
    {
        const auto epoch = timeline.sourceEpoch (frame);
        REQUIRE (epoch);
        CHECK (epoch->id == static_cast<std::uint64_t> (frame / 3 + 1));
        CHECK (epoch->start == frame / 3 * 3);
        if (frame < 9)
        {
            REQUIRE (epoch->end);
            CHECK (*epoch->end == frame / 3 * 3 + 3);
        }
        else
            CHECK_FALSE (epoch->end);
    }
}

TEST_CASE ("Render epoch start survives retiring the context and its boundary marker", "[delaytimeline]")
{
    Timeline timeline;
    REQUIRE (timeline.prepare (2));
    REQUIRE (timeline.append (0, 20, 1, 0, firstContext));
    REQUIRE (timeline.retireBefore (20));
    CHECK (timeline.segmentCount() == 0);
    CHECK (timeline.boundaryCount() == 0);
    const auto held = timeline.currentRenderEpoch();
    REQUIRE (held);
    CHECK (held->start == 0);
    CHECK_FALSE (held->end);
    REQUIRE (timeline.append (20, 20, 1, 0, secondContext));
    REQUIRE (timeline.append (40, 10, 2, 40, firstContext));
    const auto old = timeline.sourceEpoch (25);
    REQUIRE (old);
    CHECK (old->start == 0);
    REQUIRE (old->end);
    CHECK (*old->end == 40);
    REQUIRE (timeline.retireBefore (40));
    const auto current = timeline.currentRenderEpoch();
    REQUIRE (current);
    CHECK (current->id == 2);
    CHECK (current->start == 40);
    CHECK_FALSE (current->end);
    CHECK (held->start == 0); // Returned values survive retirement and ring reuse.
    CHECK_FALSE (held->end);
}

TEST_CASE ("Timeline can begin inside an existing transport epoch without changing absolute frames", "[delaytimeline]")
{
    Timeline timeline;
    REQUIRE (timeline.prepare (17));
    REQUIRE (timeline.reset (100));
    REQUIRE (timeline.append (100, 20, 5, 20, firstContext));
    REQUIRE (timeline.retireBefore (105));
    const auto found = timeline.lookup (105);
    const auto epoch = timeline.sourceEpoch (105);
    REQUIRE (found);
    REQUIRE (epoch);
    CHECK (found->offset == 5);
    CHECK (epoch->start == 20);
    CHECK_FALSE (epoch->end);
    CHECK_FALSE (timeline.prepare (-1));
    CHECK_FALSE (timeline.prepare (std::numeric_limits<std::int64_t>::max()));
    REQUIRE (timeline.lookup (105));
    CHECK (timeline.capacity() == 19);
}

TEST_CASE ("Submitted callback metadata never claims that PCM has arrived", "[delaytimeline]")
{
    Timeline timeline;
    REQUIRE (timeline.prepare (4));
    REQUIRE (timeline.append (0, 100000, 1, 0, firstContext));
    CHECK (timeline.submittedEnd() == 100000);
    CHECK_FALSE (timeline.renderFrame (0));
    // Metadata is intentionally queryable before per-sample input ingestion.
    const auto futureMetadata = timeline.lookup (99999);
    REQUIRE (futureMetadata);
    CHECK (futureMetadata->offset == 99999);
    CHECK_FALSE (timeline.lookup (100000));
    CHECK_FALSE (timeline.sourceEpoch (100000));
    CHECK_FALSE (timeline.retireBefore (100001));
    CHECK_FALSE (timeline.lookup (-1));
    CHECK_FALSE (timeline.renderFrame (-1));
}

TEST_CASE ("Invalid callback continuity and epochs preserve accepted timeline state", "[delaytimeline]")
{
    Timeline timeline;
    REQUIRE (timeline.prepare (4));
    REQUIRE (timeline.append (0, 3, 10, 0, firstContext));
    REQUIRE (timeline.append (999, 0, 0, -1, secondContext)); // Zero frames are ignored.
    for (const auto fields : { std::array<std::int64_t, 4> { 2, 1, 10, 0 }, { 4, 1, 10, 0 },
                               { 3, -1, 10, 0 }, { 3, 1, 10, 1 }, { 3, 1, 9, 3 },
                               { 3, 1, 11, 2 }, { 3, 1, 0, 3 }, { -1, 1, 11, 3 } })
    {
        CHECK_FALSE (timeline.append (fields[0], fields[1], static_cast<std::uint64_t> (fields[2]), fields[3], secondContext));
        CHECK (timeline.submittedEnd() == 3);
        CHECK (timeline.segmentCount() == 1);
        CHECK (timeline.boundaryCount() == 1);
    }
    REQUIRE (timeline.retireBefore (2));
    CHECK_FALSE (timeline.retireBefore (1));
    REQUIRE (timeline.lookup (2));
    CHECK_FALSE (timeline.lookup (1));
    REQUIRE (timeline.append (3, 1, 11, 3, secondContext));
}

TEST_CASE ("Timeline reset clears metadata but never reuses transport identities", "[delaytimeline]")
{
    Timeline timeline;
    CHECK_FALSE (timeline.lookup (0));
    CHECK_FALSE (timeline.append (0, 1, 1, 0, firstContext));
    REQUIRE (timeline.prepare (2));
    REQUIRE (timeline.append (0, 1, 1, 0, firstContext));
    const auto copy = timeline.lookup (0);
    REQUIRE (copy);
    REQUIRE (timeline.reset());
    CHECK_FALSE (timeline.lookup (0));
    CHECK_FALSE (timeline.currentRenderEpoch());
    CHECK_FALSE (timeline.append (0, 1, 1, 0, secondContext));
    REQUIRE (timeline.append (0, 1, 2, 0, secondContext));
    CHECK (copy->segment.context.setting == 10);
    CHECK_FALSE (timeline.reset (-1));
    REQUIRE (timeline.lookup (0));
}

TEST_CASE ("Timeline rejects capacity frame arithmetic and transport counter exhaustion", "[delaytimeline]")
{
    Timeline timeline;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    CHECK_FALSE (timeline.prepare (-1));
    CHECK_FALSE (timeline.prepare (maximum));
    REQUIRE (timeline.prepare (0));
    REQUIRE (timeline.reset (maximum - 2));
    CHECK_FALSE (timeline.append (maximum - 2, 3, 1, maximum - 2, firstContext));
    REQUIRE (timeline.append (maximum - 2, 2, std::numeric_limits<std::uint64_t>::max(), maximum - 2, firstContext));
    CHECK (timeline.submittedEnd() == maximum);
    REQUIRE (timeline.lookup (maximum - 1));
    CHECK_FALSE (timeline.append (maximum, 1, std::numeric_limits<std::uint64_t>::max(), maximum - 2, firstContext));
    REQUIRE (timeline.reset());
    CHECK_FALSE (timeline.append (0, 1, 1, 0, firstContext));
    CHECK (timeline.submittedEnd() == 0);
    CHECK (timeline.segmentCount() == 0);
}

TEST_CASE ("Delay timeline ingestion lookup retirement and reset allocate nothing", "[delaytimeline]")
{
    Timeline timeline;
    REQUIRE (timeline.prepare (17));
    bool accepted = true;
    std::size_t allocations = 0;
    {
        testing::AllocationCounter counter;
        for (std::int64_t input = 0; input < 10000; ++input)
        {
            accepted &= timeline.append (input, 1, static_cast<std::uint64_t> (input / 3 + 1), input / 3 * 3, firstContext);
            const auto rendered = timeline.renderFrame (input);
            if (rendered)
            {
                const auto context = timeline.lookup (*rendered);
                const auto epoch = timeline.sourceEpoch (*rendered);
                accepted &= context && context->segment.firstFrame == *rendered && context->offset == 0;
                accepted &= epoch && epoch->id == static_cast<std::uint64_t> (*rendered / 3 + 1)
                            && epoch->start == *rendered / 3 * 3;
                accepted &= timeline.retireBefore (*rendered + 1);
            }
        }
        accepted &= timeline.reset();
        allocations = counter.count();
    }
    CHECK (accepted);
    CHECK (allocations == 0);
}
