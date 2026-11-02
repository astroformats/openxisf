// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/pixels.h"

#include <openxisf/error.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>

#include "codec/codecs.h"
#include "codec/compressed_block.h"
#include "codec/compression.h"
#include "container/block_attributes.h"
#include "core/data_encoding.h"
#include "core/parallel.h"
#include "crypto/hash.h"
#include "model/unit.h"
#include "support/bytes.h"
#include "support/faulty_io.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"
#include "support/pixel_units.h"
#include "support/regions.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::detail::unit;

// A gray 8-bit image of 10 pixels in an attached block.
std::vector<std::byte> attached_image()
{
    return openxisf::test::file_with_attachments(
        openxisf::test::header_xml(R"(<Image geometry="10:1:1" sampleFormat="UInt8" location="attachment:{0}"/>)"),
        {openxisf::test::pattern(10)});
}

// The image of attached_image(), in memory.
unit open_attached_image()
{
    return openxisf::test::open_internal(attached_image());
}

TEST(pixels, progress_is_reported_after_each_piece_read)
{
    const unit opened = open_attached_image();
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    const openxisf::pixel_read_options options{.progress = [&calls](std::uint64_t done, std::uint64_t total) {
        calls.emplace_back(done, total);
        return true;
    }};
    EXPECT_EQ(openxisf::detail::read_pixels(opened, 0, options, 4), openxisf::test::pattern(10));
    EXPECT_EQ(calls, (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 10}, {4, 10}, {8, 10}, {10, 10}}));
}

TEST(pixels, a_read_cancelled_between_pieces_stops_there)
{
    const unit opened = open_attached_image();
    int calls = 0;
    const openxisf::pixel_read_options options{
        .progress = [&calls](std::uint64_t, std::uint64_t) { return ++calls < 3; }};
    std::vector<std::byte> destination(10, std::byte{0xEE});
    EXPECT_TRUE(openxisf::test::throws<openxisf::cancelled_error>(
        errc::cancelled, [&] { openxisf::detail::read_pixels(opened, 0, destination, options, 4); }));
    EXPECT_EQ(calls, 3);
    // Two pieces were read, straight into the destination; the last one was not.
    std::vector<std::byte> expected(10, std::byte{0xEE});
    std::ranges::copy(openxisf::test::pattern(8), expected.begin());
    EXPECT_EQ(destination, expected);
}

TEST(pixels, data_that_keep_their_layout_are_read_straight_into_the_destination)
{
    // The source records where it was asked to write.
    const std::vector<std::byte> file = openxisf::test::file_with_attachments(
        openxisf::test::header_xml(R"(<Image geometry="10:1:1" sampleFormat="UInt8" location="attachment:{0}"/>)"),
        {openxisf::test::pattern(10)});
    std::vector<std::span<std::byte>> reads;
    auto source = std::make_unique<openxisf::callback_source>(
        file.size(), [&file, &reads](std::uint64_t offset, std::span<std::byte> destination) {
            reads.push_back(destination);
            std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(), destination.begin());
        });
    const unit opened(std::move(source), {});
    reads.clear();

    std::vector<std::byte> destination(10);
    openxisf::detail::read_pixels(opened, 0, destination, {});
    EXPECT_EQ(destination, openxisf::test::pattern(10));
    ASSERT_EQ(reads.size(), 1U);
    EXPECT_EQ(reads.front().data(), destination.data());
    EXPECT_EQ(reads.front().size(), destination.size());
}

// Success when read() throws what a faulty source throws for a failure of kind.
testing::AssertionResult failed_with(openxisf::test::fault kind, const std::function<void()>& read)
{
    using openxisf::test::fault;
    switch (kind) {
    case fault::error:
        return openxisf::test::throws<openxisf::io_error>(errc::read_failed, read);
    case fault::short_transfer:
        return openxisf::test::throws<openxisf::io_error>(errc::end_of_data, read);
    case fault::foreign_exception:
        break;
    }
    try {
        read();
    } catch (const openxisf::test::injected_fault&) {
        return testing::AssertionSuccess();
    } catch (const std::exception& other) {
        return testing::AssertionFailure() << "threw another exception: " << other.what();
    }
    return testing::AssertionFailure() << "threw nothing";
}

TEST(pixels, a_failure_of_the_source_stops_a_read_there_and_passes_through)
{
    // An attached image of 10 pixels and a compressed one, read in pieces of 4 bytes into a vector or into memory of
    // the caller, from a source whose read of each piece in turn fails in each way: the read throws what the source
    // threw, and reads nothing after it.
    using openxisf::test::fault;
    using openxisf::test::faulty_source;
    std::vector<std::byte> compressed;
    openxisf::detail::zlib_compress(openxisf::test::pattern(10), 6, compressed);
    const openxisf::memory_source file(openxisf::test::file_with_attachments(
        openxisf::test::header_xml(
            R"(<Image geometry="10:1:1" sampleFormat="UInt8" location="attachment:{0}"/>)"
            R"(<Image geometry="10:1:1" sampleFormat="UInt8" compression="zlib:10" location="attachment:{1}"/>)"),
        {openxisf::test::pattern(10), compressed}));
    const openxisf::pixel_read_options options{.progress = [](std::uint64_t, std::uint64_t) { return true; }};
    // The reads of the open: the first 16 bytes and the header.
    constexpr std::size_t open_reads = 2;
    for (const std::size_t index : {0U, 1U}) {
        const std::size_t pieces = index == 0 ? 3 : (compressed.size() + 3) / 4;
        for (const bool into_vector : {true, false}) {
            for (const fault kind : {fault::error, fault::short_transfer, fault::foreign_exception}) {
                for (std::size_t piece = 1; piece <= pieces; ++piece) {
                    auto source = std::make_unique<faulty_source>(file, open_reads + piece, kind);
                    const faulty_source& faulty = *source;
                    const unit opened(std::move(source), {});
                    std::vector<std::byte> destination(10);
                    EXPECT_TRUE(
                        failed_with(kind,
                                    [&] {
                                        if (into_vector) {
                                            destination = openxisf::detail::read_pixels(opened, index, options, 4);
                                        } else {
                                            openxisf::detail::read_pixels(opened, index, destination, options, 4);
                                        }
                                    }))
                        << "image " << index << ", piece " << piece;
                    EXPECT_EQ(faulty.reads(), open_reads + piece) << "image " << index << ", piece " << piece;
                }
            }
        }
    }
}

