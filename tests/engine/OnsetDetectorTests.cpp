// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/OnsetDetector.h"
#include "AllocationCounter.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace livecut::engine;

namespace
{
using Signal = std::vector<std::array<float, 2>>;

void feed (OnsetDetector& detector, const Signal& audio, std::size_t blockSize = 127)
{
    bool accepted = true;
    for (std::size_t start = 0; start < audio.size(); start += blockSize)
        for (auto position = start; position < std::min (start + blockSize, audio.size()); ++position)
            accepted &= detector.process (static_cast<std::int64_t> (position), audio[position][0], audio[position][1]);
    REQUIRE (accepted);
}

std::vector<OnsetMarker> markers (const OnsetDetector& detector)
{
    std::vector<OnsetMarker> result;
    for (std::size_t i = 0; i < detector.markerCount(); ++i)
    {
        const auto candidate = detector.marker (i);
        REQUIRE (candidate);
        result.push_back (*candidate);
    }
    return result;
}

Signal percussion (int rate)
{
    Signal audio (static_cast<std::size_t> (rate / 5));
    // Three same-level, short decaying attacks, including a 12ms pair.
    for (const auto milliseconds : { 20, 32, 100 })
        for (int i = 0; i < rate / 250; ++i)
        {
            const auto sample = static_cast<float> (std::exp (-static_cast<double> (i) / (rate * 0.0005)));
            const auto position = static_cast<std::size_t> (rate * milliseconds / 1000 + i);
            audio[position] = { sample, -sample };
        }
    return audio;
}
} // namespace

TEST_CASE ("Onset markers timestamp linked stereo impulses with bounded publication delay", "[onset]")
{
    for (const auto channel : { 0, 1, 2 })
    {
        OnsetDetector detector;
        REQUIRE (detector.prepare (48000, 8));
        Signal audio (4800);
        audio[1013] = { channel == 1 ? 0.0f : 1.0f, channel == 0 ? 0.0f : -1.0f };
        audio[1014] = audio[1013]; // Equal peaks choose the earliest frame.
        feed (detector, audio);
        REQUIRE (detector.markerCount() == 1);
        const auto hit = detector.marker (0);
        REQUIRE (hit);
        CHECK (hit->epoch == detector.epoch());
        CHECK (hit->frame == 1013);
        CHECK (hit->detectedAt >= hit->frame);
        CHECK (hit->detectedAt - hit->frame < detector.windowFrames());
        CHECK (hit->detectedAt == 1055);
    }
}

TEST_CASE ("Onset analysis is unchanged by host block partition and resolves close percussion", "[onset]")
{
    for (const auto rate : { 8000, 44100, 48000, 96000, 192000 })
    {
        const auto audio = percussion (rate);
        std::vector<OnsetMarker> expected;
        for (const auto block : { 1, 17, 128, 511 })
        {
            OnsetDetector detector;
            REQUIRE (detector.prepare (rate, 8));
            feed (detector, audio, static_cast<std::size_t> (block));
            const auto actual = markers (detector);
            REQUIRE (actual.size() == 3);
            if (expected.empty())
                expected = actual;
            for (std::size_t i = 0; i < actual.size(); ++i)
            {
                CHECK (actual[i].epoch == expected[i].epoch);
                CHECK (actual[i].frame == expected[i].frame);
                CHECK (actual[i].detectedAt == expected[i].detectedAt);
                CHECK (std::bit_cast<std::uint64_t> (actual[i].riseDb)
                       == std::bit_cast<std::uint64_t> (expected[i].riseDb));
                const std::array milliseconds { 20, 32, 100 };
                const auto physical = rate * milliseconds[i] / 1000;
                CHECK (actual[i].frame >= physical);
                CHECK (actual[i].frame - physical < detector.windowFrames());
                CHECK (actual[i].detectedAt - actual[i].frame < detector.windowFrames());
            }
        }
    }
}

