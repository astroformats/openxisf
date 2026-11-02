// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "io/thread_safe_source.h"

#include <openxisf/error.h>

#include "io/range_check.h"

#include <string>
#include <utility>

namespace openxisf::detail {

namespace {

std::unique_ptr<input_source> require(std::unique_ptr<input_source> source)
{
    if (!source) {
        throw usage_error(errc::invalid_argument, "the source is null");
    }
    return source;
}

// The content of source in memory, which is all of it when there is any.
std::span<const std::byte> bytes_of(const input_source& source, std::uint64_t size)
{
    const std::span<const std::byte> bytes = source.bytes();
    if (!bytes.empty() && bytes.size() != size) {
        throw usage_error(errc::invalid_argument, "the source gives " + std::to_string(bytes.size()) +
                                                      " bytes in memory, and its size is " + std::to_string(size));
    }
    return bytes;
}

} // namespace

thread_safe_source::thread_safe_source(std::unique_ptr<input_source> source, bool shared_with_writers)
    : source_(require(std::move(source))), size_(source_->size()), bytes_(bytes_of(*source_, size_)),
      description_(source_->description()), concurrent_(source_->supports_concurrent_reads()),
      shared_with_writers_(shared_with_writers)
{}

void thread_safe_source::read(std::uint64_t offset, std::span<std::byte> destination) const
{
    check_read_range(size_, offset, destination.size(), description_);
    if (destination.empty()) {
        return;
    }
    if (concurrent_) {
        source_->read(offset, destination);
    } else {
        const std::scoped_lock lock(mutex_);
        source_->read(offset, destination);
    }
}

} // namespace openxisf::detail
