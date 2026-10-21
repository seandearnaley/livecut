// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/AudioHistory.h"
#include "engine/Cut.h"
#include "engine/ImpactEffects.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace livecut::engine
{

// Appended family override: the original four-value Procedure stays unchanged.
enum class FractureProcedure { classic, grainCut, tapeCut };

struct FractureParameters
{
    FractureProcedure procedure = FractureProcedure::classic;
    float activity = 0.5f;

    struct Grain
    {
        float sizeMs = 40.0f;                 // [10,160]
        float density = 2.0f;                 // [0.5,4]
        float positionSpreadMs = 40.0f;       // [0,250]
        float pitchSpreadCents = 300.0f;      // [0,1200]
        float panSpread = 0.3f;               // [0,1]
    } grain;

    struct Tape
    {
        enum class Curve { linear, smooth };
        enum class Cycle { halfBeat, oneBeat, twoBeats, fourBeats };
        float startSpeed = 1.0f;              // [0,1]
        float endSpeed = 0.0f;                // [0,1]
        Curve curve = Curve::smooth;
        Cycle cycle = Cycle::oneBeat;
        bool alternate = false;
    } tape;
};

enum class FractureState
{
    off, waiting, playing, ended, sourceLost, transformBudgetExhausted,
    capacityError, contextUnavailable
};

// Audio-thread snapshot copied into the processor's display state; never owns audio.
struct FractureDiagnostics
{
    FractureProcedure family = FractureProcedure::classic;
    FractureState state = FractureState::off;
    SourceRegion source {};
    std::uint64_t transportEpoch = 0;
    std::uint64_t cutsAdmitted = 0;
    std::uint64_t grainsLaunched = 0;
    std::size_t activeVoices = 0;
    double tapeSpeed = 0.0;
};

// Exact discrete integral of the chosen speed curve, including zero speed.
class TapeTrajectory
{
public:
    void start (double from, double to, int frames, FractureParameters::Tape::Curve);
    double speed (int frame) const;
    double position (int frame) const;
    std::int64_t sourceFrames() const;
private:
    double from = 1.0, to = 0.0;
    int frames = 1;
    FractureParameters::Tape::Curve curve = FractureParameters::Tape::Curve::smooth;
};

class LivePlayer;

// Prepared finite-support interpolation for Grain only. Classic is untouched.
class GrainResampler
{
public:
    static constexpr int phaseCount = 256, bucketCount = 25, maximumTaps = 192;
    void prepare();
    static int bucketFor (double rate);
    static int tapsFor (double rate);
    static int leftGuard (double rate) { return tapsFor (rate) / 2 - 1; }
    static int rightGuard (double rate) { return tapsFor (rate) / 2; }
    struct Kernel
    {
        const float* coefficients = nullptr;
        int taps = 0;
        int leftGuard = 0;
    };
    Kernel kernelFor (double rate) const;
    std::optional<StereoFrame> read (const LivePlayer&, SourceRegion, double position, double rate) const;
    std::optional<StereoFrame> read (const LivePlayer&, SourceRegion, double position, Kernel) const;
    std::span<const float> coefficientsFor (double rate, int phase) const;
private:
    std::array<std::size_t,bucketCount> offsets {};
    std::vector<float> coefficients;
};

struct FractureCutContext
{
    CutIdentity identity;
    Cut cut;
    SourceRegion source {};
    std::uint64_t transportEpoch = 0;
    std::uint64_t ordinal = 0; // new-family outer cuts since render reset, across phrases
    int outputFrames = 0;
    int fadeFrames = 1;
    bool reverse = false;
    bool completeTapeSource = false;
};

// Owns eight Grain voices and one Tape trajectory; no processing allocations.
class FractureReader
{
public:
    void prepare (double sampleRate);
    void reset();
    void setParameters (const FractureParameters&);
    static std::int64_t grainPoolFrames (const FractureParameters::Grain&, double cents, double sampleRate);
    void startCut (const FractureCutContext&);
    void fail (FractureState);
    void retimeTape (int age, int remainingFrames);
    // Returns true exactly once when the source is first admitted for this cut.
    bool process (const LivePlayer&, int age, int soundingFrames, float& left, float& right);
    const FractureDiagnostics& diagnostics() const { return state; }
private:
    struct Voice
    {
        bool active = false;
        int age = 0;
        double sourceOffset = 0.0, rate = 1.0;
        float ll = 1, lr = 0, rl = 0, rr = 1;
        GrainResampler::Kernel kernel;
    };
    bool launch();
    double sampleRate = 48000;
    FractureParameters parameters;
    FractureCutContext context;
    FractureDiagnostics state;
    GrainResampler resampler;
    std::array<Voice,8> voices {};
    TapeTrajectory tape;
    int grainFrames = 2, firstReady = -1, tapeSegmentAge = 0;
    std::uint64_t nextGrain = 0;
    double tapeBase = 0.0;
    bool admitted = false;
};

// Independent tagged selection shared by new-family planning and grain voices.
double fractureDraw (const CutIdentity&, std::uint64_t tag, std::uint64_t ordinal = 0);

} // namespace livecut::engine
