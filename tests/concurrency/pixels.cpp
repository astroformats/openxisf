// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Pixel reads from 16 threads at once on one reader, whose images are attached, compressed and embedded, on a source
// that supports concurrent reads and on one that the library serializes, and in a data blocks file; and reads of
// regions. ThreadSanitizer runs these tests in CI.

#include <openxisf/io.h>
#include <openxisf/reader.h>
#include <openxisf/types.h>

#include "codec/compressed_block.h"
#include "codec/compression.h"
#include "container/block_attributes.h"
#include "core/xoshiro.h"
#include "crypto/hash.h"
#include "support/bytes.h"
#include "support/cursor_source.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"
#include "support/regions.h"
#include "support/threads.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::pixel_storage;
using openxisf::reader;
using openxisf::test::cursor_source;

constexpr std::size_t thread_count = 16;
constexpr int reads_per_thread = 40;
constexpr std::size_t width = 64;
constexpr std::size_t height = 48;
constexpr std::size_t channels = 3;

// The samples of an RGB UInt16 image in a storage model: 1000c + p for pixel p of channel c, wrapped to 16 bits.
std::vector<std::uint16_t> values(pixel_storage storage)
{
    constexpr std::size_t pixels = width * height;
    std::vector<std::uint16_t> result(pixels * channels);
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t p = 0; p < pixels; ++p) {
            result[storage == pixel_storage::planar ? (c * pixels) + p : (p * channels) + c] =
                static_cast<std::uint16_t>((1000 * c) + p);
        }
    }
    return result;
}

// The samples in native byte order, as read_pixels() returns them.
std::vector<std::byte> samples(pixel_storage storage)
{
    const std::vector<std::uint16_t> native = values(storage);
    std::vector<std::byte> bytes(native.size() * sizeof(std::uint16_t));
    std::memcpy(bytes.data(), native.data(), bytes.size());
    return bytes;
}

// The data blocks of the first two images: the planar samples in little-endian byte order, and the same compressed
// with zlib and byte shuffling.
std::vector<std::vector<std::byte>> image_blocks()
{
    std::vector<std::byte> planar;
    for (const std::uint16_t value : values(pixel_storage::planar)) {
        planar.push_back(static_cast<std::byte>(value & 0xFFU));
        planar.push_back(static_cast<std::byte>(value >> 8U));
    }
    openxisf::detail::compressed_block compressed =
        openxisf::detail::compress_block(planar, {.codec = openxisf::detail::compression_codec::zlib, .item_size = 2});
    return {std::move(planar), std::move(compressed.data)};
}

// The header of three images: the blocks of image_blocks() at the given locations, and an image of two pixels
// embedded.
std::string header_of_three_images(std::string_view first, std::string_view second)
{
    const std::string geometry = R"(geometry=")" + std::to_string(width) + ":" + std::to_string(height) + R"(:3")";
    return openxisf::test::header_xml(
        "<Image " + geometry + R"( sampleFormat="UInt16" colorSpace="RGB" location=")" + std::string(first) + R"("/>)" +
        "<Image " + geometry + R"( sampleFormat="UInt16" colorSpace="RGB" location=")" + std::string(second) +
        R"(" compression="zlib+sh:)" + std::to_string(width * height * channels * 2) + R"(:2"/>)" +
        R"(<Image geometry="2:1:1" sampleFormat="UInt8" location="embedded"><Data encoding="hex">0102</Data></Image>)");
}

// A monolithic unit of the three images, the first two attached.
std::vector<std::byte> unit_of_three_images()
{
    return openxisf::test::file_with_attachments(header_of_three_images("attachment:{0}", "attachment:{1}"),
                                                 image_blocks());
}

// Every thread reads images at random in both storage models, into a vector or into memory of its own, and compares
// them with the expected bytes. Returns the number of wrong reads, failed reads included.
int read_from_threads(const reader& file)
{
    const std::vector<std::byte> planar = samples(pixel_storage::planar);
    const std::vector<std::byte> normal = samples(pixel_storage::normal);
    const std::vector<std::byte> small{std::byte{1}, std::byte{2}};
    std::atomic<int> wrong{0};
    openxisf::test::run_threads(thread_count, [&](std::size_t t) {
        openxisf::detail::xoshiro256starstar random({4, 5, 6, t + 1});
        std::vector<std::byte> destination;
        for (int i = 0; i < reads_per_thread; ++i) {
            const std::size_t index = random() % 3;
            const pixel_storage storage = random() % 2 == 0 ? pixel_storage::planar : pixel_storage::normal;
            const std::vector<std::byte>& expected =
                index == 2 ? small : (storage == pixel_storage::planar ? planar : normal);
            try {
                if (random() % 2 == 0) {
                    destination = file.read_pixels(index, {.storage = storage});
                } else {
                    destination.assign(expected.size(), std::byte{0xEE});
                    file.read_pixels(index, destination, {.storage = storage});
                }
                if (destination != expected) {
                    ++wrong;
                }
            } catch (const std::exception&) {
                ++wrong;
            }
        }
    });
    return wrong.load();
}

