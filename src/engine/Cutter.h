// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/CutProcedures.h"
#include "engine/Effects.h"
#include "engine/ImpactEffects.h"
#include "engine/Fracture.h"
#include "engine/FrequencyEffects.h"
#include "engine/RouteRack.h"
#include "engine/LivePlayer.h"
#include "engine/AttackCandidates.h"

#include <cstdint>
#include <array>
#include <span>
#include <vector>

namespace livecut::engine
{

enum class Procedure
{
    cutProc11,
    warpCut,
    sqPusher,
    bounce,
    rush, // the pad's Rush and Loops only: the procedure parameter keeps its four choices
    loops
};

struct Timing
{
    double bpm = 120.0;
    double beatsPerBar = 4.0; // quarter notes per bar: 4 * numerator / denominator
};

// The host's musical position at the first sample of a buffer.
struct Position
{
    std::int64_t bar = 0;
    double beatsIntoBar = 0.0; // quarter notes since that bar began
};

// Cuts a live stereo signal in time with the host. Phrases start on bar lines
// and last a whole number of bars; each block within a phrase is chosen by the
// active cut procedure and played by a LivePlayer.
class Cutter
{
public:
    // Cuts never last longer than a bar, so the recording buffer holds a bar
    // this long. Slower bars are passed through untouched.
    static constexpr double maxBarSeconds = 8.0;

    Cutter();

    // Allocates everything the cutter needs. Call before processing, never on the audio thread.
    void prepare (double sampleRate, std::int64_t latencyFrames = 0);

    // Structural changes land on the next bar line.
    void setProcedure (Procedure procedure);
    void setUnitsPerBar (int unitsPerBar);

    // Requests are consumed at the start of the next rendered sample.
    // Idle passes input through; leaving idle resumes on a unit boundary.
    void setIdle (bool idle);
    // While cutting, restart on the next boundary of the current subdivision.
    void restartAtNextUnit();

    // Takes effect from the next block.
    void setParameters (const CutParameters& parameters);

    // Take effect from the next block.
    void setCrusher (const CrusherParameters& parameters);
    void setComb (const CombParameters& parameters);
    void setImpact (const ImpactParameters& parameters);
    void setFracture (const FractureParameters& parameters);
    void setFrequency (const FrequencyParameters& parameters);
    void setResonator (const ResonatorParameters& parameters);
    void setEffectRoute (EffectRoute route);

    // Takes effect from the next phrase.
    void setSeed (std::uint32_t seed);
    void setSwing (double percent);
    void setAccent (float amount);

    struct TransformParameters { bool attackAlign = false; double thresholdDb = 6.0, reverseChance = 0.0; };
    struct TransportBounds { std::uint64_t epoch; std::int64_t first; std::optional<std::int64_t> end; };
    struct SourceContext
    {
        virtual ~SourceContext() = default;
        // Queried at planning/activation, never cached across future input epochs.
        virtual std::optional<TransportBounds> sourceBoundsAt (std::int64_t renderFrame) const = 0;
    };
    void setTransforms (TransformParameters);
    void setTransformSource (const OnsetDetector& detector, const SourceContext& source)
    { onsetDetector = &detector; sourceContext = &source; }
    enum class TransformReason { off, awaitingCut, searchPending, noAttack, sourceIncomplete, tooLong,
                                 sourceLost, applied, eligibleForward, silentCut, analysisError };
    struct TransformStats
    {
        std::uint64_t eligible = 0, attackAligned = 0, reversed = 0, ineligible = 0;
        std::int64_t lastSourceStart = -1;
        TransformReason reason = TransformReason::off;
    };
    const TransformStats& transformStats() const { return transforms; }

    void setFadeMilliseconds (double milliseconds);

    // Forgets the current phrase. Until the next unit boundary the input
    // passes through; cutting then resumes in the current bar.
    void reset();
    // Drops render/planner state and a pending restart, preserving raw history and idle.
    void resetRendering();

    // Synchronous sample events; implementations must never allocate or wait.
    // phraseStarted precedes that phrase's first output sample. fullStart is
    // false for the initial partial phrase after a mid-bar start/seek.
    struct Observer
    {
        virtual ~Observer() = default;
        virtual void barStarted (int, std::int64_t) {}
        virtual void discontinuity (int sampleOffset) = 0;
        // Exclusive end, including numSamples when a phrase ends exactly at
        // the host buffer boundary. Does not generate the next phrase's cuts.
        virtual void phraseCompleted (int sampleOffset) = 0;
        virtual void phraseStarted (int sampleOffset, bool fullStart) = 0;
        virtual void processOutput (int sampleOffset, float dryLeft, float dryRight, float& left, float& right) = 0;
    };

    // Cuts a stereo buffer in place. `position` is the host's position at its first sample.
    void process (float* left, float* right, int numSamples, const Timing& timing, const Position& position, Observer* observer = nullptr);

