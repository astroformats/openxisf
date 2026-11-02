// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// What an untrusted header costs in memory, through the public reader, and what an opened unit keeps of it.

#include <openxisf/error.h>
#include <openxisf/reader.h>

#include "allocation/allocations.h"
#include "codec/compressed_block.h"
#include "codec/compression.h"
#include "container/block_attributes.h"
#include "core/parallel.h"
#include "model/pixels.h"
#include "model/unit.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::reader;
using openxisf::test::bytes;
using openxisf::test::bytes_allocated_by;
using openxisf::test::header_xml;

// Attributes of a data block made of separators, 20 MiB each: a location, a compression and a list of subblocks.
TEST(reader, attributes_of_separators_take_no_memory_in_proportion_to_their_length)
{
    if (!openxisf::test::counts_the_allocations_of_the_library()) {
        GTEST_SKIP() << "the allocations of the library are not counted in this build";
    }
    const std::string separators(std::size_t{20} << 20, ':');
    const auto image = [](std::string_view attributes) {
        return R"(<Image geometry="1:1:1" sampleFormat="UInt8" colorSpace="Gray" )" + std::string(attributes) + "/>";
    };
    const std::string header =
        header_xml(image(R"(location="attachment:)" + separators + "\"") +
                   image(R"(location="attachment:{0}" compression="zlib)" + separators + "\"") +
                   image(R"(location="attachment:{0}" compression="zlib:1" subblocks=")" + separators + "\""));
    std::vector<std::byte> unit = openxisf::test::file_with_attachments(header, {bytes("\x01")});

    std::vector<openxisf::diagnostic> diagnostics;
    const std::uint64_t allocated = bytes_allocated_by([&unit, &diagnostics] {
        const reader file = openxisf::test::open_unit(std::move(unit));
        diagnostics.assign(file.diagnostics().begin(), file.diagnostics().end());
    });
    ASSERT_EQ(diagnostics.size(), 3U) << openxisf::test::describe(diagnostics);
    EXPECT_EQ(diagnostics[0].code, errc::invalid_location);
    EXPECT_EQ(diagnostics[1].code, errc::invalid_compression);
    EXPECT_EQ(diagnostics[2].code, errc::invalid_subblocks);
    // The text of the header is read once.
    EXPECT_LT(allocated, 2 * header.size());
}

// An image of 256 MiB whose compressed data block has 9 bytes, which no codec decodes to that much: the block is
// unavailable from the open, and a read of its samples allocates none of them.
TEST(reader, a_few_stored_bytes_cannot_make_a_read_allocate_what_they_declare)
{
    if (!openxisf::test::counts_the_allocations_of_the_library()) {
        GTEST_SKIP() << "the allocations of the library are not counted in this build";
    }
    for (const std::string_view codec : {"zlib", "lz4", "lz4hc", "zstd"}) {
        SCOPED_TRACE(codec);
        const std::string header = header_xml(
            R"(<Image geometry="16384:16384:1" sampleFormat="UInt8" colorSpace="Gray" location="attachment:{0}" )"
            R"(compression=")" +
            std::string(codec) + R"(:268435456"/>)");
        const reader file =
            openxisf::test::open_unit(openxisf::test::file_with_attachments(header, {bytes("123456789")}));
        ASSERT_EQ(file.diagnostics().size(), 1U) << openxisf::test::describe(file.diagnostics());
        EXPECT_EQ(file.diagnostics().front().code, errc::invalid_subblocks);
        const std::uint64_t allocated = bytes_allocated_by([&file] {
            EXPECT_TRUE(openxisf::test::throws<openxisf::invalid_data_error>(
                errc::invalid_subblocks, [&file] { (void)file.read_pixels<std::uint8_t>(0); }));
            EXPECT_TRUE(openxisf::test::throws<openxisf::invalid_data_error>(errc::invalid_subblocks,
                                                                             [&file] { (void)file.read_pixels(0); }));
        });
        EXPECT_LT(allocated, std::uint64_t{1} << 20);
    }
}

