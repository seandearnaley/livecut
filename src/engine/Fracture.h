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
        // Appended for the pad's motions: quadratic is the stop's 1 - u^2 brake, and
        // catchUp the spin-up that winds from still back onto the track (from 0 to 1 only).
        enum class Curve { linear, smooth, quadratic, catchUp };
        enum class Cycle { halfBeat, oneBeat, twoBeats, fourBeats };
        // cycle is the Classic page's: every cut a stop of `cycle` beats. stops is
        // the pad's: the track plays, and stops brake onto the grid (layOutTapeStops).
        enum class Layout { cycle, stops };
        float startSpeed = 1.0f;              // [0,1]
        float endSpeed = 0.0f;                // [0,1]
        Curve curve = Curve::smooth;
        Cycle cycle = Cycle::oneBeat;
        bool alternate = false;
        Layout layout = Layout::cycle;
        float stopBeats = 1.0f;               // (0,16]: how long a stop is asked to be
        float stopDensity = 0.0f;             // [0,4]: 0 stops every four bars, 4 every beat
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

// Exact discrete integral of the chosen speed curve, including zero speed. A
// catch-up is the closed form instead: over T frames its position is
// T (3u^2 - 3u^3 + u^4) at u = n / T, never ahead of frame n, and exactly T at
// the end, where its speed is 1 with no acceleration left. It peaks at 1.25.
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

// The speeds a tape cut runs between. A cycle cut takes the page's, swapped on
// odd ordinals with Alternate; a motion takes its own.
struct TapeCurve
{
    double from = 1.0, to = 0.0;
    FractureParameters::Tape::Curve curve = FractureParameters::Tape::Curve::smooth;
};
TapeCurve tapeCurveFor (const FractureParameters::Tape&, TapeMotion, std::uint64_t ordinal);

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

// A tape motion cut off by a restart plays on as a tail while the cutter fades
// it out under what follows, so a switch away from Tape never splices a slowed
// tape onto the input. Plain data, so taking one never allocates.
struct TapeTail
{
    SourceRegion source {};
    TapeTrajectory trajectory;
    double base = 0.0;
    int age = 0, segmentAge = 0;
    float ll = 1, lr = 0, rl = 0, rr = 1;
    // Renders the next frame of the motion; false once its source is gone.
    bool next (const LivePlayer&, float& left, float& right);
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
    // The sounding motion from its next frame on, if a pad motion is playing.
    std::optional<TapeTail> tail() const;
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
    int grainFrames = 2, firstReady = -1, tapeSegmentAge = 0, nextAge = 0;
    std::uint64_t nextGrain = 0;
    double tapeBase = 0.0;
    bool admitted = false;
};

// Independent tagged selection shared by new-family planning and grain voices.
double fractureDraw (const CutIdentity&, std::uint64_t tag, std::uint64_t ordinal = 0);

// The pad's Tape. Stops end on whole beats inside a bar and on bar lines, and
// the cut after each one spins up for this many beats (a 32nd note).
inline constexpr double tapeSpinUpBeats = 0.125;

// How rarely a stop may end at `beat` of host `bar`, where `beat` is a whole beat
// inside the bar or the bar line (`beatsPerBar`): 0 at the end of the last bar of
// every four (counted from the host's bar 0), 1 at any other even bar's end, 2 at
// any other bar line, 3 on an even beat inside the bar, 4 on any other beat.
int tapeStopLevel (std::int64_t bar, double beat, double beatsPerBar);

// Whether a stop ends there: a fixed draw from the seed, the bar and the beat
// against clamp (density + 1 - level, 0, 1). So raising the density only adds
// stops, and the same spot in the same bar always stops the same way.
bool tapeStopEnds (std::uint32_t seed, double density, std::int64_t bar, double beat, double beatsPerBar);

// How long a stop ending there is: min (stopBeats, the bar, half the gap back to
// the latest stop ending in the two bars before it, or a whole bar if none).
double tapeStopBeats (const FractureParameters::Tape&, std::uint32_t seed, std::int64_t bar, double beat, double beatsPerBar);

// Lays out host `bar` from `fromBeat` to its line as play, stop and spin-up
// cuts of `unitsPerBeat` units, summing exactly to `blockUnits`. Opens with a
// spin-up when the block before ended in a stop. A stop that would begin before
// what's already laid out (after a mid-bar engage) is skipped, never shortened.
// Returns whether the bar ends in a stop, so the next block opens with its spin-up.
// Allocates nothing when `cuts` has maxCutsPerBlock reserved.
bool layOutTapeStops (const FractureParameters::Tape&, std::uint32_t seed, std::int64_t bar, double fromBeat,
                      double beatsPerBar, double unitsPerBeat, double blockUnits, bool spinUpFirst,
                      std::vector<Cut>& cuts);

} // namespace livecut::engine
