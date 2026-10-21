// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/AllocationCounter.h"
#include "engine/Cutter.h"
#include "engine/DelayTimeline.h"
#include "engine/ImpactEffects.h"
#include "engine/SwingGrid.h"
#include "legacy/LegacyCutter.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace livecut::engine;

namespace
{
constexpr int rate = 8000;

template <typename T>
bool sameNumber (T first, T second)
{
    return std::memcmp (&first, &second, sizeof (T)) == 0;
}


Position positionAt (double beats)
{
    const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
    return { bar, beats - static_cast<double> (bar) * 4.0 };
}

CutParameters repeatedCuts()
{
    CutParameters parameters;
    parameters.minPhraseBars = parameters.maxPhraseBars = 4;
    parameters.minRepeats = parameters.maxRepeats = 7;
    parameters.stutterChance = 0.0;
    return parameters;
}

ImpactParameters audibleImpact()
{
    ImpactParameters parameters;
    parameters.attackMs = 0.0f;
    parameters.decayMs = 100.0f;
    parameters.filter.enabled = true;
    parameters.filter.cutoffHz = 850.0f;
    parameters.filter.q = 1.4f;
    parameters.filter.envelopeOctaves = 1.5f;
    parameters.filter.repeatOctaves = -2.0f;
    parameters.drive.enabled = true;
    parameters.drive.driveDb = 14.0f;
    parameters.drive.envelopeDb = 5.0f;
    parameters.drive.repeatDb = -6.0f;
    parameters.transient.enabled = true;
    parameters.transient.attackDb = 9.0f;
    parameters.transient.sustainDb = -6.0f;
    return parameters;
}

StereoFrame inputAt (int frame)
{
    const auto pulse = frame % 1000 < 40 ? 1.0f : 0.25f;
    return { pulse * std::sin (static_cast<float> (frame) * .71f),
             pulse * std::cos (static_cast<float> (frame) * .53f) };
}

void configure (Cutter& cutter, Procedure procedure = Procedure::cutProc11)
{
    cutter.prepare (rate);
    cutter.setSeed (7);
    cutter.setProcedure (procedure);
    cutter.setParameters (repeatedCuts());
    cutter.setFadeMilliseconds (2.0);
}

void processFrame (Cutter& cutter, int frame, float& left, float& right,
                   double bpm = 120.0, double startBeat = 0.0)
{
    const auto raw = inputAt (frame);
    left = raw.left;
    right = raw.right;
    cutter.process (&left, &right, 1, { bpm, 4.0 }, positionAt (startBeat + frame * bpm / 60.0 / rate));
}

bool sameCuts (const Cutter& first, const Cutter& second)
{
    if (! sameNumber (first.blockStartUnits(), second.blockStartUnits())
        || ! sameNumber (first.blockEndUnits(), second.blockEndUnits())
        || first.phraseStartBar() != second.phraseStartBar()
        || first.currentCuts().size() != second.currentCuts().size())
        return false;
    for (std::size_t i = 0; i < first.currentCuts().size(); ++i)
    {
        const auto& a = first.currentCuts()[i];
        const auto& b = second.currentCuts()[i];
        if (! sameNumber (a.units, b.units) || ! sameNumber (a.gate, b.gate) || ! sameNumber (a.pan, b.pan) || ! sameNumber (a.amp, b.amp) || ! sameNumber (a.cents, b.cents))
            return false;
    }
    return true;
}
} // namespace

TEST_CASE ("Each enabled Impact module changes the cut audio", "[impact][integration][audible]")
{
    const auto module = GENERATE (0, 1, 2);
    Cutter dry, effected;
    configure (dry);
    configure (effected);
    auto impact = audibleImpact();
    impact.filter.enabled = module == 0;
    impact.drive.enabled = module == 1;
    impact.transient.enabled = module == 2;
    effected.setImpact (impact);
    double difference = 0.0;
    for (int frame = 0; frame < rate; ++frame)
    {
        float left, right, dryLeft, dryRight;
        processFrame (dry, frame, dryLeft, dryRight);
        processFrame (effected, frame, left, right);
        difference += std::abs (left - dryLeft) + std::abs (right - dryRight);
    }
    CHECK (difference > 1.0);
    CHECK (effected.error() == Cutter::Error::none);
}

