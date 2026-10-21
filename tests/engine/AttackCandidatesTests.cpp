// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/AttackCandidates.h"
#include "AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <limits>

using namespace livecut::engine;
using State = AttackCandidates::State;

namespace
{
// Fixed analysis fixtures expose the same read-only interface as OnsetDetector.
// This also permits testing an explicit cache overflow despite the real
// detector's refractory bound making >5 legitimate candidates impossible.
struct Analysis
{
    std::uint64_t currentEpoch = 1;
    std::int64_t first = 0, written = 2000, analyzed = 2000;
    int hop = 8;
    std::array<OnsetMarker, 8> markers {};
    std::size_t size = 0;
    mutable int scans = 0;
    std::uint64_t epoch() const { return currentEpoch; }
    std::int64_t epochFirstFrame() const { return first; }
    std::int64_t nextFrame() const { return written; }
    std::int64_t analyzedFrames() const { return analyzed; }
    int windowFrames() const { return hop; }
    std::size_t markerCount() const { ++scans; return size; }
    std::optional<OnsetMarker> marker (std::size_t i) const { return i < size ? std::optional { markers[i] } : std::nullopt; }
};
} // namespace

TEST_CASE ("Attack cache includes all five inclusive search endpoints and latches preroll", "[attackcache]")
{
    OnsetDetector detector;
    REQUIRE (detector.prepare (8000, 16, 3.0));
    for (std::int64_t frame = 0; frame < 400; ++frame)
    {
        const auto impulse = frame <= 320 && frame % 80 == 0 ? 1.0f : 0.0f;
        REQUIRE (detector.process (frame, impulse, -impulse));
    }
    AttackCandidates cache;
    REQUIRE (cache.reset (160, detector.epoch(), 0, 8000, 3.0) == State::pending);
    REQUIRE (cache.update (detector) == State::ready);
    REQUIRE (cache.count() == 5);
    for (std::size_t i = 0; i < 5; ++i)
    {
        const auto marker = cache.candidate (i);
        REQUIRE (marker);
        CHECK (marker->frame == static_cast<std::int64_t> (i) * 80);
    }
    CHECK_FALSE (cache.candidate (5));
    const auto choice = cache.nearestEligible (10, [] (auto, auto) { return true; });
    REQUIRE (choice);
    CHECK (choice->marker.frame == 160);
    CHECK (choice->sourceStart == 144); // ceil(2ms * 8kHz), linked for both channels.
}

TEST_CASE ("Attack cache waits for the full future window and becomes immutable after ready", "[attackcache]")
{
    Analysis analysis;
    analysis.written = analysis.analyzed = 1256;
    analysis.markers[0] = { 1, 1100, 1103, 10.0 };
    analysis.size = 1;
    AttackCandidates cache;
    REQUIRE (cache.reset (1100, 1, 0, 8000, 6.0) == State::pending);
    CHECK (cache.update (analysis) == State::pending); // Inclusive end1260 requires completed window1264.
    CHECK (cache.count() == 0);
    CHECK (analysis.scans == 0);
    analysis.written = analysis.analyzed = 1264;
    REQUIRE (cache.update (analysis) == State::ready);
    CHECK (analysis.scans == 1);
    analysis.markers[0] = { 1, 1120, 1127, 20.0 };
    analysis.size = 0;
    CHECK (cache.update (analysis, -1) == State::ready); // Ready never consults later metadata.
    CHECK (analysis.scans == 1);
    const auto choice = cache.nearestEligible (8, [] (auto, auto) { return true; });
    REQUIRE (choice);
    CHECK (choice->marker.frame == 1100);
    CHECK (choice->sourceStart == 1084);
}

TEST_CASE ("Closed epoch analysis discards only its partial tail after closure is known", "[attackcache]")
{
    Analysis analysis;
    analysis.currentEpoch = 2;
    analysis.first = 343;
    analysis.written = analysis.analyzed = 1000;
    analysis.markers[0] = { 1, 302, 308, 10.0 };
    analysis.markers[1] = { 1, 342, 342, 10.0 }; // No published marker may come from the discarded tail.
    analysis.markers[2] = { 2, 350, 350, 10.0 };
    analysis.size = 3;
    AttackCandidates cache;
    REQUIRE (cache.reset (330, 1, 5, 8000, 3.0) == State::pending);
    CHECK (cache.update (analysis) == State::pending); // Future epoch time cannot imply old closure.
    CHECK (analysis.scans == 0);
    REQUIRE (cache.update (analysis, 343) == State::ready);
    REQUIRE (cache.count() == 1); // prefix=5+floor((343-5)/8)*8=341; the tail supplies no markers.
    REQUIRE (cache.candidate (0));
    CHECK (cache.candidate (0)->frame == 302);
}

