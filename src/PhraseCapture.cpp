// SPDX-License-Identifier: GPL-3.0-or-later
#include "PhraseCapture.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <utility>

namespace livecut
{
struct PhraseCapture::Storage
{
    enum class Ownership { free, writing, ready, leased };
    struct Slot
    {
        std::atomic<Ownership> ownership { Ownership::free };
        std::array<std::vector<float>, 2> channels;
        std::size_t length = 0;
        bool spectralWarmup = false;
    };
    std::array<Slot, 2> slots;
    std::atomic<int> latest { -1 };
    double sampleRate = 0.0;
    std::size_t capacity = 0, written = 0;
    int writing = 0;
    bool recording = false, overflow = false;
    static_assert (std::atomic<Ownership>::is_always_lock_free);
    static_assert (std::atomic<int>::is_always_lock_free);
};

PhraseCapture::Lease::Lease (std::shared_ptr<Storage> storageIn, int slotIn)
    : owner (std::move (storageIn)), slot (slotIn) {}
PhraseCapture::Lease::Lease (Lease&& other) noexcept
    : owner (std::move (other.owner)), slot (other.slot) {}
PhraseCapture::Lease& PhraseCapture::Lease::operator= (Lease&& other) noexcept
{
    if (this != &other)
    {
        release();
        owner = std::move (other.owner);
        slot = other.slot;
    }
    return *this;
}
PhraseCapture::Lease::~Lease() { release(); }
void PhraseCapture::Lease::release()
{
    if (owner != nullptr)
    {
        owner->slots[static_cast<std::size_t> (slot)].ownership.store (Storage::Ownership::ready, std::memory_order_release);
        owner.reset();
    }
}
std::span<const float> PhraseCapture::Lease::channel (std::size_t index) const
{
    assert (owner != nullptr && index < 2);
    const auto& source = owner->slots[static_cast<std::size_t> (slot)];
    return { source.channels[index].data(), source.length };
}
double PhraseCapture::Lease::sampleRate() const { return owner != nullptr ? owner->sampleRate : 0.0; }

bool PhraseCapture::Lease::spectralWarmup() const
{
    return owner != nullptr && owner->slots[static_cast<std::size_t> (slot)].spectralWarmup;
}

PhraseCapture::Prepared PhraseCapture::prepareStorage (double sampleRate, std::size_t capacityFrames)
{
    assert (std::isfinite (sampleRate) && sampleRate > 0.0);
    auto prepared = std::make_shared<Storage>();
    prepared->sampleRate = sampleRate;
    prepared->capacity = capacityFrames != 0 ? std::min (capacityFrames, maxFrames)
                                            : static_cast<std::size_t> (std::min (std::ceil (maxSeconds * sampleRate), static_cast<double> (maxFrames)));
    for (auto& slot : prepared->slots)
        for (auto& channel : slot.channels)
            channel.resize (prepared->capacity);
    prepared->slots[0].ownership.store (Storage::Ownership::writing, std::memory_order_relaxed);
    Prepared result;
    result.storage = std::move (prepared);
    return result;
}

PhraseCapture::Prepared PhraseCapture::installPrepared (Prepared prepared)
{
    assert (prepared.storage != nullptr);
    const std::lock_guard lock (preparationMutex);
    storage.swap (prepared.storage);
    active = storage.get();
    available.store (false, std::memory_order_release);
    seconds.store (static_cast<double> (active->capacity) / active->sampleRate, std::memory_order_relaxed);
    currentStatus.store (Status::empty, std::memory_order_relaxed);
    return prepared;
}

void PhraseCapture::prepare (double sampleRate, std::size_t capacityFrames)
{
    auto retired = installPrepared (prepareStorage (sampleRate, capacityFrames));
}

void PhraseCapture::interrupt()
{
    if (active == nullptr)
        return;
    active->recording = false;
    active->written = 0;
    active->overflow = false;
    currentStatus.store (Status::interrupted, std::memory_order_relaxed);
}

void PhraseCapture::startPhrase (bool completeStart)
{
    if (active == nullptr)
        return;
    if (! completeStart)
    {
        interrupt();
        return;
    }
    completePhrase();
    auto& s = *active;
    if (! s.overflow && (! available.load (std::memory_order_relaxed) || status() == Status::interrupted))
        currentStatus.store (Status::recording, std::memory_order_relaxed);
    s.written = 0;
    s.overflow = false;
    s.slots[static_cast<std::size_t> (s.writing)].spectralWarmup = false;
    s.recording = true;
}

void PhraseCapture::completePhrase()
{
    if (active == nullptr)
        return;
    auto& s = *active;
    if (! s.recording)
        return;
    s.recording = false;
    if (s.overflow || s.written == 0)
        return;
    const auto next = 1 - s.writing;
    auto& nextSlot = s.slots[static_cast<std::size_t> (next)];
    auto ownership = nextSlot.ownership.load (std::memory_order_acquire);
    if (ownership != Storage::Ownership::leased
        && nextSlot.ownership.compare_exchange_strong (ownership, Storage::Ownership::writing,
                                                       std::memory_order_acq_rel))
    {
        auto& completed = s.slots[static_cast<std::size_t> (s.writing)];
        completed.length = s.written;
        completed.ownership.store (Storage::Ownership::ready, std::memory_order_release);
        s.latest.store (s.writing, std::memory_order_release);
        s.writing = next;
        available.store (true, std::memory_order_release);
        currentStatus.store (Status::ready, std::memory_order_relaxed);
    }
    else
        currentStatus.store (Status::busy, std::memory_order_relaxed);
}

void PhraseCapture::push (float left, float right, bool spectralWarmup)
{
    if (active == nullptr || ! active->recording || active->overflow)
        return;
    auto& s = *active;
    if (s.written == s.capacity)
    {
        s.overflow = true;
        currentStatus.store (Status::oversize, std::memory_order_relaxed);
        return;
    }
    auto& slot = s.slots[static_cast<std::size_t> (s.writing)];
    slot.spectralWarmup |= spectralWarmup;
    slot.channels[0][s.written] = left;
    slot.channels[1][s.written] = right;
    ++s.written;
}

PhraseCapture::Lease PhraseCapture::acquireLatest() const
{
    std::shared_ptr<Storage> source;
    {
        const std::lock_guard lock (preparationMutex);
        source = storage;
    }
    if (source == nullptr)
        return {};
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto latest = source->latest.load (std::memory_order_acquire);
        if (latest < 0)
            return {};
        auto& slot = source->slots[static_cast<std::size_t> (latest)];
        auto expected = Storage::Ownership::ready;
        if (! slot.ownership.compare_exchange_strong (expected, Storage::Ownership::leased, std::memory_order_acq_rel))
            continue;
        if (source->latest.load (std::memory_order_acquire) == latest)
            return { std::move (source), latest };
        slot.ownership.store (Storage::Ownership::ready, std::memory_order_release);
    }
    return {};
}
} // namespace livecut
