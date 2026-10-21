// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Parameters.h"
#include "engine/DelayTimeline.h"
#include "engine/OnsetDetector.h"
#include "spectral/SpectralStage.h"
#include <limits>
#include <stdexcept>

namespace livecut
{
struct RenderContext
{
    Settings settings;
    engine::Timing timing;
    engine::Position position;
    double ppq = 0.0;
    bool running = false, bypass = false, resetHold = false;
};

// One physical stream, exclusively mutated by audio after preparation/swap.
// The processor keeps Hold, mixer and CaptureService stable across mode changes.
struct StreamResources : private engine::Cutter::SourceContext
{
    using Timeline = engine::DelayTimeline<RenderContext>;
    StreamResources (double rate, ProcessingMode selected, bool spectralRequested = false, spectral::TestHooks* hooks = nullptr)
        : sampleRate (rate), mode (selected), spectralEnabled (spectralRequested)
    {
        if (!std::isfinite (rate) || rate < 8000.0 || rate > 384000.0)
            throw std::invalid_argument ("Unsupported processing rate");
        if (mode == ProcessingMode::perform && spectralEnabled)
            throw std::invalid_argument ("Perform mode requires Spectral Engine off");
        baseLatency = mode == ProcessingMode::studio ? static_cast<int> (std::lround (.5 * rate)) : 0;
        if (spectralEnabled)
        {
            spectral = std::make_unique<spectral::Stage> (spectral::Preparation{rate, true, hooks});
            stopPublication = std::make_shared<const spectral::StopHandle> (spectral->stopHandle ());
        }
        latency = baseLatency + (spectral ? spectral->latency () : 0);
        cutter.prepare (rate, baseLatency);
        if (!detector.prepare (rate, 8192, 3.0) || !timeline.prepare (baseLatency))
            throw std::runtime_error ("Cannot prepare stream metadata");
        cutter.setTransformSource (detector, *this);
        if (mode == ProcessingMode::perform)
        {
            performance = std::make_unique<engine::PerformanceEngine>();
            performance->prepare (rate);
        }
    }

    bool appendContext (RenderContext context, int frames, std::uint64_t &epochCounter)
    {
        if (frames == 0)
            return true;
        auto discontinuous = !haveInput || context.running != lastRunning;
        if (haveInput && context.running && lastRunning)
            discontinuous |= std::abs (context.ppq - expectedPpq) > .001;
        if (discontinuous)
        {
            if (epochCounter == std::numeric_limits<std::uint64_t>::max ())
                return false;
            inputEpoch = ++epochCounter;
            epochStart = inputFrame;
            if (!detector.beginTransportEpoch (inputEpoch))
                return false;
        }
        if (!timeline.append (inputFrame, frames, inputEpoch, epochStart, context))
            return false;
        haveInput = true;
        lastRunning = context.running;
        if (context.running)
            expectedPpq = context.ppq + frames * context.timing.bpm / 60.0 / sampleRate;
        return true;
    }

    std::optional<engine::Cutter::TransportBounds> sourceBoundsAt (std::int64_t frame) const override
    {
        if (const auto epoch = timeline.sourceEpoch (frame))
            return engine::Cutter::TransportBounds{epoch->id, epoch->start, epoch->end};
        return std::nullopt;
    }

    engine::Cutter cutter;
    std::unique_ptr<engine::PerformanceEngine> performance;
    engine::OnsetDetector detector;
    Timeline timeline;
    std::optional<Timeline::Lookup> rendering;
    double sampleRate;
    ProcessingMode mode;
    bool spectralEnabled = false;
    std::unique_ptr<spectral::Stage> spectral;
    std::shared_ptr<const spectral::StopHandle> stopPublication;
    int latency = 0, baseLatency = 0;
    std::int64_t inputFrame = 0, nextRender = 0, epochStart = 0;
    double expectedPpq = 0.0;
    std::uint64_t inputEpoch = 0, renderEpoch = 0;
    bool haveInput = false, lastRunning = false, fault = false;
};
} // namespace livecut