TEST_CASE ("Silence and steady tones do not produce repeating onset markers", "[onset]")
{
    for (const auto rate : { 44100, 48000, 96000 })
        for (const auto frequency : { 20.0, 50.0, 100.0, 250.0, 997.0, 10000.0 })
        {
            OnsetDetector detector;
            REQUIRE (detector.prepare (rate, 128, 3.0));
            Signal audio (static_cast<std::size_t> (rate));
            for (int i = rate / 10; i < rate; ++i)
            {
                const auto value = static_cast<float> (0.5 * std::sin (2.0 * std::numbers::pi * frequency * (i - rate / 10) / rate));
                audio[static_cast<std::size_t> (i)] = { value, -value };
            }
            feed (detector, audio);
            REQUIRE (detector.markerCount() >= 1);
            CHECK (detector.markerCount() == 1);
            REQUIRE (detector.reset());
            feed (detector, Signal (static_cast<std::size_t> (rate)));
            CHECK (detector.markerCount() == 0);
        }
}

TEST_CASE ("Damped oscillating percussion produces attack candidates without tail retriggers", "[onset]")
{
    for (const auto rate : { 44100, 48000, 96000 })
    {
        OnsetDetector detector;
        REQUIRE (detector.prepare (rate, 8, 3.0));
        Signal audio (static_cast<std::size_t> (rate / 2));
        for (const auto milliseconds : { 20, 100, 300 })
            for (int i = 0; i < rate / 25; ++i)
            {
                const auto time = static_cast<double> (i) / rate;
                const auto value = static_cast<float> (std::sin (2.0 * std::numbers::pi * 100.0 * time)
                                                       * std::exp (-time / 0.006));
                audio[static_cast<std::size_t> (rate * milliseconds / 1000 + i)] = { value, -value };
            }
        feed (detector, audio);
        const auto actual = markers (detector);
        REQUIRE (actual.size() == 3);
        for (std::size_t i = 0; i < actual.size(); ++i)
        {
            const std::array milliseconds { 20, 100, 300 };
            const auto physical = rate * milliseconds[i] / 1000;
            CHECK (actual[i].frame >= physical);
            CHECK (actual[i].frame - physical < rate * 3 / 1000);
        }
    }
}

TEST_CASE ("Onset refractory period suppresses hits less than ten milliseconds apart", "[onset]")
{
    OnsetDetector detector;
    REQUIRE (detector.prepare (48000, 8, 3.0));
    Signal audio (4800);
    for (const auto frame : { 960, 1200, 1536 })
        audio[static_cast<std::size_t> (frame)] = { 1.0f, -1.0f };
    feed (detector, audio);
    REQUIRE (detector.markerCount() == 2);
    CHECK (detector.marker (0)->frame == 960);
    CHECK (detector.marker (1)->frame == 1536);
}

TEST_CASE ("Soft attacks produce candidates and sensitivity is a minimum energy rise in dB", "[onset]")
{
    Signal audio (4800);
    for (int i = 0; i < 960; ++i)
    {
        const auto value = 0.03f * static_cast<float> (i) / 960.0f;
        audio[static_cast<std::size_t> (960 + i)] = { value, -value };
    }
    OnsetDetector sensitive;
    REQUIRE (sensitive.prepare (48000, 8, 3.0));
    feed (sensitive, audio);
    REQUIRE (sensitive.markerCount() == 1);
    const auto candidate = sensitive.marker (0);
    REQUIRE (candidate);
    CHECK (candidate->frame >= 960);
    CHECK (candidate->frame < 1200);
    CHECK (candidate->riseDb >= 3.0);
    OnsetDetector selective;
    REQUIRE (selective.prepare (48000, 8, 24.0));
    feed (selective, audio);
    CHECK (selective.markerCount() == 0);
}

