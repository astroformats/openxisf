// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace openxisf::detail {

/// Throws io_error with errc::end_of_data unless the length bytes at offset lie within a source of size bytes. The
/// description names the source in the message.
void check_read_range(std::uint64_t size, std::uint64_t offset, std::size_t length, std::string_view description);

/// Throws usage_error unless the length bytes at offset lie within the position bytes a sink has written.
void check_rewrite_range(std::uint64_t position, std::uint64_t offset, std::size_t length);

/// Throws usage_error unless the length bytes at offset lie within a device of size bytes, whose size a write never
/// changes. The description names the device in the message.
void check_write_range(std::uint64_t size, std::uint64_t offset, std::size_t length, std::string_view description);

} // namespace openxisf::detail