TEST(pixels, an_unavailable_block_throws_its_error_before_any_progress)
{
    const unit opened = openxisf::test::open_internal(openxisf::test::file_with_attachments(
        openxisf::test::header_xml(
            R"(<Image geometry="2:1:1" sampleFormat="UInt8" compression="rle:2" location="attachment:{0}"/>)"),
        {openxisf::test::bytes("ab")}));
    int calls = 0;
    const openxisf::pixel_read_options options{.progress = [&calls](std::uint64_t, std::uint64_t) {
        ++calls;
        return true;
    }};
    EXPECT_TRUE(openxisf::test::throws<openxisf::unsupported_error>(
        errc::unsupported_compression, [&] { (void)openxisf::detail::read_pixels(opened, 0, options); }));
    EXPECT_EQ(calls, 0);
}

// The regions of interest.

using openxisf::geometry;
using openxisf::pixel_read_options;
using openxisf::pixel_region;
using openxisf::pixel_storage;

using openxisf::test::block_form;
using openxisf::test::describe;
using openxisf::test::image_attributes;
using openxisf::test::in_byte_order;
using openxisf::test::native_samples;
using openxisf::test::row_of;
using openxisf::test::unit_of;

TEST(pixels, every_region_of_an_image_reads_as_the_whole_read_holds_it)
{
    // 4 x 3 pixels of 2 channels, stored in each form, from a source in memory and from one read as a file is, read in
    // each model, with rows that follow each other and with a pitch, in pieces of the default size and of 3 bytes,
    // which are shorter than a run, with and without verifying the checksum, into a vector and into memory of the
    // caller, whose bytes between the rows stay as they were.
    const geometry image{.dimensions = {4, 3}, .channels = 2};
    const std::vector<pixel_region> regions = openxisf::test::every_region(image);
    ASSERT_EQ(regions.size(), 180U);
    for (const block_form& form : {
             block_form{},
             block_form{.storage = pixel_storage::normal},
             block_form{.big_endian = true},
             block_form{.storage = pixel_storage::normal, .checksum = true, .big_endian = true},
             block_form{.checksum = true},
             block_form{.compressed = true},
             block_form{.storage = pixel_storage::normal, .compressed = true, .subblock_size = 7},
             block_form{.compressed = true, .subblock_size = 10, .shuffled = true},
             block_form{.storage = pixel_storage::normal, .compressed = true, .shuffled = true, .checksum = true},
             block_form{.embedded = true},
             block_form{.storage = pixel_storage::normal,
                        .compressed = true,
                        .subblock_size = 6,
                        .shuffled = true,
                        .big_endian = true,
                        .embedded = true},
         }) {
        for (const bool in_memory : {true, false}) {
            SCOPED_TRACE(in_memory ? "in memory" : "read as a file is");
            const unit opened = in_memory ? openxisf::test::open_internal(unit_of(image, form))
                                          : openxisf::test::open_file_like(unit_of(image, form));
            ASSERT_TRUE(opened.diagnostics.empty()) << describe(form);
            const std::vector<std::byte> whole = openxisf::detail::read_pixels(opened, 0, {});
            ASSERT_EQ(whole, native_samples(image, form.storage)) << describe(form);
            for (const pixel_region& region : regions) {
                for (const pixel_storage storage : {pixel_storage::planar, pixel_storage::normal}) {
                    for (const bool pitched : {false, true}) {
                        const std::uint64_t pitch = pitched ? row_of(region, storage) + 3 : 0;
                        const std::vector<std::byte> expected =
                            openxisf::test::region_of(whole, image, 2, form.storage, region, storage, pitch);
                        // Memory of the caller, with 5 bytes more when there is a pitch.
                        std::vector<std::byte> filled = openxisf::test::region_of(whole, image, 2, form.storage, region,
                                                                                  storage, pitch, std::byte{0xEE});
                        filled.resize(filled.size() + (pitched ? 5 : 0), std::byte{0xEE});
                        for (const std::size_t piece : {openxisf::detail::default_piece_size, std::size_t{3}}) {
                            for (const bool verify : {true, false}) {
                                const pixel_read_options options{
                                    .storage = storage, .region = region, .pitch = pitch, .verify_checksum = verify};
                                EXPECT_EQ(openxisf::detail::read_pixels(opened, 0, options, piece), expected)
                                    << describe(form) << ", " << describe(region) << " pitch " << pitch << " piece "
                                    << piece;
                                std::vector<std::byte> destination(filled.size(), std::byte{0xEE});
                                openxisf::detail::read_pixels(opened, 0, destination, options, piece);
                                EXPECT_EQ(destination, filled) << describe(form) << ", " << describe(region)
                                                               << " pitch " << pitch << " piece " << piece;
                            }
                        }
                    }
                }
            }
        }
    }
}