// limits.h gives what an image takes in the reader, about 2 KB, for limits::max_model_objects. A copy of what a unit
// of 1,000 images keeps asks for that memory, without the room that its vectors keep for growth: 1.6 KB for each
// image with libc++, 1.75 KB with libstdc++ and the standard library of MSVC, 2.15 KB in a Debug build of MSVC.
TEST(reader, an_image_takes_about_2_kb_in_the_reader)
{
    if (!openxisf::test::counts_the_allocations_of_the_library()) {
        GTEST_SKIP() << "the allocations of the library are not counted in this build";
    }
    constexpr std::uint64_t count = 1'000;
    std::string body;
    for (std::uint64_t i = 0; i < count; ++i) {
        body += R"(<Image geometry="1:1:1" sampleFormat="UInt8" colorSpace="Gray" location="attachment:{0}"/>)";
    }
    const openxisf::detail::unit opened =
        openxisf::test::open_internal(openxisf::test::file_with_attachments(header_xml(body), {bytes("\x01")}));
    ASSERT_EQ(opened.images.infos.size(), count);
    ASSERT_TRUE(openxisf::test::no_diagnostics(opened.diagnostics));

    const std::uint64_t kept = bytes_allocated_by([&opened] {
        const std::vector<openxisf::detail::data_block> blocks = opened.blocks;
        const openxisf::detail::unit_properties properties = opened.properties;
        const openxisf::detail::unit_images images = opened.images;
        const std::vector<openxisf::diagnostic> diagnostics = opened.diagnostics;
    });
    EXPECT_GT(kept, count * 1'400);
    EXPECT_LT(kept, count * 2'600);
}

// A unit of one UInt16 image of width x height pixels and channels, of zeros, in the storage model of storage, and
// compressed with zlib in subblocks of subblock_size bytes unless it is 0.
std::vector<std::byte> zeros_of(std::uint64_t width, std::uint64_t height, std::uint64_t channels,
                                std::string_view storage, std::uint64_t subblock_size = 0)
{
    std::vector<std::byte> stored(width * height * channels * 2);
    std::string attributes = R"(geometry=")" + std::to_string(width) + ":" + std::to_string(height) + ":" +
                             std::to_string(channels) + R"(" sampleFormat="UInt16" pixelStorage=")" +
                             std::string(storage) + "\"";
    if (subblock_size != 0) {
        openxisf::detail::compressed_block block = openxisf::detail::compress_block(
            stored, {.codec = openxisf::detail::compression_codec::zlib, .max_subblock_size = subblock_size});
        stored = std::move(block.data);
        attributes += R"( compression=")" + openxisf::detail::format_compression(block.compression) +
                      R"(" subblocks=")" + openxisf::detail::format_subblocks(block.compression.subblocks) + "\"";
    }
    return openxisf::test::file_with_attachments(header_xml("<Image " + attributes + R"( location="attachment:{0}"/>)"),
                                                 {stored});
}

// A region read run by run, straight into memory of the caller, allocates the description of the region alone: rows
// of 80,000 bytes, of which the region reads 20 each.
TEST(reader, a_region_read_run_by_run_allocates_nothing_but_its_description)
{
    const openxisf::detail::unit opened = openxisf::test::open_file_like(zeros_of(40000, 64, 1, "Planar"));
    std::vector<std::byte> destination(std::size_t{10} * 64 * 2);
    const std::uint64_t allocated = bytes_allocated_by([&opened, &destination] {
        openxisf::detail::read_pixels(opened, 0, destination,
                                      {.region = openxisf::pixel_region{.origin = {100, 0}, .size = {10, 64}}});
    });
    EXPECT_LT(allocated, 1024U);
}

// The runs of a region read together go through one staging buffer of the piece size, whatever the size of the region:
// one channel of a tile of 512 x 512 pixels in normal storage spans 3 MiB, read in pieces of 64 KiB. From a source in
// memory they are taken in place, without the buffer.
TEST(reader, the_runs_of_a_region_read_together_take_one_staging_buffer)
{
    constexpr std::size_t piece = std::size_t{64} << 10;
    const std::vector<std::byte> unit = zeros_of(1024, 1024, 3, "Normal");
    const auto allocated_by = [&unit](bool in_memory) {
        const openxisf::detail::unit opened =
            in_memory ? openxisf::test::open_internal(unit) : openxisf::test::open_file_like(unit);
        std::vector<std::byte> destination(std::size_t{512} * 512 * 2);
        return bytes_allocated_by([&opened, &destination] {
            openxisf::detail::read_pixels(opened, 0, destination,
                                          {.region = openxisf::pixel_region{.origin = {256, 256},
                                                                            .size = {512, 512},
                                                                            .first_channel = 1,
                                                                            .channel_count = 1}},
                                          piece);
        });
    };
    const std::uint64_t from_file = allocated_by(false);
    EXPECT_GE(from_file, piece);
    EXPECT_LT(from_file, piece + 1024);
    EXPECT_LT(allocated_by(true), 1024U);
}

// A region of a compressed block decodes a batch of subblocks at a time, as many as the threads, into one buffer, and
// reads their stored bytes into another: a column band 10 pixels wide over all 1024 rows of an image in subblocks of
// 64 KiB, 32 rows each, decodes every subblock with the memory of one batch.
TEST(reader, a_region_of_a_compressed_block_takes_one_batch_of_subblocks_whatever_its_height)
{
    constexpr std::uint64_t subblock = std::uint64_t{64} << 10;
    const openxisf::detail::unit opened = openxisf::test::open_file_like(zeros_of(1024, 1024, 1, "Planar", subblock));
    const auto allocated_by = [&opened](std::uint64_t height) {
        std::vector<std::byte> destination(10 * height * 2);
        return bytes_allocated_by([&opened, &destination, height] {
            openxisf::detail::read_pixels(opened, 0, destination,
                                          {.region = openxisf::pixel_region{.origin = {500, 0}, .size = {10, height}}});
        });
    };
    const std::uint64_t batch = std::min<std::uint64_t>(openxisf::detail::concurrency(), 32);
    const std::uint64_t tall = allocated_by(1024);
    // A batch of decoded subblocks, and their stored bytes, which zeros make small.
    EXPECT_LT(tall, (batch * (subblock + 1024)) + 16384);
    // As much as a band of one batch, and a little more for the list of the subblocks to decode and the positions of
    // those of each batch.
    EXPECT_LT(tall, allocated_by(32 * batch) + 16384);
}

} // namespace
