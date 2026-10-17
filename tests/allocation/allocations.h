// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstdint>
#include <new>
#include <utility>

// The memory that code asks for, counted, and allocations made to fail, by the operator new of
// openxisf_allocation_tests (allocations.cpp), which replaces that of the standard library in every test of that
// executable. Code that does not call it is neither counted nor failed: pugixml, which calls malloc(), and the library
// itself in a shared build on Windows, where it is a DLL with an operator new of its own. The internal code that the
// tests call directly is compiled into the test executable in every build.

namespace openxisf::test {

/// Makes operator new count the bytes that it is asked for, from zero.
void start_counting_allocations() noexcept;

/// Stops the count, and returns it.
[[nodiscard]] std::uint64_t stop_counting_allocations() noexcept;

/// Makes the call of operator new that follows `index` others, from now on, throw std::bad_alloc, once.
void fail_allocation(std::uint64_t index) noexcept;

/// Stops failing, and returns whether an allocation failed since fail_allocation().
[[nodiscard]] bool stop_failing_allocations() noexcept;

/// Whether the allocations of the library reach this operator new, which they do not in a shared build on Windows.
[[nodiscard]] bool counts_the_allocations_of_the_library();

/// Whether an allocation can fail without ending the program: not with the debug standard library of MSVC, whose
/// noexcept moves of containers allocate when iterators are checked.
[[nodiscard]] constexpr bool can_fail_allocations() noexcept
{
#if defined(_ITERATOR_DEBUG_LEVEL) && _ITERATOR_DEBUG_LEVEL != 0
    return false;
#else
    return true;
#endif
}

/// The bytes that operator new is asked for while function runs, by every thread, which in the tests that use it is
/// the calling thread alone.
template <typename Function> [[nodiscard]] std::uint64_t bytes_allocated_by(Function&& function)
{
    start_counting_allocations();
    try {
        std::forward<Function>(function)();
    } catch (...) {
        (void)stop_counting_allocations();
        throw;
    }
    return stop_counting_allocations();
}

/// Calls function with its first allocation failing, then with its second failing, and so on, and check after each
/// failure, until a call makes fewer allocations than the one that would fail. Returns the number of allocations made
/// to fail.
template <typename Function, typename Check>
std::uint64_t fail_each_allocation(const Function& function, const Check& check)
{
    for (std::uint64_t index = 0;; ++index) {
        fail_allocation(index);
        try {
            function();
        } catch (const std::bad_alloc&) {
            if (!stop_failing_allocations()) {
                throw;
            }
            check();
            continue;
        } catch (...) {
            (void)stop_failing_allocations();
            throw;
        }
        if (!stop_failing_allocations()) {
            return index;
        }
        // The function went on after its allocation failed, which it may.
        check();
    }
}

} // namespace openxisf::test
