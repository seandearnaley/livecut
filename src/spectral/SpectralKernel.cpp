// SPDX-License-Identifier: GPL-3.0-or-later
#include "SpectralKernel.h"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
namespace livecut::spectral
{
namespace
{
constexpr auto pi = std::numbers::pi;
constexpr auto infinityFrame = std::numeric_limits<std::int64_t>::max ();
double wrap (double phase)
{
    return std::remainder (phase, 2 * pi);
}
} // namespace
Configuration Configuration::prepare (double rate, int reference)
{
    if (!std::isfinite (rate) || rate < 8000 || rate > 384000 || reference < 1024 || reference > 8192 ||
        !std::has_single_bit (static_cast<unsigned> (reference)))
        throw std::invalid_argument ("Unsupported FFT preparation configuration");
    Configuration c;
    c.referenceSizeAt48k = reference;
    c.targetSeconds = reference / 48000.0;
    c.size = static_cast<int> (std::bit_ceil (static_cast<unsigned> (std::ceil ((rate / 48000.0) * reference))));
    c.hop = c.size / 4;
    c.order = static_cast<int> (std::bit_width (static_cast<unsigned> (c.size)) - 1);
    return c;
}
Wola::Wola (double rate, int reference)
    : config (Configuration::prepare (rate, reference)), fft (makeJuceFft (config.order)), sampleRate (rate)
{
    if (!std::isfinite (rate) || rate < 8000 || rate > 384000)
        throw std::invalid_argument ("Unsupported prototype rate");
    window.resize (static_cast<std::size_t> (config.size));
    outputTags.resize (static_cast<std::size_t> (2 * config.size));
    for (auto &c : channels)
    {
        c.input.resize (static_cast<std::size_t> (config.size));
        c.output.resize (static_cast<std::size_t> (2 * config.size));
        c.spectrum.resize (static_cast<std::size_t> (2 * config.size));
        c.bins.resize (static_cast<std::size_t> (config.size / 2 + 1));
    }
    for (int n = 0; n < config.size; ++n)
        window[static_cast<std::size_t> (n)] = std::sqrt (.5 - .5 * std::cos (2 * pi * n / config.size));
}
void Wola::setControls (Controls c)
{
    if (!std::isfinite (c.smearMs) || c.smearMs < 0 || c.smearMs > 2000)
        throw std::invalid_argument ("Invalid smear time");
    controls = c;
    smearCoefficient = c.smearMs > 0 ? std::exp (-config.hop / (.001 * c.smearMs * sampleRate)) : 0;
    state.waitingForHistory = !admitted || (c.freeze && !state.frozen);
}
void Wola::clearAnalysis ()
{
    for (auto &channel : channels)
    {
        std::fill (channel.input.begin (), channel.input.end (), 0);
        std::fill (channel.spectrum.begin (), channel.spectrum.end (), 0);
        std::fill (channel.bins.begin (), channel.bins.end (), Bin{});
    }
    completeFrames = 0;
    state.frozen = false;
    state.waitingForHistory = controls.freeze;
    state.freezeCaptureEnd = -1;
    state.lastAnalysisEnd = 0;
    state.dominantMagnitude = state.dominantAdvance = state.dominantPhase = 0;
    state.dominantBin = 0;
}
void Wola::reset ()
{
    const auto highestEpoch = state.epoch;
    clearAnalysis ();
    for (auto &channel : channels)
        std::fill (channel.output.begin (), channel.output.end (), 0);
    state = {};
    state.epoch = highestEpoch;
    nextSource = epochStart = 0;
    completeFrames = 0;
    state.waitingForHistory = true;
    admitted = false;
    firstSynthesisSource = -1;
    std::fill (outputTags.begin (), outputTags.end (), OutputTag{});
}
bool Wola::beginEpoch (std::uint64_t id)
{
    if (id <= state.epoch)
        return false;
    const auto boundary = nextSource + config.size;
    if (admitted)
    {
        auto end = (nextSource / config.hop + 1) * config.hop;
        while (end - config.size < nextSource)
        {
            ++state.closureWindows;
            renderWindow (end, boundary);
            end += config.hop;
        }
    }
    // No array clear here. Input validity uses absolute epochStart; bins reset
    // lazily in the first full analysis using their epoch tag.
    epochStart = nextSource;
    state.epoch = id;
    completeFrames = 0;
    admitted = false;
    firstSynthesisSource = -1;
    state.frozen = false;
    state.waitingForHistory = true;
    state.freezeCaptureEnd = -1;
    ++state.epochResets;
    return true;
}
void Wola::renderWindow (std::int64_t end, std::int64_t upperOutput)
{
    const auto start = end - config.size;
    if (!admitted && start < epochStart)
        return;
    ++state.analysisWindows;
    const bool complete = start >= epochStart && end <= nextSource;
    if (complete)
        ++completeFrames;
    const bool capture = controls.freeze && !state.frozen && complete && completeFrames >= 2;
    if (!controls.freeze)
        state.frozen = false;
    for (auto &c : channels)
    {
        std::fill (c.spectrum.begin (), c.spectrum.end (), 0);
        for (int n = 0; n < config.size; ++n)
        {
            const auto source = start + n;
            const auto input = source >= epochStart && source >= 0 && source < nextSource
                                   ? c.input[static_cast<std::size_t> (source % config.size)]
                                   : 0.0f;
            c.spectrum[static_cast<std::size_t> (n)] = static_cast<float> (input * window[static_cast<std::size_t> (n)]);
        }
        fft->forward (c.spectrum.data ());
        for (int k = 0; k <= config.size / 2; ++k)
        {
            auto &b = c.bins[static_cast<std::size_t> (k)];
            if (b.epoch != state.epoch)
            {
                b = {};
                b.epoch = state.epoch;
            }
            const auto real = c.spectrum[static_cast<std::size_t> (2 * k)], imag = c.spectrum[static_cast<std::size_t> (2 * k + 1)];
            const auto phase = std::atan2 (imag, real), magnitude = std::hypot (real, imag);
            const auto expected = 2 * pi * k * config.hop / config.size;
            if (!b.primed)
            {
                b.phase = phase;
                b.advance = expected;
                b.primed = true;
            }
            if (magnitude > 1e-8 && b.previousMagnitude > 1e-8)
                b.advance = expected + wrap (phase - b.previousPhase - expected);
            b.previousPhase = phase;
            b.previousMagnitude = magnitude;
            b.magnitude = magnitude;
            if (capture)
            {
                b.frozenMagnitude = magnitude;
                b.phase = phase;
            }
        }
    }
    if (!admitted && completeFrames < 2)
        return;
    if (!admitted)
    {
        admitted = true;
        firstSynthesisSource = start;
    }
    if (capture)
    {
        for (int k = 0; k <= config.size / 2; ++k)
        {
            auto &l = channels[0].bins[static_cast<std::size_t> (k)];
            auto &r = channels[1].bins[static_cast<std::size_t> (k)];
            const auto advance = l.magnitude >= r.magnitude ? l.advance : r.advance;
            l.frozenAdvance = r.frozenAdvance = (k == 0 || k == config.size / 2) ? 0 : advance;
        }
        state.frozen = true;
        state.freezeCaptureEnd = end;
    }
    state.waitingForHistory = !admitted || (controls.freeze && !state.frozen);
    state.lastAnalysisEnd = end;
    double dominant = 0;
    for (auto &c : channels)
    {
        for (int k = 0; k <= config.size / 2; ++k)
        {
            auto &b = c.bins[static_cast<std::size_t> (k)];
            double magnitude = b.magnitude;
            if (state.frozen)
            {
                if (!capture)
                    b.phase = wrap (b.phase + b.frozenAdvance);
                magnitude = b.frozenMagnitude;
            }
            else if (controls.smearMs > 0)
            {
                b.phase = wrap (b.phase + b.advance);
            }
            else
                b.phase = b.previousPhase;
            b.smoothedMagnitude = smearCoefficient * b.smoothedMagnitude + (1 - smearCoefficient) * magnitude;
            if (controls.smearMs > 0)
                magnitude = b.smoothedMagnitude;
            // Fresh epochs return before synthesis until admission. A Freeze
            // request still awaiting an eligible full analysis does not revoke
            // an already admitted live spectrum, including its partial closures.
            if (state.frozen || controls.smearMs > 0)
            {
                c.spectrum[static_cast<std::size_t> (2 * k)] = static_cast<float> (magnitude * std::cos (b.phase));
                c.spectrum[static_cast<std::size_t> (2 * k + 1)] =
                    (k == 0 || k == config.size / 2) ? 0.0f : static_cast<float> (magnitude * std::sin (b.phase));
            }
            if (magnitude > dominant)
            {
                dominant = magnitude;
                state.dominantBin = k;
                state.dominantMagnitude = magnitude;
                state.dominantAdvance = state.frozen ? b.frozenAdvance : b.advance;
                state.dominantPhase = b.phase;
            }
        }
        fft->inverse (c.spectrum.data ());
        // Product of equal root-periodic-Hann windows sums to 2 for N/4 hops.
        // Explicit scheduler: input window [end-N,end) writes [end,end+N).
        // This scheduler's exact unity delay is measured, not inferred as N-H.
        for (int n = 0; n < config.size; ++n)
        {
            const auto target = end + n;
            if (target >= epochStart + config.size && target < upperOutput)
            {
                const auto slot = static_cast<std::size_t> (target % (2 * config.size));
                c.output[slot] += c.spectrum[static_cast<std::size_t> (n)] * window[static_cast<std::size_t> (n)] * .5;
                outputTags[slot] = {target, state.epoch,
                                    target - config.size < firstSynthesisSource + config.size - config.hop ? SpectralState::priming
                                                                                                           : SpectralState::ready};
            }
        }
    }
}
Frame Wola::process (Frame input)
{
    const auto current = nextSource;
    channels[0].input[static_cast<std::size_t> (current % config.size)] = input.left;
    channels[1].input[static_cast<std::size_t> (current % config.size)] = input.right;
    ++nextSource;
    if (nextSource % config.hop == 0)
        renderWindow (nextSource, infinityFrame);
    const auto index = static_cast<std::size_t> (current % (2 * config.size));
    const Frame output{static_cast<float> (channels[0].output[index]), static_cast<float> (channels[1].output[index])};
    channels[0].output[index] = channels[1].output[index] = 0;
    state.outputState = outputTags[index].frame == current ? outputTags[index].state : SpectralState::waiting;
    state.outputEpoch = outputTags[index].frame == current ? outputTags[index].epoch : 0;
    outputTags[index] = {};
    return output;
}
std::size_t Wola::explicitStorageBytes () const
{
    auto total = sizeof (*this) + window.capacity () * sizeof (double) + outputTags.capacity () * sizeof (OutputTag);
    for (const auto &c : channels)
        total += c.input.capacity () * sizeof (float) + c.output.capacity () * sizeof (double) + c.spectrum.capacity () * sizeof (float) +
                 c.bins.capacity () * sizeof (Bin);
    return total;
}
} // namespace livecut::spectral
