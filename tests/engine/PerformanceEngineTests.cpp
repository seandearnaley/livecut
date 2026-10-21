// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/PerformanceEngine.h"
#include "engine/AllocationCounter.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

using namespace livecut::engine;
namespace
{
constexpr double rate = 8000.0;
constexpr int barFrames = 16000;
StereoFrame signal (int frame)
{
    const auto phase = frame % barFrames;
    const auto hit = phase % 2000;
    const auto envelope = std::exp (-static_cast<float> (hit) / 500.0f);
    return { envelope * std::sin (static_cast<float> (phase) * 0.37f),
             envelope * std::cos (static_cast<float> (phase) * 0.21f) * 0.6f };
}
std::vector<StereoFrame> run (int partition, PerformanceParameters parameters)
{
    PerformanceEngine engine;
    engine.prepare (rate);
    std::vector<StereoFrame> output;
    output.reserve (barFrames * 3);
    for (int first = 0; first < barFrames * 3; first += partition)
    {
        engine.beginContext ({120.0, 4.0}, {first / barFrames, (first % barFrames) / 4000.0}, 7, true, parameters);
        for (int i = first; i < std::min (first + partition, barFrames * 3); ++i)
            output.push_back (engine.processOne (signal (i)).frame);
    }
    return output;
}
}

TEST_CASE ("Performance captures one complete bar then replays its immutable raw source", "[performance][capture]")
{
    PerformanceEngine engine;
    engine.prepare (rate);
    engine.beginContext ({120.0, 4.0}, {0, 0.0}, 10, true, {});
    for (int i = 0; i < barFrames; ++i)
    {
        const auto input = signal (i);
        const auto result = engine.processOne (input);
        CHECK (result.frame.left == input.left);
        CHECK (result.frame.right == input.right);
        CHECK (result.barCompletedAfter == (i == barFrames - 1));
    }
    CHECK (engine.diagnostics().hasSource);
    engine.beginContext ({120.0, 4.0}, {1, 0.0}, 10, true, {});
    for (int i = 0; i < barFrames; ++i)
    {
        const auto result = engine.processOne ({0.9f, -0.9f});
        const auto expected = signal (i);
        REQUIRE (result.frame.left == expected.left);
        REQUIRE (result.frame.right == expected.right);
        if (i == 0)
        {
            CHECK_FALSE (result.barCompleted);
            CHECK (result.barStarted);
            CHECK (result.fullStart);
        }
    }
    CHECK (engine.diagnostics().state == PerformanceEngine::State::playing);
    CHECK (engine.diagnostics().sourceFrames == barFrames);
    CHECK (engine.diagnostics().sourceGeneration == 1);
    CHECK_FALSE (engine.diagnostics().captureArmed);
}

TEST_CASE ("Performance plans separate output event time from captured source identity", "[performance][plan]")
{
    PerformanceParameters parameters;
    parameters.variation = 1.0f;
    parameters.density = 1.0f;
    PerformanceEngine engine;
    engine.prepare (rate);
    engine.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, parameters);
    for (int i = 0; i <= barFrames; ++i)
        engine.processOne (signal (i));
    REQUIRE (engine.events().size() == 32);
    bool reordered = false;
    for (const auto& event : engine.events())
    {
        CHECK (event.sourceId == engine.diagnostics().sourceGeneration);
        CHECK (event.sourceOffset >= 0);
        CHECK (event.sourceEnd > event.sourceOffset);
        CHECK (event.sourceEnd <= barFrames);
        if (event.startStep % 8 == 0)
        {
            CHECK (event.role == PerformanceEngine::Role::anchor);
            CHECK (event.sourceOffset == event.startStep * barFrames / 32);
        }
        reordered |= event.sourceOffset != event.startStep * barFrames / 32;
    }
    CHECK (reordered);
}

TEST_CASE ("Performance output is independent of host buffer partition", "[performance][timing]")
{
    PerformanceParameters parameters;
    parameters.variation = 0.83f;
    parameters.density = 0.95f;
    parameters.grain = 0.8f;
    const auto sample = run (1, parameters);
    const auto block = run (257, parameters);
    REQUIRE (sample.size() == block.size());
    for (std::size_t i = 0; i < sample.size(); ++i)
    {
        REQUIRE (std::abs (sample[i].left - block[i].left) < 1.0e-6f);
        REQUIRE (std::abs (sample[i].right - block[i].right) < 1.0e-6f);
    }
}

