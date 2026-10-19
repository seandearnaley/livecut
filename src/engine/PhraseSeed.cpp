// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/PhraseSeed.h"

namespace livecut::engine
{

std::uint32_t phraseSeed (std::uint32_t seed, std::int64_t bar)
{
    // SplitMix64's finaliser (Steele, Lea and Flood) over the seed in the high
    // word and the bar in the low word. It is a bijection on 64 bits in which
    // every input bit affects every output bit, so neither neighbouring bars
    // nor neighbouring seed settings produce related seeds.
    //
    // Keying on the bar alone means a looped region repeats its cuts every pass.
    auto mixed = (std::uint64_t { seed } << 32) ^ static_cast<std::uint64_t> (bar);
    mixed += 0x9e3779b97f4a7c15u;
    mixed = (mixed ^ (mixed >> 30)) * 0xbf58476d1ce4e5b9u;
    mixed = (mixed ^ (mixed >> 27)) * 0x94d049bb133111ebu;
    mixed ^= mixed >> 31;
    return static_cast<std::uint32_t> (mixed >> 32);
}

} // namespace livecut::engine
