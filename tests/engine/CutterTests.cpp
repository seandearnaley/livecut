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
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher, Procedure::bounce);
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
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher, Procedure::bounce);
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
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher, Procedure::bounce);
    Cutter cutter;
    cutter.prepare (sampleRate);
    cutter.setProcedure (procedure);
    cutter.setUnitsPerBar (32);
    CutParameters params;
    params.ratchetChance = 1.0;
    params.stutterChance = 1.0;
    params.stutterArea = 1.0;
    params.maxPhraseBars = 8;
    params.minCents = -1200.0f;
    params.maxCents = 2400.0f;
    cutter.setParameters (params);
    CrusherParameters crusher;
    crusher.enabled = true;
    crusher.minBits = 2.0f;
    crusher.minHz = 2000.0;
    cutter.setCrusher (crusher);
    CombParameters comb;
    comb.enabled = true;
    comb.type = CombParameters::Type::feedBack;
    comb.minDelayMs = 1.0f;
    comb.maxDelayMs = 50.0f;
    cutter.setComb (comb);
    Host host;
    Buffer buffer (256);

    AllocationCounter allocations;
    std::size_t largestBlock = 0;
    constexpr int buffers = 32 * 96000 / 256;

    for (int i = 0; i < buffers; ++i)
    {
        // Exercise the transitions too: a subdivision change, a jump and a tempo change.
        if (i == buffers / 4)
            cutter.setUnitsPerBar (16);

        if (i == buffers / 2)
            host.beats += 7.3;

        if (i == 3 * buffers / 4)
            host.bpm = 90.0;

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
                               : procedure == Procedure::bounce   ? 12u
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

// Found in review: a subdivision changed while stopped was applied after the
// cutter had worked out where to start in the old subdivision, so the first
// block started at the wrong unit, or ran past the end of a one-bar phrase.
TEST_CASE ("Changes made while stopped are in place before cutting resumes mid-bar", "[cutter]")
{
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher, Procedure::bounce);
    Cutter cutter;
    cutter.prepare (sampleRate);
    cutter.setProcedure (procedure);
    CutParameters params;
    params.minPhraseBars = 1;
    params.maxPhraseBars = 1;
    cutter.setParameters (params);
    cutter.setUnitsPerBar (16);
    Host host;
    runSamples (cutter, host, samplesPerBar (host));
    REQUIRE (cutter.activeUnitsPerBar() == 16);

    cutter.reset();
    cutter.setUnitsPerBar (8);
    host.beats = 5 * 4.0 + 3.0; // bar 5, beat 4: unit 6 at 8 units per bar

    REQUIRE_NOTHROW (runSamples (cutter, host, 1));
    CHECK (cutter.isCutting());
    CHECK (cutter.activeUnitsPerBar() == 8);
    CHECK (cutter.phraseStartBar() == 5);
    CHECK_THAT (cutter.blockStartUnits(), WithinAbs (6.0, 1.0e-9));
    REQUIRE_NOTHROW (runSamples (cutter, host, 2 * samplesPerBar (host)));
}

// Found in review: a host whose bar start lags behind its position (or a
// count-in before bar 0) reports beats outside the bar, which were only wrapped once.
TEST_CASE ("Host positions outside the bar are counted into the right bar", "[cutter]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    const Host host;
    float left[1] {};
    float right[1] {};

    cutter.process (left, right, 1, host.timing(), Position { 3, 9.0 }); // really bar 5, beat 2
    CHECK (cutter.isCutting());
    CHECK (cutter.phraseStartBar() == 5);
    CHECK_THAT (cutter.blockStartUnits(), WithinAbs (2.0, 1.0e-9));

    cutter.reset();
    cutter.process (left, right, 1, host.timing(), Position { 2, -1.0 }); // really bar 1, beat 4
    CHECK (cutter.isCutting());
    CHECK (cutter.phraseStartBar() == 1);
    CHECK_THAT (cutter.blockStartUnits(), WithinAbs (6.0, 1.0e-9));
}

// Found in review: blocks were sized in samples at the tempo they started at,
// so slowing down part way through one left the rest of it silent.
TEST_CASE ("A tempo drop part way through a block doesn't leave it silent", "[cutter]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    cutter.setFadeMilliseconds (0.0);
    Host host;
    std::vector<float> left (512, 0.5f);
    std::vector<float> right (512, 0.5f);

    cutter.process (left.data(), right.data(), 512, host.timing(), host.position());
    host.advance (512);
    host.bpm = 60.0;

    int longestSilence = 0;
    int silence = 0;

    for (int i = 0; i < 2 * 4 * 48000 / 512; ++i)
    {
        std::fill (left.begin(), left.end(), 0.5f);
        std::fill (right.begin(), right.end(), 0.5f);
        cutter.process (left.data(), right.data(), 512, host.timing(), host.position());
        host.advance (512);
        REQUIRE (cutter.isCutting());

        for (const auto sample : left)
        {
            silence = std::abs (sample) > 0.0f ? 0 : silence + 1;
            longestSilence = std::max (longestSilence, silence);
        }
    }

    // With the gate fully open, the only silent samples are the single first sample of each cut's fade-in.
    CHECK (longestSilence <= 1);
}

