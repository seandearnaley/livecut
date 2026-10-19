// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/Random.h"

#include <algorithm>
#include <cassert>
#include <numeric>

namespace livecut::engine
{

std::size_t pickWeighted (std::span<const double> weights, double unitDraw)
{
    assert (! weights.empty());
    assert (unitDraw >= 0.0 && unitDraw < 1.0);

    const auto target = unitDraw * std::accumulate (weights.begin(), weights.end(), 0.0);
    auto cumulative = 0.0;

    for (std::size_t i = 0; i < weights.size(); ++i)
    {
        assert (weights[i] > 0.0);
        cumulative += weights[i];

        if (target < cumulative)
            return i;
    }

    // Rounding in the running sum can leave the very top of the range just past it.
    return weights.size() - 1;
}

Random::Random (std::uint32_t seed) : engine (seed) {}

void Random::reseed (std::uint32_t seed)
{
    engine.seed (seed);
}

double Random::unit()
{
    static_assert (std::mt19937::max() == 0xffffffffu);
    return static_cast<double> (engine()) * 0x1p-32;
}

double Random::uniform (double a, double b)
{
    return a + (b - a) * unit();
}

int Random::integer (int a, int b)
{
    const auto [low, high] = std::minmax (a, b);
    const auto range = static_cast<std::uint64_t> (static_cast<std::int64_t> (high) - low) + 1;

    // Reject the draws past the last whole multiple of the range, so every value is equally likely.
    constexpr std::uint64_t outcomes = std::uint64_t { 1 } << 32;
    const auto limit = outcomes - outcomes % range;
    std::uint64_t draw = engine();

    while (draw >= limit)
        draw = engine();

    return static_cast<int> (low + static_cast<std::int64_t> (draw % range));
}

bool Random::chance (double probability)
{
    return unit() < probability;
}

std::size_t Random::weighted (std::span<const double> weights)
{
    return pickWeighted (weights, unit());
}

} // namespace livecut::engine
