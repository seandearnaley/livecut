// SPDX-License-Identifier: GPL-3.0-or-later

#include "AllocationCounter.h"
#include "engine/Cutter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace livecut::engine;
using livecut::engine::testing::AllocationCounter;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double sampleRate = 48000.0;

// A host transport playing at a steady tempo.
struct Host
{
    double bpm = 120.0;
    double beatsPerBar = 4.0;
    double beats = 0.0; // quarter notes since bar 0

    Timing timing() const { return { bpm, beatsPerBar }; }

    Position position() const
    {
        const auto bar = std::floor (beats / beatsPerBar);
        return { static_cast<std::int64_t> (bar), beats - bar * beatsPerBar };
    }

    void advance (int samples) { beats += samples * bpm / 60.0 / sampleRate; }
};

struct Buffer
{
    explicit Buffer (int samples) : left (static_cast<std::size_t> (samples)), right (static_cast<std::size_t> (samples))
    {
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            left[i] = std::sin (0.01f * static_cast<float> (i));
            right[i] = std::cos (0.013f * static_cast<float> (i));
        }
    }

    bool operator== (const Buffer& other) const
    {
        return std::memcmp (left.data(), other.left.data(), left.size() * sizeof (float)) == 0
            && std::memcmp (right.data(), other.right.data(), right.size() * sizeof (float)) == 0;
    }

    std::vector<float> left, right;
};

void run (Cutter& cutter, Host& host, Buffer& buffer)
{
    cutter.process (buffer.left.data(), buffer.right.data(), static_cast<int> (buffer.left.size()), host.timing(), host.position());
    host.advance (static_cast<int> (buffer.left.size()));
}

void runSamples (Cutter& cutter, Host& host, int samples, int bufferSize = 32)
{
    for (int done = 0; done < samples; done += bufferSize)
    {
        Buffer buffer (std::min (bufferSize, samples - done));
        run (cutter, host, buffer);
    }
}

int samplesPerBar (const Host& host)
{
    return static_cast<int> (std::lround (host.beatsPerBar * 60.0 / host.bpm * sampleRate));
}
} // namespace

// Livecut only noticed a unit boundary that fell after the first sample of a buffer,
// so pressing play on a bar line skipped the downbeat and left a bar of silence.
TEST_CASE ("Cutting starts on the first sample when playback starts on a bar line", "[cutter]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    Host host;

    runSamples (cutter, host, 1);

    CHECK (cutter.isCutting());
    CHECK (cutter.phraseStartBar() == 0);
    CHECK_THAT (cutter.blockStartUnits(), WithinAbs (0.0, 1.0e-9));
}

TEST_CASE ("Starting between units plays the input live until the next unit", "[cutter]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    Host host;
    host.beats = 0.1; // 0.4 beats before the second unit at 8 units per bar

    Buffer buffer (9600);
    const auto untouched = buffer;
    run (cutter, host, buffer);

    CHECK (buffer == untouched);
    CHECK_FALSE (cutter.isCutting());

    runSamples (cutter, host, 1);
    CHECK (cutter.isCutting());
    CHECK_THAT (cutter.blockStartUnits(), WithinAbs (1.0, 1.0e-9));
}

TEST_CASE ("Blocks run back to back and every phrase ends on a bar line", "[cutter]")
{
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher);
    const auto unitsPerBar = GENERATE (6, 8, 16, 32);
    Cutter cutter;
    cutter.prepare (sampleRate);
    cutter.setProcedure (procedure);
    cutter.setUnitsPerBar (unitsPerBar);
    Host host;

    std::int64_t lastPhrase = -1;
    int blocks = 0;

    for (int buffer = 0; buffer < 16 * 96000 / 32; ++buffer)
    {
        const auto previousStart = cutter.blockStartUnits();
        const auto previousCuts = std::vector<Cut> (cutter.currentCuts().begin(), cutter.currentCuts().end());
        runSamples (cutter, host, 32);
        REQUIRE (cutter.isCutting());

        if (cutter.phraseStartBar() != lastPhrase)
        {
            // The previous phrase's last block must end exactly where the new phrase begins.
            if (lastPhrase >= 0)
            {
                const auto previousPhraseUnits = static_cast<double> ((cutter.phraseStartBar() - lastPhrase) * unitsPerBar);
                REQUIRE_THAT (previousStart + totalUnits (previousCuts), WithinAbs (previousPhraseUnits, 1.0e-6));
            }

            REQUIRE_THAT (cutter.blockStartUnits(), WithinAbs (0.0, 1.0e-9));
            lastPhrase = cutter.phraseStartBar();
            ++blocks;
        }
        else if (std::abs (cutter.blockStartUnits() - previousStart) > 1.0e-9)
        {
            REQUIRE_THAT (cutter.blockStartUnits(), WithinAbs (previousStart + totalUnits (previousCuts), 1.0e-6));
            ++blocks;
        }
    }

    CHECK (blocks > 16);
}

