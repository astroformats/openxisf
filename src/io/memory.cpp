// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/io.h>

#include "core/checked_math.h"
#include "io/range_check.h"

#include <algorithm>
#include <utility>

namespace openxisf {

memory_source::memory_source(std::span<const std::byte> data) noexcept : data_(data) {}

memory_source::memory_source(std::vector<std::byte> data) noexcept : owned_(std::move(data)), data_(owned_) {}

std::uint64_t memory_source::size() const
{
    return data_.size();
}

void memory_source::read(std::uint64_t offset, std::span<std::byte> destination) const
{
    detail::check_read_range(data_.size(), offset, destination.size(), {});
    std::ranges::copy(data_.subspan(detail::checked_cast<std::size_t>(offset), destination.size()),
                      destination.begin());
}

bool memory_source::supports_concurrent_reads() const
{
    return true;
}

std::span<const std::byte> memory_source::bytes() const
{
    return data_;
}

void memory_sink::write(std::span<const std::byte> data)
{
    data_.insert(data_.end(), data.begin(), data.end());
}

std::uint64_t memory_sink::position() const
{
    return data_.size();
}

bool memory_sink::can_rewrite() const
{
    return true;
}

void memory_sink::rewrite(std::uint64_t offset, std::span<const std::byte> data)
{
    detail::check_rewrite_range(data_.size(), offset, data.size());
    std::ranges::copy(data, data_.begin() + static_cast<std::ptrdiff_t>(offset));
}

std::span<const std::byte> memory_sink::data() const noexcept
{
    return data_;
}

std::vector<std::byte> memory_sink::release() noexcept
{
    return std::exchange(data_, {});
}

memory_device::memory_device(std::span<std::byte> data) noexcept : data_(data) {}

std::uint64_t memory_device::size() const
{
    return data_.size();
}

void memory_device::read(std::uint64_t offset, std::span<std::byte> destination) const
{
    detail::check_read_range(data_.size(), offset, destination.size(), {});
    std::ranges::copy(data_.subspan(detail::checked_cast<std::size_t>(offset), destination.size()),
                      destination.begin());
}

void memory_device::write(std::uint64_t offset, std::span<const std::byte> data)
{
    detail::check_write_range(data_.size(), offset, data.size(), {});
    std::ranges::copy(data, data_.begin() + static_cast<std::ptrdiff_t>(offset));
}

bool memory_device::supports_concurrent_reads() const
{
    return true;
}

bool memory_device::supports_concurrent_writes() const
{
    return true;
}

} // namespace openxisf