    // One physical input clock, independent of the renderer's original contexts.
    bool ingestInputFrame (std::int64_t frame, StereoFrame raw);
    std::int64_t nextInputFrame() const { return livePlayer.sourceForNextFrame().firstFrame; }
    std::optional<StereoFrame> rawInputFrame (std::int64_t frame) const { return livePlayer.rawFrame (frame); }
    // A context is an ORIGINAL host segment, possibly split over output buffers.
    void beginRenderContext (const Timing&, const Position&, std::int64_t firstFrame, int sampleOffset, Observer*);
    void renderOne (std::int64_t frame, float& left, float& right, int sampleOffset, Observer*);
    void endRenderContext (int sampleOffset, Observer*);

    // Next sample's position, including a pending bar wrap; valid within a render context.
    Position renderPosition() const;

    bool isCutting() const { return cutting && hasActivePlan; }
    bool isTimingSupported() const { return timingSupported; }
    Procedure activeProcedure() const { return procedure; }
    int activeUnitsPerBar() const { return unitsPerBar; }
    std::int64_t phraseStartBar() const { return phraseBar; }
    double blockStartUnits() const { return hasActivePlan ? activePlan.start : blockStart; } // within the phrase
    double blockEndUnits() const { return hasActivePlan ? activePlan.end : blockEnd; }
    int phraseBarCount() const { return phraseBars; }
    double positionInPhrase() const { return phrasePosition; } // units, at the end of the last buffer
    std::span<const Cut> currentCuts() const { return { activePlan.cuts.data(), hasActivePlan ? activePlan.count : 0 }; }
    SourceRegion activeSourceRegion() const { return activePlan.source; }
    std::size_t maximumQueuedPlans() const { return maximumQueued; }
    enum class Error { none, planOverflow, sourceUnavailable, renderDiscontinuity };
    Error error() const { return processingError; }
    double swingPercent() const { return swing; }
    const LivePlayer& player() const { return livePlayer; }
    const BitCrusher& crusher() const { return bitCrusher; }
    const Comb& comb() const { return combFilter; }
    const ImpactEffects& impact() const { return routeRack.impact(); }
    const FractureDiagnostics& fractureDiagnostics() const { return fractureReader.diagnostics(); }
    // The pad's Loops: the phrase's loop. Frames are -1 until captured and read.
    struct LoopDiagnostics
    {
        std::int64_t captureFrame = -1; // the first frame of the pass it captured
        std::int64_t readFrame = -1;    // where every replay of the phrase reads from
        std::array<float, 4> gains { 1.0f, 1.0f, 1.0f, 1.0f }; // the whole loop, its half, quarter and eighth
        bool snapped = false;           // the read frame came from a hit, not the grid
    };
    const LoopDiagnostics& loopDiagnostics() const { return loop; }
    const Resonator& resonator() const { return routeRack.resonator(); }
    const RouteRack::Diagnostics& routeDiagnostics() const { return routeRack.diagnostics(); }

private:
    CutProcedure& procedureFor (Procedure p);
    double samplesPerUnit (const Timing& timing) const;
    void startPhrase (std::int64_t bar, int unitsIntoBar, const Timing& timing);
    void planBlock (const Timing& timing);
    void activatePlans (std::int64_t bar, double beats, double halfSample, const Timing& timing);
    void renderSwung (float& left, float& right, std::int64_t bar, double beats, double beatsPerSample, const Timing& timing, std::int64_t renderedExclusiveEnd);
    double physicalDistance (double phraseUnits, std::int64_t bar, double beats, const Timing& timing) const;
    void startEffectsCut();
    void startImpactCut();
    void chooseFractureBlock (const BlockContext&);
    void startFractureCut (std::int64_t frame);
    void renderSource (std::int64_t frame, float& left, float& right, bool advanceBySamples);
    void selectTransform (std::int64_t renderFrame);
    void applyStructuralChanges();
    void drop (int sampleOffset, Observer* observer);
    std::optional<LivePlayer::Tail> rushSound (std::int64_t frame) const;
    void mixTail (std::optional<LivePlayer::Tail>& tail, int& age, int frames, float& left, float& right);

