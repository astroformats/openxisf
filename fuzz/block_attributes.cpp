// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The attributes of data blocks. The input holds, one per line: location, byteOrder, checksum, compression, subblocks,
// the element (Image, unless the line starts with P for a Property or C for an ICCProfile), the encoding of a Data
// element (none when the line is empty), and the character data of the element or of its Data element; whatever
// follows is attached to the file after the header. An empty line leaves its attribute out. The Property is a ByteArray
// without a length attribute, which the reader takes from its block, with a warning, so that the reader loads and
// decodes a block of any size as the value of the property.
//
// Each parser takes its line: it returns a value that respects its invariants, or throws the error of a malformed or an
// unsupported value. Then a unit with that element is opened: every block that is available reads, and what it returns
// matches its descriptor and its checksum, and decompresses to its uncompressed size or fails as corrupt data; every
// block that is not available fails with the error that was reported. Any other exception escapes and fails the run.

#include "container/block_attributes.h"

#include <openxisf/error.h>

#include "container/data_block.h"
#include "core/utf8.h"
#include "crypto/hash.h"
#include "model/unit.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace detail = openxisf::detail;

// Stops the run, so that the fuzzer reports the input that broke a property.
void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

constexpr std::size_t field_count = 8;

struct fields
{
    std::array<std::string_view, field_count> lines{};
    std::string_view payload{};
};

fields split(std::string_view input)
{
    fields result;
    for (std::string_view& line : result.lines) {
        const std::size_t end = input.find('\n');
        line = input.substr(0, end);
        input = end == std::string_view::npos ? std::string_view() : input.substr(end + 1);
    }
    result.payload = input;
    return result;
}

// Calls parse, and checks the result with check. A malformed value is an invalid_data_error, and an unsupported one an
// unsupported_error, with one of the codes of the parser; nothing else may escape.
template <typename Parse, typename Check>
void parse_one(Parse parse, Check check, std::initializer_list<openxisf::errc> codes)
{
    try {
        check(parse());
    } catch (const openxisf::invalid_data_error& failure) {
        require(std::ranges::find(codes, failure.code()) != codes.end());
    } catch (const openxisf::unsupported_error& failure) {
        require(std::ranges::find(codes, failure.code()) != codes.end());
    }
}

void parse_each(const fields& input)
{
    parse_one([&input] { return detail::parse_location(input.lines[0]); },
              [](const detail::block_location& location) {
                  const bool external =
                      location.kind == detail::location_kind::url || location.kind == detail::location_kind::path;
                  require(external != location.reference.empty());
                  require(external || !location.index_id);
                  require(location.kind != detail::location_kind::path || location.reference.starts_with('/') ||
                          location.reference.starts_with("@header_dir/"));
              },
              {openxisf::errc::invalid_location});
    parse_one([&input] { return detail::parse_byte_order(input.lines[1]); }, [](detail::byte_order) {},
              {openxisf::errc::invalid_byte_order});
    parse_one([&input] { return detail::parse_checksum(input.lines[2]); },
              [](const detail::block_checksum& checksum) {
                  require(checksum.digest.size() == detail::digest_size(checksum.algorithm));
              },
              {openxisf::errc::invalid_checksum, openxisf::errc::unsupported_checksum});
    parse_one([&input] { return detail::parse_compression(input.lines[3]); },
              [](const detail::block_compression& compression) { require(compression.subblocks.empty()); },
              {openxisf::errc::invalid_compression, openxisf::errc::unsupported_compression});
    parse_one([&input] { return detail::parse_subblocks(input.lines[4]); },
              [](const std::vector<detail::subblock>& subblocks) { require(!subblocks.empty()); },
              {openxisf::errc::invalid_subblocks});
    parse_one([&input] { return detail::parse_encoding(input.lines[6]); }, [](detail::block_encoding) {},
              {openxisf::errc::invalid_location});
}

std::string escaped(std::string_view text)
{
    std::string result;
    for (const char c : text) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result += c;
        }
    }
    return result;
}

std::string attribute(std::string_view name, std::string_view value)
{
    return value.empty() ? std::string() : " " + std::string(name) + "=\"" + escaped(value) + "\"";
}

// A monolithic file with one element that has the attributes of the input, followed by the payload.
std::vector<std::byte> unit_of(const fields& input)
{
    std::string start = R"(<Image geometry="1:1:1" sampleFormat="UInt8" colorSpace="Gray")";
    std::string end = "</Image>";
    if (input.lines[5].starts_with('P')) {
        start = R"(<Property id="Fuzz:Value" type="ByteArray")";
        end = "</Property>";
    } else if (input.lines[5].starts_with('C')) {
        start = "<ICCProfile";
        end = "</ICCProfile>";
    }
    std::string element = start + attribute("location", input.lines[0]) + attribute("byteOrder", input.lines[1]) +
                          attribute("checksum", input.lines[2]) + attribute("compression", input.lines[3]) +
                          attribute("subblocks", input.lines[4]) + ">";
    if (input.lines[6].empty()) {
        element += escaped(input.lines[7]);
    } else {
        element += "<Data" + attribute("encoding", input.lines[6]) + ">" + escaped(input.lines[7]) + "</Data>";
    }
    element += end;

    const std::string header =
        R"(<?xml version="1.0" encoding="UTF-8"?><xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">)" +
        element + "</xisf>";
    std::vector<std::byte> file;
    for (const char c : std::string_view("XISF0100")) {
        file.push_back(static_cast<std::byte>(c));
    }
    for (std::size_t i = 0; i < 4; ++i) {
        file.push_back(static_cast<std::byte>((header.size() >> (8 * i)) & 0xFFU));
    }
    file.resize(16);
    for (const char c : header + std::string(input.payload)) {
        file.push_back(static_cast<std::byte>(c));
    }
    return file;
}