TEST(pixels, regions_of_every_sample_format_come_in_the_byte_order_of_the_host)
{
    // 5 x 3 pixels of 3 channels in each sample format and storage model, in big-endian byte order: a region of the
    // last two channels, in both models and with a pitch, holds the samples of the whole read, each of them, and each
    // part of a complex one, in the byte order of the host.
    const geometry image{.dimensions = {5, 3}, .channels = 3};
    const pixel_region region{.origin = {1, 1}, .size = {3, 2}, .first_channel = 1, .channel_count = 2};
    for (const openxisf::sample_format format :
         {openxisf::sample_format::uint8, openxisf::sample_format::uint16, openxisf::sample_format::uint32,
          openxisf::sample_format::uint64, openxisf::sample_format::float32, openxisf::sample_format::float64,
          openxisf::sample_format::complex32, openxisf::sample_format::complex64}) {
        const std::size_t size = openxisf::sample_size(format);
        for (const pixel_storage stored : {pixel_storage::planar, pixel_storage::normal}) {
            const unit opened = openxisf::test::open_internal(openxisf::test::file_with_attachments(
                openxisf::test::header_xml(R"(<Image geometry="5:3:3" sampleFormat=")" +
                                           std::string(openxisf::sample_format_name(format)) + R"(" pixelStorage=")" +
                                           std::string(openxisf::pixel_storage_name(stored)) +
                                           R"(" bounds="0:1" byteOrder="big" location="attachment:{0}"/>)"),
                {openxisf::test::pattern(std::size_t{5} * 3 * 3 * size)}));
            ASSERT_TRUE(opened.diagnostics.empty()) << openxisf::sample_format_name(format);
            const std::vector<std::byte> whole = openxisf::detail::read_pixels(opened, 0, {});
            for (const pixel_storage storage : {pixel_storage::planar, pixel_storage::normal}) {
                const std::uint64_t pitch = row_of(region, storage) + 2;
                EXPECT_EQ(
                    openxisf::detail::read_pixels(opened, 0, {.storage = storage, .region = region, .pitch = pitch}),
                    openxisf::test::region_of(whole, image, size, stored, region, storage, pitch))
                    << openxisf::sample_format_name(format) << " " << openxisf::pixel_storage_name(stored) << " into "
                    << openxisf::pixel_storage_name(storage);
            }
        }
    }
}

TEST(pixels, a_region_of_a_three_dimensional_image_reads_as_the_whole_read_holds_it)
{
    const geometry image{.dimensions = {3, 2, 2}, .channels = 2};
    for (const block_form& form : {block_form{}, block_form{.storage = pixel_storage::normal},
                                   block_form{.compressed = true, .subblock_size = 8, .shuffled = true}}) {
        const unit opened = openxisf::test::open_internal(unit_of(image, form));
        const std::vector<std::byte> whole = openxisf::detail::read_pixels(opened, 0, {});
        for (const pixel_region& region : openxisf::test::every_region(image)) {
            for (const pixel_storage storage : {pixel_storage::planar, pixel_storage::normal}) {
                const std::uint64_t pitch = row_of(region, storage) + 1;
                EXPECT_EQ(
                    openxisf::detail::read_pixels(opened, 0, {.storage = storage, .region = region, .pitch = pitch}),
                    openxisf::test::region_of(whole, image, 2, form.storage, region, storage, pitch))
                    << describe(form) << ", " << describe(region);
            }
        }
    }
}

// A unit in memory whose source records the reads after the open: where each starts, and its destination. The
// source is shared with writers when asked, as the device of an editor is.
class recorded_unit
{
public:
    struct read
    {
        std::uint64_t offset = 0;
        std::span<std::byte> destination{};
    };

    explicit recorded_unit(std::vector<std::byte> file, bool shared_with_writers = false)
        : file_(std::move(file)), opened_(std::make_unique<openxisf::callback_source>(
                                              file_.size(),
                                              [this](std::uint64_t offset, std::span<std::byte> destination) {
                                                  reads_.push_back({.offset = offset, .destination = destination});
                                                  std::copy_n(file_.begin() + static_cast<std::ptrdiff_t>(offset),
                                                              destination.size(), destination.begin());
                                              }),
                                          openxisf::read_options{}, shared_with_writers)
    {
        reads_.clear();
    }

    recorded_unit(const recorded_unit&) = delete;
    recorded_unit& operator=(const recorded_unit&) = delete;
    recorded_unit(recorded_unit&&) = delete;
    recorded_unit& operator=(recorded_unit&&) = delete;
    ~recorded_unit() = default;

    [[nodiscard]] const unit& opened() const noexcept
    {
        return opened_;
    }

    [[nodiscard]] const std::vector<read>& reads() const noexcept
    {
        return reads_;
    }

    [[nodiscard]] std::uint64_t bytes_read() const noexcept
    {
        std::uint64_t total = 0;
        for (const read& each : reads_) {
            total += each.destination.size();
        }
        return total;
    }

    void forget() noexcept
    {
        reads_.clear();
    }

    // Where the block of the first image starts in the file.
    [[nodiscard]] std::uint64_t block_position() const
    {
        return openxisf::test::descriptor_at(opened_, "/xisf/Image[1]").location.position;
    }

private:
    std::vector<std::byte> file_;
    std::vector<read> reads_;
    unit opened_;
};

