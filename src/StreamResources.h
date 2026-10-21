// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Parameters.h"
#include "engine/DelayTimeline.h"
#include "engine/OnsetDetector.h"
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
struct StreamResources
{
    using Timeline = engine::DelayTimeline<RenderContext>;
    StreamResources (double rate, ProcessingMode selected) : sampleRate (rate), mode (selected)
    {
        if (! std::isfinite (rate) || rate < 8000.0 || rate > 384000.0)
            throw std::invalid_argument ("Unsupported processing rate");
        latency = mode == ProcessingMode::studio ? static_cast<int> (std::lround (.5 * rate)) : 0;
        cutter.prepare (rate, latency);
        if (! detector.prepare (rate, 8192, 3.0) || ! timeline.prepare (latency))
            throw std::runtime_error ("Cannot prepare stream metadata");
    }

    bool appendContext (RenderContext context, int frames, std::uint64_t& epochCounter)
    {
        if (frames == 0)
            return true;
        auto discontinuous = ! haveInput || context.running != lastRunning;
        if (haveInput && context.running && lastRunning)
            discontinuous |= std::abs (context.ppq - expectedPpq) > .001;
        if (discontinuous)
        {
            if (epochCounter == std::numeric_limits<std::uint64_t>::max())
                return false;
            inputEpoch = ++epochCounter;
            epochStart = inputFrame;
            if (! detector.beginTransportEpoch (inputEpoch))
                return false;
        }
        if (! timeline.append (inputFrame, frames, inputEpoch, epochStart, context))
            return false;
        haveInput = true;
        lastRunning = context.running;
        if (context.running)
            expectedPpq = context.ppq + frames * context.timing.bpm / 60.0 / sampleRate;
        return true;
    }

    engine::Cutter cutter;
    engine::OnsetDetector detector;
    Timeline timeline;
    std::optional<Timeline::Lookup> rendering;
    double sampleRate;
    ProcessingMode mode;
    int latency = 0;
    std::int64_t inputFrame = 0, nextRender = 0, epochStart = 0;
    double expectedPpq = 0.0;
    std::uint64_t inputEpoch = 0, renderEpoch = 0;
    bool haveInput = false, lastRunning = false, fault = false;
};
} // namespace livecut
