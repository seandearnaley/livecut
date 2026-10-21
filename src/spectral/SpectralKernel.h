// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace livecut::spectral
{
struct Configuration
{
    int referenceSizeAt48k = 4096, size = 4096, hop = 1024, order = 12;
    double targetSeconds = 4096.0 / 48000.0;
    static Configuration prepare (double rate, int referenceSizeAt48k);
};
enum class SpectralState
{
    waiting,
    priming,
    ready
};
struct Frame
{
    float left = 0, right = 0;
};
struct FftPlan
{
    virtual ~FftPlan () = default;
    virtual void forward (float *twoN) noexcept = 0;
    virtual void inverse (float *twoN) noexcept = 0;
};
std::unique_ptr<FftPlan> makeJuceFft (int order);

class Wola
{
  public:
    struct Controls
    {
        bool freeze = false;
        double smearMs = 0;
    };
    struct Diagnostics
    {
        std::int64_t lastAnalysisEnd = 0, freezeCaptureEnd = -1;
        std::uint64_t epoch = 1;
        bool frozen = false, waitingForHistory = false;
        double dominantMagnitude = 0, dominantAdvance = 0, dominantPhase = 0;
        int dominantBin = 0;
        SpectralState outputState = SpectralState::waiting;
        std::uint64_t outputEpoch = 0;
        std::uint64_t analysisWindows = 0, closureWindows = 0, epochResets = 0;
    };
    explicit Wola (double rate, int referenceSizeAt48k = 4096);
    void setControls (Controls);
    void reset ();
    bool beginEpoch (std::uint64_t);
    Frame process (Frame);
    const Diagnostics &diagnostics () const
    {
        return state;
    }
    int declaredLatency () const
    {
        return config.size;
    }
    const Configuration &configuration () const
    {
        return config;
    }
    std::size_t explicitStorageBytes () const;
    std::int64_t sourceFrames () const
    {
        return nextSource;
    }

  private:
    struct Bin
    {
        std::uint64_t epoch = 0;
        double previousPhase = 0, previousMagnitude = 0;
        double phase = 0, advance = 0, magnitude = 0, smoothedMagnitude = 0;
        double frozenMagnitude = 0, frozenAdvance = 0;
        bool primed = false;
    };
    struct Channel
    {
        std::vector<float> input;
        std::vector<double> output;
        std::vector<float> spectrum;
        std::vector<Bin> bins;
    };
    void renderWindow (std::int64_t exclusiveEnd, std::int64_t upperOutput);
    void clearAnalysis ();
    Configuration config;
    std::unique_ptr<FftPlan> fft;
    std::array<Channel, 2> channels;
    std::vector<double> window;
    struct OutputTag
    {
        std::int64_t frame = -1;
        std::uint64_t epoch = 0;
        SpectralState state = SpectralState::waiting;
    };
    std::vector<OutputTag> outputTags;
    bool admitted = false;
    std::int64_t firstSynthesisSource = -1;
    Controls controls;
    Diagnostics state;
    double sampleRate, smearCoefficient = 0;
    std::int64_t nextSource = 0, epochStart = 0;
    int completeFrames = 0;
};

} // namespace livecut::spectral