TEST_CASE ("Closed epochs use their own alignment and do not wait for impossible analysis", "[attackcache]")
{
    Analysis analysis;
    analysis.first = 5;
    analysis.written = 343;
    analysis.analyzed = 341;
    analysis.markers[0] = { 1, 340, 340, 10.0 };
    analysis.size = 1;
    AttackCandidates cache;
    REQUIRE (cache.reset (330, 1, 5, 8000, 3.0) == State::pending);
    CHECK (cache.update (analysis) == State::pending);
    REQUIRE (cache.update (analysis, 343) == State::ready);
    CHECK (cache.count() == 1);
    AttackCandidates empty;
    REQUIRE (empty.reset (6, 1, 5, 8000, 3.0) == State::pending);
    analysis.size = 0;
    analysis.written = 7;
    analysis.analyzed = 5;
    REQUIRE (empty.update (analysis, 7) == State::ready);
    CHECK (empty.count() == 0); // Ready-empty is distinct from pending.
}

TEST_CASE ("Attack selection tests entire spans before choosing nearest and uses earlier ties", "[attackcache]")
{
    Analysis analysis;
    analysis.markers[0] = { 1, 1040, 1047, 10.0 };
    analysis.markers[1] = { 1, 1090, 1095, 10.0 };
    analysis.markers[2] = { 1, 1160, 1167, 10.0 };
    analysis.size = 3;
    AttackCandidates cache;
    REQUIRE (cache.reset (1100, 1, 0, 8000, 3.0) == State::pending);
    REQUIRE (cache.update (analysis) == State::ready);
    int checked = 0;
    const auto choice = cache.nearestEligible (80, [&] (std::int64_t start, std::int64_t count)
    {
        ++checked;
        CHECK (count == 80);
        return start + count <= 1150; // Nearest starts1074 but its complete source exceeds1150.
    });
    REQUIRE (choice);
    CHECK (choice->marker.frame == 1040);
    CHECK (checked == 3);
    const auto tie = cache.nearestEligible (10, [] (auto start, auto) { return start != 1074; });
    REQUIRE (tie);
    CHECK (tie->marker.frame == 1040);
    CHECK_FALSE (cache.nearestEligible (0, [] (auto, auto) { return true; }));
    CHECK_FALSE (cache.nearestEligible (80, [] (auto, auto) { return false; }));
}

TEST_CASE ("Attack source eligibility is fresh per activation but candidate origin never moves", "[attackcache]")
{
    Analysis analysis;
    analysis.markers[0] = { 1, 1100, 1103, 12.0 };
    analysis.size = 1;
    AttackCandidates cache;
    REQUIRE (cache.reset (1100, 1, 0, 8000, 6.0) == State::pending);
    REQUIRE (cache.update (analysis) == State::ready);
    std::int64_t availableEnd = 1100;
    const auto eligible = [&] (std::int64_t start, std::int64_t count) { return start >= 1000 && start + count <= availableEnd; };
    CHECK_FALSE (cache.nearestEligible (32, eligible));
    availableEnd = 1200;
    const auto selected = cache.nearestEligible (32, eligible);
    REQUIRE (selected);
    CHECK (selected->sourceStart == 1084);
    availableEnd = 1100;
    CHECK_FALSE (cache.nearestEligible (32, eligible));
    CHECK (selected->sourceStart == 1084);
}

TEST_CASE ("Attack cache reports overflow explicitly and never publishes a truncated list", "[attackcache]")
{
    Analysis analysis;
    analysis.size = 6;
    for (std::size_t i = 0; i < analysis.size; ++i)
        analysis.markers[i] = { 1, 1000 + static_cast<std::int64_t> (i), 1007, 12.0 };
    AttackCandidates cache;
    REQUIRE (cache.reset (1100, 1, 0, 8000, 3.0) == State::pending);
    CHECK (cache.update (analysis) == State::overflow);
    CHECK (cache.count() == 0);
    CHECK_FALSE (cache.nearestEligible (10, [] (auto, auto) { return true; }));
    analysis.size = 0;
    CHECK (cache.update (analysis) == State::overflow);
    CHECK (analysis.scans == 1);
}