TEST_CASE ("Performance texture preserves attack cores and moves during a bar", "[performance][texture]")
{
    PerformanceParameters dry;
    dry.density = 1.0f;
    auto wet = dry;
    wet.grain = 1.0f;
    wet.character = 1.0f;
    const auto original = run (113, dry);
    const auto textured = run (113, wet);
    bool changedTail = false;
    for (int i = barFrames; i < 2 * barFrames; ++i)
    {
        const auto attackAge = (i - barFrames) % 500;
        if (attackAge < 80)
            REQUIRE (original[static_cast<std::size_t> (i)].left == textured[static_cast<std::size_t> (i)].left);
        else
            changedTail |= std::abs (original[static_cast<std::size_t> (i)].left - textured[static_cast<std::size_t> (i)].left) > 1.0e-4f;
    }
    CHECK (changedTail);
}

TEST_CASE ("Performance seeks discard captured audio and midbar starts wait for a full bar", "[performance][transport]")
{
    PerformanceEngine engine;
    engine.prepare (rate);
    engine.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, {});
    for (int i = 0; i <= barFrames; ++i)
        engine.processOne (signal (i));
    REQUIRE (engine.diagnostics().hasSource);
    engine.beginContext ({120.0, 4.0}, {7, 2.0}, 2, true, {});
    const auto result = engine.processOne ({0.25f, -0.5f});
    CHECK (result.discontinuity);
    CHECK_FALSE (result.fullStart);
    CHECK_FALSE (engine.diagnostics().hasSource);
    CHECK (engine.diagnostics().state == PerformanceEngine::State::waitingForBar);
    CHECK (result.frame.left == 0.25f);
}

TEST_CASE ("Performance captures and renders without callback allocation", "[performance][allocations]")
{
    PerformanceEngine engine;
    engine.prepare (rate);
    PerformanceParameters parameters;
    parameters.grain = parameters.variation = parameters.density = 1.0f;
    std::size_t count = 0;
    for (int first = 0; first < 5 * barFrames; first += 64)
    {
        const testing::AllocationCounter allocations;
        engine.beginContext ({120.0, 4.0}, {first / barFrames, (first % barFrames) / 4000.0}, 1, true, parameters, {first == 2 * barFrames});
        for (int i = first; i < std::min (first + 64, 5 * barFrames); ++i)
            engine.processOne (signal (i));
        count += allocations.count();
    }
    CHECK (count == 0);
    CHECK (engine.diagnostics().error == PerformanceEngine::Error::none);
    CHECK (engine.diagnostics().sourceGeneration == 2);
}

TEST_CASE ("Performance rejects unsupported timing explicitly", "[performance][timing]")
{
    PerformanceEngine engine;
    engine.prepare (rate);
    engine.beginContext ({20.0, 4.0}, {0, 0.0}, 1, true, {});
    const auto result = engine.processOne ({0.25f, 0.5f});
    CHECK (engine.diagnostics().state == PerformanceEngine::State::unsupportedTiming);
    CHECK (result.frame.left == 0.25f);
    CHECK_FALSE (engine.diagnostics().hasSource);
}

TEST_CASE ("Performance latches rhythm on eighth-bar steps seed on bars and texture continuously", "[performance][controls]")
{
    PerformanceEngine engine;
    engine.prepare (rate);
    PerformanceParameters parameters;
    engine.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, parameters);
    for (int i = 0; i < barFrames + 100; ++i)
        engine.processOne (signal (i));
    parameters.seed = 91;
    parameters.fill = true;
    parameters.grain = 1.0f;
    engine.beginContext ({120.0, 4.0}, {1, .025}, 1, true, parameters);
    for (int i = 100; i < 130; ++i)
        engine.processOne (signal (i));
    CHECK (engine.diagnostics().applied.grain > .7f);
    CHECK_FALSE (engine.diagnostics().applied.fill);
    CHECK (engine.diagnostics().applied.seed == 1);
    CHECK (engine.diagnostics().seedPending);
    for (int i = 130; i <= 2000; ++i)
        engine.processOne (signal (i));
    CHECK (engine.diagnostics().applied.fill);
    CHECK (engine.events().size() == 32);
    for (int i = 2001; i <= barFrames; ++i)
        engine.processOne (signal (i));
    CHECK (engine.diagnostics().applied.seed == 91);
    CHECK_FALSE (engine.diagnostics().seedPending);
}

TEST_CASE ("Performance recapture keeps the old source immutable until the new full bar completes", "[performance][capture]")
{
    PerformanceEngine engine;
    engine.prepare (rate);
    engine.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, {});
    for (int i = 0; i < barFrames + 100; ++i)
        engine.processOne ({.25f, -.25f});
    engine.beginContext ({120.0, 4.0}, {1, .025}, 1, true, {}, {true});
    for (int i = 100; i < 2 * barFrames; ++i)
    {
        const auto result = engine.processOne ({.75f, -.75f});
        REQUIRE (result.frame.left == .25f);
        REQUIRE (engine.diagnostics().sourceGeneration == (i == 2 * barFrames - 1 ? 2 : 1));
    }
    for (int i = 0; i < 32; ++i)
        engine.processOne ({.9f, -.9f});
    CHECK (engine.diagnostics().sourceGeneration == 2);
    CHECK (engine.processOne ({.9f, -.9f}).frame.left == .75f);
}

