// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace livecut::engine::testing
{

// A stereo test signal every platform computes to the same bits, for pinned
// hashes: libm's sin and cos differ in their last bits between toolchains.
// Integer-period triangle waves, each one correctly rounded division of two
// integers, and a hashed-integer noise, mixed with power-of-two weights so that
// no product rounds and fused multiply-adds change nothing.
inline float portableSignal (std::int64_t frame, int channel)
{
    const auto triangle = [frame] (std::int64_t period, std::int64_t offset)
    {
        const auto phase = (frame + offset) % period;
        const auto distance = 2 * phase - period;
        return static_cast<double> (2 * (distance < 0 ? -distance : distance) - period) / static_cast<double> (period); // [-1, 1]
    };
    auto hash = static_cast<std::uint32_t> (frame) * 2654435761u + static_cast<std::uint32_t> (channel + 1) * 40503u;
    hash ^= hash >> 15;
    hash *= 2246822519u;
    hash ^= hash >> 13;
    const auto noise = static_cast<double> (static_cast<std::int32_t> (hash)) / 2147483648.0; // [-1, 1)
    if (channel == 0)
        return static_cast<float> (.5 * triangle (449, 0) + .25 * triangle (10301, 0) + .0625 * noise);
    return static_cast<float> (.5 * triangle (331, 97) + .0625 * noise);
}

} // namespace livecut::engine::testing
