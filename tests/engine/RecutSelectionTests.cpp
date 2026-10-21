// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/Cutter.h"
#include "engine/OnsetDetector.h"
#include "engine/AllocationCounter.h"
#include "engine/PhraseSeed.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cmath>

using namespace livecut::engine;

template <typename T>
concept RecutSelection = requires (T& cutter)
{
    typename T::SourceContext;
    typename T::TransformParameters;
    cutter.transformStats();
    cutter.setTransforms (typename T::TransformParameters {});
};

template <typename T>
struct SelectionRig : T::SourceContext
{
    SelectionRig (int ahead, bool attack, double reverse, bool transient = false)
        : lookahead (ahead), transientInput (transient)
    {
        cutter.prepare (8000.0, ahead);
        valid &= detector.prepare (8000.0, 8192, 3.0);
        cutter.setTransformSource (detector, *this);
        cutter.setTransforms ({ attack, 6.0, reverse });
        cutter.setProcedure (Procedure::warpCut);
        CutParameters params;
        params.minPhraseBars = params.maxPhraseBars = 1;
        params.straightChance = 1.0;
        params.minPan = params.maxPan = 0.0f;
        cutter.setParameters (params);
        cutter.beginRenderContext ({ 120.0, 1.0 }, {}, 0, 0, nullptr);
    }
    std::optional<typename T::TransportBounds> sourceBoundsAt (std::int64_t frame) const override
    {
        ++boundsQueries;
        return typename T::TransportBounds { 1, 0, endKnownAt >= 0 && written > endKnownAt ? epochEnd : std::nullopt };
    }
    StereoFrame input (std::int64_t frame) const
    {
        if (transientInput && frame == 96)
            return { .9f, -.6f };
        return { .01f + static_cast<float> (frame) * .000001f,
                 -.03f - static_cast<float> (frame) * .000002f };
    }
    StereoFrame step (std::int64_t frame)
    {
        const testing::AllocationCounter counter;
        while (written <= frame + lookahead)
        {
            if (closeDetector && epochEnd && written == *epochEnd)
                valid &= detector.beginTransportEpoch (2);
            const auto raw = input (written);
            valid &= cutter.ingestInputFrame (written, raw) && detector.process (written, raw.left, raw.right);
            ++written;
        }
        auto raw = input (frame);
        cutter.renderOne (frame, raw.left, raw.right, static_cast<int> (frame), nullptr);
        allocations += counter.count();
        return raw;
    }
    T cutter;
    OnsetDetector detector;
    int lookahead;
    bool transientInput, closeDetector = false;
    std::int64_t written = 0, endKnownAt = -1;
    std::optional<std::int64_t> epochEnd;
    mutable int boundsQueries = 0;
    bool valid = true;
    std::size_t allocations = 0;
};

TEST_CASE ("Recut selects a complete stereo reverse span before the cut's first sample", "[recut][selection]")
{
    []<typename T>()
    {
        if constexpr (! RecutSelection<T>)
            FAIL ("Cutter lacks per-cut complete-source transform selection");
        else
        {
            SelectionRig<T> rig (4000, false, 1.0);
            rig.step (0);
            const auto frames = rig.cutter.player().currentSoundingFrames();
            REQUIRE (frames > 16);
            bool exact = true;
            for (int i = 1; i < frames; ++i)
            {
                const auto actual = rig.step (i);
                const auto source = rig.input (frames - 1 - i);
                const auto envelope = fadeEnvelope (i, 1, frames);
                exact &= actual.left == envelope * source.left && actual.right == envelope * source.right;
            }
            CHECK (exact);
            CHECK (rig.cutter.transformStats().eligible == 1);
            CHECK (rig.cutter.transformStats().reversed == 1);
            CHECK (rig.cutter.transformStats().lastSourceStart == 0);
            CHECK (rig.valid);
            CHECK (rig.allocations == 0);
        }
    }.template operator()<Cutter>();
}

TEST_CASE ("Attack Align requires a qualifying complete attack before Reverse is eligible", "[recut][selection]")
{
    []<typename T>()
    {
        if constexpr (! RecutSelection<T>)
            FAIL ("Cutter lacks attack-conditioned reverse eligibility");
        else
        {
            SelectionRig<T> combined (4000, true, 1.0), ordinary (4000, false, 0.0);
            bool exact = true;
            for (int i = 0; i < 4000; ++i)
            {
                const auto actual = combined.step (i), expected = ordinary.step (i);
                exact &= actual.left == expected.left && actual.right == expected.right;
            }
            CHECK (exact);
            CHECK (combined.cutter.transformStats().eligible == 0);
            CHECK (combined.cutter.transformStats().reversed == 0);
            CHECK (combined.cutter.transformStats().ineligible > 0);
            CHECK (combined.valid);
            CHECK (combined.allocations == 0);
        }
    }.template operator()<Cutter>();
}

