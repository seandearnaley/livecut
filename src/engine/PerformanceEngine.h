// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/Cutter.h"
#include "engine/Fracture.h"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace livecut::engine
{
struct PerformanceParameters
{
    float variation = 0.0f, density = 0.5f, grain = 0.0f, character = 0.5f;
    bool fill = false;
    std::uint32_t seed = 1;
};

struct PerformanceRequests { bool capture = false; };

// A causal, zero-latency performance instrument with an explicit one-bar source.
// Waiting monitors raw input. The first full bar is captured automatically;
// thereafter the immutable source changes only after another Capture request.
class PerformanceEngine
{
public:
    static constexpr int stepsPerBar = 32;
    static constexpr double maximumBarSeconds = 8.0;
    enum class State { waitingForHost, waitingForBar, capturing, playing, unsupportedTiming, error };
    enum class Error { none, invalidContext, invalidParameters, sourceCapacity, sourceIdentityExhausted };
    enum class Role { anchor, original, variation, repeat, rest };
    struct Event
    {
        int startStep = 0, endStep = 0;
        std::uint64_t sourceId = 0;
        std::int64_t sourceOffset = 0, sourceEnd = 0;
        float gate = 1.0f;
        Role role = Role::original;
    };
    struct Diagnostics
    {
        State state = State::waitingForHost;
        Error error = Error::none;
        bool hasSource = false, captureArmed = true, seedPending = false;
        std::uint64_t sourceGeneration = 0, barsRendered = 0, grainsLaunched = 0;
        std::int64_t sourceFrames = 0;
        int activeGrains = 0, currentStep = 0;
        double playhead = 0.0, sourceBpm = 0.0;
        PerformanceParameters requested, applied;
    };
    struct Result
    {
        StereoFrame frame {};
        // Events precede frame; completion closes the preceding one-bar phrase.
        bool discontinuity = false, barStarted = false, barCompleted = false, fullStart = false;
        // Exclusive-end completion follows frame. Dispatch after recording it.
        bool barCompletedAfter = false;
    };

    // Only prepare allocates. Invalid sample rates throw before changing state.
    void prepare (double sampleRate);
    void beginContext (const Timing&, const Position&, std::uint64_t transportEpoch, bool running,
                       const PerformanceParameters&, PerformanceRequests = {});
    Result processOne (StereoFrame raw);
    const Diagnostics& diagnostics() const noexcept { return display; }
    std::span<const Event> events() const noexcept { return { plan.data(), planCount }; }

private:
    struct Bank
    {
        std::vector<StereoFrame> frames;
        std::array<std::int64_t, stepsPerBar + 1> boundaries {}, attacks {};
        std::array<double, stepsPerBar> energy {}, rise {};
        std::int64_t count = 0;
        std::uint64_t generation = 0;
    };
    struct Voice
    {
        bool active = false;
        double position = 0.0, rate = 1.0;
        int age = 0, length = 0;
        std::int64_t end = 0;
    };
    void interrupt();
    bool validParameters (const PerformanceParameters&) const;
    void startBar (std::int64_t bar, double phase, Result&);
    void record (StereoFrame, double phase);
    void finishCapture();
    void makePlan (std::int64_t bar);
    void startEvent (int step, double phase);
    StereoFrame render (double phase);
    StereoFrame read (double position, int bank, double rate, GrainResampler::Kernel) const;
    void launchGrain (const Event&, double basePosition, int attackFrames);
    std::uint32_t draw (std::uint32_t tag, std::uint32_t ordinal) const;
    void fail (Error);

    // 3 * ceil(8 seconds * sampleRate) stereo frames: ~9.2MB at 48kHz.
    // The third bank retains an outgoing source while its splice crossfades.
    std::array<Bank, 3> banks;
    int sourceBank = 0, captureBank = 1;
    bool recording = false, captureFull = false, haveBar = false, running = false, contextValid = false;
    bool discontinuityPending = false, captureRequested = true;
    double sampleRate = 48000.0, beatsPerSample = 0.0, captureEnergy = 0.0, captureSmoothing = 0.0;
    double smoothedGrain = 0.0, smoothedCharacter = 0.5, smoothing = 0.0;
    Timing timing;
    Position position;
    std::uint64_t epoch = 0, nextGeneration = 1;
    std::int64_t contextOffset = 0, activeBar = 0;
    int recordedStep = 0, scheduledStep = -1, eventIndex = -1, eventAge = 0, crossfadeAge = 0;
    int crossfadeFrames = 1, attackFrames = 1, nextGrainAge = 0;
    int mainBank = 0, previousBank = 0;
    double mainPosition = 0.0, mainRate = 1.0, previousPosition = 0.0, previousRate = 1.0;
    double cachedMainRate = -1.0, cachedPreviousRate = -1.0;
    bool fading = false, haveVoice = false, barCompletedAtEnd = false;
    std::array<Event, stepsPerBar> plan {};
    std::size_t planCount = 0;
    std::array<Voice, 4> grains {};
    GrainResampler resampler;
    GrainResampler::Kernel mainKernel, previousKernel;
    Diagnostics display;
};
} // namespace livecut::engine