TEST(pixels, a_band_of_full_rows_is_one_read_per_channel_straight_into_the_destination)
{
    // Planes of 128 KiB: the channels of a band of 16 rows are 96 KiB apart.
    const geometry image{.dimensions = {1024, 64}, .channels = 3};
    const recorded_unit file(unit_of(image, {}));
    const pixel_region band{.origin = {0, 16}, .size = {1024, 16}};
    std::vector<std::byte> destination(band.sample_count(image) * 2);
    openxisf::detail::read_pixels(file.opened(), 0, destination, {.region = band});
    ASSERT_EQ(file.reads().size(), 3U);
    for (std::size_t c = 0; c < 3; ++c) {
        EXPECT_EQ(file.reads()[c].offset, file.block_position() + (c * 128 * 1024) + (std::uint64_t{16} * 2048)) << c;
        EXPECT_EQ(file.reads()[c].destination.data(), destination.data() + (c * 32 * 1024)) << c;
        EXPECT_EQ(file.reads()[c].destination.size(), 32U * 1024) << c;
    }
    EXPECT_EQ(destination, openxisf::test::region_of(native_samples(image, pixel_storage::planar), image, 2,
                                                     pixel_storage::planar, band, pixel_storage::planar));
}

TEST(pixels, the_pixels_of_a_channel_subset_in_normal_storage_are_read_together)
{
    // One channel of a tile of 32 x 32 pixels in normal storage is a run of one sample per pixel, 4 bytes apart within
    // a row and 1.5 KiB from one row to the next: one read of the 48 KiB from the first to the last, through the
    // staging buffer, or a read for each piece of it when the pieces are smaller.
    const geometry image{.dimensions = {256, 256}, .channels = 3};
    recorded_unit file(unit_of(image, {.storage = pixel_storage::normal}));
    const pixel_region tile{.origin = {64, 64}, .size = {32, 32}, .first_channel = 1, .channel_count = 1};
    const std::uint64_t first = file.block_position() + ((((std::uint64_t{64} * 256) + 64) * 3) + 1) * 2;
    const std::uint64_t hull = ((((std::uint64_t{31} * 256) + 31) * 3) + 1) * 2;
    const std::vector<std::byte> expected = openxisf::test::region_of(
        native_samples(image, pixel_storage::normal), image, 2, pixel_storage::normal, tile, pixel_storage::normal);
    EXPECT_EQ(openxisf::detail::read_pixels(file.opened(), 0, {.region = tile}), expected);
    ASSERT_EQ(file.reads().size(), 1U);
    EXPECT_EQ(file.reads().front().offset, first);
    EXPECT_EQ(file.reads().front().destination.size(), hull);

    file.forget();
    EXPECT_EQ(openxisf::detail::read_pixels(file.opened(), 0, {.region = tile}, 8192), expected);
    EXPECT_LE(file.reads().size(), (hull / 8192) + 2);
    EXPECT_GT(file.reads().size(), 1U);
}

TEST(pixels, runs_far_apart_are_read_one_by_one_straight_into_the_destination)
{
    // Rows of 80,000 bytes: the rows of a region 10 pixels wide are 79,980 bytes apart, more than the gap that is read
    // through, and fill less than a quarter of the bytes between the first and the last.
    const geometry image{.dimensions = {40000, 4}, .channels = 1};
    const recorded_unit file(unit_of(image, {}));
    const pixel_region region{.origin = {100, 0}, .size = {10, 4}};
    std::vector<std::byte> destination(80);
    openxisf::detail::read_pixels(file.opened(), 0, destination, {.region = region});
    ASSERT_EQ(file.reads().size(), 4U);
    for (std::size_t y = 0; y < 4; ++y) {
        EXPECT_EQ(file.reads()[y].offset, file.block_position() + (y * 80000) + 200) << y;
        EXPECT_EQ(file.reads()[y].destination.data(), destination.data() + (y * 20)) << y;
    }
    EXPECT_EQ(destination, openxisf::test::region_of(native_samples(image, pixel_storage::planar), image, 2,
                                                     pixel_storage::planar, region, pixel_storage::planar));
}

TEST(pixels, a_region_dense_enough_is_read_through_its_gaps)
{
    // Rows of 100,000 bytes, of which a region takes the first 25,000: gaps of 75,000 bytes, more than the gap that is
    // read through, but the region holds a third of the bytes from the first to the last, which are read in one read.
    // Rows of 15,000 bytes hold less than a quarter, and are read one by one.
    const geometry image{.dimensions = {50000, 3}, .channels = 1};
    recorded_unit file(unit_of(image, {}));
    EXPECT_EQ(openxisf::detail::read_pixels(file.opened(), 0, {.region = pixel_region{.size = {12500, 3}}}).size(),
              75000U);
    ASSERT_EQ(file.reads().size(), 1U);
    EXPECT_EQ(file.reads().front().destination.size(), 225000U);
    file.forget();
    EXPECT_EQ(openxisf::detail::read_pixels(file.opened(), 0, {.region = pixel_region{.size = {7500, 3}}}).size(),
              45000U);
    EXPECT_EQ(file.reads().size(), 3U);
}