TEST_CASE ("Neutral Impact preserves legacy audio including Crusher and Comb", "[impact][integration][legacy]")
{
    const auto procedure = GENERATE (0, 1, 2, 3);
    const auto neutral = GENERATE (0, 1, 2);
    Cutter current;
    legacy::Cutter reference;
    configure (current, static_cast<Procedure> (procedure));
    reference.prepare (rate);
    reference.setSeed (7);
    reference.setProcedure (static_cast<legacy::Procedure> (procedure));
    reference.setParameters (repeatedCuts());
    reference.setFadeMilliseconds (2.0);
    const CrusherParameters crusher { true, 6.0f, 14.0f, 1700.0, 6000.0 };
    const CombParameters comb { true, CombParameters::Type::feedBack, 0.4f, 2.0f, 17.0f };
    current.setCrusher (crusher);
    reference.setCrusher (crusher);
    current.setComb (comb);
    reference.setComb (comb);
    auto impact = audibleImpact();
    if (neutral == 0)
        impact.filter.enabled = impact.drive.enabled = impact.transient.enabled = false;
    else if (neutral == 1)
        impact.filter.mix = impact.drive.mix = impact.transient.mix = 0.0f;
    else
    {
        impact.filter.enabled = impact.drive.enabled = false;
        impact.transient.attackDb = impact.transient.sustainDb = 0.0f;
    }
    current.setImpact (impact);
    bool identical = true;
    for (int frame = 0; frame < 3 * rate; ++frame)
    {
        const auto raw = inputAt (frame);
        auto left = raw.left, right = raw.right, oldLeft = raw.left, oldRight = raw.right;
        const auto position = positionAt (static_cast<double> (frame) / 4000.0);
        current.process (&left, &right, 1, { 120.0, 4.0 }, position);
        reference.process (&oldLeft, &oldRight, 1, { 120.0, 4.0 }, { position.bar, position.beatsIntoBar });
        identical &= sameNumber (left, oldLeft) && sameNumber (right, oldRight);
    }
    CHECK (identical);
    CHECK (current.error() == Cutter::Error::none);
}

TEST_CASE ("Impact toggles and probability leave musical and legacy effect draws unchanged", "[impact][integration][random]")
{
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher, Procedure::bounce);
    Cutter reference, effected;
    for (auto* cutter : { &reference, &effected })
    {
        configure (*cutter, procedure);
        cutter->setSwing (75.0);
        cutter->setCrusher ({ true, 6.0f, 14.0f, 1700.0, 6000.0 });
        cutter->setComb ({ true, CombParameters::Type::feedBack, .4f, 2.0f, 17.0f });
    }
    auto impact = audibleImpact();
    bool unchanged = true;
    for (int frame = 0; frame < 4 * rate; ++frame)
    {
        if (frame % 1000 == 0)
        {
            impact.drive.enabled = frame % 3000 != 0;
            impact.filter.chance = static_cast<float> (frame % 7000) / 7000.0f;
            impact.transient.chance = 1.0f - impact.filter.chance;
            effected.setImpact (impact);
        }
        float left, right, otherLeft, otherRight;
        processFrame (reference, frame, left, right);
        processFrame (effected, frame, otherLeft, otherRight);
        unchanged &= sameCuts (reference, effected)
                  && sameNumber (reference.crusher().bits(), effected.crusher().bits())
                  && sameNumber (reference.crusher().holdSamples(), effected.crusher().holdSamples())
                  && sameNumber (reference.comb().delaySamples(), effected.comb().delaySamples());
    }
    CHECK (unchanged);
}

TEST_CASE ("Impact cut triggers occur on the first physical sample and never the previous cut's last", "[impact][integration][onset]")
{
    const auto swing = GENERATE (50.0, 75.0);
    Cutter cutter;
    configure (cutter);
    cutter.setSwing (swing);
    cutter.setImpact (audibleImpact());
    float left, right;
    processFrame (cutter, 0, left, right);
    const auto initial = cutter.impact().diagnostics();
    REQUIRE (initial.cutsStarted == 1);
    REQUIRE (cutter.player().cuts().size() > 1);
    const auto physicalFrame = [&] (double units)
    {
        const auto bar = std::floor (units / 8.0);
        const auto beat = (units - bar * 8.0) / 2.0;
        return static_cast<int> (std::lround ((bar * 4.0 + *SwingGrid::toPhysical (beat, 4.0, swing)) * 4000.0));
    };
    std::vector<int> onsets;
    for (const auto& cut : cutter.player().cuts())
        onsets.push_back (physicalFrame (cut.logicalStart));
    const auto blockEnd = physicalFrame (cutter.blockEndUnits());
    bool correct = true;
    for (int frame = 1; frame < blockEnd; ++frame)
    {
        processFrame (cutter, frame, left, right);
        const auto state = cutter.impact().diagnostics();
        const auto count = static_cast<std::uint64_t> (std::upper_bound (onsets.begin(), onsets.end(), frame) - onsets.begin());
        correct &= state.cutsStarted == count;
        if (std::binary_search (onsets.begin(), onsets.end(), frame))
            correct &= sameNumber (state.envelope, 1.0);
    }
    CHECK (correct);
}

