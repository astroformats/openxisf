// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "allocation/allocations.h"

#include <openxisf/format.h>
#include <openxisf/property.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>

namespace {

// Whether operator new counts, and what it has counted; whether it fails, the allocations before the one that fails,
// and whether one failed. They are constants until a test counts or fails, so that operator new can read them from
// the first static initializer on; a thread_local, read that early, crashed Clang 15 on MinGW.
std::atomic<bool> counting{false};
std::atomic<std::uint64_t> counted{0};
std::atomic<bool> failing{false};
std::atomic<std::uint64_t> before_failure{0};
std::atomic<bool> failed{false};

} // namespace

namespace openxisf::test {

void start_counting_allocations() noexcept
{
    counted.store(0, std::memory_order_relaxed);
    counting.store(true, std::memory_order_seq_cst);
}

std::uint64_t stop_counting_allocations() noexcept
{
    counting.store(false, std::memory_order_seq_cst);
    return counted.load(std::memory_order_relaxed);
}

void fail_allocation(std::uint64_t index) noexcept
{
    before_failure.store(index, std::memory_order_relaxed);
    failed.store(false, std::memory_order_relaxed);
    failing.store(true, std::memory_order_seq_cst);
}

bool stop_failing_allocations() noexcept
{
    failing.store(false, std::memory_order_seq_cst);
    return failed.load(std::memory_order_relaxed);
}

bool counts_the_allocations_of_the_library()
{
    // A String longer than any small-string buffer, formatted by the library into a string that it allocates.
    const property_value value(std::string(256, 'a'));
    std::string text;
    const std::uint64_t allocated = bytes_allocated_by([&value, &text] { text = format_value(value); });
    return allocated != 0;
}

} // namespace openxisf::test

// The replaceable allocation functions of the test executable. The memory comes from malloc(), as that of the standard
// libraries does, so that a block allocated on one side of a DLL, such as that of GoogleTest, and freed on the other is
// still freed by the function that matches. The array and nothrow forms are replaced too, though those of the standard
// libraries call these, since a sanitizer brings forms of its own: the nothrow operator new of AddressSanitizer, which
// GoogleTest calls as it runs a suite, gave memory of its allocator to the free() of these.

void* operator new(std::size_t size)
{
    if (counting.load(std::memory_order_relaxed)) {
        counted.fetch_add(size, std::memory_order_relaxed);
    }
    if (failing.load(std::memory_order_relaxed)) {
        if (before_failure.load(std::memory_order_relaxed) == 0) {
            failing.store(false, std::memory_order_relaxed);
            failed.store(true, std::memory_order_relaxed);
            throw std::bad_alloc();
        }
        before_failure.fetch_sub(1, std::memory_order_relaxed);
    }
    // An allocation function gets its memory from malloc().
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    if (void* block = std::malloc(size == 0 ? 1 : size)) {
        return block;
    }
    throw std::bad_alloc();
}

void operator delete(void* block) noexcept
{
    // The memory of operator new comes from malloc().
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(block);
}

void operator delete(void* block, std::size_t /*size*/) noexcept
{
    // The memory of operator new comes from malloc().
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    std::free(block);
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    try {
        return ::operator new(size);
    } catch (const std::bad_alloc&) {
        return nullptr;
    }
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, tag);
}

void operator delete(void* block, const std::nothrow_t& /*tag*/) noexcept
{
    ::operator delete(block);
}

void operator delete[](void* block) noexcept
{
    ::operator delete(block);
}

void operator delete[](void* block, std::size_t /*size*/) noexcept
{
    ::operator delete(block);
}

void operator delete[](void* block, const std::nothrow_t& /*tag*/) noexcept
{
    ::operator delete(block);
}
