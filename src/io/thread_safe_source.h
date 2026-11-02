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

/// A source as the library uses it: any number of threads may read at once. Reads are checked against the size before
/// they reach the source, and a source that does not support concurrent reads gets them one at a time.
class thread_safe_source
{
public:
    /// Takes the source, and asks it for its size, its bytes in memory and its description once. Throws usage_error
    /// when source is null, or when it gives bytes in memory of another size than its own. shared_with_writers says
    /// that other threads may write bytes of the source while it is read (see shared_with_writers()).
    explicit thread_safe_source(std::unique_ptr<input_source> source, bool shared_with_writers = false);

    [[nodiscard]] std::uint64_t size() const noexcept
    {
        return size_;
    }

    /// The content of the source in memory, as input_source::bytes() gives it, which any number of threads may read at
    /// once; empty when it is read through read() alone.
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept
    {
        return bytes_;
    }

    [[nodiscard]] const std::string& description() const noexcept
    {
        return description_;
    }

    /// True when other threads may write bytes of the source while it is read, as they may write the device of an
    /// editor: a read of a region then reads the bytes of its runs alone, never through the bytes between them.
    [[nodiscard]] bool shared_with_writers() const noexcept
    {
        return shared_with_writers_;
    }

    /// Reads like input_source::read(). A read beyond the size is an io_error with errc::end_of_data, which the source
    /// never sees. Exceptions of the source pass through unchanged.
    void read(std::uint64_t offset, std::span<std::byte> destination) const;

private:
    std::unique_ptr<input_source> source_;
    std::uint64_t size_;
    std::span<const std::byte> bytes_;
    std::string description_;
    bool concurrent_;
    bool shared_with_writers_;
    mutable std::mutex mutex_;
};

} // namespace openxisf::detail