TEST_CASE ("Attack selection freezes one linked stereo source with the declared preroll", "[recut][selection]")
{
    const auto reverse = GENERATE (false, true);
    [=]<typename T>()
    {
        if constexpr (! RecutSelection<T>)
            FAIL ("Cutter lacks attack source selection");
        else
        {
            SelectionRig<T> rig (4000, true, reverse ? 1.0 : 0.0, true);
            rig.step (0);
            const auto frames = rig.cutter.player().currentSoundingFrames();
            REQUIRE (rig.cutter.transformStats().attackAligned == 1);
            CHECK (rig.cutter.transformStats().lastSourceStart == 80); // peak96 - 2ms16
            bool exact = true;
            for (int i = 1; i < frames; ++i)
            {
                const auto actual = rig.step (i);
                const auto source = rig.input (80 + (reverse ? frames - 1 - i : i));
                const auto envelope = fadeEnvelope (i, 1, frames);
                exact &= actual.left == envelope * source.left && actual.right == envelope * source.right;
            }
            CHECK (exact);
            CHECK (rig.valid);
            CHECK (rig.allocations == 0);
        }
    }.template operator()<Cutter>();
}

TEST_CASE ("Live marks an incomplete first reverse cut ineligible without changing its planned output", "[recut][selection]")
{
    []<typename T>()
    {
        if constexpr (! RecutSelection<T>)
            FAIL ("Cutter lacks honest Live source availability checks");
        else
        {
            SelectionRig<T> recut (0, false, 1.0), ordinary (0, false, 0.0);
            auto actual = recut.step (0), expected = ordinary.step (0);
            const auto frames = recut.cutter.player().currentSoundingFrames();
            bool exact = actual.left == expected.left && actual.right == expected.right;
            for (int i = 1; i < frames; ++i)
            {
                actual = recut.step (i);
                expected = ordinary.step (i);
                exact &= actual.left == expected.left && actual.right == expected.right;
            }
            CHECK (exact);
            CHECK (recut.cutter.transformStats().ineligible == 1);
            CHECK (recut.cutter.transformStats().eligible == 0);
            CHECK (recut.cutter.transformStats().reversed == 0);
            CHECK (recut.allocations == 0);
        }
    }.template operator()<Cutter>();
}


// Build a reproducible two-cut, one-unit repeat fixture using the same public
// procedure contract. No seed is assumed to produce a particular random draw.
static std::uint32_t oneUnitRepeatSeed (const CutParameters& params)
{
    CutProc11 procedure;
    std::vector<Cut> cuts;
    for (std::uint32_t seed = 1; seed < 100; ++seed)
    {
        Random random (phraseSeed (seed, 0));
        const auto bars = procedure.choosePhraseBars (random, params);
        procedure.chooseBlock (random, params, { 0.0, static_cast<double> (bars * 32), 32, 1.0 }, cuts);
        if (cuts.size() == 2 && cuts[0].units == 1.0)
            return seed;
    }
    FAIL ("Cannot construct the bounded repeat fixture");
    return 0;
}

TEST_CASE ("A cached original context refreshes its epoch end at each cut activation", "[recut][selection][epoch]")
{
    []<typename T>()
    {
        if constexpr (! RecutSelection<T>)
            FAIL ("Cutter lacks fresh transform epoch bounds");
        else
        {
            SelectionRig<T> rig (64, true, 1.0, true);
            CutParameters params;
            params.minPhraseBars = params.maxPhraseBars = 1;
            params.minRepeats = params.maxRepeats = 1;
            params.minPan = params.maxPan = 0.0f;
            params.stutterChance = 0.0;
            rig.cutter.setProcedure (Procedure::cutProc11);
            rig.cutter.setUnitsPerBar (32);
            rig.cutter.setParameters (params);
            rig.cutter.setSeed (oneUnitRepeatSeed (params));
            rig.epochEnd = 130;
            rig.endKnownAt = 130;
            rig.closeDetector = true;
            rig.step (0); // input64: original epoch end is still unknown
            REQUIRE (rig.cutter.player().currentSoundingFrames() == 125);
            CHECK (rig.cutter.transformStats().reason == T::TransformReason::searchPending);
            for (int r = 1; r <= 125; ++r)
                rig.step (r);
            // Physical input189 has now closed epoch1 at130 while this same
            // original render context still plays frame125. The marker96's
            // preroll origin80 plus the full125-frame source crosses130.
            CHECK (rig.cutter.transformStats().reason == T::TransformReason::sourceIncomplete);
            CHECK (rig.cutter.transformStats().ineligible == 2);
            CHECK (rig.cutter.transformStats().reversed == 0);
            CHECK (rig.boundsQueries >= 3);
            CHECK (rig.valid);
            CHECK (rig.allocations == 0);
        }
    }.template operator()<Cutter>();
}

