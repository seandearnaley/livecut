// SPDX-License-Identifier: GPL-3.0-or-later

#include "AllocationCounter.h"

#include <cstdlib>
#include <new>

namespace
{
thread_local int countingScopes = 0;
thread_local std::size_t allocations = 0;
} // namespace

// The other forms of operator new and delete forward to these two by default.
void* operator new (std::size_t size)
{
    if (countingScopes > 0)
        ++allocations;

    if (auto* memory = std::malloc (size == 0 ? 1 : size))
        return memory;

    throw std::bad_alloc();
}

void operator delete (void* memory) noexcept
{
    std::free (memory);
}

namespace livecut::engine::testing
{

AllocationCounter::AllocationCounter() : startCount (allocations)
{
    ++countingScopes;
}

AllocationCounter::~AllocationCounter()
{
    --countingScopes;
}

std::size_t AllocationCounter::count() const
{
    return allocations - startCount;
}

} // namespace livecut::engine::testing
