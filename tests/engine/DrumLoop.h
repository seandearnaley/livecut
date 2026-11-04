// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <vector>

namespace livecut::engine::testing
{
// A 120 bpm loop at 48 kHz, after the review's: a kick falling 150 to 50 Hz on
// 1, the and of 2, and 3, a snare (noise and 180 Hz) on 2 and 4, hats on 8ths,
// and a sustained bass line, two notes a bar, the second a fourth above the first.
struct DrumLoop
{
    enum class Bass { sine, square };
    std::vector<float> samples;

    DrumLoop (double bassHz, Bass bass)
    {
        constexpr double rate = 48000.0, pi = 3.141592653589793;
        const int bar = static_cast<int> (2.0 * rate);
        samples.assign (static_cast<std::size_t> (bar), 0.0f);
        std::uint32_t state = 99;
        const auto noise = [&state]
        {
            state = state * 1664525u + 1013904223u;
            return static_cast<double> (static_cast<std::int32_t> (state)) / 2147483648.0;
        };
        const auto add = [&] (double beat, double seconds, const std::function<double (double)>& voice)
        {
            const auto start = static_cast<int> (beat * rate / 2.0);
            for (int i = 0; i < static_cast<int> (seconds * rate); ++i)
                samples[static_cast<std::size_t> ((start + i) % bar)] += static_cast<float> (voice (i / rate));
        };
        for (const auto beat : { 0.0, 1.5, 2.0 })
            add (beat, 0.6, [] (double t)
                 { return 0.9 * std::exp (-t / 0.18) * std::sin (2 * pi * (50.0 * t + 3.0 * (1 - std::exp (-t / 0.03)))); });
        for (const auto beat : { 1.0, 3.0 })
            add (beat, 0.4, [&noise] (double t)
                 { return 0.45 * std::exp (-t / 0.10) * noise() + 0.3 * std::exp (-t / 0.06) * std::sin (2 * pi * 180 * t); });
        for (int k = 0; k < 8; ++k)
        {
            double previous = 0.0;
            add (k / 2.0, 0.12, [&] (double t)
                 {
                     const auto w = noise();
                     const auto hat = 0.18 * std::exp (-t / 0.025) * (w - previous);
                     previous = w;
                     return hat;
                 });
        }
        for (int i = 0; i < bar; ++i)
        {
            const auto t = i / rate, within = std::fmod (t, 1.0);
            const auto hz = i < bar / 2 ? bassHz : bassHz * 4.0 / 3.0;
            const auto envelope = std::min (1.0, within / 0.01) * std::min (1.0, (1.0 - within) / 0.01);
            const auto wave = std::sin (2 * pi * hz * t);
            samples[static_cast<std::size_t> (i)] += static_cast<float> (0.6 * envelope * (bass == Bass::square ? (wave >= 0 ? 0.8 : -0.8) : wave));
        }
    }
};

} // namespace livecut::engine::testing
