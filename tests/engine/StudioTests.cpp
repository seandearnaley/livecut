// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/Cutter.h"
#include "engine/DelayTimeline.h"
#include "engine/AllocationCounter.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <cstring>
#include <cmath>
#include <vector>

using namespace livecut::engine;

template <typename T>
concept SeparateRenderer = requires (T& cutter, StereoFrame raw, float& sample)
{
    cutter.ingestInputFrame (std::int64_t {}, raw);
    cutter.rawInputFrame (std::int64_t {});
    cutter.nextInputFrame();
    cutter.beginRenderContext (Timing {}, Position {}, std::int64_t {}, 0, nullptr);
    cutter.renderOne (std::int64_t {}, sample, sample, 0, nullptr);
    cutter.endRenderContext (0, nullptr);
};

TEST_CASE ("Separate rendering equals delayed Live with original context boundaries", "[studio][render]")
{
    const auto procedure = GENERATE (Procedure::cutProc11, Procedure::warpCut, Procedure::sqPusher, Procedure::bounce);
    const auto swing = GENERATE (50.0, 75.0);
    const auto block = GENERATE (137, 512);
    [&]<typename T>()
    {
        if constexpr (! SeparateRenderer<T>)
            FAIL ("Cutter does not yet expose separate raw ingestion and original-context rendering");
        else
        {
            constexpr int rate = 8000, latency = rate / 2, total = rate * 4;
            T live, delayed;
            CutParameters parameters;
            parameters.minPhraseBars = parameters.maxPhraseBars = 2;
            parameters.minCents = -900.0f;
            parameters.maxCents = 700.0f;
            parameters.minPan = -0.7f;
            parameters.maxPan = 0.6f;
            for (auto* cutter : { &live, &delayed })
            {
                cutter->prepare (rate);
                cutter->setProcedure (procedure);
                cutter->setParameters (parameters);
                cutter->setSwing (swing);
                cutter->setFadeMilliseconds (4.0);
                cutter->setCrusher ({ true, 6.0f, 14.0f, 1700.0, 6000.0 });
                cutter->setComb ({ true, CombParameters::Type::feedBack, 0.4f, 2.0f, 17.0f });
            }
            struct Context { Timing timing; Position position; };
            DelayTimeline<Context> timeline;
            REQUIRE (timeline.prepare (latency));
            std::vector<float> inputLeft (total + latency), inputRight (inputLeft.size());
            for (std::size_t i = 0; i < inputLeft.size(); ++i)
            {
                inputLeft[i] = std::sin (static_cast<float> (i) * .013f);
                inputRight[i] = std::cos (static_cast<float> (i) * .017f);
            }
            auto expectedLeft = inputLeft, expectedRight = inputRight;
            std::vector<float> actualLeft (total), actualRight (total);
            std::int64_t input = 0;
            double beats = 0.0;
            std::size_t allocations = 0;
            bool valid = true;
            for (int first = 0; first < total + latency; first += block)
            {
                const auto frames = std::min (block, total + latency - first);
                const auto bpm = first < 10000 ? 120.0 : 83.0;
                const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
                const Context context { { bpm, 4.0 }, { bar, beats - static_cast<double> (bar) * 4.0 } };
                live.process (expectedLeft.data() + first, expectedRight.data() + first, frames, context.timing, context.position);
                valid &= timeline.append (first, frames, 1, 0, context);
                for (int i = 0; i < frames; ++i, ++input)
                {
                    const auto t = static_cast<std::size_t> (input);
                    const testing::AllocationCounter counter;
                    valid &= delayed.ingestInputFrame (input, { inputLeft[t], inputRight[t] });
                    const auto r = timeline.renderFrame (input);
                    if (r && *r < total)
                    {
                        const auto original = timeline.lookup (*r);
                        if (! original)
                        {
                            valid = false;
                            break;
                        }
                        if (original->offset == 0)
                            delayed.beginRenderContext (original->segment.context.timing, original->segment.context.position, *r, i, nullptr);
                        const auto raw = delayed.rawInputFrame (*r);
                        if (! raw)
                        {
                            valid = false;
                            break;
                        }
                        auto left = raw->left, right = raw->right;
                        delayed.renderOne (*r, left, right, i, nullptr);
                        actualLeft[static_cast<std::size_t> (*r)] = left;
                        actualRight[static_cast<std::size_t> (*r)] = right;
                        if (original->offset + 1 == original->segment.frameCount)
                            delayed.endRenderContext (i + 1, nullptr);
                        valid &= timeline.retireBefore (*r + 1);
                    }
                    allocations += counter.count();
                }
                beats += frames * bpm / 60.0 / rate;
            }
            CHECK (valid);
            CHECK (delayed.nextInputFrame() == total + latency);
            CHECK (allocations == 0);
            CHECK (delayed.error() == T::Error::none);
            CHECK (std::memcmp (actualLeft.data(), expectedLeft.data(), total * sizeof (float)) == 0);
            CHECK (std::memcmp (actualRight.data(), expectedRight.data(), total * sizeof (float)) == 0);
        }
    }.template operator()<Cutter>();
}

TEST_CASE ("Rendering reset preserves ingested future audio and duplicate ingest is explicit", "[studio][render]")
{
    Cutter cutter;
    cutter.prepare (8000.0);
    REQUIRE (cutter.ingestInputFrame (0, { .25f, -.25f }));
    CHECK_FALSE (cutter.ingestInputFrame (0, { .75f, -.75f }));
    CHECK (cutter.error() == Cutter::Error::renderDiscontinuity);
    cutter.resetRendering();
    CHECK (cutter.nextInputFrame() == 1);
    const auto retained = cutter.rawInputFrame (0);
    REQUIRE (retained);
    CHECK (retained->left == .25f);
    CHECK (retained->right == -.25f);
    REQUIRE (cutter.ingestInputFrame (1, { .5f, -.5f }));
    cutter.beginRenderContext ({ 120.0, 4.0 }, {}, 0, 0, nullptr);
    float left = .25f, right = -.25f;
    cutter.renderOne (1, left, right, 0, nullptr); // expected render frame is zero
    CHECK (left == 0.0f);
    CHECK (right == 0.0f);
    CHECK (cutter.error() == Cutter::Error::renderDiscontinuity);
}
