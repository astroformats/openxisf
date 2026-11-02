// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/limits.h>
#include <openxisf/reader.h>

#include "core/diagnostic_log.h"
#include "io/thread_safe_source.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Where the header of a unit is: after the first 16 bytes of a monolithic file (spec §9.2), or the whole of a header
// file (spec §9.3).

namespace openxisf::detail {

/// The first bytes of a monolithic file (spec §9.2).
inline constexpr std::string_view monolithic_signature = "XISF0100";

/// Where the header of a monolithic file starts: after the signature, the header length and the reserved field.
inline constexpr std::uint64_t monolithic_header_offset = 16;

/// The least header length of a monolithic file: 65 bytes, which the code of spec §9.2 gives an empty header, from
/// "<?xml" to "</xisf>". A well-formed header can be shorter, with a self-closing root element.
inline constexpr std::uint32_t min_header_length = 65;

/// How the unit that starts with the bytes start is stored: monolithic after the signature of a monolithic file, and
/// distributed after XML, which an optional UTF-8 byte order mark may precede; empty for neither, which
/// read_unit_header() refuses.
[[nodiscard]] std::optional<unit_storage> storage_of(std::string_view start) noexcept;

/// The header of a unit as found in its source, not yet parsed.
struct unit_header
{
    unit_storage storage = unit_storage::monolithic;
    /// Where the header starts in the source.
    std::uint64_t offset = 0;
    /// Where the header ends in the source: the first byte after the header length of a monolithic file, where
    /// attached blocks can start, and the end of a header file.
    std::uint64_t end = 0;
    /// The XML of the header, without the zero bytes that a header length may count after it.
    std::string text{};
};

/// Reads the header of the unit in source. A source that starts with the signature of a monolithic file holds its
/// header after the first 16 bytes. A source that starts with XML, after an optional UTF-8 byte order mark, is a header
/// file, which is all header. Only the content decides, never a file name.
///
/// Throws invalid_data_error when the source is neither (errc::not_an_xisf_unit), or when the header length of a
/// monolithic file is below the minimum or beyond the end of the file (errc::invalid_header_length). Throws limit_error
/// when the header is larger than limits.max_header_size.
[[nodiscard]] unit_header read_unit_header(const thread_safe_source& source, const limits& limits, diagnostic_log& log);

} // namespace openxisf::detail