std::optional<std::uint64_t> sum(const std::vector<detail::subblock>& subblocks, bool compressed)
{
    std::uint64_t total = 0;
    for (const detail::subblock& part : subblocks) {
        const std::uint64_t size = compressed ? part.compressed_size : part.uncompressed_size;
        if (size > UINT64_MAX - total) {
            return std::nullopt;
        }
        total += size;
    }
    return total;
}

void check_decompression(const detail::unit& opened, const detail::data_block& block,
                         const detail::block_compression& compression)
{
    try {
        require(detail::read_block(opened.source, block, opened.limits).size() == compression.uncompressed_size);
    } catch (const openxisf::integrity_error& failure) {
        require(failure.code() == openxisf::errc::corrupt_compressed_data);
    } catch (const openxisf::limit_error& failure) {
        require(failure.code() == openxisf::errc::allocation_too_large ||
                failure.code() == openxisf::errc::zstd_window_too_large);
    }
}

void check_available(const detail::unit& opened, const detail::data_block& block,
                     const detail::block_descriptor& descriptor)
{
    std::vector<std::byte> stored;
    try {
        stored = detail::read_stored_block(opened.source, block, opened.limits);
    } catch (const openxisf::integrity_error& failure) {
        // Only an attached block is verified when it is read.
        require(failure.code() == openxisf::errc::checksum_mismatch);
        require(descriptor.location.kind == detail::location_kind::attachment && descriptor.checksum.has_value());
        return;
    } catch (const openxisf::limit_error& failure) {
        require(failure.code() == openxisf::errc::allocation_too_large);
        return;
    }

    if (descriptor.location.kind == detail::location_kind::attachment) {
        require(stored.size() == descriptor.location.size);
        require(descriptor.location.position + descriptor.location.size <= opened.source.size());
    }
    if (descriptor.checksum) {
        require(detail::compute_digest(descriptor.checksum->algorithm, stored) == descriptor.checksum->digest);
    }
    if (descriptor.compression && !descriptor.compression->subblocks.empty()) {
        require(sum(descriptor.compression->subblocks, false) == descriptor.compression->uncompressed_size);
        require(sum(descriptor.compression->subblocks, true) == stored.size());
    }
    if (descriptor.compression) {
        check_decompression(opened, block, *descriptor.compression);
    }
}

void check_unavailable(const detail::unit& opened, const detail::data_block& block, const openxisf::diagnostic& problem)
{
    require(problem.severity == openxisf::severity::error);
    try {
        (void)detail::read_stored_block(opened.source, block, opened.limits);
    } catch (const openxisf::error& failure) {
        require(failure.code() == problem.code);
        return;
    }
    std::abort();
}

// The unit, or nothing when it is refused as a whole: invalid data, an unsupported feature or a limit.
std::unique_ptr<detail::unit> open(std::span<const std::byte> unit)
{
    const openxisf::read_options options{.limits = {.max_header_size = std::uint64_t{1} << 20,
                                                    .max_allocation = std::uint64_t{1} << 20,
                                                    .max_ancillary_data = std::uint64_t{1} << 20,
                                                    .max_zstd_window = std::uint64_t{1} << 16}};
    try {
        return std::make_unique<detail::unit>(std::make_unique<openxisf::memory_source>(unit), options);
    } catch (const openxisf::invalid_data_error&) {
        return nullptr;
    } catch (const openxisf::unsupported_error&) {
        return nullptr;
    } catch (const openxisf::limit_error&) {
        return nullptr;
    }
}

void open_and_read(std::span<const std::byte> unit)
{
    const std::unique_ptr<detail::unit> opened = open(unit);
    if (!opened) {
        return;
    }
    for (const openxisf::diagnostic& entry : opened->diagnostics) {
        require(!entry.message.empty() && detail::is_valid_utf8(entry.message));
    }
    for (const detail::data_block& block : opened->blocks) {
        // A block is available, or has the problem that makes it unavailable.
        if (block.descriptor) {
            check_available(*opened, block, *block.descriptor);
        } else if (block.problem) {
            check_unavailable(*opened, block, *block.problem);
        } else {
            std::abort();
        }
    }
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const fields input = split(std::string_view(reinterpret_cast<const char*>(data), size));
    parse_each(input);
    const std::vector<std::byte> unit = unit_of(input);
    open_and_read(unit);
    return 0;
}
