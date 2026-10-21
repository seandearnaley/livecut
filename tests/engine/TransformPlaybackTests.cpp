// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/LivePlayer.h"
#include "engine/TransformSpan.h"
#include "AllocationCounter.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <array>
#include <cmath>
#include <cstdint>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

namespace
{
StereoFrame inputFrame (std::int64_t frame)
{
    return { static_cast<float> (frame % 997 + 1) / 1000.0f,
             -static_cast<float> (frame % 991 + 1) / 500.0f };
}
bool ingest (LivePlayer& player, std::int64_t first, std::int64_t count)
{
    bool valid = true;
    for (auto frame = first; frame < first + count; ++frame)
    {
        const auto raw = inputFrame (frame);
        valid &= player.ingestFrame (raw.left, raw.right);
    }
    return valid;
}
StereoFrame render (LivePlayer& player, bool advance = true)
{
    StereoFrame output { 9.0f, -9.0f };
    player.renderFrame (output.left, output.right, advance);
    return output;
}
}

TEST_CASE ("Transformed forward and reverse preserve fractional rate pan and fades", "[player][transform][playback]")
{
    const auto cents = GENERATE (-1200.0f, 0.0f, 1200.0f, 2400.0f);
    const auto pan = GENERATE (-1.0f, 0.0f, 1.0f);
    const auto fade = GENERATE (1, 4);
    const auto reverse = GENERATE (false, true);
    LivePlayer player;
    player.prepare (64);
    player.setFadeSamples (fade);
    REQUIRE (ingest (player, 0, 64));
    const std::array cuts { Cut { 1.0 }, Cut { 8.0, 1.0, pan, .5f, cents } };
    player.startBlock (cuts, 1.0, player.sourceAtFrame (0));
    render (player); // prior sample rolls over without consuming the new cut
    REQUIRE (player.currentCut() == 1);
    REQUIRE (player.currentSoundingFrames() == 8);
    const auto pitch = std::pow (2.0, cents / 1200.0);
    const auto frames = requiredSourceFrames (8, pitch, 64);
    REQUIRE (frames);
    const SourceRegion source { player.retainedSource().epoch, 10, *frames };
    REQUIRE (player.setCurrentTransform (source, reverse));
    CHECK (player.cuts()[1].transformDecided);
    for (int frame = 0; frame < 8; ++frame)
    {
        const auto coordinate = reverse ? static_cast<double> (*frames - 1) - frame * pitch : frame * pitch;
        const auto index = static_cast<std::int64_t> (coordinate);
        const auto first = inputFrame (10 + index), next = inputFrame (11 + index);
        const auto fraction = static_cast<float> (coordinate - static_cast<double> (index));
        auto left = first.left + fraction * (next.left - first.left);
        auto right = first.right + fraction * (next.right - first.right);
        if (pan < 0.0f) { left += right; right = 0.0f; }
        else if (pan > 0.0f) { right += left; left = 0.0f; }
        const auto envelope = .5f * fadeEnvelope (frame, fade, 8);
        const auto output = render (player);
        CHECK_THAT (output.left, WithinAbs (left * envelope, 1.0e-7));
        CHECK_THAT (output.right, WithinAbs (right * envelope, 1.0e-7));
    }
    CHECK_FALSE (player.hasSourceError());
}

TEST_CASE ("Transform eligibility uses the complete physical gate without classic source truncation", "[player][transform][playback]")
{
    LivePlayer player;
    player.prepare (16);
    REQUIRE (ingest (player, 0, 32));
    const std::array cuts { Cut { 4.0 }, Cut { 16.0, 1.0, 0.0f, 1.0f, 1200.0f } };
    player.startBlock (cuts, 1.0, player.sourceAtFrame (0));
    for (int frame = 0; frame < 4; ++frame) render (player);
    REQUIRE (player.currentCut() == 1);
    CHECK (player.currentSoundingFrames() == 16);
    CHECK (player.cuts()[1].playable < 16);
    CHECK_FALSE (player.setCurrentTransform ({ player.retainedSource().epoch, 0, 16 }, true));
    CHECK_FALSE (player.cuts()[1].transformDecided);
    player.markCurrentTransformDecided();
    CHECK (player.cuts()[1].transformDecided);
}

