// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace livecut
{

// One immutable, complete phrase, in processing-rate stereo float samples.
// Capture and file jobs own snapshots independently of the processor/editor.
struct CapturedPhrase
{
    double sampleRate = 0.0;
    std::array<std::vector<float>, 2> channels;
    std::size_t size() const { return channels[0].size(); }
};

// Single audio producer, one non-audio reader. Two prepared buffers avoid a
// callback copy: the reader leases the published slot while the other records.
// A lease spanning a boundary skips that candidate visibly instead of waiting.
class PhraseCapture
{
    struct Storage;

public:
    static constexpr double maxSeconds = 64.0;
    static constexpr std::size_t maxFrames = 64 * 192000;
    enum class Status { empty, recording, ready, interrupted, busy, oversize };

    class Lease
    {
    public:
        Lease() = default;
        Lease (Lease&&) noexcept;
        Lease& operator= (Lease&&) noexcept;
        ~Lease();
        explicit operator bool() const { return owner != nullptr; }
        std::span<const float> channel (std::size_t index) const;
        double sampleRate() const;

    private:
        friend class PhraseCapture;
        Lease (std::shared_ptr<Storage>, int);
        void release();
        std::shared_ptr<Storage> owner;
        int slot = -1;
    };

    // Build storage off the callback guard, install only during exclusion.
    // Returned retired storage is destroyed by the caller after releasing it.
    class Prepared
    {
    public:
        Prepared() = default;
        Prepared (Prepared&&) noexcept = default;
        Prepared& operator= (Prepared&&) noexcept = default;
        Prepared (const Prepared&) = delete;
        Prepared& operator= (const Prepared&) = delete;

    private:
        friend class PhraseCapture;
        std::shared_ptr<Storage> storage;
    };
    static Prepared prepareStorage (double sampleRate, std::size_t capacityFrames = 0);
    Prepared installPrepared (Prepared prepared);

    // Host lifecycle only; never concurrent with process/push/startPhrase.
    // Existing leases retain old storage across prepare and destruction.
    void prepare (double sampleRate, std::size_t capacityFrames = 0);
    void startPhrase (bool completeStart);
    void push (float left, float right);
    void completePhrase();
    void interrupt();

    // Non-audio thread. No sample memory is read until the ownership CAS wins.
    Lease acquireLatest() const;
    bool hasLatest() const { return available.load (std::memory_order_acquire); }
    Status status() const { return currentStatus.load (std::memory_order_relaxed); }
    double capacitySeconds() const { return seconds.load (std::memory_order_relaxed); }

private:
    mutable std::mutex preparationMutex;
    std::shared_ptr<Storage> storage;
    Storage* active = nullptr; // host-synchronised prepare/process lifecycle
    std::atomic<Status> currentStatus { Status::empty };
    std::atomic<bool> available { false };
    std::atomic<double> seconds { 0.0 };
    static_assert (std::atomic<Status>::is_always_lock_free);
};

} // namespace livecut
