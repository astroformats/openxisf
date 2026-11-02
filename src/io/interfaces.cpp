// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/error.h>
#include <openxisf/io.h>

namespace openxisf {

// The destructors are the key functions: defining them here emits the type information once, in the library, so that
// the classes are the same type on both sides of a shared-library boundary.

input_source::~input_source() = default;

bool input_source::supports_concurrent_reads() const
{
    return false;
}

std::string input_source::description() const
{
    return {};
}

std::span<const std::byte> input_source::bytes() const
{
    return {};
}

output_sink::~output_sink() = default;

bool output_sink::can_rewrite() const
{
    return false;
}

void output_sink::rewrite(std::uint64_t /*offset*/, std::span<const std::byte> /*data*/)
{
    throw usage_error(errc::invalid_argument, "this sink cannot rewrite");
}

void output_sink::finish() {}

random_access_device::~random_access_device() = default;

void random_access_device::flush() {}

bool random_access_device::supports_concurrent_writes() const
{
    return false;
}

} // namespace openxisf
