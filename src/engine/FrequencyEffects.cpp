// SPDX-License-Identifier: GPL-3.0-or-later

#include "engine/FrequencyEffects.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numbers>

namespace livecut::engine
{
namespace
{
constexpr double pi = std::numbers::pi;

double agm (double arithmetic, double geometric)
{
    for (int iteration = 0; iteration < 16; ++iteration)
    {
        const auto next = std::sqrt (arithmetic * geometric);
        arithmetic = .5 * (arithmetic + geometric);
        geometric = next;
    }
    return arithmetic;
}
} // namespace

void FrequencyEffects::Ramp::setTarget (double value, std::uint32_t samples)
{
    const auto change = value - target;
    if (change == 0.0)
        return;
    target = value;
    remaining = samples;
    increment = (target - current) / samples;
}

double FrequencyEffects::Ramp::next()
{
    if (remaining > 0)
        current = --remaining == 0 ? target : current + increment;
    return current;
}

double FrequencyEffects::Allpass::process (double input, double coefficient)
{
    const auto value = coefficient * (input + y2) - x2;
    const auto output = std::abs (value) < 1.0e-30 ? 0.0 : value;
    x2 = x1;
    x1 = input;
    y2 = y1;
    y1 = output;
    return output;
}

std::array<double, 2> FrequencyEffects::Quadrature::process (
    double input, const std::array<double, coefficientCount>& coefficients)
{
    auto real = input, imag = input;
    for (std::size_t section = 0; section < coefficientCount; ++section)
        if ((section & 1U) == 0)
            real = sections[section].process (real, coefficients[section]);
        else
            imag = sections[section].process (imag, coefficients[section]);
    const auto previous = delayed;
    delayed = imag;
    return { real, previous };
}

void FrequencyEffects::prepare (double newSampleRate)
{
    // Engine timing fixtures use 1 kHz; the processor supports 8–384 kHz.
    assert (std::isfinite (newSampleRate) && newSampleRate >= 1000.0 && newSampleRate <= 384000.0);
    sampleRate = newSampleRate;
    rampSamples = static_cast<std::uint32_t> (std::ceil (.002 * sampleRate));

    // Elliptic two-path allpass design: Laurent de Soras's HIIR designer,
    // https://github.com/LostRobotMusic/hiir/blob/main/PolyphaseIir2Designer.h
    // Use the accurate elliptic nome/AGM identities (NIST DLMF 22.2.E1,
    // 19.8.E5), rather than a short nome expansion at high sample rates.
    // Absolute 20 Hz transitions, fixed eight sections per path. Coefficients
    // are computed only at prepare, never during sample processing.
    const auto k = std::pow (std::tan ((1.0 - 80.0 / sampleRate) * pi / 4.0), 2.0);
    const auto q = std::exp (-pi * agm (1.0, std::sqrt (1.0 - k * k)) / agm (1.0, k));
    constexpr auto order = static_cast<int> (2 * coefficientCount + 1);
    for (std::size_t index = 0; index < coefficientCount; ++index)
    {
        const auto c = static_cast<double> (index + 1);
        double numerator = 0.0, denominator = .5;
        for (int term = 0; term < 32; ++term)
        {
            const auto sign = (term & 1) == 0 ? 1.0 : -1.0;
            numerator += sign * std::pow (q, term * (term + 1)) * std::sin ((2 * term + 1) * c * pi / order);
            if (term > 0)
                denominator += sign * std::pow (q, term * term) * std::cos (2 * term * c * pi / order);
        }
        const auto ratio = numerator * std::pow (q, .25) / denominator;
        const auto square = ratio * ratio;
        const auto x = std::sqrt ((1.0 - square * k) * (1.0 - square / k)) / (1.0 + square);
        coefficients[index] = (1.0 - x) / (1.0 + x);
        assert (std::isfinite (coefficients[index]) && coefficients[index] > 0.0 && coefficients[index] < 1.0);
    }
    reset();
}

void FrequencyEffects::reset()
{
    channels = {};
    frequency = wet = mode = {};
    phase = 0.0;
}

void FrequencyEffects::setParameters (const FrequencyParameters& next)
{
    assert (std::isfinite (next.chance) && std::isfinite (next.mix)
            && std::isfinite (next.rateHz) && std::isfinite (next.envelopeHz) && std::isfinite (next.repeatHz));
    assert (next.mode == FrequencyParameters::Mode::ring || next.mode == FrequencyParameters::Mode::frequencyShift);
    parameters = next;
    parameters.chance = std::clamp (parameters.chance, 0.0f, 1.0f);
    parameters.mix = std::clamp (parameters.mix, 0.0f, 1.0f);
    parameters.rateHz = std::clamp (parameters.rateHz, -2000.0f, 2000.0f);
    parameters.envelopeHz = std::clamp (parameters.envelopeHz, -2000.0f, 2000.0f);
    parameters.repeatHz = std::clamp (parameters.repeatHz, -2000.0f, 2000.0f);
}

void FrequencyEffects::process (float& left, float& right, double envelope,
                                double repeatProgress, bool selected)
{
    assert (std::isfinite (left) && std::isfinite (right)
            && std::isfinite (envelope) && std::isfinite (repeatProgress));
    const auto target = std::clamp (parameters.rateHz + parameters.envelopeHz * std::clamp (envelope, 0.0, 1.0)
                                   + parameters.repeatHz * std::clamp (repeatProgress, 0.0, 1.0), -2000.0, 2000.0);
    const auto shifted = parameters.mode == FrequencyParameters::Mode::frequencyShift;
    frequency.setTarget (shifted ? target : std::abs (target), rampSamples);
    mode.setTarget (shifted ? 1.0 : 0.0, rampSamples);
    // Zero shift through the allpass pair would retain its phase coloration.
    // Explicitly fade to exact dry at zero Hz, including a settled carrier.
    wet.setTarget (parameters.enabled && selected && target != 0.0 ? parameters.mix : 0.0, rampSamples);
    const auto hz = frequency.next(), mix = wet.next(), shiftMix = mode.next();
    const auto cosine = std::cos (phase), sine = std::sin (phase);
    const std::array<float*, 2> samples { &left, &right };
    for (std::size_t channel = 0; channel < samples.size(); ++channel)
    {
        const auto dry = static_cast<double> (*samples[channel]);
        const auto pair = channels[channel].process (dry, coefficients);
        const auto ring = dry * cosine;
        const auto translated = pair[0] * cosine - pair[1] * sine;
        const auto processed = ring + shiftMix * (translated - ring);
        if (mix > 0.0)
            *samples[channel] = static_cast<float> (dry + mix * (processed - dry));
    }
    // Keep carrier and quadrature warm while disabled or chance-false. No
    // cut-local oscillator reset or legacy random-stream consumption occurs.
    phase += 2.0 * pi * hz / sampleRate;
    if (phase >= pi)
        phase -= 2.0 * pi;
    else if (phase < -pi)
        phase += 2.0 * pi;
}
} // namespace livecut::engine
