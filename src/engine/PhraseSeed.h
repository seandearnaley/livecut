// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace livecut::engine
{

// The seed the cutter gives its random generator at the start of the phrase
// that begins at `bar`, given the plugin's seed setting.
//
// Because the cutter reseeds on every phrase, the cuts at any position in the
// song depend only on (seed, bar). An offline bounce therefore cuts exactly
// as playback did, and changing the seed setting gives a different, equally
// repeatable set of cuts.
std::uint32_t phraseSeed (std::uint32_t seed, std::int64_t bar);

} // namespace livecut::engine
