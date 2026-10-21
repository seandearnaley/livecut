// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "engine/AudioHistory.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <vector>

namespace livecut::engine
{
// Entirely audio-thread-owned. The published wet phrase is never written while
// held or fading out; WAV capture has separate post-mix ownership and storage.
class WetHold
{
public:
    enum class State { live, waiting, pendingEntry, held, pendingRelease, oversize };
    static constexpr std::size_t maxFrames = 64 * 192000;
    void prepare (double rate, std::size_t frames = 0)
    {
        sampleRate = rate;
        capacity = frames != 0 ? std::min (frames, maxFrames)
                              : static_cast<std::size_t> (std::min (std::ceil (64.0 * rate), static_cast<double> (maxFrames)));
        for (auto& slot : slots)
            slot.resize (capacity);
        transitionSamples = std::max (1, static_cast<int> (std::lround (rate * 0.005)));
        seamFrames = std::max (1.0, rate * 0.002);
        interrupt();
    }
    void request (bool on)
    {
        requested = on;
        updateState();
    }
    void interrupt()
    {
        recording = ready = held = overflow = false;
        written = length = 0;
        amount = 0.0f;
        phase = 0.0;
        currentState = requested ? State::waiting : State::live;
    }
    void startPhrase (bool fullStart, double quarterNotes)
    {
        recording = fullStart && ! frozen();
        written = 0;
        candidateBeats = quarterNotes;
        overflow = false;
    }
    void completePhrase()
    {
        if (! recording)
            return;
        recording = false;
        if (overflow || written == 0 || ! (candidateBeats > 0.0))
            return;
        latest = writing;
        writing = 1 - writing;
        length = written;
        durationBeats = candidateBeats;
        ready = true;
        updateState();
    }
    // Called after phrase completion and before the new bar's first sample.
    void barStarted()
    {
        if (requested && ! held && ready)
        {
            held = true;
            recording = false;
            phase = 0.0;
        }
        else if (! requested && held)
        {
            held = false;
            recording = false; // rearm at the next complete natural phrase start
        }
        updateState();
    }
    void process (float& left, float& right, double quarterNotesPerSample)
    {
        const StereoFrame live { left, right };
        if (recording && ! frozen())
        {
            if (written == capacity)
            {
                overflow = true;
                recording = ready = false;
                currentState = State::oversize;
            }
            else
                slots[static_cast<std::size_t> (writing)][written++] = live;
        }
        const auto step = 1.0f / static_cast<float> (transitionSamples);
        if (held)
            amount = std::min (1.0f, amount + step);
        else
            amount = std::max (0.0f, amount - step);
        if (amount <= 0.0f)
            return; // preserve the live lane exactly when Hold is off
        const auto frame = phase / durationBeats * static_cast<double> (length);
        const auto first = std::min (static_cast<std::size_t> (frame), length - 1);
        const auto second = (first + 1) % length;
        const auto fraction = static_cast<float> (frame - static_cast<double> (first));
        const auto& audio = slots[static_cast<std::size_t> (latest)];
        auto heldLeft = audio[first].left + fraction * (audio[second].left - audio[first].left);
        auto heldRight = audio[first].right + fraction * (audio[second].right - audio[first].right);
        const auto seam = std::min (seamFrames, static_cast<double> (length) / 4.0);
        const auto window = static_cast<float> (std::clamp (std::min (frame, static_cast<double> (length) - frame) / seam, 0.0, 1.0));
        heldLeft *= window;
        heldRight *= window;
        left = live.left + amount * (heldLeft - live.left);
        right = live.right + amount * (heldRight - live.right);
        phase = std::fmod (phase + quarterNotesPerSample, durationBeats);
    }
    State state() const { return currentState; }
    bool hasPhrase() const { return ready; }
    bool isHeld() const { return held; }
    double capacitySeconds() const { return static_cast<double> (capacity) / sampleRate; }
    std::size_t phraseFrames() const { return length; }
    double phaseBeats() const { return phase; }

private:
    bool frozen() const { return held || amount > 0.0f; }
    void updateState()
    {
        if (held)
            currentState = requested ? State::held : State::pendingRelease;
        else if (requested)
            currentState = ready ? State::pendingEntry : overflow ? State::oversize : State::waiting;
        else
            currentState = State::live;
    }
    std::array<std::vector<StereoFrame>, 2> slots;
    std::size_t capacity = 0, written = 0, length = 0;
    int writing = 0, latest = 1, transitionSamples = 1;
    double sampleRate = 48000.0, seamFrames = 96.0, candidateBeats = 0.0, durationBeats = 0.0, phase = 0.0;
    float amount = 0.0f;
    bool requested = false, recording = false, ready = false, held = false, overflow = false;
    State currentState = State::live;
};
} // namespace livecut::engine
