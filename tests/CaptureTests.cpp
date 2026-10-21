// SPDX-License-Identifier: GPL-3.0-or-later
#include "CaptureService.h"
#include "OutputMixer.h"
#include "PhraseCapture.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using livecut::PhraseCapture;
using Catch::Matchers::WithinAbs;

namespace
{
void phrase (PhraseCapture& capture, float value, int samples = 8)
{
    for (int i = 0; i < samples; ++i)
        capture.push (value, -value);
    capture.startPhrase (true);
}
bool awaitWorker (livecut::CaptureService& service)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    while (service.isBusy() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    return ! service.isBusy();
}
} // namespace

TEST_CASE ("Capture exposes only complete bounded phrases", "[capture]")
{
    PhraseCapture capture;
    capture.prepare (48000.0, 8);
    capture.startPhrase (false);
    phrase (capture, 0.25f);
    CHECK_FALSE (capture.acquireLatest());
    phrase (capture, 0.5f);
    auto lease = capture.acquireLatest();
    REQUIRE (lease);
    REQUIRE (lease.channel (0).size() == 8);
    CHECK (lease.sampleRate() == 48000.0);
    for (std::size_t i = 0; i < 8; ++i)
    {
        CHECK (lease.channel (0)[i] == 0.5f);
        CHECK (lease.channel (1)[i] == -0.5f);
    }
    lease = {};
    phrase (capture, 0.75f, 9);
    CHECK (capture.status() == PhraseCapture::Status::oversize);
    auto previous = capture.acquireLatest();
    REQUIRE (previous);
    CHECK (previous.channel (0)[0] == 0.5f);
}

TEST_CASE ("Stop and seek discard partial audio and retain the last complete phrase", "[capture]")
{
    PhraseCapture capture;
    capture.prepare (48000.0, 16);
    capture.startPhrase (true);
    capture.push (0.25f, 0.25f);
    capture.interrupt();
    CHECK_FALSE (capture.acquireLatest());
    capture.startPhrase (true);
    phrase (capture, 0.5f);
    capture.push (1.0f, 1.0f);
    capture.interrupt();
    auto stopped = capture.acquireLatest();
    REQUIRE (stopped);
    CHECK (stopped.channel (0).size() == 8);
    CHECK (stopped.channel (0)[0] == 0.5f);
}

TEST_CASE ("A leased phrase never changes when publication is busy", "[capture][ownership]")
{
    PhraseCapture capture;
    capture.prepare (48000.0, 16);
    capture.startPhrase (true);
    phrase (capture, 0.25f);
    auto lease = capture.acquireLatest();
    REQUIRE (lease);
    for (int i = 0; i < 20; ++i)
        phrase (capture, 0.5f);
    CHECK (capture.status() == PhraseCapture::Status::busy);
    for (const auto value : lease.channel (0))
        CHECK (value == 0.25f);
    lease = {};
    phrase (capture, 0.75f);
    auto next = capture.acquireLatest();
    REQUIRE (next);
    CHECK (next.channel (0)[0] == 0.75f);
}

TEST_CASE ("Concurrent capture leases survive publication and preparation", "[capture][ownership]")
{
    PhraseCapture capture;
    capture.prepare (48000.0, 32);
    capture.startPhrase (true);
    phrase (capture, 1.0f, 32);
    std::atomic<bool> done { false }, valid { true };
    std::atomic<int> leases { 0 };
    std::thread reader ([&]
    {
        while (! done.load())
        {
            auto lease = capture.acquireLatest();
            if (! lease)
                continue;
            ++leases;
            const auto left = lease.channel (0);
            const auto right = lease.channel (1);
            if (left.size() != 32 || right.size() != 32)
                valid = false;
            const auto first = left[0];
            std::this_thread::yield();
            for (std::size_t i = 0; i < left.size(); ++i)
                if (left[i] != first || right[i] != -first)
                    valid = false;
        }
    });
    for (int iteration = 0; iteration < 10000; ++iteration)
    {
        phrase (capture, static_cast<float> (iteration + 2), 32);
        if (iteration % 100 == 0)
            std::this_thread::yield();
    }
    done = true;
    reader.join();
    CHECK (valid.load());
    CHECK (leases.load() > 0);
    auto old = capture.acquireLatest();
    REQUIRE (old);
    const auto oldSample = old.channel (0)[0];
    capture.prepare (96000.0, 16);
    CHECK (old.sampleRate() == 48000.0);
    CHECK (old.channel (0)[0] == oldSample);
    CHECK_FALSE (capture.acquireLatest());
}

