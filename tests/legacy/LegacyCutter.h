// Test-only 0.3.0 oracle from f054b36e232ecd20f1986d5856f42bdcf96bc9bf.
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "engine/CutProcedures.h"
#include "engine/Effects.h"
#include "legacy/LegacyPlayer.h"

#include <cstdint>
#include <span>
#include <vector>

namespace livecut::engine::legacy
{

enum class Procedure
{
    cutProc11,
    warpCut,
    sqPusher,
    bounce
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
    void prepare (double sampleRate);

    // Structural changes land on the next bar line.
    void setProcedure (Procedure procedure);
    void setUnitsPerBar (int unitsPerBar);

    // Takes effect from the next block.
    void setParameters (const CutParameters& parameters);

    // Take effect from the next block.
    void setCrusher (const CrusherParameters& parameters);
    void setComb (const CombParameters& parameters);

    // Takes effect from the next phrase.
    void setSeed (std::uint32_t seed);

    void setFadeMilliseconds (double milliseconds);

    // Forgets the current phrase. Until the next unit boundary the input
    // passes through; cutting then resumes in the current bar.
    void reset();

    // Synchronous sample events; implementations must never allocate or wait.
    // phraseStarted precedes that phrase's first output sample. fullStart is
    // false for the initial partial phrase after a mid-bar start/seek.
    struct Observer
    {
        virtual ~Observer() = default;
        virtual void discontinuity (int sampleOffset) = 0;
        // Exclusive end, including numSamples when a phrase ends exactly at
        // the host buffer boundary. Does not generate the next phrase's cuts.
        virtual void phraseCompleted (int sampleOffset) = 0;
        virtual void phraseStarted (int sampleOffset, bool fullStart) = 0;
        virtual void processOutput (int sampleOffset, float dryLeft, float dryRight, float& left, float& right) = 0;
    };

    // Cuts a stereo buffer in place. `position` is the host's position at its first sample.
    void process (float* left, float* right, int numSamples, const Timing& timing, const Position& position, Observer* observer = nullptr);

    bool isCutting() const { return cutting; }
    bool isTimingSupported() const { return timingSupported; }
    Procedure activeProcedure() const { return procedure; }
    int activeUnitsPerBar() const { return unitsPerBar; }
    std::int64_t phraseStartBar() const { return phraseBar; }
    double blockStartUnits() const { return blockStart; } // within the phrase
    double blockEndUnits() const { return blockEnd; }
    int phraseBarCount() const { return phraseBars; }
    double positionInPhrase() const { return phrasePosition; } // units, at the end of the last buffer
    std::span<const Cut> currentCuts() const { return cuts; }
    const LivePlayer& player() const { return livePlayer; }
    const BitCrusher& crusher() const { return bitCrusher; }
    const Comb& comb() const { return combFilter; }

private:
    CutProcedure& procedureFor (Procedure p);
    double samplesPerUnit (const Timing& timing) const;
    void startPhrase (std::int64_t bar, int unitsIntoBar, const Timing& timing);
    void startBlock (const Timing& timing);
    void startEffectsCut();
    void applyStructuralChanges();

    double sampleRate = 48000.0;
    CutProc11 cutProc11;
    WarpCut warpCut;
    SQPusher sqPusher;
    Bounce bounce;
    Random random;
    LivePlayer livePlayer;
    BitCrusher bitCrusher;
    Comb combFilter;
    CutParameters params;
    CrusherParameters crusherParams;
    CombParameters combParams;
    std::size_t effectsCut = 0; // the cut the effects were last set for
    std::vector<Cut> cuts;

    Procedure procedure = Procedure::cutProc11;
    Procedure pendingProcedure = Procedure::cutProc11;
    int unitsPerBar = 8;
    int pendingUnitsPerBar = 8;
    std::uint32_t seed = 1;
    double fadeMilliseconds = 0.0;

    bool timingSupported = true;
    bool cutting = false;
    bool phraseCompletionNotified = false;
    bool havePosition = false;
    std::int64_t expectedBar = 0;
    double expectedBeatsIntoBar = 0.0;
    double previousBeatsPerBar = 4.0;

    std::int64_t phraseBar = 0;
    int phraseBars = 0;
    double blockStart = 0.0;
    double blockEnd = 0.0;
    double phrasePosition = 0.0;
};

} // namespace livecut::engine::legacy