TEST_CASE ("Attack cache rejects invalid configuration closure and integer bounds", "[attackcache]")
{
    AttackCandidates cache;
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    const auto nan = std::numeric_limits<double>::quiet_NaN();
    for (const auto rate : { 0.0, 7999.0, 384001.0, nan })
        CHECK (cache.reset (1000, 1, 0, rate, 3.0) == State::invalid);
    for (const auto threshold : { 2.0, 25.0, nan })
        CHECK (cache.reset (1000, 1, 0, 8000, threshold) == State::invalid);
    CHECK (cache.reset (-1, 1, 0, 8000, 3.0) == State::invalid);
    CHECK (cache.reset (1000, 0, 0, 8000, 3.0) == State::invalid);
    CHECK (cache.reset (1000, 1, 1001, 8000, 3.0) == State::invalid);
    CHECK (cache.reset (maximum, 1, 0, 8000, 3.0) == State::invalid);
    Analysis analysis;
    REQUIRE (cache.reset (1000, 1, 0, 8000, 3.0) == State::pending);
    CHECK (cache.update (analysis, 1000) == State::invalid);
    REQUIRE (cache.reset (1000, 1, 0, 8000, 3.0) == State::pending);
    analysis.first = 5;
    CHECK (cache.update (analysis) == State::invalid);
}

TEST_CASE ("Attack cache rejects mismatched detector window configuration", "[attackcache]")
{
    Analysis analysis;
    analysis.hop = 48;
    AttackCandidates cache;
    REQUIRE (cache.reset (1000, 1, 0, 8000, 3.0) == State::pending);
    CHECK (cache.update (analysis) == State::invalid);
}

TEST_CASE ("Attack cache separately rounds search and preroll and filters measured rise", "[attackcache]")
{
    Analysis analysis;
    analysis.hop = 44;
    analysis.analyzed = 1980;
    analysis.markers[0] = { 1, 117, 131, 5.0 }; // Exact inclusive left endpoint.
    analysis.markers[1] = { 1, 1000, 1001, 5.0 }; // Below this block's latched threshold.
    analysis.markers[2] = { 1, 1883, 1891, 7.0 }; // Exact inclusive right endpoint.
    analysis.size = 3;
    AttackCandidates cache;
    REQUIRE (cache.reset (1000, 1, 0, 44101, 6.0) == State::pending);
    REQUIRE (cache.update (analysis) == State::ready);
    REQUIRE (cache.count() == 1);
    const auto choice = cache.nearestEligible (4, [] (auto, auto) { return true; });
    REQUIRE (choice);
    CHECK (choice->marker.frame == 1883); // ceil(20ms*44101)=883, not882.
    CHECK (choice->sourceStart == 1794); // ceil(2ms*44101)=89, not88.
    CHECK_FALSE (cache.nearestEligible (std::numeric_limits<std::int64_t>::max(), [] (auto, auto) { return true; }));
}

TEST_CASE ("Attack threshold filters measured rise and old epoch markers remain selectable", "[attackcache]")
{
    OnsetDetector detector;
    REQUIRE (detector.prepare (8000, 128, 3.0));
    for (std::int64_t frame = 0; frame < 400; ++frame)
        REQUIRE (detector.process (frame, frame == 160 ? 1.0f : 0.0f, 0.0f));
    const auto oldEpoch = detector.epoch();
    REQUIRE (detector.beginTransportEpoch (oldEpoch + 1));
    for (std::int64_t frame = 400; frame < 800; ++frame)
        REQUIRE (detector.process (frame, frame == 480 ? 1.0f : 0.0f, 0.0f));
    AttackCandidates cache;
    REQUIRE (cache.reset (160, oldEpoch, 0, 8000, 24.0) == State::pending);
    REQUIRE (cache.update (detector, 400) == State::ready);
    REQUIRE (cache.count() == 1);
    const auto selected = cache.nearestEligible (20, [] (auto, auto) { return true; });
    REQUIRE (selected);
    CHECK (selected->marker.epoch == oldEpoch);
    CHECK (selected->sourceStart == 144);
}

TEST_CASE ("Attack cache preparation readiness and eligibility allocate nothing", "[attackcache]")
{
    Analysis analysis;
    analysis.markers[0] = { 1, 1100, 1103, 12.0 };
    analysis.size = 1;
    AttackCandidates cache;
    bool valid = true;
    std::size_t allocations = 0;
    {
        testing::AllocationCounter counter;
        valid &= cache.reset (1100, 1, 0, 8000, 3.0) == State::pending;
        valid &= cache.update (analysis) == State::ready;
        for (int i = 0; i < 1000; ++i)
        {
            valid &= cache.update (analysis) == State::ready;
            valid &= cache.nearestEligible (20, [] (auto, auto) { return true; }).has_value();
        }
        allocations = counter.count();
    }
    CHECK (valid);
    CHECK (allocations == 0);
    CHECK (analysis.scans == 1);
}