TEST_CASE ("Performance varispeed follows tempo and suppresses aliased source energy", "[performance][quality][tempo]")
{
    const auto rms = [] (double frequency)
    {
        PerformanceEngine engine;
        engine.prepare (rate);
        engine.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, {});
        for (int i = 0; i < barFrames; ++i)
        {
            const auto value = static_cast<float> (std::sin (2.0 * 3.14159265358979323846 * frequency * i / rate));
            engine.processOne ({value, value});
        }
        engine.beginContext ({240.0, 4.0}, {1, 0.0}, 1, true, {});
        auto energy = 0.0;
        for (int i = 0; i < barFrames / 2; ++i)
        {
            const auto frame = engine.processOne ({}).frame;
            if (i > 100 && i < barFrames / 2 - 100)
                energy += static_cast<double> (frame.left) * frame.left;
        }
        CHECK (engine.diagnostics().error == PerformanceEngine::Error::none);
        return std::sqrt (energy / (barFrames / 2 - 201));
    };
    CHECK (rms (500.0) > .65);
    CHECK (rms (3000.0) < .01);
}

TEST_CASE ("Performance rejects a newly completed source whose tempo ratio is unsupported", "[performance][timing]")
{
    PerformanceEngine engine;
    engine.prepare (rate);
    engine.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, {});
    for (int i = 0; i < barFrames; ++i)
        engine.processOne (signal (i));
    engine.beginContext ({1200.0, 4.0}, {1, 0.0}, 1, true, {});
    REQUIRE (engine.diagnostics().state == PerformanceEngine::State::unsupportedTiming);
    const auto result = engine.processOne ({.25f, -.25f});
    CHECK (engine.diagnostics().state == PerformanceEngine::State::unsupportedTiming);
    CHECK (result.frame.left == .25f);
}

TEST_CASE ("Performance neutral replay remains exact across later bar joins", "[performance][quality]")
{
    const auto output = run (137, {});
    for (int i = barFrames; i < 3 * barFrames; ++i)
    {
        const auto expected = signal (i);
        REQUIRE (output[static_cast<std::size_t> (i)].left == expected.left);
        REQUIRE (output[static_cast<std::size_t> (i)].right == expected.right);
    }
}

TEST_CASE ("Performance protects attack cores at supported sample rates and tempo ratios", "[performance][quality][rates]")
{
    const auto sampleRate = GENERATE (8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0);
    const auto tempoRatio = GENERATE (.5, 1.0, 2.0);
    PerformanceEngine dry, textured;
    dry.prepare (sampleRate);
    textured.prepare (sampleRate);
    PerformanceParameters parameters;
    dry.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, parameters);
    parameters.grain = 1.0f;
    parameters.character = 1.0f;
    textured.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, parameters);
    const auto frames = static_cast<int> (sampleRate * 2.0);
    for (int i = 0; i < frames; ++i)
    {
        const auto value = static_cast<float> (std::sin (i * .17) * std::exp (-i / (sampleRate * .3)));
        dry.processOne ({value, -value});
        textured.processOne ({value, -value});
    }
    const auto protectedFrames = static_cast<int> (std::ceil (sampleRate * .010 / std::min (1.0, tempoRatio)));
    const testing::AllocationCounter allocations;
    dry.beginContext ({120.0 * tempoRatio, 4.0}, {1, 0.0}, 1, true, {});
    textured.beginContext ({120.0 * tempoRatio, 4.0}, {1, 0.0}, 1, true, parameters);
    for (int i = 0; i < protectedFrames; ++i)
    {
        const auto original = dry.processOne ({}).frame;
        const auto wet = textured.processOne ({}).frame;
        REQUIRE (original.left == wet.left);
        REQUIRE (original.right == wet.right);
    }
    CHECK (allocations.count() == 0);
}

TEST_CASE ("Performance nonfinite contexts and parameters fail explicitly", "[performance][invalid]")
{
    const auto invalid = GENERATE (std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -1.0);
    PerformanceEngine engine;
    engine.prepare (rate);
    engine.beginContext ({invalid, 4.0}, {0, 0.0}, 1, true, {});
    CHECK (engine.diagnostics().state == PerformanceEngine::State::error);
    const auto result = engine.processOne ({.25f, -.25f});
    CHECK (result.frame.left == 0.0f);
    CHECK (result.frame.right == 0.0f);
}

