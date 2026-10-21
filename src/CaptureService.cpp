// SPDX-License-Identifier: GPL-3.0-or-later
#include "CaptureService.h"

namespace livecut
{
CaptureService::~CaptureService() { worker.removeAllJobs (true, -1); }

void CaptureService::finish (Operation next, const juce::String& error)
{
    {
        const std::lock_guard lock (snapshotMutex);
        errorMessage = error;
    }
    operation.store (next, std::memory_order_release);
    working.store (false, std::memory_order_release);
}

bool CaptureService::requestCapture()
{
    if (working.exchange (true, std::memory_order_acq_rel))
        return false;
    auto lease = std::make_shared<PhraseCapture::Lease> (recorder.acquireLatest());
    if (! *lease)
    {
        finish (recorder.hasLatest() ? Operation::busy : Operation::empty);
        return false;
    }
    operation.store (Operation::capturing, std::memory_order_release);
    worker.addJob ([this, lease]
    {
        try
        {
            auto phrase = std::make_shared<CapturedPhrase>();
            phrase->sampleRate = lease->sampleRate();
            for (std::size_t channel = 0; channel < 2; ++channel)
            {
                const auto samples = lease->channel (channel);
                phrase->channels[channel].assign (samples.begin(), samples.end());
            }
            {
                const std::lock_guard lock (snapshotMutex);
                snapshot = std::move (phrase);
            }
            *lease = {};
            finish (Operation::captured);
        }
        catch (const std::exception& error)
        {
            *lease = {};
            finish (Operation::error, "Capture failed: " + juce::String (error.what()));
        }
    });
    return true;
}

std::shared_ptr<const CapturedPhrase> CaptureService::captured() const
{
    const std::lock_guard lock (snapshotMutex);
    return snapshot;
}

bool CaptureService::requestSave (const juce::File& target)
{
    if (working.exchange (true, std::memory_order_acq_rel))
        return false;
    auto phrase = captured();
    if (phrase == nullptr)
    {
        finish (Operation::empty);
        return false;
    }
    operation.store (Operation::saving, std::memory_order_release);
    worker.addJob ([this, phrase, target]
    {
        const auto result = writeWav (*phrase, target);
        finish (result.wasOk() ? Operation::saved : Operation::error, result.getErrorMessage());
    });
    return true;
}

juce::Result CaptureService::writeWav (const CapturedPhrase& phrase, const juce::File& target)
{
    if (phrase.size() == 0 || phrase.channels[1].size() != phrase.size() || ! (phrase.sampleRate > 0.0))
        return juce::Result::fail ("No complete stereo phrase to save");
    juce::TemporaryFile temporary (target);
    auto fileStream = temporary.getFile().createOutputStream();
    if (fileStream == nullptr)
        return juce::Result::fail ("Cannot create WAV in the selected folder");
    auto* checkedStream = fileStream.get();
    std::unique_ptr<juce::OutputStream> stream = std::move (fileStream);
    juce::WavAudioFormat format;
    const auto options = juce::AudioFormatWriterOptions()
        .withSampleRate (phrase.sampleRate)
        .withChannelLayout (juce::AudioChannelSet::stereo())
        .withBitsPerSample (32)
        .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
    auto writer = format.createWriterFor (stream, options);
    if (writer == nullptr)
        return juce::Result::fail ("Cannot create a stereo 32-bit float WAV writer");
    // Bound the writer's interleaving scratch space even for a 64-second file.
    constexpr std::size_t chunkFrames = 8192;
    for (std::size_t offset = 0; offset < phrase.size(); offset += chunkFrames)
    {
        const auto count = static_cast<int> (std::min (chunkFrames, phrase.size() - offset));
        const float* channels[] { phrase.channels[0].data() + offset, phrase.channels[1].data() + offset, nullptr };
        if (! writer->writeFromFloatArrays (channels, 2, count))
            return juce::Result::fail ("WAV write failed; check free disk space and folder permissions");
    }
    if (! writer->flush())
        return juce::Result::fail ("WAV header write failed");
    checkedStream->flush();
    if (checkedStream->getStatus().failed())
        return juce::Result::fail ("WAV flush failed: " + checkedStream->getStatus().getErrorMessage());
    writer.reset();
    if (! temporary.overwriteTargetFileWithTemporary())
        return juce::Result::fail ("Cannot replace the selected WAV file");
    return juce::Result::ok();
}

juce::String CaptureService::statusText() const
{
    juce::String operationText;
    switch (operation.load (std::memory_order_acquire))
    {
        case Operation::capturing: return "Capturing completed phrase...";
        case Operation::captured: operationText = "Captured phrase ready to save; "; break;
        case Operation::saving: return "Writing stereo float WAV...";
        case Operation::saved: operationText = "WAV saved; "; break;
        case Operation::empty:
            if (! recorder.hasLatest())
                operationText = "No completed phrase available to capture; ";
            break;
        case Operation::busy: operationText = "Capture busy; try again; "; break;
        case Operation::error:
        {
            const std::lock_guard lock (snapshotMutex);
            operationText = errorMessage + "; ";
            break;
        }
        case Operation::idle: break;
    }
    const auto previous = recorder.hasLatest() ? " (last full phrase available)" : "";
    switch (recorder.status())
    {
        case PhraseCapture::Status::empty: return operationText + "Play a complete phrase to enable Capture";
        case PhraseCapture::Status::recording: return operationText + "Recording a complete phrase..." + previous;
        case PhraseCapture::Status::ready: return operationText + "Last completed phrase ready to capture";
        case PhraseCapture::Status::interrupted: return operationText + "Waiting for a full phrase" + juce::String (previous);
        case PhraseCapture::Status::busy: return operationText + "Capture busy: new phrase skipped" + juce::String (previous);
        case PhraseCapture::Status::oversize: return operationText + "Phrase exceeds " + juce::String (recorder.capacitySeconds(), 1) + "s capacity" + previous;
    }
    return {};
}
} // namespace livecut