TEST_CASE ("Impact settings are frozen when a swung block is planned", "[impact][integration][snapshot]")
{
    Cutter cutter;
    configure (cutter);
    cutter.setSwing (75.0);
    // At the logical eighth, its swung physical onset is still 1000 frames away.
    auto left = .5f, right = -.25f;
    cutter.process (&left, &right, 1, { 120.0, 4.0 }, { 0, .5 });
    CHECK_FALSE (cutter.isCutting());
    cutter.setImpact (audibleImpact());
    for (int frame = 1; frame <= 1000; ++frame)
        processFrame (cutter, frame, left, right, 120.0, .5);
    REQUIRE (cutter.isCutting());
    const auto state = cutter.impact().diagnostics();
    CHECK (state.cutsStarted == 1);
    CHECK_FALSE (state.filterActive);
    CHECK_FALSE (state.driveActive);
    CHECK_FALSE (state.transientActive);
}

TEST_CASE ("Impact edits wait for the next block rather than the next repeat", "[impact][integration][snapshot]")
{
    Cutter cutter, reference;
    configure (cutter);
    configure (reference);
    float left, right, otherLeft, otherRight;
    processFrame (cutter, 0, left, right);
    processFrame (reference, 0, otherLeft, otherRight);
    const auto blockEnd = static_cast<int> (std::lround (cutter.blockEndUnits() * 2000.0));
    cutter.setImpact (audibleImpact());
    bool identical = true;
    for (int frame = 1; frame < blockEnd; ++frame)
    {
        processFrame (cutter, frame, left, right);
        processFrame (reference, frame, otherLeft, otherRight);
        identical &= sameNumber (left, otherLeft) && sameNumber (right, otherRight);
    }
    CHECK (identical);
    processFrame (cutter, blockEnd, left, right);
    const auto state = cutter.impact().diagnostics();
    CHECK (state.filterActive);
    CHECK (state.driveActive);
    CHECK (state.transientActive);
}

TEST_CASE ("Impact modulation schedules are exact across host buffer partitions", "[impact][integration][partition]")
{
    struct Inspector final : Cutter::Observer
    {
        Inspector (const Cutter& c, std::vector<ImpactEffects::Diagnostics>& t) : cutter (c), trace (t) {}
        void discontinuity (int) override {}
        void phraseCompleted (int) override {}
        void phraseStarted (int, bool) override {}
        void processOutput (int offset, float, float, float&, float&) override
        {
            trace[static_cast<std::size_t> (first + offset)] = cutter.impact().diagnostics();
        }
        const Cutter& cutter;
        std::vector<ImpactEffects::Diagnostics>& trace;
        int first = 0;
    };
    const auto swing = GENERATE (50.0, 75.0);
    const auto render = [swing] (int block)
    {
        Cutter cutter;
        configure (cutter, Procedure::warpCut);
        cutter.setSwing (swing);
        auto impact = audibleImpact();
        impact.filter.chance = .43f;
        impact.drive.chance = .71f;
        impact.transient.chance = .37f;
        cutter.setImpact (impact);
        std::vector<ImpactEffects::Diagnostics> trace (24000);
        Inspector inspector (cutter, trace);
        std::vector<float> left (static_cast<std::size_t> (block)), right (left.size());
        for (int first = 0; first < 24000;)
        {
            const auto boundary = first < 10000 ? 10000 : first < 18000 ? 18000 : 24000;
            const auto frames = std::min (block, boundary - first);
            const auto bpm = first < 10000 ? 120.0 : 80.0;
            const auto beats = first < 10000 ? first / 4000.0
                             : first < 18000 ? 2.5 + (first - 10000) / 6000.0
                                             : 8.0 + (first - 18000) / 6000.0;
            for (int i = 0; i < frames; ++i)
            {
                const auto raw = inputAt (first + i);
                left[static_cast<std::size_t> (i)] = raw.left;
                right[static_cast<std::size_t> (i)] = raw.right;
            }
            inspector.first = first;
            cutter.process (left.data(), right.data(), frames, { bpm, 4.0 }, positionAt (beats), &inspector);
            first += frames;
        }
        REQUIRE (cutter.error() == Cutter::Error::none);
        return trace;
    };
    const auto expected = render (1);
    for (const auto block : { 7, 137, 512 })
    {
        const auto actual = render (block);
        std::size_t firstDifference = 0;
        while (firstDifference < expected.size())
        {
            const auto& a = expected[firstDifference];
            const auto& b = actual[firstDifference];
            if (a.cutsStarted != b.cutsStarted || ! sameNumber (a.envelope, b.envelope)
                || ! sameNumber (a.repeatProgress, b.repeatProgress) || a.filterActive != b.filterActive
                || a.driveActive != b.driveActive || a.transientActive != b.transientActive)
                break;
            ++firstDifference;
        }
        INFO ("Swing " << swing << ", block " << block << ", first modulation difference " << firstDifference);
        CHECK (firstDifference == expected.size());
    }
}