TEST_CASE ("Performance fractional-tempo output is bit-identical across partitions", "[performance][timing][fractional]")
{
    const auto render = [] (int partition)
    {
        PerformanceEngine engine;
        engine.prepare (rate);
        PerformanceParameters parameters;
        parameters.variation = .83f;
        parameters.density = .9f;
        parameters.grain = .7f;
        std::vector<StereoFrame> output;
        for (int first = 0; first < 40000; first += partition)
        {
            const auto beats = first * 174.0 / (60.0 * rate);
            const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
            engine.beginContext ({174.0, 4.0}, {bar, beats - static_cast<double> (bar) * 4.0}, 1, true, parameters);
            for (int i = first; i < std::min (first + partition, 40000); ++i)
                output.push_back (engine.processOne (signal (i)).frame);
        }
        return output;
    };
    const auto reference = render (1);
    const auto partitioned = render (257);
    for (std::size_t i = 0; i < reference.size(); ++i)
    {
        INFO ("frame " << i);
        REQUIRE (reference[i].left == partitioned[i].left);
        REQUIRE (reference[i].right == partitioned[i].right);
    }
}

TEST_CASE ("Performance neutral fractional-tempo replay has no internal splice offsets", "[performance][quality][fractional]")
{
    PerformanceEngine engine;
    engine.prepare (rate);
    engine.beginContext ({174.0, 4.0}, {0, 0.0}, 1, true, {});
    constexpr int captured = 11034;
    for (int i = 0; i < captured; ++i)
    {
        const auto value = static_cast<float> (i) / 16384.0f;
        engine.processOne ({value, value});
    }
    REQUIRE (engine.diagnostics().sourceFrames == captured);
    for (int i = captured; i < 2 * captured; ++i)
    {
        const auto result = engine.processOne ({});
        const auto phase = std::max (0.0, (i * 174.0 / (60.0 * rate) - 4.0) / 4.0);
        const auto expected = phase * captured / 16384.0;
        if (phase > .01 && phase < .99)
            REQUIRE (std::abs (result.frame.left - expected) < 1.0e-6);
    }
}

TEST_CASE ("Performance rejects invalid axes and overflowing bar positions", "[performance][invalid]")
{
    SECTION ("Nonfinite public target")
    {
        PerformanceEngine engine;
        engine.prepare (rate);
        PerformanceParameters parameters;
        parameters.grain = std::numeric_limits<float>::quiet_NaN();
        engine.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, parameters);
        CHECK (engine.processOne ({}).frame.left == 0.0f);
        CHECK (engine.diagnostics().error == PerformanceEngine::Error::invalidParameters);
    }
    SECTION ("Integer bar identity must not wrap")
    {
        PerformanceEngine engine;
        engine.prepare (rate);
        engine.beginContext ({120.0, 4.0}, {std::numeric_limits<std::int64_t>::max(), 3.99975}, 1, true, {});
        engine.processOne ({});
        engine.processOne ({});
        CHECK (engine.diagnostics().error == PerformanceEngine::Error::invalidContext);
    }
}

TEST_CASE ("Performance one-sample host seeks invalidate the continuous clock source", "[performance][timing][coherence]")
{
    const auto direction = GENERATE (-1.0, 1.0);
    const auto nextBpm = GENERATE (120.0, 121.0);
    PerformanceEngine engine;
    engine.prepare (rate);
    engine.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, {});
    for (int i = 0; i < barFrames; ++i)
        engine.processOne (signal (i));
    REQUIRE (engine.diagnostics().hasSource);
    const auto beats = 4.0 + direction / 4000.0;
    const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
    engine.beginContext ({nextBpm, 4.0}, {bar, beats - static_cast<double> (bar) * 4.0}, 1, true, {});
    const auto result = engine.processOne ({.37f, -.37f});
    CHECK (result.discontinuity);
    CHECK_FALSE (engine.diagnostics().hasSource);
    CHECK (engine.diagnostics().activeGrains == 0);
    CHECK (result.frame.left == .37f);
    CHECK (engine.diagnostics().error == PerformanceEngine::Error::none);
}

TEST_CASE ("Performance fractional host drift is bounded against the continuous clock", "[performance][timing][coherence]")
{
    PerformanceEngine engine;
    engine.prepare (rate);
    engine.beginContext ({120.0, 4.0}, {0, 0.0}, 1, true, {});
    for (int i = 0; i < barFrames; ++i)
        engine.processOne (signal (i));
    for (int block = 0; block < 3; ++block)
    {
        const auto driftSamples = (block + 1) * .2;
        const auto beat = (block * 16 + driftSamples) / 4000.0;
        engine.beginContext ({120.0, 4.0}, {1, beat}, 1, true, {});
        const auto result = engine.processOne ({.1f, -.1f});
        CHECK (result.discontinuity == (block == 2));
        CHECK (engine.diagnostics().hasSource == (block != 2));
        for (int i = 1; i < 16; ++i)
            engine.processOne ({.1f, -.1f});
    }
}