    struct BlockPlan
    {
        std::array<Cut, maxCutsPerBlock> cuts {};
        std::size_t count = 0;
        SourceRegion source {};
        std::int64_t phraseBar = 0, activationBar = 0;
        double activationBeat = 0.0, start = 0.0, end = 0.0;
        int unitsPerBar = 8;
        std::array<double, 6> effects {};
        CombParameters comb;
        ImpactParameters impact;
        FractureParameters fracture;
        FrequencyParameters frequency;
        ResonatorParameters resonator;
        TransformParameters transform;
        AttackCandidates attacks;
        std::uint64_t transportEpoch = 0;
        std::uint64_t firstCutOrdinal = 0;
        std::uint32_t transformSeed = 1;
        std::uint32_t impactSeed = 1;
        bool rush = false; // the pad's Rush: counts elapsed time exactly, and plays the input once its cuts run out
        bool loops = false; // the pad's Loops: the same, and its replays read the phrase's loop
        double loopIntoPass = -1.0; // a Loops track block: units into its pass; -1 for any other block
    };
    // Rush's and Loops' plans: the track is the input, and their joins crossfade.
    static bool seamless (const BlockPlan& plan) { return plan.rush || plan.loops; }
    void startLoopsBlock (const Timing& timing);
    // Positive swing delays <=1/4 beat. For >=1-beat bars at most eight
    // 1/32-bar block starts plus a boundary/recovery plan await activation.
    // Sub-quarter bars are straight; SQPusher fills are >=1/8 beat and Bounce
    // adds at most one fractional recovery. Sixteen slots include that guard.
    std::array<BlockPlan, 16> plans;
    BlockPlan activePlan;
    std::size_t queueHead = 0, queued = 0, maximumQueued = 0;
    bool hasActivePlan = false;
    SourceRegion planningSource {};
    TransformParameters transformParameters;
    TransformStats transforms;
    const OnsetDetector* onsetDetector = nullptr;
    const SourceContext* sourceContext = nullptr;
    Error processingError = Error::none;
    double swing = 50.0, pendingSwing = 50.0;
    float accent = 0.0f;
    std::uint64_t cutOrdinal = 0;
    std::uint64_t fractureOrdinal = 0; // independent of the phrase-local legacy accent ordinal
    bool emittedBar = false;
    std::int64_t lastEmittedBar = 0;

    double sampleRate = 48000.0;
    CutProc11 cutProc11;
    WarpCut warpCut;
    SQPusher sqPusher;
    Bounce bounce;
    Rush rush;
    Loops loops;
    Random random;
    LivePlayer livePlayer;
    BitCrusher bitCrusher;
    Comb combFilter;
    FractureReader fractureReader;
    RouteRack routeRack;
    CutParameters params;
    CrusherParameters crusherParams;
    CombParameters combParams;
    ImpactParameters impactParams;
    FractureParameters fractureParams;
    FrequencyParameters frequencyParams;
    ResonatorParameters resonatorParams;
    EffectRoute pendingEffectRoute = EffectRoute::impactResonatorFrequency;
    FractureProcedure fractureProcedure = FractureProcedure::classic;
    bool tapeSpinUpNext = false; // the pad's Tape: the block before ended in a stop
    std::optional<TapeTail> tapeTail; // the pad's Tape: a motion cut off by a restart, fading out
    std::optional<TapeTail> tapeRunOn; // the pad's Tape: the last motion, running on to the next block
    // The pad's Tape: the frame of the last rendered sample if it was a stop's
    // silence, else -1. Unlike the spin-up flag it survives resetRendering, so a
    // loop that wraps where a stop ends opens with a spin-up. It counts only for a
    // block that starts on the very next frame: after a pause in rendering (a
    // stopped transport, unsupported timing) the input was heard in between.
    std::int64_t tapeSilentFrame = -1;
    int tapeTailAge = 0, tapeTailFrames = 1;
    std::optional<LivePlayer::Tail> rushTail; // the pad's Rush: a cut cut off by a restart, fading out
    int rushTailAge = 0;
    // The pad's Rush: a Classic program's cut a switch into Rush cut off, fading out
    // under Rush's track before the effects, so the outgoing effects shape it.
    std::optional<LivePlayer::Tail> rushEntry;
    int rushEntryAge = 0;
    // The pad's Rush: a roll's last cut, running on under the next for 2 ms at each
    // join, from frame rushJoinFrame. rushCut is the roll's last rendered cut.
    std::optional<LivePlayer::Tail> rushJoin;
    int rushJoinAge = 0, rushJoinFrames = 1;
    std::int64_t rushJoinFrame = -1;
    std::size_t rushCut = 0;
    LoopDiagnostics loop; // the pad's Loops: this phrase's loop
    std::optional<LivePlayer::Tail> loopRunOn; // the pad's Loops: a replay block's last cut, run on to the next block
    std::optional<std::size_t> fractureCut;
    bool frequencySelected = false;
    double fractureBpm = 0.0;
    std::size_t effectsCut = 0; // the cut the effects were last set for
    std::optional<std::size_t> impactCut;
    std::uint32_t phraseImpactSeed = 1;
    std::vector<Cut> cuts;

    Procedure procedure = Procedure::cutProc11;
    Procedure pendingProcedure = Procedure::cutProc11;
    int unitsPerBar = 8;
    int pendingUnitsPerBar = 8;
    std::uint32_t seed = 1;
    double fadeMilliseconds = 0.0;

    bool timingSupported = true;
    bool cutting = false;
    bool idle = false, pendingIdle = false, restartPending = false;
    bool phraseCompletionNotified = false;
    bool havePosition = false;
    std::int64_t expectedBar = 0;
    double expectedBeatsIntoBar = 0.0;
    double previousBeatsPerBar = 4.0;

    Timing renderTiming;
    std::int64_t renderBar = 0, renderCursor = 0;
    double renderBeats = 0.0, renderBeatsPerSample = 0.0;
    bool renderContextActive = false;

    std::int64_t phraseBar = 0;
    int phraseBars = 0;
    double blockStart = 0.0;
    double blockEnd = 0.0;
    double phrasePosition = 0.0;
};

} // namespace livecut::engine