TEST(concurrency_pixels, threads_read_the_pixels_of_one_reader_at_once)
{
    const reader file(std::make_unique<openxisf::memory_source>(unit_of_three_images()));
    ASSERT_EQ(file.images().size(), 3U);
    EXPECT_EQ(read_from_threads(file), 0);
}

TEST(concurrency_pixels, a_source_without_concurrent_reads_is_read_one_thread_at_a_time)
{
    const std::vector<std::byte> unit = unit_of_three_images();
    auto source = std::make_unique<cursor_source>(unit);
    const cursor_source& cursor = *source;
    const reader file(std::move(source));
    ASSERT_EQ(file.images().size(), 3U);
    EXPECT_EQ(read_from_threads(file), 0);
    EXPECT_FALSE(cursor.overlapped());
}

// A unit of three images of the samples of values() for regions: in normal storage, attached; in planar storage,
// compressed with byte shuffling in subblocks of 1 KiB; and in planar storage with a checksum, attached.
std::vector<std::byte> unit_for_regions()
{
    const auto little_endian = [](pixel_storage storage) {
        std::vector<std::byte> bytes;
        for (const std::uint16_t value : values(storage)) {
            bytes.push_back(static_cast<std::byte>(value & 0xFFU));
            bytes.push_back(static_cast<std::byte>(value >> 8U));
        }
        return bytes;
    };
    const std::vector<std::byte> planar = little_endian(pixel_storage::planar);
    const openxisf::detail::compressed_block compressed = openxisf::detail::compress_block(
        planar, {.codec = openxisf::detail::compression_codec::zlib, .item_size = 2, .max_subblock_size = 1024});
    const openxisf::detail::block_checksum checksum{
        .algorithm = openxisf::detail::hash_algorithm::sha1,
        .digest = openxisf::detail::compute_digest(openxisf::detail::hash_algorithm::sha1, planar)};
    const std::string geometry = R"(geometry=")" + std::to_string(width) + ":" + std::to_string(height) + R"(:3")";
    return openxisf::test::file_with_attachments(
        openxisf::test::header_xml(
            "<Image " + geometry + R"( sampleFormat="UInt16" colorSpace="RGB" pixelStorage="Normal")" +
            R"( location="attachment:{0}"/>)" + "<Image " + geometry +
            R"( sampleFormat="UInt16" colorSpace="RGB" location="attachment:{1}" compression=")" +
            openxisf::detail::format_compression(compressed.compression) + R"(" subblocks=")" +
            openxisf::detail::format_subblocks(compressed.compression.subblocks) + R"("/>)" + "<Image " + geometry +
            R"( sampleFormat="UInt16" colorSpace="RGB" location="attachment:{2}" checksum=")" +
            openxisf::detail::format_checksum(checksum) + R"("/>)"),
        {little_endian(pixel_storage::normal), compressed.data, planar});
}

