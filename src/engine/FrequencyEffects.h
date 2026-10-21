// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace livecut::engine
{
struct FrequencyParameters
{
    enum class Mode { ring, frequencyShift };
    bool enabled = false;
    Mode mode = Mode::ring;
    float chance = 1.0f, mix = 1.0f;
    float rateHz = 100.0f, envelopeHz = 0.0f, repeatHz = 0.0f;
};

// Ring retains both sidebands; frequencyShift approximates single-sideband
// translation with rate-designed IIR quadrature. No fixed buffered latency;
// phase/group delay are frequency dependent. Content crossing Nyquist aliases.
// DC is outside the quadrature band and can gain 3 dB when shifted. Neither
// mode is a limiter. Settled zero Hz/mix/selection is exact dry after a 2 ms ramp.
// Finite inputs/controls are required, matching the other engine effects.
class FrequencyEffects
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParameters (const FrequencyParameters&);
    // Chance is latched by Cutter's independent tagged cut identity. This
    // module consumes its decision, the existing AD envelope and repeat progress.
    void process (float& left, float& right, double envelope = 0.0,
                  double repeatProgress = 0.0, bool selected = true);

private:
    struct Ramp
    {
        double current = 0.0, target = 0.0, increment = 0.0;
        std::uint32_t remaining = 0;
        void setTarget (double value, std::uint32_t samples);
        double next();
    };
    struct Allpass
    {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;
        double process (double input, double coefficient);
    };
    static constexpr std::size_t coefficientCount = 16;
    struct Quadrature
    {
        std::array<Allpass, coefficientCount> sections;
        double delayed = 0.0;
        std::array<double, 2> process (double input, const std::array<double, coefficientCount>&);
    };
    FrequencyParameters parameters;
    std::array<double, coefficientCount> coefficients {};
    std::array<Quadrature, 2> channels;
    Ramp frequency, wet, mode;
    double sampleRate = 48000.0, phase = 0.0;
    std::uint32_t rampSamples = 96;
};
} // namespace livecut::engine
