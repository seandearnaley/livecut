// SPDX-License-Identifier: GPL-3.0-or-later
//
// Ported from Livecut, Copyright 2003-2004 Remy Muller.

#include "engine/LivePlayer.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace livecut::engine
{

float fadeEnvelope (int t, int fade, int length)
{
    const auto time = static_cast<double> (t);
    const auto fadeLength = static_cast<double> (fade);
    const auto attack = 1.0 - std::exp (-5.0 * time / fadeLength);
    const auto release = 1.0 - std::exp (5.0 * (time - length) / fadeLength);
    return static_cast<float> (attack * release);
}

int playableSamples (int length, double rate, int recorded, int capacity)
{
    const auto readable = [&] (int t)
    {
        const auto index = static_cast<std::int64_t> (static_cast<double> (t) * rate);
        const auto available = std::min<std::int64_t> (capacity, std::int64_t { recorded } + t + 1);
        return index + 1 < available;
    };

    // The read head moves at `rate` while the recording advances one sample at
    // a time into a finite buffer, so once a sample is unreadable every later
    // one is too: bisect for the first unreadable sample.
    auto low = 0;
    auto high = length;

    while (low < high)
    {
        const auto middle = low + (high - low) / 2;

        if (readable (middle))
            low = middle + 1;
        else
            high = middle;
    }

    return low;
}

void LivePlayer::prepare (int newCapacity)
{
    capacity = newCapacity;
    recordedLeft.assign (static_cast<std::size_t> (capacity), 0.0f);
    recordedRight.assign (static_cast<std::size_t> (capacity), 0.0f);
    playing.clear();
    playing.reserve (maxCutsPerBlock);
    recorded = 0;
    current = 0;
    position = 0;
}

void LivePlayer::setFadeSamples (int samples)
{
    fade = std::max (samples, 1);
}

void LivePlayer::startBlock (std::span<const Cut> cuts, double samplesPerUnit)
{
    assert (cuts.size() <= maxCutsPerBlock);
    playing.clear();
    recorded = 0;
    current = 0;
    position = 0;

    auto units = 0.0;
    auto start = 0;

    for (std::size_t i = 0; i < cuts.size(); ++i)
    {
        const auto& cut = cuts[i];
        units += cut.units;
        const auto end = static_cast<int> (std::lround (units * samplesPerUnit));
        const auto size = end - start;

        if (size > 0)
        {
            assert (size <= capacity);
            PlayingCut playingCut;
            playingCut.start = start;
            playingCut.size = size;

            const auto soundingLength = std::min (static_cast<int> (std::lround (size * cut.gate)), size);
            playingCut.pitched = i > 0 && std::abs (cut.cents) > 1.0e-3f;
            playingCut.rate = playingCut.pitched ? std::pow (2.0, cut.cents / 1200.0) : 1.0;
            playingCut.playable = playingCut.pitched
                                      ? playableSamples (soundingLength, playingCut.rate, std::min (start, capacity), capacity)
                                      : soundingLength;

            // Livecut's pan rotates the stereo pair: the input on the far side
            // folds into the near channel as the cut moves away from centre.
            const auto angle = static_cast<double> (cut.pan) * std::numbers::pi / 2.0;
            const auto amp = static_cast<double> (cut.amp);
            playingCut.leftToLeft = static_cast<float> (amp * (cut.pan < 0.0f ? 1.0 : std::cos (angle)));
            playingCut.leftToRight = static_cast<float> (amp * (cut.pan < 0.0f ? 0.0 : std::sin (angle)));
            playingCut.rightToLeft = static_cast<float> (amp * (cut.pan > 0.0f ? 0.0 : -std::sin (angle)));
            playingCut.rightToRight = static_cast<float> (amp * (cut.pan > 0.0f ? 1.0 : std::cos (angle)));

            playing.push_back (playingCut);
        }

        start = end;
    }
}

void LivePlayer::process (float& left, float& right)
{
    if (recorded < capacity)
    {
        recordedLeft[static_cast<std::size_t> (recorded)] = left;
        recordedRight[static_cast<std::size_t> (recorded)] = right;
        ++recorded;
    }

    if (current >= playing.size())
    {
        left = 0.0f;
        right = 0.0f;
        return;
    }

    const auto& cut = playing[current];
    auto outLeft = 0.0f;
    auto outRight = 0.0f;

    if (position < cut.playable)
    {
        float inLeft;
        float inRight;

        if (cut.pitched)
        {
            const auto read = static_cast<double> (position) * cut.rate;
            const auto index = static_cast<std::size_t> (read);
            const auto fraction = static_cast<float> (read - static_cast<double> (index));
            inLeft = recordedLeft[index] + fraction * (recordedLeft[index + 1] - recordedLeft[index]);
            inRight = recordedRight[index] + fraction * (recordedRight[index + 1] - recordedRight[index]);
        }
        else
        {
            inLeft = recordedLeft[static_cast<std::size_t> (position)];
            inRight = recordedRight[static_cast<std::size_t> (position)];
        }

        const auto envelope = fadeEnvelope (position, fade, cut.playable);
        outLeft = envelope * (cut.leftToLeft * inLeft + cut.rightToLeft * inRight);
        outRight = envelope * (cut.leftToRight * inLeft + cut.rightToRight * inRight);
    }

    left = outLeft;
    right = outRight;

    if (++position >= cut.size)
    {
        ++current;
        position = 0;
    }
}

} // namespace livecut::engine
