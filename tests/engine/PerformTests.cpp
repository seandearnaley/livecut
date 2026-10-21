// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/Cutter.h"
#include "engine/WetHold.h"
#include "engine/Accent.h"
#include "engine/SwingGrid.h"
#include "engine/AllocationCounter.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>
#include <map>
#include <vector>

using namespace livecut::engine;
using Catch::Matchers::WithinAbs;

TEST_CASE ("Source-addressed player retains a whole 64-second block and its interpolation guard", "[perform][source]")
{
    LivePlayer player;
    constexpr int extent = 8000;
    player.prepare (extent);
    const std::vector<Cut> cuts (8, Cut { 1.0, 1.0, 0.0f, 1.0f, 0.0f });
    const auto source = player.sourceForNextFrame();
    player.startBlock (cuts, extent, source);
    bool failed = false;
    for (int sample = 0; sample < extent * 8; ++sample)
    {
        auto left = static_cast<float> (sample + 1) / 1000.0f;
        auto right = -left;
        player.ingestFrame (left, right);
        player.renderFrame (left, right);
        if (sample > extent * 7 && sample % 113 == 0)
        {
            const auto position = sample % extent;
            const auto expected = static_cast<float> (position + 1) / 1000.0f * fadeEnvelope (position, 1, extent);
            if (std::abs (left - expected) > 1.0e-6f || std::abs (right + expected) > 1.0e-6f)
                failed = true;
        }
    }
    CHECK_FALSE (failed);
    CHECK_FALSE (player.hasSourceError());
}

TEST_CASE ("Swing preserves unswung source origins through a tempo change", "[perform][swing]")
{
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher, Procedure::bounce);
    auto origins = [&] (double swing)
    {
        Cutter cutter;
        cutter.prepare (1000.0);
        cutter.setProcedure (procedure);
        cutter.setUnitsPerBar (32);
        cutter.setSwing (swing);
        CutParameters parameters;
        parameters.minPhraseBars = parameters.maxPhraseBars = 2;
        parameters.minCents = -400.0f;
        parameters.maxCents = 800.0f;
        cutter.setParameters (parameters);
        std::map<std::pair<std::int64_t, double>, std::int64_t> result;
        double beats = 0.0;
        for (int sample = 0; sample < 18000; ++sample)
        {
            const auto bpm = sample < 2300 ? 120.0 : 83.0;
            const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
            float left = std::sin (static_cast<float> (sample) * 0.1f), right = left;
            cutter.process (&left, &right, 1, { bpm, 4.0 }, { bar, beats - static_cast<double> (bar) * 4.0 });
            if (cutter.isCutting())
                result[{ cutter.phraseStartBar(), cutter.blockStartUnits() }] = cutter.activeSourceRegion().firstFrame;
            if (cutter.error() != Cutter::Error::none)
                FAIL ("source/queue error during supported swing");
            beats += bpm / 60000.0;
        }
        CHECK (cutter.maximumQueuedPlans() <= 16);
        return result;
    };
    const auto straight = origins (50.0);
    const auto swung = origins (75.0);
    REQUIRE (swung.size() > 10);
    for (const auto& [identity, origin] : swung)
    {
        const auto found = straight.find (identity);
        REQUIRE (found != straight.end());
        CHECK (found->second == origin);
    }
}

TEST_CASE ("The fixed plan queue handles dense subdivisions and fractional meters", "[perform][swing][allocations]")
{
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher, Procedure::bounce);
    const auto meter = GENERATE (0.5, 1.0, 1.5, 3.0, 3.5);
    Cutter cutter;
    cutter.prepare (1000.0);
    cutter.setProcedure (procedure);
    cutter.setUnitsPerBar (32);
    cutter.setSwing (75.0);
    cutter.setAccent (1.0f);
    CutParameters parameters;
    parameters.minPhraseBars = parameters.maxPhraseBars = 8;
    parameters.stutterChance = 1.0;
    parameters.activity = 1.0;
    cutter.setParameters (parameters);
    double beats = 0.0;
    std::size_t allocationCount = 0;
    for (int sample = 0; sample < 12000; ++sample)
    {
        float left = 0.5f, right = -0.25f;
        const auto bar = static_cast<std::int64_t> (std::floor (beats / meter));
        const testing::AllocationCounter allocations;
        cutter.process (&left, &right, 1, { 120.0, meter }, { bar, beats - static_cast<double> (bar) * meter });
        allocationCount += allocations.count();
        beats += 0.002;
    }
    CHECK (allocationCount == 0);
    CHECK (cutter.error() == Cutter::Error::none);
    CHECK (cutter.maximumQueuedPlans() <= 16);
}

TEST_CASE ("Accent is deterministic bounded attenuation and does not change patterns", "[perform][accent]")
{
    for (std::uint64_t ordinal = 0; ordinal < 64; ++ordinal)
    {
        CHECK_THAT (accentGain (0.0f, 7, -3, ordinal), WithinAbs (1.0, 0.0));
        const auto gain = accentGain (1.0f, 7, -3, ordinal);
        CHECK (gain >= 0.75f);
        CHECK (gain <= 1.0f);
        CHECK_THAT (gain, WithinAbs (accentGain (1.0f, 7, -3, ordinal), 0.0));
    }
}

