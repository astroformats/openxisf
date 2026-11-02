// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "container/file_layout.h"

#include <openxisf/error.h>

#include "core/checked_math.h"
#include "core/endian.h"
#include "xml/xml_document.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace openxisf::detail {

namespace {

constexpr std::string_view utf8_byte_order_mark = "\xEF\xBB\xBF";

// The header length and the reserved field of a monolithic file.
constexpr std::uint64_t header_length_offset = 8;
constexpr std::uint64_t reserved_field_offset = 12;

std::string read_text(const thread_safe_source& source, std::uint64_t offset, std::uint64_t length)
{
    std::string text(checked_cast<std::size_t>(length), '\0');
    source.read(offset, std::as_writable_bytes(std::span(text)));
    return text;
}

void check_header_size(std::uint64_t size, const limits& limits)
{
    if (limits.max_header_size != 0 && size > limits.max_header_size) {
        throw limit_error(errc::header_too_large, "the header has " + std::to_string(size) +
                                                      " bytes, more than the limit of " +
                                                      std::to_string(limits.max_header_size));
    }
}

// XML starts with a tag, or with the white space that may precede one when there is no XML declaration.
bool starts_with_xml(std::string_view text) noexcept
{
    if (text.starts_with(utf8_byte_order_mark)) {
        text.remove_prefix(utf8_byte_order_mark.size());
    }
    return !text.empty() && (text.front() == '<' || is_xml_white_space(text.front()));
}

unit_header read_monolithic_header(const thread_safe_source& source, std::span<const std::byte, 16> preamble,
                                   const limits& limits, diagnostic_log& log)
{
    if (load_little_endian<std::uint32_t>(preamble.subspan<12, 4>()) != 0) {
        log.warning(errc::reserved_field_not_zero, "the reserved field of the file is not zero, and is ignored",
                    {.offset = reserved_field_offset});
    }

    const auto length = load_little_endian<std::uint32_t>(preamble.subspan<8, 4>());
    if (length < min_header_length) {
        throw invalid_data_error(errc::invalid_header_length,
                                 "the header length is " + std::to_string(length) + " bytes, less than the " +
                                     std::to_string(min_header_length) + " of the shortest header",
                                 {.offset = header_length_offset});
    }
    if (length > source.size() - monolithic_header_offset) {
        throw invalid_data_error(errc::invalid_header_length,
                                 "the header length of " + std::to_string(length) +
                                     " bytes goes past the end of the file, which has " +
                                     std::to_string(source.size()) + " bytes",
                                 {.offset = header_length_offset});
    }
    check_header_size(length, limits);

    unit_header header{.storage = unit_storage::monolithic,
                       .offset = monolithic_header_offset,
                       .end = monolithic_header_offset + length,
                       .text = read_text(source, monolithic_header_offset, length)};

    // Zeros that a writer counted in the header length, as if they were part of it: harmless, and they would make the
    // XML invalid.
    const std::size_t last = header.text.find_last_not_of('\0');
    const std::size_t xml_length = last == std::string::npos ? 0 : last + 1;
    if (xml_length < header.text.size()) {
        log.warning(errc::invalid_header_length,
                    "the header length counts " + std::to_string(header.text.size() - xml_length) +
                        " zero bytes after the XML, which are ignored",
                    {.offset = header_length_offset});
        header.text.resize(xml_length);
    }
    return header;
}

} // namespace

std::optional<unit_storage> storage_of(std::string_view start) noexcept
{
    if (start.starts_with(monolithic_signature)) {
        return unit_storage::monolithic;
    }
    if (starts_with_xml(start)) {
        return unit_storage::distributed;
    }
    return std::nullopt;
}

unit_header read_unit_header(const thread_safe_source& source, const limits& limits, diagnostic_log& log)
{
    std::array<char, monolithic_header_offset> preamble{};
    const std::size_t available = static_cast<std::size_t>(std::min<std::uint64_t>(source.size(), preamble.size()));
    source.read(0, std::as_writable_bytes(std::span(preamble).first(available)));
    const std::optional<unit_storage> storage = storage_of(std::string_view(preamble.data(), available));

    if (storage == unit_storage::monolithic) {
        if (available < preamble.size()) {
            throw invalid_data_error(errc::invalid_header_length, "the file ends before its header length",
                                     {.offset = header_length_offset});
        }
        return read_monolithic_header(source, std::as_bytes(std::span(preamble)), limits, log);
    }
    if (!storage) {
        throw invalid_data_error(errc::not_an_xisf_unit,
                                 "the source is not an XISF unit: it starts neither with the signature XISF0100 nor "
                                 "with XML");
    }
    check_header_size(source.size(), limits);
    return {.storage = unit_storage::distributed,
            .offset = 0,
            .end = source.size(),
            .text = read_text(source, 0, source.size())};
}

} // namespace openxisf::detail
