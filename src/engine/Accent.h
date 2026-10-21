// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace livecut::engine
{
// Independent from the musical RNG: enabling accents cannot change a pattern.
inline float accentGain (float amount, std::uint32_t seed, std::int64_t phraseBar, std::uint64_t cutOrdinal)
{
    if (amount <= 0.0f)
        return 1.0f;
    auto value = static_cast<std::uint64_t> (phraseBar) ^ (static_cast<std::uint64_t> (seed) << 32)
               ^ (cutOrdinal * 0x9e3779b97f4a7c15ULL);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    value ^= value >> 31;
    const auto unit = static_cast<float> (value >> 40) / 16777216.0f;
    return 1.0f - 0.25f * amount * unit;
}
} // namespace livecut::engine