// Livecut started a block's first cut before telling the effects about the
// block, so that cut used the previous block's random settings.
TEST_CASE ("Effects receive a block's settings before its first cut", "[cutter][effects]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    CrusherParameters crusher;
    crusher.enabled = true;
    crusher.minBits = 4.0f;
    crusher.maxBits = 4.0f;
    cutter.setCrusher (crusher);
    Host host;

    runSamples (cutter, host, 1);

    REQUIRE (cutter.isCutting());
    CHECK_THAT (cutter.crusher().bits(), WithinAbs (4.0, 1.0e-12));
}

TEST_CASE ("Effect settings step evenly from cut to cut through a block", "[cutter][effects]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    cutter.setProcedure (Procedure::warpCut);
    CutParameters params;
    params.straightChance = 0.0; // every block is a roll of at least 4 cuts
    cutter.setParameters (params);
    CombParameters comb;
    comb.minDelayMs = 5.0f;
    comb.maxDelayMs = 45.0f;
    cutter.setComb (comb);
    Host host;

    runSamples (cutter, host, 1);
    REQUIRE (cutter.player().cuts().size() >= 4);

    std::vector<double> delays;
    std::size_t lastCut = cutter.player().currentCut();
    delays.push_back (cutter.comb().delaySamples());

    while (delays.size() < 4)
    {
        runSamples (cutter, host, 1, 1);

        if (cutter.player().currentCut() != lastCut)
        {
            lastCut = cutter.player().currentCut();
            delays.push_back (cutter.comb().delaySamples());
        }
    }

    REQUIRE (std::abs (delays[1] - delays[0]) > 1.0e-6);
    CHECK_THAT (delays[2] - delays[1], WithinAbs (delays[1] - delays[0], 1.0e-6));
    CHECK_THAT (delays[3] - delays[2], WithinAbs (delays[1] - delays[0], 1.0e-6));
}

TEST_CASE ("Switching an effect on or off leaves the cuts unchanged", "[cutter][effects]")
{
    auto cutSequence = [] (bool effectsOn)
    {
        Cutter cutter;
        cutter.prepare (sampleRate);
        CrusherParameters crusher;
        crusher.enabled = effectsOn;
        crusher.minBits = 3.0f;
        cutter.setCrusher (crusher);
        CombParameters comb;
        comb.enabled = effectsOn;
        cutter.setComb (comb);
        Host host;
        std::vector<double> starts;

        for (int i = 0; i < 8 * 96000 / 64; ++i)
        {
            runSamples (cutter, host, 64);
            starts.push_back (cutter.blockStartUnits() + 1000.0 * static_cast<double> (cutter.phraseStartBar()));
        }

        return starts;
    };

    CHECK (cutSequence (false) == cutSequence (true));
}

TEST_CASE ("An enabled crusher quantises the cut audio", "[cutter][effects]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    CrusherParameters crusher;
    crusher.enabled = true;
    crusher.minBits = 2.0f;
    crusher.maxBits = 2.0f;
    cutter.setCrusher (crusher);
    Host host;

    Buffer buffer (4800);
    run (cutter, host, buffer);

    REQUIRE (cutter.isCutting());

    for (const auto sample : buffer.left)
        REQUIRE_THAT (std::fmod (std::abs (sample) + 1.0e-6, 0.5), WithinAbs (0.0, 1.0e-4));
}

TEST_CASE ("The cutter reports where it is in the phrase", "[cutter]")
{
    Cutter cutter;
    cutter.prepare (sampleRate);
    Host host;

    runSamples (cutter, host, 18000); // 1.5 units at 8 units per bar and 120 BPM

    REQUIRE (cutter.isCutting());
    CHECK (cutter.phraseBarCount() >= 1);
    CHECK_THAT (cutter.positionInPhrase(), WithinAbs (1.5, 1.0e-3));
    CHECK (cutter.blockEndUnits() > cutter.blockStartUnits());
}

