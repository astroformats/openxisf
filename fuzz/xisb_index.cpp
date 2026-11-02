// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Data blocks files (spec §9.4). The input starts with a byte n, taken modulo 8, and n identifiers of 8 bytes in
// little-endian byte order; the rest is a data blocks file.
//
// Its block index is read for those identifiers: it gives elements of those identifiers only, or throws the error of
// an invalid file or of the limits of nodes and elements. Then a header file whose images name the file with each
// identifier, and once as a whole, is opened with a resolver that serves it, and every image whose block is available
// reads exactly the bytes its index element points to; every other block fails with the error that was reported. Any
// other exception escapes and fails the run.

#include <openxisf/error.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>

#include "container/block_attributes.h"
#include "container/blocks_file.h"
#include "container/data_block.h"
#include "core/utf8.h"
#include "model/unit.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
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

// Limits that inputs reach, above the 1,100 elements of the largest seed.
constexpr openxisf::limits small_limits{
    .max_allocation = std::uint64_t{1} << 20, .max_index_nodes = 64, .max_index_elements = 1'200};

struct fuzz_input
{
    std::vector<std::uint64_t> ids{};
    std::span<const std::uint8_t> file{};
};

fuzz_input split(std::span<const std::uint8_t> data)
{
    fuzz_input input;
    if (data.empty()) {
        return input;
    }
    const std::size_t count = data.front() % 8U;
    data = data.subspan(1);
    for (std::size_t i = 0; i < count && data.size() >= 8; ++i) {
        std::uint64_t id = 0;
        for (std::size_t b = 0; b < 8; ++b) {
            id |= std::uint64_t{data[b]} << (8U * b);
        }
        input.ids.push_back(id);
        data = data.subspan(8);
    }
    input.file = data;
    return input;
}

std::span<const std::byte> bytes_of(std::span<const std::uint8_t> data) noexcept
{
    return std::as_bytes(data);
}

void read_index(const fuzz_input& input)
{
    const std::unordered_set<std::uint64_t> wanted(input.ids.begin(), input.ids.end());
    const detail::thread_safe_source source(std::make_unique<openxisf::memory_source>(bytes_of(input.file)));
    try {
        const detail::block_index index = detail::read_block_index(source, wanted, small_limits);
        for (const detail::index_element& element : index.elements) {
            require(wanted.contains(element.id) && index.find(element.id) == &element);
        }
        for (const std::uint64_t id : index.duplicates) {
            require(index.find(id) != nullptr && index.is_duplicate(id));
        }
    } catch (const openxisf::invalid_data_error& failure) {
        require(failure.code() == openxisf::errc::invalid_blocks_file);
    } catch (const openxisf::limit_error& failure) {
        require(failure.code() == openxisf::errc::too_many_index_nodes ||
                failure.code() == openxisf::errc::too_many_index_elements);
    }
}

// A header file with an image for each identifier, every second one compressed so that the uncompressed length of
// its element matters, and an image that is the whole file.
std::string header_of(const fuzz_input& input)
{
    std::string body;
    for (std::size_t i = 0; i < input.ids.size(); ++i) {
        body +=
            R"(<Image geometry="1:1:1" sampleFormat="UInt8" colorSpace="Gray" location="path(@header_dir/f.xisb):)" +
            detail::format_index_id(input.ids[i]) + "\"" + (i % 2 == 1 ? R"( compression="zlib:100")" : "") + "/>";
    }
    body += R"x(<Image geometry="1:1:1" sampleFormat="UInt8" colorSpace="Gray" location="path(@header_dir/f.xisb)"/>)x";
    return R"(<?xml version="1.0" encoding="UTF-8"?><xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">)" +
           body + "</xisf>";
}

void check_available(const detail::unit& opened, const detail::data_block& block,
                     const detail::block_descriptor& descriptor)
{
    require(descriptor.external != nullptr);
    const detail::block_location& location = descriptor.location;
    require(location.position <= descriptor.external->size() &&
            location.size <= descriptor.external->size() - location.position);
    require(!location.index_id || location.position >= detail::blocks_file_header_size);
    try {
        require(detail::read_stored_block(opened.source, block, opened.limits).size() == location.size);
    } catch (const openxisf::limit_error& failure) {
        require(failure.code() == openxisf::errc::allocation_too_large);
    }
}

void open_unit(const fuzz_input& input)
{
    const std::string header = header_of(input);
    const std::span<const std::byte> content = bytes_of(input.file);
    const std::vector<std::byte> file(content.begin(), content.end());
    const openxisf::read_options options{
        .resolver =
            [&file](const openxisf::external_reference& reference) {
                require(reference.form == openxisf::location_form::relative_path && reference.location == "f.xisb");
                return std::make_unique<openxisf::memory_source>(std::span<const std::byte>(file));
            },
        .limits = small_limits};
    const detail::unit opened(std::make_unique<openxisf::memory_source>(std::as_bytes(std::span(header))), options);
    for (const openxisf::diagnostic& entry : opened.diagnostics) {
        require(!entry.message.empty() && detail::is_valid_utf8(entry.message));
    }
    for (const detail::data_block& block : opened.blocks) {
        if (block.descriptor) {
            check_available(opened, block, *block.descriptor);
            continue;
        }
        require(block.problem.has_value() && block.problem->severity == openxisf::severity::error);
        try {
            (void)detail::read_stored_block(opened.source, block, opened.limits);
            std::abort();
        } catch (const openxisf::error& failure) {
            require(block.problem.has_value() && failure.code() == block.problem->code);
        }
    }
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const fuzz_input input = split(std::span(data, size));
    read_index(input);
    open_unit(input);
    return 0;
}