TEST(pixels, a_source_shared_with_writers_is_read_run_by_run_never_through_its_gaps)
{
    // The device of an editor, whose bytes between the runs of a region other threads may be writing: the dense
    // region above, read in one read from a source of the reader's, is read in three reads of its rows, straight into
    // the destination, and so are the rows of a region 10 pixels wide 128 bytes apart, within the gap that is read
    // through otherwise.
    const geometry dense{.dimensions = {50000, 3}, .channels = 1};
    const recorded_unit wide(unit_of(dense, {}), true);
    std::vector<std::byte> destination(75000);
    openxisf::detail::read_pixels(wide.opened(), 0, destination, {.region = pixel_region{.size = {12500, 3}}});
    ASSERT_EQ(wide.reads().size(), 3U);
    for (std::size_t y = 0; y < 3; ++y) {
        EXPECT_EQ(wide.reads()[y].offset, wide.block_position() + (y * 100000)) << y;
        EXPECT_EQ(wide.reads()[y].destination.data(), destination.data() + (y * 25000)) << y;
        EXPECT_EQ(wide.reads()[y].destination.size(), 25000U) << y;
    }
    const geometry image{.dimensions = {64, 8}, .channels = 1};
    const recorded_unit narrow(unit_of(image, {}), true);
    const pixel_region region{.origin = {4, 0}, .size = {10, 8}};
    EXPECT_EQ(openxisf::detail::read_pixels(narrow.opened(), 0, {.region = region}),
              openxisf::test::region_of(native_samples(image, pixel_storage::planar), image, 2, pixel_storage::planar,
                                        region, pixel_storage::planar));
    ASSERT_EQ(narrow.reads().size(), 8U);
    for (std::size_t y = 0; y < 8; ++y) {
        EXPECT_EQ(narrow.reads()[y].offset, narrow.block_position() + (y * 128) + 8) << y;
        EXPECT_EQ(narrow.reads()[y].destination.size(), 20U) << y;
    }
}

TEST(pixels, a_region_of_the_whole_image_is_read_as_the_whole_image)
{
    // A block with a checksum, which a whole read verifies in the destination, where a region is read into a buffer
    // of the library first.
    const geometry image{.dimensions = {64, 8}, .channels = 2};
    const recorded_unit file(unit_of(image, {.checksum = true}));
    std::vector<std::byte> destination(std::size_t{64} * 8 * 2 * 2);
    openxisf::detail::read_pixels(file.opened(), 0, destination, {.region = pixel_region{}, .pitch = 64});
    ASSERT_EQ(file.reads().size(), 1U);
    EXPECT_EQ(file.reads().front().destination.data(), destination.data());
    EXPECT_EQ(destination, native_samples(image, pixel_storage::planar));
}

TEST(pixels, a_block_with_a_checksum_is_read_whole_and_verified_unless_the_application_opts_out)
{
    const geometry image{.dimensions = {40000, 4}, .channels = 1};
    recorded_unit file(unit_of(image, {.checksum = true}));
    const pixel_region region{.origin = {100, 0}, .size = {10, 4}};
    const std::vector<std::byte> expected = openxisf::test::region_of(
        native_samples(image, pixel_storage::planar), image, 2, pixel_storage::planar, region, pixel_storage::planar);
    EXPECT_EQ(openxisf::detail::read_pixels(file.opened(), 0, {.region = region}), expected);
    ASSERT_EQ(file.reads().size(), 1U);
    EXPECT_EQ(file.reads().front().offset, file.block_position());
    EXPECT_EQ(file.reads().front().destination.size(), 320000U);
    file.forget();
    EXPECT_EQ(openxisf::detail::read_pixels(file.opened(), 0, {.region = region, .verify_checksum = false}), expected);
    EXPECT_EQ(file.reads().size(), 4U);
}

// The unit with the first digit of the digest of its block changed: the digest of other data.
std::vector<std::byte> with_another_digest(std::vector<std::byte> file)
{
    const std::string text = openxisf::test::text(file);
    const std::size_t digit = text.find(':', text.find("checksum=\"")) + 1;
    file.at(digit) = file.at(digit) == std::byte{'0'} ? std::byte{'1'} : std::byte{'0'};
    return file;
}

TEST(pixels, data_that_fail_their_checksum_are_read_only_when_the_application_opts_out)
{
    // A block as it is and one compressed in subblocks, whose digests are those of other data, from a source in
    // memory, verified in place, and from one read as a file is: whole reads and regions fail, unless the application
    // opts out.
    const geometry image{.dimensions = {64, 64}, .channels = 1};
    const std::vector<std::byte> samples = native_samples(image, pixel_storage::planar);
    for (const block_form& form :
         {block_form{.checksum = true}, block_form{.compressed = true, .subblock_size = 512, .checksum = true}}) {
        const std::vector<std::byte> file = with_another_digest(unit_of(image, form));
        for (const bool in_memory : {true, false}) {
            const unit opened = in_memory ? openxisf::test::open_internal(file) : openxisf::test::open_file_like(file);
            for (const pixel_read_options& options :
                 {pixel_read_options{}, pixel_read_options{.region = pixel_region{}},
                  pixel_read_options{.region = pixel_region{.origin = {8, 20}, .size = {8, 10}}}}) {
                EXPECT_TRUE(openxisf::test::throws<openxisf::integrity_error>(
                    errc::checksum_mismatch, [&] { (void)openxisf::detail::read_pixels(opened, 0, options); }))
                    << describe(form) << (in_memory ? " in memory" : "");
                pixel_read_options unverified = options;
                unverified.verify_checksum = false;
                EXPECT_EQ(openxisf::detail::read_pixels(opened, 0, unverified),
                          options.region ? openxisf::test::region_of(samples, image, 2, pixel_storage::planar,
                                                                     openxisf::test::value_of(options.region),
                                                                     pixel_storage::planar)
                                         : samples)
                    << describe(form) << (in_memory ? " in memory" : "");
            }
        }
    }
}

