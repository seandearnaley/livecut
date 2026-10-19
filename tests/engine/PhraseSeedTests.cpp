// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/PhraseSeed.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <set>

using livecut::engine::phraseSeed;

TEST_CASE ("Phrase seeds are repeatable", "[phraseseed]")
{
    CHECK (phraseSeed (5, 12) == phraseSeed (5, 12));
    CHECK (phraseSeed (1, -1) == phraseSeed (1, -1));
}

// Phrases can start on any bar, including the bars of a count-in before bar 0.
TEST_CASE ("Every bar of a long song gets its own phrase seed", "[phraseseed]")
{
    std::set<std::uint32_t> seeds;

    for (std::int64_t bar = -16; bar < 10000; ++bar)
        seeds.insert (phraseSeed (1, bar));

    CHECK (seeds.size() == 10016);
}

// A seed setting must not just replay another setting's phrases a few bars later.
TEST_CASE ("Each seed setting gives its own phrase seeds", "[phraseseed]")
{
    for (std::uint32_t seed = 1; seed < 16; ++seed)
    {
        std::set<std::uint32_t> these;
        std::set<std::uint32_t> next;

        for (std::int64_t bar = 0; bar < 1000; ++bar)
        {
            these.insert (phraseSeed (seed, bar));
            next.insert (phraseSeed (seed + 1, bar));
        }

        int shared = 0;

        for (const auto value : these)
            shared += next.count (value) > 0 ? 1 : 0;

        CHECK (shared == 0);
    }
}
