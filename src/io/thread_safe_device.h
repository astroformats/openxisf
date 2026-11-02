// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/io.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>

namespace openxisf::detail {

/// A device as the editor uses it: any number of threads may read and write at once, at ranges that do not overlap.
/// Ranges are checked against the size before they reach the device, and a device that does not support concurrent
/// writes gets every call one at a time, its reads included.
class thread_safe_device
{
public:
    /// Takes the device, and asks it for its size, its description and what it supports once. Throws usage_error when
    /// device is null.
    explicit thread_safe_device(std::unique_ptr<random_access_device> device);

    [[nodiscard]] std::uint64_t size() const noexcept
    {
        return size_;
    }

    [[nodiscard]] const std::string& description() const noexcept
    {
        return description_;
    }

    /// Reads like input_source::read(). A read beyond the size is an io_error with errc::end_of_data, which the device
    /// never sees. Exceptions of the device pass through unchanged.
    void read(std::uint64_t offset, std::span<std::byte> destination) const;

    /// Writes like random_access_device::write(). A write beyond the size is a usage_error, which the device never
    /// sees. Exceptions of the device pass through unchanged.
    void write(std::uint64_t offset, std::span<const std::byte> data) const;

    /// Flushes like random_access_device::flush().
    void flush() const;

    /// A source that reads through this device, for a unit opened from it, which must not outlive the device.
    [[nodiscard]] std::unique_ptr<input_source> source() const;

private:
    // The calls that take the mutex: every one, unless the device supports concurrent writes; then the reads alone,
    // unless it supports concurrent reads too.
    [[nodiscard]] bool locks_reads() const noexcept
    {
        return !concurrent_writes_ || !concurrent_reads_;
    }

    std::unique_ptr<random_access_device> device_;
    std::uint64_t size_;
    std::string description_;
    bool concurrent_reads_;
    bool concurrent_writes_;
    mutable std::mutex mutex_;
};

} // namespace openxisf::detail
