// Test entry point plus counting replacements of the global allocation functions (ADA-RT-ALLOC).
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "TestSupport.h"

#include <cstdlib>
#include <new>

namespace adatest
{
thread_local bool countAllocations = false;
std::atomic<long> allocationCount { 0 };
}

namespace
{
void count() noexcept
{
    if (adatest::countAllocations)
        adatest::allocationCount.fetch_add (1, std::memory_order_relaxed);
}

void* allocate (std::size_t n)
{
    count();
    if (void* p = std::malloc (n == 0 ? 1 : n))
        return p;
    throw std::bad_alloc();
}

void* allocateAligned (std::size_t n, std::align_val_t a)
{
    count();
#if defined(_WIN32)
    if (void* p = _aligned_malloc (n == 0 ? 1 : n, (std::size_t) a))
        return p;
#else
    const auto align = (std::size_t) a;
    if (void* p = std::aligned_alloc (align, ((n == 0 ? 1 : n) + align - 1) / align * align))
        return p;
#endif
    throw std::bad_alloc();
}

void release (void* p) noexcept
{
    if (p != nullptr)
        count();
    std::free (p);
}

void releaseAligned (void* p) noexcept
{
    if (p != nullptr)
        count();
#if defined(_WIN32)
    _aligned_free (p);
#else
    std::free (p);
#endif
}
} // namespace

void* operator new (std::size_t n) { return allocate (n); }
void* operator new[] (std::size_t n) { return allocate (n); }
void* operator new (std::size_t n, const std::nothrow_t&) noexcept
{
    try { return allocate (n); } catch (...) { return nullptr; }
}
void* operator new[] (std::size_t n, const std::nothrow_t&) noexcept
{
    try { return allocate (n); } catch (...) { return nullptr; }
}
void* operator new (std::size_t n, std::align_val_t a) { return allocateAligned (n, a); }
void* operator new[] (std::size_t n, std::align_val_t a) { return allocateAligned (n, a); }

void operator delete (void* p) noexcept { release (p); }
void operator delete[] (void* p) noexcept { release (p); }
void operator delete (void* p, std::size_t) noexcept { release (p); }
void operator delete[] (void* p, std::size_t) noexcept { release (p); }
void operator delete (void* p, const std::nothrow_t&) noexcept { release (p); }
void operator delete[] (void* p, const std::nothrow_t&) noexcept { release (p); }
void operator delete (void* p, std::align_val_t) noexcept { releaseAligned (p); }
void operator delete[] (void* p, std::align_val_t) noexcept { releaseAligned (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept { releaseAligned (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { releaseAligned (p); }
