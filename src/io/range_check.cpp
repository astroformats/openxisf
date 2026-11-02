// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "io/range_check.h"

#include <openxisf/error.h>

#include <string>

namespace openxisf::detail {

void check_read_range(std::uint64_t size, std::uint64_t offset, std::size_t length, std::string_view description)
{
    if (offset > size || length > size - offset) {
        const std::string source = description.empty() ? "the source" : std::string(description);
        throw io_error(errc::end_of_data,
                       "a read of " + std::to_string(length) + " bytes goes past the end of " + source +
                           ", which has " + std::to_string(size) + " bytes",
                       {}, {.offset = offset});
    }
}

void check_rewrite_range(std::uint64_t position, std::uint64_t offset, std::size_t length)
{
    if (offset > position || length > position - offset) {
        throw usage_error(errc::invalid_argument, "a rewrite of " + std::to_string(length) + " bytes at offset " +
                                                      std::to_string(offset) + " goes past the " +
                                                      std::to_string(position) + " bytes already written");
    }
}

void check_write_range(std::uint64_t size, std::uint64_t offset, std::size_t length, std::string_view description)
{
    if (offset > size || length > size - offset) {
        const std::string device = description.empty() ? "the device" : std::string(description);
        throw usage_error(errc::invalid_argument, "a write of " + std::to_string(length) + " bytes at offset " +
                                                      std::to_string(offset) + " goes past the end of " + device +
                                                      ", which has " + std::to_string(size) + " bytes");
    }
}

} // namespace openxisf::detail