// A source in memory that counts the reads that reach it.
class counted_memory_source final : public openxisf::input_source
{
public:
    counted_memory_source(std::vector<std::byte> content, std::size_t& reads)
        : content_(std::move(content)), reads_(reads)
    {}

    [[nodiscard]] std::uint64_t size() const override
    {
        return content_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        ++reads_;
        std::ranges::copy(std::span(content_).subspan(offset, destination.size()), destination.begin());
    }

    [[nodiscard]] std::span<const std::byte> bytes() const override
    {
        return content_;
    }

private:
    std::vector<std::byte> content_;
    std::size_t& reads_;
};

TEST(pixels, a_region_of_a_source_in_memory_is_taken_from_its_bytes_without_a_read)
{
    // Rows far apart, and a region dense enough that a file is read through its gaps, of a block as it is, of one with
    // a checksum, verified in place, and of blocks compressed in subblocks, one of them shuffled and with a checksum:
    // after the open, no read reaches a source that gives its bytes in memory.
    const geometry image{.dimensions = {40000, 4}, .channels = 1};
    const std::vector<std::byte> samples = native_samples(image, pixel_storage::planar);
    for (const block_form& form :
         {block_form{}, block_form{.checksum = true}, block_form{.compressed = true, .subblock_size = 8192},
          block_form{.compressed = true, .subblock_size = 8192, .shuffled = true, .checksum = true}}) {
        std::size_t reads = 0;
        const unit opened(std::make_unique<counted_memory_source>(unit_of(image, form), reads), {});
        reads = 0;
        for (const pixel_region& region :
             {pixel_region{.origin = {100, 0}, .size = {10, 4}}, pixel_region{.size = {12500, 3}}}) {
            for (const bool verify : {true, false}) {
                EXPECT_EQ(
                    openxisf::detail::read_pixels(opened, 0, {.region = region, .verify_checksum = verify}),
                    openxisf::test::region_of(samples, image, 2, pixel_storage::planar, region, pixel_storage::planar))
                    << describe(form) << ", " << describe(region);
            }
        }
        EXPECT_EQ(reads, 0U) << describe(form);
    }
}

// The subblocks of the block of the first image, and where their stored bytes start.
std::vector<std::pair<std::uint64_t, openxisf::detail::subblock>> placed_subblocks(const unit& opened)
{
    std::vector<std::pair<std::uint64_t, openxisf::detail::subblock>> placed;
    std::uint64_t position = openxisf::test::descriptor_at(opened, "/xisf/Image[1]").location.position;
    for (const openxisf::detail::subblock& part :
         openxisf::test::value_of(openxisf::test::descriptor_at(opened, "/xisf/Image[1]").compression).subblocks) {
        placed.emplace_back(position, part);
        position += part.compressed_size;
    }
    return placed;
}

// The stored bytes of the subblocks of placed in [first, end).
std::uint64_t stored_bytes(const std::vector<std::pair<std::uint64_t, openxisf::detail::subblock>>& placed,
                           std::size_t first, std::size_t end)
{
    std::uint64_t total = 0;
    for (std::size_t i = first; i < end; ++i) {
        total += placed[i].second.compressed_size;
    }
    return total;
}

TEST(pixels, only_the_subblocks_that_hold_the_region_are_read_and_decoded)
{
    // 64 x 64 pixels in subblocks of 512 bytes, 4 rows each: rows 20 to 29 are in subblocks 5 to 7, read in batches of
    // as many as the threads.
    const geometry image{.dimensions = {64, 64}, .channels = 1};
    const recorded_unit file(unit_of(image, {.compressed = true, .subblock_size = 512}));
    const auto placed = placed_subblocks(file.opened());
    ASSERT_EQ(placed.size(), 16U);
    const pixel_region region{.origin = {8, 20}, .size = {8, 10}};
    EXPECT_EQ(openxisf::detail::read_pixels(file.opened(), 0, {.region = region}),
              openxisf::test::region_of(native_samples(image, pixel_storage::planar), image, 2, pixel_storage::planar,
                                        region, pixel_storage::planar));
    const std::size_t batch = openxisf::detail::concurrency();
    EXPECT_EQ(file.reads().size(), (3 + batch - 1) / batch);
    EXPECT_EQ(file.reads().front().offset, placed[5].first);
    EXPECT_EQ(file.bytes_read(), stored_bytes(placed, 5, 8));
}

TEST(pixels, a_compressed_block_with_a_checksum_is_read_whole_and_decoded_in_part)
{
    const geometry image{.dimensions = {64, 64}, .channels = 1};
    recorded_unit file(unit_of(image, {.compressed = true, .subblock_size = 512, .checksum = true}));
    const auto placed = placed_subblocks(file.opened());
    const pixel_region region{.origin = {8, 20}, .size = {8, 10}};
    const std::vector<std::byte> expected = openxisf::test::region_of(
        native_samples(image, pixel_storage::planar), image, 2, pixel_storage::planar, region, pixel_storage::planar);
    EXPECT_EQ(openxisf::detail::read_pixels(file.opened(), 0, {.region = region}), expected);
    ASSERT_EQ(file.reads().size(), 1U);
    EXPECT_EQ(file.bytes_read(), stored_bytes(placed, 0, placed.size()));
    file.forget();
    EXPECT_EQ(openxisf::detail::read_pixels(file.opened(), 0, {.region = region, .verify_checksum = false}), expected);
    EXPECT_EQ(file.bytes_read(), stored_bytes(placed, 5, 8));
}

