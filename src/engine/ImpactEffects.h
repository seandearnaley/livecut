// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace livecut::engine
{

struct CutIdentity
{
    std::uint32_t seed = 0;
    std::int64_t phraseBar = 0;
    double blockStartUnits = 0.0;
    double cutStartUnits = 0.0;
    std::size_t cutIndex = 0;
    std::size_t cutCount = 1;
};

struct ImpactParameters
{
    struct Filter
    {
        enum class Type { lowPass, bandPass, highPass };
        bool enabled = false;
        Type type = Type::lowPass;
        float chance = 1.0f;
        float cutoffHz = 12000.0f; // base [30, 18000]; modulated cutoff [30, 0.45 * sample rate]
        float q = 0.707f; // [0.5, 8]
        float envelopeOctaves = 0.0f; // [-6, 6]
        float repeatOctaves = 0.0f; // [-4, 4]
        float mix = 1.0f;
    } filter;

    struct Drive
    {
        enum class Mode { drive, wavefold };
        bool enabled = false;
        Mode mode = Mode::drive;
        float chance = 1.0f;
        float driveDb = 6.0f; // total drive clamps [0, 30]; enabled 0 dB is still nonlinear
        float envelopeDb = 0.0f; // [-24, 24]
        float repeatDb = 0.0f; // [-24, 24]
        float mix = 1.0f;
    } drive;

    struct Transient
    {
        bool enabled = false;
        float chance = 1.0f;
        float attackDb = 0.0f; // [-18, 18]
        float sustainDb = 0.0f; // [-18, 18]
        float timeMs = 40.0f; // [10, 200]
        float mix = 1.0f;
    } transient;

    float attackMs = 1.0f; // [0, 25]
    float decayMs = 80.0f; // [5, 1000], decay time to -60 dB
};

// JUCE-free, allocation-free stereo processing. Production rates are 8–384 kHz;
// the engine also accepts 1 kHz for the existing deterministic timing fixtures.
// Order: TPT state-variable filter, first-order ADAA drive, linked transient dynamics.
// ADAA has a half-sample group delay and high-frequency droop while active; there
// is no lookahead or integer delay buffer, and alias reduction is not alias removal.
class ImpactEffects
{
public:
    struct Diagnostics
    {
        std::uint64_t cutsStarted = 0;
        double envelope = 0.0;
        double repeatProgress = 0.0;
        bool filterActive = false;
        bool driveActive = false;
        bool transientActive = false;
    };

    void prepare (double sampleRate);
    void reset();
    void setParameters (const ImpactParameters&);
    void startCut (const CutIdentity&);
    void process (float& left, float& right);

    const Diagnostics& diagnostics() const { return state; }

private:
    struct Ramp
    {
        double current = 0.0, target = 0.0, increment = 0.0;
        std::uint32_t remaining = 0;
        void setTarget (double value, std::uint32_t samples);
        double next();
    };
    struct FilterState { double ic1 = 0.0, ic2 = 0.0; };
    struct DriveState
    {
        double previous = 0.0, previousPrimitive = 0.0;
        double dcInput = 0.0, dcOutput = 0.0;
        bool primed = false;
    };

    double nextEnvelope();
    void processFilter (double& left, double& right, double envelope);
    void processDrive (double& left, double& right, double envelope);
    void processTransient (double& left, double& right);

    ImpactParameters parameters;
    Diagnostics state;
    double sampleRate = 48000.0;
    std::uint32_t rampSamples = 96;
    double dcPole = 0.0;
    double decayMultiplier = 0.0;
    double adEnvelope = 0.0;
    std::uint64_t attackSamples = 0, attackPosition = 0;
    std::array<Ramp, 3> wet;
    std::array<FilterState, 2> filterState;
    std::array<DriveState, 2> driveState;
    double fastEnvelope = 0.0, slowEnvelope = 0.0;
    double transientAttack = 0.0, transientRelease = 0.0;
    double transientSlowAttack = 0.0, transientSlowRelease = 0.0;
};

} // namespace livecut::engine