TEST_CASE ("Wet Hold quantizes transitions freezes audio and follows tempo by varispeed", "[perform][hold]")
{
    WetHold hold;
    hold.prepare (1000.0, 1000);
    hold.request (true);
    hold.barStarted();
    CHECK (hold.state() == WetHold::State::waiting);
    hold.startPhrase (false, 4.0);
    float left = 0.5f, right = -0.5f;
    hold.process (left, right, 0.004);
    hold.completePhrase();
    CHECK_FALSE (hold.hasPhrase());
    hold.startPhrase (true, 4.0);
    for (int frame = 0; frame < 1000; ++frame)
    {
        left = 0.5f;
        right = -0.5f;
        hold.process (left, right, 0.004);
    }
    hold.completePhrase();
    CHECK (hold.state() == WetHold::State::pendingEntry);
    CHECK_FALSE (hold.isHeld());
    hold.barStarted();
    CHECK (hold.isHeld());
    for (int frame = 0; frame < 100; ++frame)
    {
        left = 0.0f;
        right = 0.0f;
        hold.process (left, right, 0.008);
    }
    CHECK_THAT (hold.phaseBeats(), WithinAbs (0.8, 1.0e-10));
    CHECK_THAT (left, WithinAbs (0.5, 1.0e-6));
    CHECK_THAT (right, WithinAbs (-0.5, 1.0e-6));
    hold.startPhrase (true, 4.0);
    for (int frame = 0; frame < 2000; ++frame)
    {
        left = right = 0.0f;
        hold.process (left, right, 0.004);
    }
    hold.completePhrase();
    CHECK (hold.phraseFrames() == 1000);
    CHECK_THAT (left, WithinAbs (0.5, 1.0e-6));
    hold.request (false);
    CHECK (hold.state() == WetHold::State::pendingRelease);
    CHECK (hold.isHeld());
    hold.barStarted();
    CHECK_FALSE (hold.isHeld());
    for (int frame = 0; frame < 10; ++frame)
    {
        left = 0.25f;
        right = -0.25f;
        hold.process (left, right, 0.004);
    }
    CHECK_THAT (left, WithinAbs (0.25, 1.0e-6));
    hold.request (true);
    hold.interrupt();
    CHECK_FALSE (hold.hasPhrase());
    CHECK_FALSE (hold.isHeld());
    CHECK (hold.state() == WetHold::State::waiting);
}

TEST_CASE ("Renderer preserves a double source fraction that rounds to float one", "[perform][source]")
{
    LivePlayer player;
    player.prepare (16);
    const auto source = player.sourceForNextFrame();
    const std::array cuts { Cut { 1.0 } };
    player.startBlock (cuts, 16.0, source);
    player.ingestFrame (0.25f, -0.25f);
    const auto fraction = std::nextafter (1.0, 0.0);
    CHECK_FALSE (player.readSourceFrame (fraction)); // a fractional read requires its neighbour
    player.ingestFrame (0.75f, -0.75f);
    const auto frame = player.readSourceFrame (fraction);
    REQUIRE (frame);
    CHECK_THAT (frame->left, WithinAbs (0.75, 0.0));
    CHECK_THAT (frame->right, WithinAbs (-0.75, 0.0));
}

TEST_CASE ("Swing activates block and internal cut endpoints at mapped musical positions", "[perform][swing]")
{
    Cutter cutter;
    cutter.prepare (1000.0);
    cutter.setProcedure (Procedure::warpCut);
    cutter.setSwing (75.0);
    CutParameters parameters;
    parameters.minPhraseBars = parameters.maxPhraseBars = 1;
    parameters.straightChance = 0.0;
    parameters.regularChance = 1.0;
    cutter.setParameters (parameters);
    double beats = 0.0;
    std::pair<std::int64_t, std::size_t> previous { -1, 999 };
    int checked = 0;
    bool valid = true;
    for (int sample = 0; sample < 5000; ++sample)
    {
        const auto bpm = sample < 333 ? 120.0 : 91.0;
        const auto step = bpm / 60000.0;
        const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
        float left = 0.5f, right = -0.5f;
        cutter.process (&left, &right, 1, { bpm, 4.0 }, { bar, beats - static_cast<double> (bar) * 4.0 });
        const auto index = cutter.player().currentCut();
        const auto identity = std::pair { cutter.activeSourceRegion().firstFrame, index };
        if (cutter.isCutting() && identity != previous && index < cutter.currentCuts().size())
        {
            auto logicalUnits = cutter.blockStartUnits();
            for (std::size_t cut = 0; cut < index; ++cut)
                logicalUnits += cutter.currentCuts()[cut].units;
            const auto bars = std::floor (logicalUnits / 8.0);
            const auto logicalBeat = (logicalUnits - bars * 8.0) * 0.5;
            const auto physical = *SwingGrid::toPhysical (logicalBeat, 4.0, 75.0);
            const auto expected = (static_cast<double> (cutter.phraseStartBar()) + bars) * 4.0 + physical;
            if (std::abs (beats - expected) > 0.6 * step)
                valid = false;
            ++checked;
            previous = identity;
        }
        beats += step;
    }
    CHECK (valid);
    CHECK (checked > 30);
}

TEST_CASE ("A meter change clears queued swung onsets and resumes in the new meter", "[perform][swing]")
{
    Cutter cutter;
    cutter.prepare (1000.0);
    cutter.setProcedure (Procedure::warpCut);
    cutter.setUnitsPerBar (32);
    cutter.setSwing (75.0);
    std::vector<float> left (900, 0.5f), right (900, -0.5f);
    cutter.process (left.data(), right.data(), 900, { 120.0, 4.0 }, { 0, 0.0 });
    cutter.process (left.data(), right.data(), 1, { 120.0, 1.5 }, { 1, 0.3 });
    CHECK_FALSE (cutter.isCutting());
    cutter.process (left.data(), right.data(), 100, { 120.0, 1.5 }, { 1, 0.302 });
    REQUIRE (cutter.isCutting());
    CHECK (cutter.phraseStartBar() == 1);
    CHECK (cutter.activeSourceRegion().firstFrame >= 900);
    CHECK (cutter.error() == Cutter::Error::none);
}