TEST(pixels, a_shuffled_block_is_decoded_in_the_subblocks_that_hold_bytes_of_the_region)
{
    // Byte 0 of the samples of the region, items 1288 to 1871, is in subblocks 2 and 3, and byte 1, 4096 bytes on, in
    // subblocks 10 and 11.
    const geometry image{.dimensions = {64, 64}, .channels = 1};
    const recorded_unit file(unit_of(image, {.compressed = true, .subblock_size = 512, .shuffled = true}));
    const auto placed = placed_subblocks(file.opened());
    ASSERT_EQ(placed.size(), 16U);
    const pixel_region region{.origin = {8, 20}, .size = {8, 10}};
    EXPECT_EQ(openxisf::detail::read_pixels(file.opened(), 0, {.region = region}),
              openxisf::test::region_of(native_samples(image, pixel_storage::planar), image, 2, pixel_storage::planar,
                                        region, pixel_storage::planar));
    EXPECT_EQ(file.bytes_read(), stored_bytes(placed, 2, 4) + stored_bytes(placed, 10, 12));
}

TEST(pixels, a_region_of_an_external_block_is_read_from_its_file)
{
    // A data blocks file in memory, and one read as a file is.
    const geometry image{.dimensions = {16, 4}, .channels = 2};
    const std::vector<std::byte> data = in_byte_order(native_samples(image, pixel_storage::planar), false);
    const std::vector<std::byte> blocks = openxisf::test::blocks_file_of({data}, {1});
    const std::string header = openxisf::test::header_xml("<Image " + image_attributes(image, pixel_storage::planar) +
                                                          R"( location="path(@header_dir/unit.xisb):0x1"/>)");
    for (const bool in_memory : {true, false}) {
        const unit opened = openxisf::test::open_internal(
            openxisf::test::bytes(header),
            {.resolver = in_memory ? openxisf::test::memory_resolver({{"unit.xisb", blocks}})
                                   : openxisf::external_resolver([&blocks](const openxisf::external_reference&) {
                                         return openxisf::test::file_like_source(blocks);
                                     })});
        ASSERT_TRUE(opened.diagnostics.empty());
        const pixel_region region{.origin = {3, 1}, .size = {5, 2}, .first_channel = 1};
        EXPECT_EQ(openxisf::detail::read_pixels(opened, 0, {.region = region, .pitch = 7}),
                  openxisf::test::region_of(native_samples(image, pixel_storage::planar), image, 2,
                                            pixel_storage::planar, region, pixel_storage::planar, 7))
            << (in_memory ? "in memory" : "read as a file is");
    }
}

TEST(pixels, the_progress_of_a_region_read_as_stored_counts_its_bytes)
{
    // A run of 4 bytes, read in pieces of 2 straight into the destination, or taken at once from a source in memory;
    // and a channel subset of normal storage, read through the staging buffer at once.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    const auto record = [&calls](std::uint64_t done, std::uint64_t total) {
        calls.emplace_back(done, total);
        return true;
    };
    const pixel_read_options run{.progress = record, .region = pixel_region{.origin = {3, 0}, .size = {4, 1}}};
    const unit from_file = openxisf::test::open_file_like(attached_image());
    EXPECT_EQ(openxisf::detail::read_pixels(from_file, 0, run, 2), openxisf::test::bytes("\x03\x04\x05\x06"));
    EXPECT_EQ(calls, (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 4}, {2, 4}, {4, 4}}));
    calls.clear();
    const unit in_memory = open_attached_image();
    EXPECT_EQ(openxisf::detail::read_pixels(in_memory, 0, run, 2), openxisf::test::bytes("\x03\x04\x05\x06"));
    EXPECT_EQ(calls, (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 4}, {4, 4}}));

    calls.clear();
    const unit normal = openxisf::test::open_file_like(
        unit_of({.dimensions = {4, 3}, .channels = 2}, {.storage = pixel_storage::normal}));
    (void)openxisf::detail::read_pixels(
        normal, 0, {.progress = record, .region = pixel_region{.size = {2, 2}, .first_channel = 1}});
    EXPECT_EQ(calls, (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 8}, {8, 8}}));
}

TEST(pixels, the_progress_of_a_region_decoded_counts_the_stored_bytes_read_and_the_decoded_ones)
{
    // Subblocks 5 to 7 of a block without a checksum, read and decoded, and of one with a checksum, read whole and
    // verified first; from a source in memory, decoded alone.
    const geometry image{.dimensions = {64, 64}, .channels = 1};
    for (const bool checksum : {false, true}) {
        const std::vector<std::byte> file =
            unit_of(image, {.compressed = true, .subblock_size = 512, .checksum = checksum});
        for (const bool in_memory : {false, true}) {
            const unit opened = in_memory ? openxisf::test::open_internal(file) : openxisf::test::open_file_like(file);
            const auto placed = placed_subblocks(opened);
            std::uint64_t last = 0;
            std::uint64_t reported_total = 0;
            (void)openxisf::detail::read_pixels(opened, 0,
                                                {.progress =
                                                     [&](std::uint64_t done, std::uint64_t total) {
                                                         EXPECT_GE(done, last);
                                                         last = done;
                                                         reported_total = total;
                                                         return true;
                                                     },
                                                 .region = pixel_region{.origin = {8, 20}, .size = {8, 10}}});
            const std::uint64_t read =
                in_memory ? 0 : (checksum ? stored_bytes(placed, 0, placed.size()) : stored_bytes(placed, 5, 8));
            EXPECT_EQ(reported_total, read + (std::uint64_t{3} * 512)) << checksum << in_memory;
            EXPECT_EQ(last, reported_total) << checksum << in_memory;
        }
    }
}

