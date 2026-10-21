// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/Cutter.h"
#include "legacy/LegacyCutter.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace livecut::engine;

TEST_CASE ("Neutral Perform reproduces the immutable 0.3 engine sample for sample", "[cutter][perform][legacy]")
{
    const auto procedure = GENERATE (0, 1, 2, 3);
    const auto blockSize = GENERATE (137, 512);
    Cutter current;
    legacy::Cutter reference;
    constexpr double rate = 8000.0;
    current.prepare (rate);
    reference.prepare (rate);
    current.setProcedure (static_cast<Procedure> (procedure));
    reference.setProcedure (static_cast<legacy::Procedure> (procedure));
    CutParameters parameters;
    parameters.minPhraseBars = parameters.maxPhraseBars = 8;
    parameters.minRepeats = parameters.maxRepeats = 12;
    parameters.minCents = -900.0f;
    parameters.maxCents = 700.0f;
    parameters.minPan = -0.7f;
    parameters.maxPan = 0.6f;
    parameters.minAmp = 0.3f;
    parameters.maxAmp = 0.9f;
    current.setParameters (parameters);
    reference.setParameters (parameters);
    current.setFadeMilliseconds (4.0);
    reference.setFadeMilliseconds (4.0);
    CrusherParameters crusher;
    crusher.enabled = true;
    crusher.minBits = 6.0f;
    crusher.maxBits = 14.0f;
    crusher.minHz = 1700.0;
    crusher.maxHz = 6000.0;
    current.setCrusher (crusher);
    reference.setCrusher (crusher);
    CombParameters comb;
    comb.enabled = true;
    comb.type = CombParameters::Type::feedBack;
    comb.feedback = 0.4f;
    comb.minDelayMs = 2.0f;
    comb.maxDelayMs = 17.0f;
    current.setComb (comb);
    reference.setComb (comb);
    std::vector<float> left (static_cast<std::size_t> (blockSize)), right (left.size()), oldLeft (left.size()), oldRight (left.size());
    double beats = 0.0;
    // Slow eight-bar phrases exercise blocks/repeats that live far beyond the
    // original eight-second source extent. Change tempo after ten seconds.
    for (int done = 0; done < 70 * static_cast<int> (rate); done += blockSize)
    {
        const auto bpm = done < 10 * rate ? 31.0 : 47.0;
        for (int sample = 0; sample < blockSize; ++sample)
        {
            left[static_cast<std::size_t> (sample)] = std::sin (static_cast<float> (done + sample) * 0.013f);
            right[static_cast<std::size_t> (sample)] = std::cos (static_cast<float> (done + sample) * 0.017f);
        }
        oldLeft = left;
        oldRight = right;
        const auto bar = static_cast<std::int64_t> (std::floor (beats / 4.0));
        const auto into = beats - static_cast<double> (bar) * 4.0;
        current.process (left.data(), right.data(), blockSize, { bpm, 4.0 }, { bar, into });
        reference.process (oldLeft.data(), oldRight.data(), blockSize, { bpm, 4.0 }, { bar, into });
        REQUIRE (std::memcmp (left.data(), oldLeft.data(), left.size() * sizeof (float)) == 0);
        REQUIRE (std::memcmp (right.data(), oldRight.data(), right.size() * sizeof (float)) == 0);
        beats += blockSize * bpm / 60.0 / rate;
    }
}