// Livecut kept a 4-beat bar whatever the time signature, so in 3/4 every cut came out a third too long.
TEST_CASE ("The time signature sets the length of a bar", "[cutter]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    cutter.setUnitsPerBar (6);
    Host host;
    host.beatsPerBar = 3.0; // 3/4: two units per beat, 12000 samples per unit at 120 BPM

    runSamples (cutter, host, 1);
    REQUIRE (cutter.isCutting());

    int samples = 0;

    for (const auto& cut : cutter.player().cuts())
        samples += cut.size;

    CHECK (samples == std::lround (totalUnits (cutter.currentCuts()) * 12000.0));
}

TEST_CASE ("A jump in the host position restarts phrasing in the new bar", "[cutter]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    Host host;
    runSamples (cutter, host, 2 * samplesPerBar (host));
    REQUIRE (cutter.isCutting());

    host.beats = 10 * 4.0 + 0.3; // bar 10, 0.2 beats before its second unit
    runSamples (cutter, host, 32);
    CHECK_FALSE (cutter.isCutting());

    runSamples (cutter, host, 9600);
    CHECK (cutter.isCutting());
    CHECK (cutter.phraseStartBar() == 10);
}

TEST_CASE ("The same seed and position always cut the same way", "[cutter]")
{
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher);
    CutParameters params;
    params.minCents = -300.0f;
    params.maxCents = 300.0f;
    params.minAmp = 0.5f;

    auto render = [&] (std::uint32_t seed)
    {
        Cutter cutter;
        cutter.prepare (sampleRate);
        cutter.setProcedure (procedure);
        cutter.setParameters (params);
        cutter.setSeed (seed);
        Host host;
        std::vector<float> output;

        for (int i = 0; i < 8 * 96000 / 512; ++i)
        {
            Buffer buffer (512);
            run (cutter, host, buffer);
            output.insert (output.end(), buffer.left.begin(), buffer.left.end());
        }

        return output;
    };

    const auto first = render (3);
    const auto second = render (3);
    const auto other = render (4);

    CHECK (std::memcmp (first.data(), second.data(), first.size() * sizeof (float)) == 0);
    CHECK (std::memcmp (first.data(), other.data(), first.size() * sizeof (float)) != 0);
}

TEST_CASE ("Procedure and subdivision changes land on the next bar line", "[cutter]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    Host host;
    const auto bar = samplesPerBar (host);

    runSamples (cutter, host, bar / 2);
    cutter.setProcedure (Procedure::sqPusher);
    cutter.setUnitsPerBar (16);
    runSamples (cutter, host, bar / 2 - 32);

    CHECK (cutter.activeProcedure() == Procedure::cutProc11);
    CHECK (cutter.activeUnitsPerBar() == 8);

    runSamples (cutter, host, 64);

    CHECK (cutter.activeProcedure() == Procedure::sqPusher);
    CHECK (cutter.activeUnitsPerBar() == 16);
    CHECK_THAT (std::fmod (cutter.blockStartUnits(), 16.0), WithinAbs (0.0, 1.0e-9));
}

TEST_CASE ("Cutting never allocates memory", "[cutter]")
{
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher);
    Cutter cutter;
    cutter.prepare (sampleRate);
    cutter.setProcedure (procedure);
    cutter.setUnitsPerBar (32);
    CutParameters params;
    params.stutterChance = 1.0;
    params.stutterArea = 1.0;
    params.maxPhraseBars = 8;
    params.minCents = -1200.0f;
    params.maxCents = 2400.0f;
    cutter.setParameters (params);
    Host host;
    Buffer buffer (256);

    AllocationCounter allocations;
    std::size_t largestBlock = 0;

    for (int i = 0; i < 32 * 96000 / 256; ++i)
    {
        cutter.process (buffer.left.data(), buffer.right.data(), 256, host.timing(), host.position());
        host.advance (256);
        largestBlock = std::max (largestBlock, cutter.currentCuts().size());
    }

    CHECK (allocations.count() == 0);
    CHECK (cutter.isCutting());

    // Make sure stutters and rolls, not just short blocks, were exercised. At 32
    // units per bar every WarpCut block is under a beat, so its rolls top out at 16.
    const auto expectedLargest = procedure == Procedure::cutProc11 ? 5u
                               : procedure == Procedure::warpCut  ? 16u
                                                                  : 4u;
    CHECK (largestBlock >= expectedLargest);
}

TEST_CASE ("Bars too long for the recording buffer pass through untouched", "[cutter]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    Host host;
    host.bpm = 20.0; // a 12 second bar of 4/4

    Buffer buffer (512);
    const auto untouched = buffer;
    run (cutter, host, buffer);

    CHECK_FALSE (cutter.isTimingSupported());
    CHECK_FALSE (cutter.isCutting());
    CHECK (buffer == untouched);
}