TEST(pixels, a_region_read_cancelled_stops_there)
{
    // A region of 4 rows far apart, one read each: cancelled after the second.
    const geometry image{.dimensions = {40000, 4}, .channels = 1};
    const recorded_unit file(unit_of(image, {}));
    int calls = 0;
    const pixel_read_options options{.progress = [&calls](std::uint64_t, std::uint64_t) { return ++calls < 3; },
                                     .region = pixel_region{.origin = {100, 0}, .size = {10, 4}}};
    std::vector<std::byte> destination(80);
    EXPECT_TRUE(openxisf::test::throws<openxisf::cancelled_error>(
        errc::cancelled, [&] { openxisf::detail::read_pixels(file.opened(), 0, destination, options); }));
    EXPECT_EQ(file.reads().size(), 2U);
}

TEST(pixels, the_destination_of_a_region_has_its_size_or_with_a_pitch_at_least_its_span)
{
    // 3 x 2 pixels of 2 channels, 16-bit: 24 bytes, and with a pitch of 10 samples in the normal model, 1 row of 20
    // bytes and 1 of 12.
    const geometry image{.dimensions = {4, 3}, .channels = 2};
    const recorded_unit file(unit_of(image, {}));
    const pixel_region region{.origin = {1, 1}, .size = {3, 2}};
    const auto refused = [&file](std::size_t size, const pixel_read_options& options) {
        std::vector<std::byte> destination(size);
        return openxisf::test::throws<openxisf::usage_error>(
            errc::invalid_argument, [&] { openxisf::detail::read_pixels(file.opened(), 0, destination, options); });
    };
    EXPECT_TRUE(refused(23, {.region = region}));
    EXPECT_TRUE(refused(25, {.region = region}));
    EXPECT_TRUE(refused(31, {.storage = pixel_storage::normal, .region = region, .pitch = 10}));
    // A pitch shorter than a row: 6 samples in the normal model, 3 in the planar one.
    EXPECT_TRUE(refused(100, {.storage = pixel_storage::normal, .region = region, .pitch = 5}));
    EXPECT_TRUE(refused(100, {.region = region, .pitch = 2}));
    // A pitch of the whole image: 6 rows of 4 samples, 8 apart.
    EXPECT_TRUE(refused(87, {.pitch = 8}));
    EXPECT_TRUE(file.reads().empty());

    std::vector<std::byte> destination(40, std::byte{0xEE});
    openxisf::detail::read_pixels(file.opened(), 0, destination,
                                  {.storage = pixel_storage::normal, .region = region, .pitch = 10});
    // The bytes after the span stay as they are.
    EXPECT_TRUE(
        std::all_of(destination.begin() + 32, destination.end(), [](std::byte b) { return b == std::byte{0xEE}; }));
}

TEST(pixels, a_region_outside_the_image_is_refused_before_anything_is_read)
{
    const recorded_unit file(unit_of({.dimensions = {4, 3}, .channels = 2}, {}));
    for (const pixel_region& region : {pixel_region{.origin = {4, 0}}, pixel_region{.size = {5, 1}},
                                       pixel_region{.first_channel = 2}, pixel_region{.origin = {1}}}) {
        EXPECT_TRUE(openxisf::test::throws<openxisf::usage_error>(errc::invalid_argument, [&] {
            (void)openxisf::detail::read_pixels(file.opened(), 0, {.region = region});
        })) << describe(region);
    }
    EXPECT_TRUE(file.reads().empty());
}

TEST(pixels, a_region_of_an_unavailable_block_throws_the_error_of_the_block)
{
    const unit opened = openxisf::test::open_internal(openxisf::test::file_with_attachments(
        openxisf::test::header_xml(
            R"(<Image geometry="2:1:1" sampleFormat="UInt8" compression="rle:2" location="attachment:{0}"/>)"),
        {openxisf::test::bytes("ab")}));
    EXPECT_TRUE(openxisf::test::throws<openxisf::unsupported_error>(errc::unsupported_compression, [&] {
        (void)openxisf::detail::read_pixels(opened, 0, {.region = pixel_region{.size = {1, 1}}});
    }));
    // A region that does not fit is a usage error first.
    EXPECT_TRUE(openxisf::test::throws<openxisf::usage_error>(errc::invalid_argument, [&] {
        (void)openxisf::detail::read_pixels(opened, 0, {.region = pixel_region{.size = {3, 1}}});
    }));
}

TEST(pixels, the_errors_of_a_region_read_have_the_context_of_its_block)
{
    // A subblock that is not a zlib stream.
    const geometry image{.dimensions = {64, 64}, .channels = 1};
    std::vector<std::byte> file = unit_of(image, {.compressed = true, .subblock_size = 512});
    const unit before = openxisf::test::open_internal(file);
    const auto placed = placed_subblocks(before);
    ASSERT_LT(placed[6].second.compressed_size, placed[6].second.uncompressed_size);
    file[placed[6].first] ^= std::byte{0xFF};
    const unit opened = openxisf::test::open_internal(std::move(file));
    try {
        (void)openxisf::detail::read_pixels(opened, 0, {.region = pixel_region{.origin = {8, 20}, .size = {8, 10}}});
        ADD_FAILURE() << "the region was read";
    } catch (const openxisf::integrity_error& failure) {
        EXPECT_EQ(failure.code(), errc::corrupt_compressed_data);
        EXPECT_EQ(failure.context().element, "/xisf/Image[1]");
        EXPECT_EQ(failure.context().offset, placed[0].first);
    }
}

} // namespace
