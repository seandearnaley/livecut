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
        // band and sweep are engine-only, smoothed and level-held. band: a
        // high-pass at bandLowHz into a low-pass at bandHighHz. sweep: a
        // Butterworth high-pass at 30 Hz into a low-pass at cutoffHz.
        enum class Type { lowPass, bandPass, highPass, band, sweep };
        bool enabled = false;
        Type type = Type::lowPass;
        float chance = 1.0f;
        float cutoffHz = 12000.0f; // base [30, 18000]; modulated cutoff [30, 0.45 * sample rate]
        float q = 0.707f; // [0.5, 8]
        float envelopeOctaves = 0.0f; // [-6, 6]
        float repeatOctaves = 0.0f; // [-4, 4]
        float bandLowHz = 30.0f; // [30, 18000], band only
        float bandHighHz = 18000.0f; // [30, 18000], band only
        // A sweep's tail, struck by the cut after each roll: its RMS at the
        // strike as a fraction of the roll's, and its decay time to -60 dB.
        float tailLevel = 0.0f; // [0, 1], sweep only
        float tailDecayMs = 600.0f; // [50, 2000], sweep only
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

namespace detail
{
// The pink-noise power a band keeps, with its edges widthOctaves apart and
// resonance q, from the analogue high-pass/low-pass pair the band's SVFs
// approximate. In octaves: a flat band that wide keeps the same power. It is
// scale-invariant, so it depends only on the width and q. The first
// ImpactEffects::prepare() tabulates it; this interpolates that table.
inline constexpr double bandOpenOctaves = 9.23, bandOpenQ = 0.707; // 30 Hz-18 kHz, flat
inline constexpr double bandPowerStep = 1.0 / 16.0, bandPowerMargin = 6.0; // octaves
// For tests: the band's makeup reads the table directly.
double bandPinkPower (double widthOctaves, double q);
// For tests: the same integral, computed directly at any step.
double integrateBandPinkPower (double widthOctaves, double q, double stepOctaves);
// A sweep's high-pass is fixed: 30 Hz, Butterworth.
inline constexpr double sweepLowHz = 30.0, sweepLowQ = 0.707;
// The same for a sweep, whose low-pass edge is widthOctaves above its
// high-pass, with resonance q. The high-pass keeps sweepLowQ.
double sweepPinkPower (double widthOctaves, double q);
double integrateSweepPinkPower (double widthOctaves, double q, double stepOctaves);
// The tail's note for a cutoff: the nearest equal-tempered semitone, with A4
// at 440 Hz, moved by whole octaves into C4 to B5.
double tailNoteHz (double cutoffHz);
} // namespace detail

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
        // The band's effective edges and linear level-hold gain on its latest
        // sample; 0 Hz and a gain of 1 after a reset, a silent wet mix or a
        // switch into band, until the band next runs.
        double bandLowHz = 0.0, bandHighHz = 0.0;
        double levelHoldGain = 1.0;
        // A sweep's tail: its note, its amplitude before the duck, and the
        // duck's gain; 0 Hz, 0 and 1 while it is silent or reset.
        double tailHz = 0.0, tailAmplitude = 0.0, tailDuck = 1.0;
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

    // A sweep's tail: one mono sine, struck by the cut after each roll at a
    // level measured from the roll, tuned to the cutoff's note, and ducked at
    // every cut. Plain values only, so ImpactEffects stays trivially copyable.
    struct Tail
    {
        // Over the current block: the low-pass's output before the makeup,
        // and the filter's input, the roll as cut.
        double rollEnergy = 0.0, rollInputEnergy = 0.0;
        std::uint64_t rollSamples = 0;
        bool rollEnding = false; // the current cut is a roll's last
        double amplitude = 0.0, peak = 0.0, attackStep = 0.0, decay = 1.0;
        std::uint32_t attackRemaining = 0;
        double hz = 0.0, phase = 0.0, increment = 0.0;
        double duck = 1.0, duckStep = 0.0;
        std::uint32_t duckRemaining = 0;
    };

    double nextEnvelope();
    void adoptFilter();
    void startTail (const CutIdentity&);
    double nextTail();
    void resetBand();
    void processFilter (double& left, double& right, double envelope, double repeatProgress);
    void processBand (double& left, double& right, double envelope, double repeatProgress, double mix);
    void processDrive (double& left, double& right, double envelope);
    void processTransient (double& left, double& right);

    // The filter runs parameters.filter. A band switched off, or a type change
    // into or out of a band, while it sounds holds it: it adopts the requested
    // one only once its wet has ramped out.
    ImpactParameters parameters;
    ImpactParameters::Filter requestedFilter;
    // While held, the next cut detaches the filter from the shared envelope:
    // it decays on at its held rate from where it was, and keeps its repeat.
    bool filterHeld = false, filterDetached = false;
    double filterEnvelope = 0.0, filterRepeatProgress = 0.0, heldDecayMultiplier = 0.0;
    Diagnostics state;
    double sampleRate = 48000.0;
    std::uint32_t rampSamples = 96;
    double dcPole = 0.0;
    double decayMultiplier = 0.0;
    double adEnvelope = 0.0;
    std::uint64_t attackSamples = 0, attackPosition = 0;
    std::array<Ramp, 3> wet;
    std::array<FilterState, 2> filterState;
    // The band's high-pass runs on filterState and its low-pass on bandState.
    // Its edges (log2 Hz) and q glide toward targets latched by setParameters.
    std::array<FilterState, 2> bandState;
    double bandLowTarget = 0.0, bandHighTarget = 0.0;
    double bandLow = 0.0, bandHigh = 0.0, bandQ = 0.0;
    bool bandPrimed = false;
    double smoothingPole = 0.0;
    Tail tail;
    double tailRecoveryPole = 0.0;
    std::array<DriveState, 2> driveState;
    double fastEnvelope = 0.0, slowEnvelope = 0.0;
    double transientAttack = 0.0, transientRelease = 0.0;
    double transientSlowAttack = 0.0, transientSlowRelease = 0.0;
};

} // namespace livecut::engine