TEST_CASE ("A halftime bar near the longest supported bar stays in the buffer and sounds", "[cutter]")
{
    CHECK (static_cast<int> (Procedure::bounce) == 3);
    Cutter cutter;
    cutter.prepare (sampleRate);
    cutter.setProcedure (Procedure::bounce);
    cutter.setFadeMilliseconds (0.0);
    CutParameters params;
    params.halftimeChance = 1.0;
    params.ratchetChance = 0.0;
    params.minPhraseBars = params.maxPhraseBars = 2;
    cutter.setParameters (params);
    Host host;
    host.bpm = 31.0;
    const auto barSamples = samplesPerBar (host);
    REQUIRE (barSamples / sampleRate < Cutter::maxBarSeconds);

    for (int done = 0; done < barSamples; done += 256)
    {
        Buffer buffer (std::min (256, barSamples - done));
        std::fill (buffer.left.begin(), buffer.left.end(), 0.5f);
        std::fill (buffer.right.begin(), buffer.right.end(), 0.5f);
        run (cutter, host, buffer);

        REQUIRE (cutter.isTimingSupported());
        REQUIRE (cutter.isCutting());
        REQUIRE (cutter.activeProcedure() == Procedure::bounce);
        REQUIRE (cutter.currentCuts().size() == 1);
        REQUIRE_THAT (cutter.blockStartUnits(), WithinAbs (0.0, 1.0e-9));
        REQUIRE_THAT (cutter.currentCuts()[0].units, WithinAbs (8.0, 1.0e-9));
        REQUIRE_THAT (cutter.currentCuts()[0].cents, WithinAbs (-1200.0, 1.0e-6));

        for (std::size_t i = 0; i < buffer.left.size(); ++i)
        {
            if (done + static_cast<int> (i) < barSamples / 2)
                continue;

            REQUIRE (std::abs (buffer.left[i]) > 0.1f);
            REQUIRE (std::abs (buffer.right[i]) > 0.1f);
        }
    }
}

TEST_CASE ("Phrase events report every in-buffer boundary before its output sample", "[cutter][capture]")
{
    struct Events final : Cutter::Observer
    {
        void discontinuity (int offset) override { resets.push_back (offset); }
        void phraseCompleted (int offset) override { ends.push_back (offset); }
        void phraseStarted (int offset, bool full) override { starts.push_back ({ offset, full }); }
        void processOutput (int offset, float, float, float&, float&) override { samples.push_back (offset); }
        std::vector<int> resets, samples, ends;
        std::vector<std::pair<int, bool>> starts;
    } events;
    Cutter cutter;
    cutter.prepare (8000.0); // 16000 samples per 4/4 bar at 120 BPM
    CutParameters params;
    params.minPhraseBars = params.maxPhraseBars = 1;
    cutter.setParameters (params);
    Buffer buffer (49001);
    cutter.process (buffer.left.data(), buffer.right.data(), 49001, { 120.0, 4.0 }, { 0, 0.0 }, &events);
    REQUIRE (events.starts.size() == 4);
    for (int phrase = 0; phrase < 4; ++phrase)
    {
        CHECK (events.starts[static_cast<std::size_t> (phrase)].first == phrase * 16000);
        CHECK (events.starts[static_cast<std::size_t> (phrase)].second);
    }
    CHECK (events.samples.size() == 49001);
    REQUIRE (events.resets.size() == 1);
    CHECK (events.resets[0] == 0);

    events.starts.clear();
    events.resets.clear();
    cutter.process (buffer.left.data(), buffer.right.data(), 17000, { 120.0, 4.0 }, { 20, 1.0 }, &events);
    REQUIRE (events.resets.size() == 1);
    REQUIRE (events.starts.size() == 2);
    CHECK (events.starts[0].first == 0);
    CHECK_FALSE (events.starts[0].second); // unit 2 is a partial phrase
    CHECK (events.starts[1].first == 12000);
    CHECK (events.starts[1].second);
}

TEST_CASE ("An exclusive-end completion is not repeated at the next phrase start", "[cutter][capture]")
{
    struct Events final : Cutter::Observer
    {
        void discontinuity (int) override {}
        void phraseStarted (int, bool) override { ++starts; }
        void phraseCompleted (int offset) override { ++ends; lastEnd = offset; }
        void processOutput (int, float, float, float&, float&) override {}
        int starts = 0, ends = 0, lastEnd = -1;
    } events;
    Cutter cutter;
    cutter.prepare (8000.0);
    CutParameters parameters;
    parameters.minPhraseBars = parameters.maxPhraseBars = 1;
    cutter.setParameters (parameters);
    Buffer buffer (16000);
    cutter.process (buffer.left.data(), buffer.right.data(), 16000, { 120.0, 4.0 }, { 0, 0.0 }, &events);
    CHECK (events.starts == 1);
    CHECK (events.ends == 1);
    CHECK (events.lastEnd == 16000);
    cutter.process (buffer.left.data(), buffer.right.data(), 1, { 120.0, 4.0 }, { 1, 0.0 }, &events);
    CHECK (events.starts == 2);
    CHECK (events.ends == 1);
}