TEST_CASE ("Transform controls are latched to a block and not reread for its repeat", "[recut][selection][latching]")
{
    []<typename T>()
    {
        if constexpr (! RecutSelection<T>)
            FAIL ("Cutter lacks block-latched transforms");
        else
        {
            SelectionRig<T> rig (4000, false, 1.0);
            CutParameters params;
            params.minPhraseBars = params.maxPhraseBars = 1;
            params.minRepeats = params.maxRepeats = 1;
            params.minPan = params.maxPan = 0.0f;
            params.stutterChance = 0.0;
            rig.cutter.setProcedure (Procedure::cutProc11);
            rig.cutter.setUnitsPerBar (32);
            rig.cutter.setParameters (params);
            rig.cutter.setSeed (oneUnitRepeatSeed (params));
            rig.step (0);
            rig.cutter.setTransforms ({ false, 24.0, 0.0 });
            for (int r = 1; r <= 125; ++r)
                rig.step (r);
            CHECK (rig.cutter.transformStats().reversed == 2);
            for (int r = 126; r <= 250; ++r)
                rig.step (r);
            CHECK (rig.cutter.transformStats().reversed == 2);
            CHECK (rig.cutter.transformStats().reason == T::TransformReason::off);
            CHECK (rig.valid);
        }
    }.template operator()<Cutter>();
}

TEST_CASE ("Reverse decisions repeat without changing musical or effect RNG draws", "[recut][selection][determinism]")
{
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher, Procedure::bounce);
    const auto swing = GENERATE (50.0, 75.0);
    [=]<typename T>()
    {
        if constexpr (! RecutSelection<T>)
            FAIL ("Cutter lacks independent reverse decisions");
        else
        {
            SelectionRig<T> a (4000, false, .43), b (4000, false, .43), ordinary (4000, false, 0.0);
            CutParameters params;
            params.minPhraseBars = params.maxPhraseBars = 4;
            params.minCents = -700.0f;
            params.maxCents = 900.0f;
            for (auto* rig : { &a, &b, &ordinary })
            {
                rig->cutter.setParameters (params);
                rig->cutter.setProcedure (procedure);
                rig->cutter.setSwing (swing);
                rig->cutter.setCrusher ({ true, 6.0f, 12.0f, 1000.0, 7000.0 });
                rig->cutter.setComb ({ true, CombParameters::Type::feedBack, .4f, 2.0f, 19.0f });
            }
            bool exact = true, samePatterns = true;
            for (int r = 0; r < 16000; ++r)
            {
                const auto left = a.step (r), right = b.step (r);
                ordinary.step (r);
                exact &= left.left == right.left && left.right == right.right;
                const auto actual = a.cutter.currentCuts(), expected = ordinary.cutter.currentCuts();
                samePatterns &= actual.size() == expected.size();
                for (std::size_t i = 0; samePatterns && i < actual.size(); ++i)
                    samePatterns &= actual[i].units == expected[i].units && actual[i].amp == expected[i].amp
                                 && actual[i].pan == expected[i].pan && actual[i].cents == expected[i].cents
                                 && actual[i].gate == expected[i].gate;
                samePatterns &= a.cutter.crusher().bits() == ordinary.cutter.crusher().bits()
                             && a.cutter.crusher().holdSamples() == ordinary.cutter.crusher().holdSamples()
                             && a.cutter.comb().delaySamples() == ordinary.cutter.comb().delaySamples();
            }
            CHECK (exact);
            CHECK (samePatterns);
            CHECK (a.cutter.transformStats().eligible > 0);
            CHECK (a.cutter.transformStats().reversed > 0);
            CHECK (a.cutter.transformStats().reversed < a.cutter.transformStats().eligible);
            CHECK (a.valid);
            CHECK (a.cutter.error() == T::Error::none);
            CHECK (a.allocations == 0);
        }
    }.template operator()<Cutter>();
}