TEST_CASE ("Only complete bounded source spans can be selected", "[player][transform][playback]")
{
    LivePlayer player;
    player.prepare (16);
    REQUIRE (ingest (player, 0, 17));
    const std::array cuts { Cut { 4.0 } };
    player.startBlock (cuts, 1.0, player.sourceAtFrame (0));
    const auto epoch = player.retainedSource().epoch;
    CHECK (player.sourceAvailable ({ epoch, 0, 4 }));
    CHECK_FALSE (player.setCurrentTransform ({ epoch, 0, 3 }, true));
    CHECK_FALSE (player.setCurrentTransform ({ epoch, 15, 4 }, true));
    CHECK_FALSE (player.setCurrentTransform ({ epoch, 0, 17 }, true));
    CHECK_FALSE (player.setCurrentTransform ({ epoch + 1, 0, 4 }, true));
    CHECK_FALSE (player.setCurrentTransform ({ epoch, 0, 0 }, true));
    CHECK_FALSE (player.setCurrentTransform ({ epoch, -1, 4 }, true));
    REQUIRE (player.setCurrentTransform ({ epoch, 0, 4 }, true));
}

TEST_CASE ("Selection is immutable and ineligible decisions cannot change midvoice", "[player][transform][playback]")
{
    LivePlayer player;
    player.prepare (32);
    REQUIRE (ingest (player, 0, 32));
    const std::array cuts { Cut { 4.0 }, Cut { 4.0 } };
    player.startBlock (cuts, 1.0, player.sourceAtFrame (0));
    const auto epoch = player.retainedSource().epoch;
    REQUIRE (player.setCurrentTransform ({ epoch, 10, 4 }, true));
    CHECK_FALSE (player.setCurrentTransform ({ epoch, 20, 4 }, false));
    render (player);
    CHECK_FALSE (player.setCurrentTransform ({ epoch, 20, 4 }, false));
    const auto output = render (player);
    CHECK_THAT (output.left, WithinAbs (inputFrame (12).left * fadeEnvelope (1, 1, 4), 1.0e-7));
    render (player); render (player);
    REQUIRE (player.currentCut() == 1);
    CHECK_FALSE (player.cuts()[1].transformDecided);
    player.markCurrentTransformDecided();
    CHECK_FALSE (player.setCurrentTransform ({ epoch, 10, 4 }, true));
}

TEST_CASE ("A transform freezes original output budget as well as ceil-padded source", "[player][transform][playback][tempo]")
{
    LivePlayer player;
    player.prepare (64);
    REQUIRE (ingest (player, 0, 64));
    const std::array cuts { Cut { 1.0, .4, 0.0f, 1.0f, -1200.0f } };
    player.startBlock (cuts, 10.0, player.sourceAtFrame (0));
    REQUIRE (player.currentSoundingFrames() == 4);
    REQUIRE (player.setCurrentTransform ({ player.retainedSource().epoch, 10, 3 }, true));
    render (player); render (player);
    player.setSamplesPerUnit (20.0);
    CHECK (player.currentSoundingFrames() == 6);
    const auto third = render (player), fourth = render (player), fifth = render (player);
    CHECK_THAT (third.left, WithinAbs (inputFrame (11).left * fadeEnvelope (2, 1, 4), 1.0e-7));
    const auto blended = .5f * (inputFrame (10).left + inputFrame (11).left);
    CHECK_THAT (fourth.left, WithinAbs (blended * fadeEnvelope (3, 1, 4), 1.0e-7));
    CHECK_THAT (fifth.left, WithinAbs (0.0, 0.0));
    CHECK_FALSE (player.hasSourceError());
}

TEST_CASE ("Swing retiming exposes physical sounding frames and can shorten a selected voice", "[player][transform][playback][tempo]")
{
    LivePlayer player;
    player.prepare (64);
    REQUIRE (ingest (player, 0, 64));
    const std::array cuts { Cut { 1.0, .5 } };
    player.startBlock (cuts, 10.0, player.sourceAtFrame (0));
    player.retimeMusicalCut (12, 7, 1);
    REQUIRE (player.currentSoundingFrames() == 7);
    REQUIRE (player.setCurrentTransform ({ player.retainedSource().epoch, 10, 7 }, true));
    render (player, false);
    player.retimeMusicalCut (4, 2, 2);
    CHECK (player.currentSoundingFrames() == 3);
    render (player, false);
    const auto final = render (player, false), gated = render (player, false);
    CHECK_THAT (final.left, WithinAbs (inputFrame (14).left * fadeEnvelope (2, 1, 3), 1.0e-7));
    CHECK_THAT (gated.left, WithinAbs (0.0, 0.0));
}