TEST_CASE ("Impact audio is exact when original render contexts span different output buffers", "[impact][integration][partition][contexts]")
{
    const auto swing = GENERATE (50.0, 75.0);
    const auto render = [swing] (int block)
    {
        Cutter cutter;
        configure (cutter, Procedure::warpCut);
        cutter.setSwing (swing);
        cutter.setImpact (audibleImpact());
        std::vector<StereoFrame> output (24000);
        for (int first = 0; first < 24000; first += block)
        {
            const auto frames = std::min (block, 24000 - first);
            for (int i = 0; i < frames; ++i)
            {
                const auto frame = first + i;
                const auto raw = inputAt (frame);
                REQUIRE (cutter.ingestInputFrame (frame, raw));
                // These are the original input contexts. Output buffer edges
                // do not invent new PPQ origins inside one original context.
                if (frame == 0 || frame == 10000 || frame == 18000)
                {
                    const auto bpm = frame == 0 ? 120.0 : 80.0;
                    const auto beats = frame == 0 ? 0.0 : frame == 10000 ? 2.5 : 8.0;
                    cutter.beginRenderContext ({ bpm, 4.0 }, positionAt (beats), frame, i, nullptr);
                }
                auto left = raw.left, right = raw.right;
                cutter.renderOne (frame, left, right, i, nullptr);
                output[static_cast<std::size_t> (frame)] = { left, right };
                if (frame + 1 == 10000 || frame + 1 == 18000 || frame + 1 == 24000)
                    cutter.endRenderContext (i + 1, nullptr);
            }
        }
        REQUIRE (cutter.error() == Cutter::Error::none);
        return output;
    };
    const auto expected = render (1);
    for (const auto block : { 7, 137, 512 })
    {
        const auto actual = render (block);
        CHECK (std::memcmp (expected.data(), actual.data(), expected.size() * sizeof (StereoFrame)) == 0);
    }
}

TEST_CASE ("Impact rendering resets tail and onset state on a transport discontinuity", "[impact][integration][reset]")
{
    Cutter reused, fresh;
    configure (reused);
    configure (fresh);
    reused.setImpact (audibleImpact());
    fresh.setImpact (audibleImpact());
    float left, right, otherLeft, otherRight;
    for (int frame = 0; frame < 2000; ++frame)
        processFrame (reused, frame, left, right);
    reused.resetRendering();
    bool identical = true;
    for (int frame = 0; frame < 4000; ++frame)
    {
        processFrame (reused, frame, left, right, 120.0, 20.0);
        processFrame (fresh, frame, otherLeft, otherRight, 120.0, 20.0);
        identical &= sameNumber (left, otherLeft) && sameNumber (right, otherRight);
    }
    CHECK (identical);
    const auto reusedState = reused.impact().diagnostics(), freshState = fresh.impact().diagnostics();
    CHECK (reusedState.cutsStarted == freshState.cutsStarted);
}

TEST_CASE ("Impact activation and dense swung cuts do not allocate", "[impact][integration][allocations]")
{
    Cutter cutter;
    configure (cutter);
    cutter.setUnitsPerBar (32);
    cutter.setSwing (75.0);
    auto parameters = repeatedCuts();
    parameters.stutterChance = 1.0;
    parameters.stutterArea = 1.0;
    cutter.setParameters (parameters);
    cutter.setImpact (audibleImpact());
    std::size_t allocations = 0;
    for (int frame = 0; frame < 2 * rate; ++frame)
    {
        float left, right;
        const testing::AllocationCounter counter;
        processFrame (cutter, frame, left, right);
        allocations += counter.count();
    }
    CHECK (allocations == 0);
    CHECK (cutter.error() == Cutter::Error::none);
}