TEST_CASE ("Captured snapshots are independent and export exact stereo float WAV", "[capture][wav]")
{
    livecut::CaptureService service;
    auto& capture = service.rolling();
    capture.prepare (96000.0, 64);
    CHECK_FALSE (service.requestCapture());
    CHECK (service.statusText().contains ("No completed"));
    capture.startPhrase (true);
    for (int i = 0; i < 64; ++i)
        capture.push (static_cast<float> (i) / 8.0f, -static_cast<float> (i) / 16.0f);
    capture.startPhrase (true);
    REQUIRE (service.requestCapture());
    REQUIRE (awaitWorker (service));
    const auto snapshot = service.captured();
    REQUIRE (snapshot != nullptr);
    capture.interrupt();
    capture.prepare (48000.0, 8);
    CHECK (snapshot->sampleRate == 96000.0);
    CHECK (snapshot->size() == 64);
    juce::TemporaryFile file (".wav");
    REQUIRE (service.requestSave (file.getFile()));
    REQUIRE (awaitWorker (service));
    CHECK (service.statusText().startsWith ("WAV saved"));
    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (file.getFile().createInputStream().release(), true));
    REQUIRE (reader != nullptr);
    CHECK (reader->usesFloatingPointData);
    CHECK (reader->bitsPerSample == 32);
    CHECK (reader->numChannels == 2);
    CHECK (reader->sampleRate == 96000.0);
    REQUIRE (reader->lengthInSamples == 64);
    juce::AudioBuffer<float> restored (2, 64);
    REQUIRE (reader->read (&restored, 0, 64, 0, true, true));
    for (int i = 0; i < 64; ++i)
    {
        CHECK (restored.getSample (0, i) == snapshot->channels[0][static_cast<std::size_t> (i)]);
        CHECK (restored.getSample (1, i) == snapshot->channels[1][static_cast<std::size_t> (i)]);
    }
    REQUIRE (service.requestSave (file.getFile().getChildFile ("not-a-directory.wav")));
    REQUIRE (awaitWorker (service));
    CHECK (service.statusText().contains ("Cannot create"));
}

TEST_CASE ("Linear mixing has exact endpoints and smooths stereo automation", "[capture][mixer]")
{
    livecut::OutputMixer mixer;
    mixer.prepare (1000.0, 1.0f, 0.0f);
    float left = 0.1234567f, right = -0.7654321f;
    mixer.process (1.0f, -1.0f, left, right);
    CHECK (left == 0.1234567f);
    CHECK (right == -0.7654321f);
    mixer.setTargets (0.0f, 0.0f);
    for (int i = 0; i < 20; ++i)
    {
        left = right = 0.0f;
        mixer.process (1.0f, -1.0f, left, right);
        CHECK_THAT (left, WithinAbs ((i + 1) / 20.0, 1.0e-6));
        CHECK (right == -left);
    }
    CHECK (left == 1.0f);
    CHECK (right == -1.0f);
    mixer.prepare (1000.0, 0.5f, -6.0205999f);
    left = 0.0f;
    right = 1.0f;
    mixer.process (1.0f, 0.0f, left, right);
    CHECK_THAT (left, WithinAbs (0.25, 1.0e-6));
    CHECK_THAT (right, WithinAbs (0.25, 1.0e-6));
}

TEST_CASE ("Capture status follows new readiness and exposes capacity after saving", "[capture][status]")
{
    livecut::CaptureService service;
    auto& capture = service.rolling();
    capture.prepare (48000.0, 8);
    CHECK_FALSE (service.requestCapture());
    capture.startPhrase (true);
    phrase (capture, 0.5f);
    CHECK (service.statusText().contains ("ready to capture"));
    REQUIRE (service.requestCapture());
    REQUIRE (awaitWorker (service));
    juce::TemporaryFile file (".wav");
    REQUIRE (service.requestSave (file.getFile()));
    REQUIRE (awaitWorker (service));
    phrase (capture, 0.75f, 9);
    CHECK (service.statusText().contains ("exceeds"));
    CHECK (service.statusText().contains ("last full phrase available"));
}

TEST_CASE ("Saving preserves the exact selected filename", "[capture][wav]")
{
    livecut::CapturedPhrase phrase;
    phrase.sampleRate = 48000.0;
    phrase.channels[0] = { 0.25f };
    phrase.channels[1] = { -0.25f };
    juce::TemporaryFile chosen (".txt");
    const auto sibling = chosen.getFile().withFileExtension ("wav");
    REQUIRE (sibling.replaceWithText ("previous WAV must stay untouched"));
    REQUIRE (livecut::CaptureService::writeWav (phrase, chosen.getFile()).wasOk());
    CHECK (chosen.getFile().existsAsFile());
    CHECK (sibling.loadFileAsString() == "previous WAV must stay untouched");
    REQUIRE (sibling.deleteFile());
}