TEST_CASE ("Lost selected source gates explicitly without substituting ordinary audio", "[player][transform][playback]")
{
    LivePlayer player;
    player.prepare (16, 4);
    REQUIRE (ingest (player, 0, 4));
    const std::array cuts { Cut { 4.0 } };
    player.startBlock (cuts, 1.0, player.sourceAtFrame (0));
    const SourceRegion selected { player.retainedSource().epoch, 0, 4 };
    REQUIRE (player.setCurrentTransform (selected, true));
    render (player);
    REQUIRE (player.ingestFrame (.75f, -.75f)); // evicts only the selected first frame
    CHECK_FALSE (player.sourceAvailable (selected));
    const auto output = render (player);
    CHECK_THAT (output.left, WithinAbs (0.0, 0.0));
    CHECK_THAT (output.right, WithinAbs (0.0, 0.0));
    CHECK (player.hasSourceError());
}

TEST_CASE ("Transform selection rendering and rollover allocate no memory", "[player][transform][playback][allocations]")
{
    LivePlayer player;
    player.prepare (64);
    REQUIRE (ingest (player, 0, 64));
    const std::array cuts { Cut { 8.0 }, Cut { 8.0 } };
    player.startBlock (cuts, 1.0, player.sourceAtFrame (0));
    bool selected = false;
    std::size_t allocations = 0;
    {
        const testing::AllocationCounter counter;
        selected = player.setCurrentTransform ({ player.retainedSource().epoch, 10, 8 }, true);
        for (int frame = 0; frame < 8; ++frame) render (player);
        player.markCurrentTransformDecided();
        for (int frame = 0; frame < 8; ++frame) render (player);
        allocations = counter.count();
    }
    CHECK (selected);
    CHECK (allocations == 0);
}

TEST_CASE ("The combined Studio retention budget preserves the actual last reverse read", "[player][transform][playback][retention]")
{
    constexpr std::int64_t rate = 8000, extent = 8 * rate, latency = rate / 2, search = 160, preroll = 16;
    constexpr auto nominal = search + preroll;
    constexpr auto fullCapacity = 64 * rate + latency + search + preroll + 2;
    const auto retained = GENERATE_COPY (fullCapacity, fullCapacity - preroll);
    LivePlayer player;
    player.prepare (static_cast<int> (extent), static_cast<std::size_t> (retained));
    const std::array cuts { Cut { 1.0 }, Cut { 1.0 }, Cut { 1.0 }, Cut { 1.0 }, Cut { 1.0 }, Cut { 1.0 }, Cut { 1.0 }, Cut { 1.0 } };
    player.startBlock (cuts, extent, player.sourceAtFrame (nominal));
    bool inputValid = ingest (player, 0, nominal + latency);
    bool selected = false;
    StereoFrame output {};
    for (std::int64_t frame = 0; frame < 64 * rate; ++frame)
    {
        const auto raw = inputFrame (nominal + frame + latency);
        inputValid &= player.ingestFrame (raw.left, raw.right);
        if (frame == 56 * rate)
            selected = player.setCurrentTransform ({ player.retainedSource().epoch, 0, extent }, true);
        output = render (player);
    }
    REQUIRE (selected);
    if (retained == fullCapacity)
    {
        CHECK (inputValid);
        CHECK_FALSE (player.hasSourceError());
        CHECK_THAT (output.left, WithinAbs (inputFrame (0).left * fadeEnvelope (static_cast<int> (extent - 1), 1, static_cast<int> (extent)), 1.0e-8));
    }
    else
    {
        CHECK_FALSE (inputValid);
        CHECK (player.hasSourceError()); // removing preroll evicts selected source
        CHECK_THAT (output.left, WithinAbs (0.0, 0.0));
    }
}
