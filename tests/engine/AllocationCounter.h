// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace livecut::engine::testing
{

// Counts heap allocations made on this thread while it is alive. The test
// binary replaces the global operator new to do the counting.
// One-shot, thread-local allocation interleave for deterministic ownership tests.
class NextAllocation
{
public:
    NextAllocation (void (*callback) (void*), void* context);
    ~NextAllocation();
    NextAllocation (const NextAllocation&) = delete;
    NextAllocation& operator= (const NextAllocation&) = delete;
};

class AllocationCounter
{
public:
    AllocationCounter();
    ~AllocationCounter();

    AllocationCounter (const AllocationCounter&) = delete;
    AllocationCounter& operator= (const AllocationCounter&) = delete;

    std::size_t count() const;

private:
    std::size_t startCount;
};

} // namespace livecut::engine::testing
