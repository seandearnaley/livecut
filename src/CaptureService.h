// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "PhraseCapture.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace livecut
{
// All snapshot allocation and file IO happen on this service's worker. UI
// polling/callbacks hold no references to an editor; destroying one is safe.
class CaptureService
{
public:
    ~CaptureService();
    PhraseCapture& rolling() { return recorder; }
    const PhraseCapture& rolling() const { return recorder; }
    bool requestCapture();
    bool requestSave (const juce::File& target);
    bool isBusy() const { return working.load (std::memory_order_acquire); }
    std::shared_ptr<const CapturedPhrase> captured() const;
    juce::String statusText() const;

    // Also used by tests/importers; never call from the audio callback.
    static juce::Result writeWav (const CapturedPhrase&, const juce::File& target);

private:
    enum class Operation { idle, capturing, captured, saving, saved, empty, busy, error };
    void finish (Operation, const juce::String& error = {});

    PhraseCapture recorder;
    mutable std::mutex snapshotMutex;
    std::shared_ptr<const CapturedPhrase> snapshot;
    juce::String errorMessage;
    std::atomic<bool> working { false };
    std::atomic<Operation> operation { Operation::idle };
    juce::ThreadPool worker { 1 };
};
} // namespace livecut