TEST_CASE ("Nearest onset query respects epoch window strength and missing analysis", "[onset]")
{
    OnsetDetector detector;
    REQUIRE (detector.prepare (48000, 8));
    Signal audio (4800);
    audio[960] = { 1.0f, -1.0f };
    audio[1920] = { 1.0f, -1.0f };
    feed (detector, audio);
    REQUIRE (detector.markerCount() == 2);
    const auto epoch = detector.epoch();
    REQUIRE (detector.nearest (epoch, 1440, 0, 4800));
    CHECK (detector.nearest (epoch, 1440, 0, 4800)->frame == 960); // ties choose earlier
    const auto closer = detector.nearest (epoch, 1700, 0, 4800);
    REQUIRE (closer);
    CHECK (closer->frame == 1920);
    const auto exact = detector.nearest (epoch, 960, 960, 961);
    REQUIRE (exact);
    CHECK (exact->frame == 960);
    CHECK_FALSE (detector.nearest (epoch, 960, 0, 960));
    CHECK_FALSE (detector.nearest (epoch, 1700, 1000, 1900));
    CHECK_FALSE (detector.nearest (epoch, 1700, 0, 4800, 1000.0));
    CHECK_FALSE (detector.nearest (epoch + 1, 1700, 0, 4800));
    CHECK_FALSE (detector.nearest (epoch, 1700, 0, 4801));
    CHECK_FALSE (detector.nearest (epoch, 1700, -1, 4800));
    CHECK_FALSE (detector.nearest (epoch, -1, 0, 4800));
    CHECK_FALSE (detector.nearest (epoch, 1700, 2000, 1000));
    CHECK_FALSE (detector.nearest (epoch, 1700, 0, 4800, std::numeric_limits<double>::quiet_NaN()));
    REQUIRE (detector.reset());
    CHECK (detector.epoch() == epoch + 1);
    CHECK (detector.markerCount() == 0);
    CHECK_FALSE (detector.nearest (epoch, 960, 0, 4800));
}

TEST_CASE ("Marker history visibly overwrites oldest candidates and reset clears partial windows", "[onset]")
{
    OnsetDetector detector;
    REQUIRE (detector.prepare (48000, 2));
    feed (detector, percussion (48000));
    REQUIRE (detector.markerCount() == 2);
    CHECK (detector.overwrittenMarkers() == 1);
    REQUIRE (detector.marker (0));
    CHECK (detector.marker (0)->frame >= 1536);
    CHECK_FALSE (detector.marker (2));
    CHECK_FALSE (detector.nearest (detector.epoch(), 960, 960, 1000));
    REQUIRE (detector.reset());
    CHECK (detector.overwrittenMarkers() == 0);
    REQUIRE (detector.process (0, 1.0f, -1.0f));
    REQUIRE (detector.reset());
    feed (detector, Signal (1000));
    CHECK (detector.markerCount() == 0);
}

TEST_CASE ("Onset analysis rejects invalid preparation samples and timestamp discontinuities", "[onset]")
{
    OnsetDetector detector;
    CHECK_FALSE (detector.process (0, 0.0f, 0.0f));
    CHECK_FALSE (detector.prepare (0.0, 8));
    CHECK_FALSE (detector.prepare (std::numeric_limits<double>::infinity(), 8));
    CHECK_FALSE (detector.prepare (48000, 0));
    CHECK_FALSE (detector.prepare (48000, std::numeric_limits<std::size_t>::max()));
    CHECK_FALSE (detector.prepare (48000, 8, 2.0));
    CHECK_FALSE (detector.prepare (48000, 8, 25.0));
    CHECK_FALSE (detector.prepare (48000, 8, std::numeric_limits<double>::quiet_NaN()));
    REQUIRE (detector.prepare (48000, 8));
    CHECK_FALSE (detector.process (1, 1.0f, 1.0f));
    CHECK_FALSE (detector.process (-1, 1.0f, 1.0f));
    CHECK_FALSE (detector.process (0, std::numeric_limits<float>::quiet_NaN(), 0.0f));
    CHECK_FALSE (detector.process (0, 0.0f, std::numeric_limits<float>::infinity()));
    CHECK (detector.nextFrame() == 0);
    REQUIRE (detector.process (0, 0.0f, 0.0f));
    CHECK_FALSE (detector.process (0, 0.0f, 0.0f));
    CHECK (detector.nextFrame() == 1);
    CHECK (detector.analyzedFrames() == 0);
    CHECK_FALSE (detector.nearest (detector.epoch(), 0, 0, 1));
}

TEST_CASE ("Onset processing queries and reset allocate nothing after preparation", "[onset]")
{
    OnsetDetector detector;
    REQUIRE (detector.prepare (48000, 4));
    const auto audio = percussion (48000);
    bool accepted = true;
    std::size_t allocations = 0;
    {
        testing::AllocationCounter counter;
        for (std::size_t i = 0; i < audio.size(); ++i)
        {
            accepted &= detector.process (static_cast<std::int64_t> (i), audio[i][0], audio[i][1]);
            (void) detector.nearest (detector.epoch(), static_cast<std::int64_t> (i), 0, detector.nextFrame());
        }
        accepted &= detector.reset();
        allocations = counter.count();
    }
    CHECK (accepted);
    CHECK (allocations == 0);
}