TEST(concurrency_pixels, threads_read_regions_of_one_reader_at_once)
{
    // Every thread reads regions of every image at random, in both storage models, with and without a pitch, into a
    // vector or into memory of its own, and compares them with the regions of the whole image: from a source in
    // memory, whose bytes they take in place, and from one read as a file is.
    for (const bool in_memory : {true, false}) {
        const reader file(in_memory ? std::unique_ptr<openxisf::input_source>(
                                          std::make_unique<openxisf::memory_source>(unit_for_regions()))
                                    : openxisf::test::file_like_source(unit_for_regions()));
        ASSERT_EQ(file.images().size(), 3U);
        ASSERT_TRUE(file.diagnostics().empty());
        const openxisf::geometry image{.dimensions = {width, height}, .channels = channels};
        std::atomic<int> wrong{0};
        openxisf::test::run_threads(thread_count, [&](std::size_t t) {
            openxisf::detail::xoshiro256starstar random({7, 8, 9, t + 1});
            for (int i = 0; i < reads_per_thread; ++i) {
                const std::size_t index = random() % 3;
                const pixel_storage stored = index == 0 ? pixel_storage::normal : pixel_storage::planar;
                openxisf::pixel_region region{.origin = {random() % width, random() % height}};
                region.size = {1 + (random() % (width - region.origin[0])),
                               1 + (random() % (height - region.origin[1]))};
                region.first_channel = random() % channels;
                region.channel_count = 1 + (random() % (channels - region.first_channel));
                const pixel_storage storage = random() % 2 == 0 ? pixel_storage::planar : pixel_storage::normal;
                const std::uint64_t row =
                    region.size[0] * (storage == pixel_storage::normal ? region.channel_count : 1);
                const std::uint64_t pitch = random() % 2 == 0 ? 0 : row + (random() % 5);
                const std::vector<std::byte> expected =
                    openxisf::test::region_of(samples(stored), image, 2, stored, region, storage, pitch);
                try {
                    const openxisf::pixel_read_options options{.storage = storage, .region = region, .pitch = pitch};
                    std::vector<std::byte> destination;
                    if (random() % 2 == 0) {
                        destination = file.read_pixels(index, options);
                    } else {
                        destination.assign(expected.size(), std::byte{0});
                        file.read_pixels(index, destination, options);
                    }
                    if (destination != expected) {
                        ++wrong;
                    }
                } catch (const std::exception&) {
                    ++wrong;
                }
            }
        });
        EXPECT_EQ(wrong.load(), 0) << (in_memory ? "in memory" : "read as a file is");
    }
}

TEST(concurrency_pixels, threads_read_the_external_blocks_of_one_reader_at_once)
{
    // A distributed unit, whose data blocks file the library reads one thread at a time.
    const std::vector<std::vector<std::byte>> data = image_blocks();
    const std::uint64_t second = 112 + data[0].size();
    const std::vector<std::byte> blocks = openxisf::test::blocks_file(
        {{.elements =
              {{.id = 0x1111, .position = 112, .length = data[0].size()},
               {.id = 0x2222, .position = second, .length = data[1].size(), .uncompressed_length = data[0].size()}}}},
        {{.position = 112, .data = data[0]}, {.position = second, .data = data[1]}});
    const std::string header =
        header_of_three_images("path(@header_dir/unit.xisb):0x1111", "path(@header_dir/unit.xisb):0x2222");
    // The reader keeps the source that the resolver returns as long as it exists.
    const cursor_source* cursor = nullptr;
    const reader file(std::make_unique<openxisf::memory_source>(openxisf::test::bytes(header)),
                      {.resolver = [&blocks, &cursor](const openxisf::external_reference&) {
                          auto source = std::make_unique<cursor_source>(blocks);
                          cursor = source.get();
                          return source;
                      }});
    ASSERT_EQ(file.images().size(), 3U);
    ASSERT_TRUE(file.diagnostics().empty());
    ASSERT_NE(cursor, nullptr);
    EXPECT_EQ(read_from_threads(file), 0);
    EXPECT_FALSE(cursor->overlapped());
}

TEST(concurrency_pixels, threads_open_the_files_of_a_header_only_reader_once_as_they_read)
{
    // The first read of an image in a file opens it, while the threads that read the file too wait for it.
    const std::vector<std::vector<std::byte>> data = image_blocks();
    const std::vector<std::byte> first = openxisf::test::blocks_file(
        {{.elements = {{.id = 1, .position = 112, .length = data[0].size()}}}}, {{.position = 112, .data = data[0]}});
    const std::vector<std::byte> second = openxisf::test::blocks_file(
        {{.elements = {{.id = 2, .position = 112, .length = data[1].size(), .uncompressed_length = data[0].size()}}}},
        {{.position = 112, .data = data[1]}});
    const std::string header = header_of_three_images("path(@header_dir/a.xisb):1", "path(@header_dir/b.xisb):2");
    std::atomic<int> opened{0};
    const reader file(
        std::make_unique<openxisf::memory_source>(openxisf::test::bytes(header)),
        {.header_only = true, .resolver = [&first, &second, &opened](const openxisf::external_reference& reference) {
             ++opened;
             return std::make_unique<openxisf::memory_source>(
                 std::span<const std::byte>(reference.location == "a.xisb" ? first : second));
         }});
    ASSERT_EQ(file.images().size(), 3U);
    EXPECT_EQ(opened.load(), 0);
    EXPECT_EQ(read_from_threads(file), 0);
    EXPECT_EQ(opened.load(), 2);
}

} // namespace
