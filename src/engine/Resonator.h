// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace livecut::engine
{
struct ResonatorParameters
{
    bool enabled = false;
    float rootHz = 110.0f, decayMs = 500.0f, excitationDb = 0.0f, mix = 1.0f;
    std::array<int, 4> intervals { 0, 7, 12, 19 };
    std::array<float, 4> weights { 1, 1, 1, 1 };
};

class Resonator
{
public:
    enum class State { ready, invalidWeights, invalidParameters, unsupportedRate };
    struct Diagnostics
    {
        State state = State::ready;
        bool retuning = false, pending = false;
        std::uint32_t retuneFramesRemaining = 0;
        std::array<double, 4> frequencies {}, weights {};
        double poleRadius = 0.0, impulseAbsoluteSumBound = 0.0, wet = 0.0;
    };

    // Product 8..384k supports every advertised tuning. The 1k engine timing
    // fixture is explicit: every requested voice must remain <=0.4*rate.
    // Returns rate support; stored invalid parameters remain explicit in diagnostics.
    bool prepare (double sampleRate);
    void reset();
    bool setParameters (const ResonatorParameters&);
    void process (float& left, float& right);
    const Diagnostics& diagnostics() const { return status; }

private:
    struct Voice
    {
        double a1 = 0.0, a2 = 0.0, b0 = 0.0, weight = 0.0;
        std::array<double, 2> y1 {}, y2 {};
    };
    struct Bank
    {
        std::array<Voice, 4> voices;
        ResonatorParameters parameters;
        double radius = 0.0, excitation = 1.0;
    };
    State validate (const ResonatorParameters&) const;
    void configure (Bank&, const ResonatorParameters&);
    void beginRetune (const ResonatorParameters&);
    void updateDiagnostics();
    static std::array<double, 2> processBank (Bank&, double left, double right);

    std::array<Bank, 2> banks;
    std::optional<ResonatorParameters> pending;
    ResonatorParameters requested;
    Diagnostics status;
    double rate = 48000.0, wet = 0.0, wetTarget = 0.0, wetStep = 0.0;
    std::uint32_t controlFrames = 96, controlRemaining = 0;
    std::uint32_t fadeFrames = 2400, fadePosition = 0;
    std::size_t active = 0;
    bool prepared = false, fading = false, validBank = false;
};
} // namespace livecut::engine
