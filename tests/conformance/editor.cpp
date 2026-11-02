// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The editor, which writes pixel data in place, outside the specification: regions written into units of the writer and
// read back by the reader and the editor, a reserved image filled by regions, whose checksum (spec §10.5) is valid once
// committed and stale before, the samples of a big-endian block (spec §10.4) or given in the other storage model (spec
// §8.5.3), what cannot be written in place, a distributed unit (spec §9.3) among them, and the failures of a device.

#include <openxisf/editor.h>
#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>
#include <openxisf/writer.h>

#include "container/block_attributes.h"
#include "support/bytes.h"
#include "support/faulty_io.h"
#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/pixel_units.h"
#include "support/regions.h"
#include "support/temp_directory.h"
#include "support/throws.h"
#include "support/written_unit.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::geometry;
using openxisf::image_info;
using openxisf::pixel_region;
using openxisf::pixel_storage;
using openxisf::test::native_samples;
using openxisf::test::temp_directory;
using openxisf::test::throws;

openxisf::write_options basic_options()
{
    return {.creator_application = "OpenXISF tests 1.0", .creation_time = openxisf::date_time{.year = 2026}};
}

// An RGB image of 16 x 12 UInt16 samples in the model storage.
image_info frame_info(pixel_storage storage = pixel_storage::planar)
{
    image_info info;
    info.geometry = {.dimensions = {16, 12}, .channels = 3};
    info.sample_format = openxisf::sample_format::uint16;
    info.color_space = openxisf::color_space::rgb;
    info.pixel_storage = storage;
    return info;
}

// The geometry of the frames: 16 x 12 pixels of 3 channels.
geometry frame()
{
    return {.dimensions = {16, 12}, .channels = 3};
}

// Writes the samples of whole, the pixel data of the image at index in the model stored, into the editor in tiles of
// tile x tile pixels with every channel, verifying the block before its first write unless verify is false.
void write_in_tiles(openxisf::editor& edit, std::size_t index, pixel_storage stored, std::span<const std::byte> whole,
                    std::uint64_t tile, bool verify = true)
{
    for (std::uint64_t y = 0; y < frame().dimensions[1]; y += tile) {
        for (std::uint64_t x = 0; x < frame().dimensions[0]; x += tile) {
            const pixel_region region{
                .origin = {x, y},
                .size = {std::min(tile, frame().dimensions[0] - x), std::min(tile, frame().dimensions[1] - y)}};
            edit.write_pixels(index, openxisf::test::region_of(whole, frame(), 2, stored, region, stored),
                              {.region = region, .verify_checksum = verify});
        }
    }
}

// A unit of a reserved planar frame, saved into memory with options.
std::vector<std::byte> reserved_unit(const openxisf::write_options& options)
{
    openxisf::writer output(options);
    (void)output.add_reserved_image(frame_info());
    openxisf::memory_sink sink;
    output.save(sink);
    return sink.release();
}

TEST(conformance_editor, a_region_written_into_a_file_of_the_writer_reads_back_in_the_reader_and_the_editor)
{
    const temp_directory directory;
    const std::string path = directory.file("frame.xisf");
    const std::vector<std::byte> before = native_samples(frame(), pixel_storage::planar);
    openxisf::writer output(basic_options());
    (void)output.add_image(frame_info(), before);
    output.save(path);

    const pixel_region tile{.origin = {4, 3}, .size = {6, 5}, .first_channel = 1, .channel_count = 2};
    const std::vector<std::byte> data =
        openxisf::test::region_of(native_samples(frame(), pixel_storage::planar, 30000), frame(), 2,
                                  pixel_storage::planar, tile, pixel_storage::planar);
    {
        openxisf::editor edit(path);
        ASSERT_EQ(edit.images().size(), 1U);
        EXPECT_EQ(edit.image(0), frame_info());
        EXPECT_TRUE(edit.diagnostics().empty());
        edit.write_pixels(0, data, {.region = tile});
        EXPECT_EQ(edit.read_pixels(0, {.region = tile}), data);
        edit.commit();
    }
    const openxisf::reader unit(path, {.strict = true});
    EXPECT_EQ(unit.read_pixels(0), openxisf::test::with_region(before, frame(), 2, pixel_storage::planar, tile, data,
                                                               pixel_storage::planar));
}

TEST(conformance_editor, a_reserved_image_filled_by_regions_verifies_once_it_is_committed)
{
    // The frame reserved with a SHA-256 checksum verifies as zeros. Once its tiles are written, a reader finds it
    // corrupt, and the editor reads it without verifying it; once committed, both verify it.
    const temp_directory directory;
    const std::string path = directory.file("stack.xisf");
    openxisf::write_options options = basic_options();
    options.checksum = openxisf::checksum_algorithm::sha256;
    openxisf::test::write_file(openxisf::test::path_of(path), reserved_unit(options));
    EXPECT_EQ(openxisf::reader(path, {.strict = true}).read_pixels(0), std::vector<std::byte>(1152));

    const std::vector<std::byte> stacked = native_samples(frame(), pixel_storage::planar, 30000);
    openxisf::editor edit(path);
    write_in_tiles(edit, 0, pixel_storage::planar, stacked, 5);
    const openxisf::reader between(path);
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch, [&] { (void)between.read_pixels(0); }));
    EXPECT_EQ(edit.read_pixels(0), stacked);

    edit.commit();
    EXPECT_EQ(openxisf::reader(path, {.strict = true}).read_pixels(0), stacked);
    EXPECT_EQ(edit.read_pixels(0), stacked);

    // A block written again is stale again until the next commit.
    const pixel_region corner{.origin = {0, 0}, .size = {2, 2}, .channel_count = 1};
    const std::vector<std::byte> zeros(8);
    edit.write_pixels(0, zeros, {.region = corner});
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch,
                                                  [&] { (void)openxisf::reader(path).read_pixels(0); }));
    edit.commit();
    EXPECT_EQ(openxisf::reader(path, {.strict = true}).read_pixels(0, {.region = corner}), zeros);
}

TEST(conformance_editor, an_editor_destroyed_without_commit_leaves_the_checksum_stale)
{
    // An output left incomplete, by a failure for instance, is not taken for a complete one, until a block is written
    // again, without the verification that its stale digest would fail, and committed.
    openxisf::write_options options = basic_options();
    options.checksum = openxisf::checksum_algorithm::sha1;
    std::vector<std::byte> file = reserved_unit(options);
    const std::vector<std::byte> stacked = native_samples(frame(), pixel_storage::planar, 30000);
    {
        openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
        write_in_tiles(edit, 0, pixel_storage::planar, stacked, 8);
    }
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch,
                                                  [&] { (void)openxisf::test::open_unit(file).read_pixels(0); }));
    {
        openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
        edit.commit();
    }
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch,
                                                  [&] { (void)openxisf::test::open_unit(file).read_pixels(0); }));
    {
        openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
        EXPECT_TRUE(throws<openxisf::integrity_error>(
            errc::checksum_mismatch, [&] { write_in_tiles(edit, 0, pixel_storage::planar, stacked, 16); }));
        write_in_tiles(edit, 0, pixel_storage::planar, stacked, 16, false);
        edit.commit();
    }
    EXPECT_EQ(openxisf::test::open_unit(file, {.strict = true}).read_pixels(0), stacked);
}

TEST(conformance_editor, refuses_what_it_cannot_write_in_place)
{
    // A compressed image, which reads all the same, and a distributed unit, from a path or a device.
    const temp_directory directory;
    const std::vector<std::byte> samples(1152);
    openxisf::write_options options = basic_options();
    options.codec = openxisf::codec::zstd;
    openxisf::writer compressed(options);
    (void)compressed.add_image(frame_info(), samples);
    compressed.save(directory.file("compressed.xisf"));
    const std::string before = openxisf::test::read_file(directory.path() / "compressed.xisf");
    {
        openxisf::editor edit(directory.file("compressed.xisf"));
        EXPECT_TRUE(throws<openxisf::unsupported_error>(errc::not_editable, [&] { edit.write_pixels(0, samples); }));
        EXPECT_EQ(edit.read_pixels(0), samples);
        edit.commit();
    }
    EXPECT_EQ(openxisf::test::read_file(directory.path() / "compressed.xisf"), before);

    openxisf::writer distributed(basic_options());
    (void)distributed.add_image(frame_info(), samples);
    distributed.save_distributed(directory.file("unit.xish"));
    EXPECT_TRUE(throws<openxisf::unsupported_error>(errc::not_editable,
                                                    [&] { openxisf::editor edit(directory.file("unit.xish")); }));
    std::vector<std::byte> header = openxisf::test::bytes(openxisf::test::read_file(directory.path() / "unit.xish"));
    EXPECT_TRUE(throws<openxisf::unsupported_error>(
        errc::not_editable, [&] { openxisf::editor edit(std::make_unique<openxisf::memory_device>(header)); }));
}

TEST(conformance_editor, writes_a_big_endian_block_in_its_byte_order)
{
    const geometry image{.dimensions = {4, 3}, .channels = 2};
    std::vector<std::byte> file = openxisf::test::unit_of(image, {.big_endian = true});
    const std::vector<std::byte> other = native_samples(image, pixel_storage::planar, 30000);
    const pixel_region region{.origin = {1, 1}, .size = {2, 2}};
    const std::vector<std::byte> data =
        openxisf::test::region_of(other, image, 2, pixel_storage::planar, region, pixel_storage::planar);
    {
        openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
        edit.write_pixels(0, data, {.region = region});
    }
    const std::vector<std::byte> expected =
        openxisf::test::with_region(native_samples(image, pixel_storage::planar), image, 2, pixel_storage::planar,
                                    region, data, pixel_storage::planar);
    EXPECT_EQ(openxisf::test::open_unit(file, {.strict = true}).read_pixels(0), expected);
    // The file holds them in big-endian byte order.
    const std::vector<std::byte> stored = openxisf::test::in_byte_order(expected, true);
    EXPECT_NE(openxisf::test::text(file).find(openxisf::test::text(stored)), std::string::npos);
}

TEST(conformance_editor, writes_samples_given_in_the_other_storage_model_with_a_pitch)
{
    // A normal image, written from planar rows of 7 samples 9 samples apart.
    const std::vector<std::byte> before = native_samples(frame(), pixel_storage::normal);
    openxisf::writer output(basic_options());
    (void)output.add_image(frame_info(pixel_storage::normal), before);
    openxisf::memory_sink sink;
    output.save(sink);
    std::vector<std::byte> file = sink.release();

    const pixel_region region{.origin = {3, 2}, .size = {7, 4}};
    const std::vector<std::byte> data =
        openxisf::test::region_of(native_samples(frame(), pixel_storage::normal, 30000), frame(), 2,
                                  pixel_storage::normal, region, pixel_storage::planar, 9, std::byte{0xEE});
    {
        openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
        edit.write_pixels(0, data, {.storage = pixel_storage::planar, .region = region, .pitch = 9});
        EXPECT_EQ(edit.read_pixels(0, {.storage = pixel_storage::planar, .region = region, .pitch = 9}),
                  openxisf::test::region_of(native_samples(frame(), pixel_storage::normal, 30000), frame(), 2,
                                            pixel_storage::normal, region, pixel_storage::planar, 9));
    }
    EXPECT_EQ(
        openxisf::test::open_unit(file, {.strict = true}).read_pixels(0),
        openxisf::test::with_region(before, frame(), 2, pixel_storage::normal, region, data, pixel_storage::planar, 9));
}

TEST(conformance_editor, typed_reads_and_writes_take_the_type_of_the_sample_format)
{
    image_info info = frame_info();
    info.sample_format = openxisf::sample_format::float32;
    info.bounds = openxisf::bounds{};
    const std::vector<float> zeros(info.geometry.sample_count());
    openxisf::writer output(basic_options());
    (void)output.add_image(info, std::span(zeros));
    openxisf::memory_sink sink;
    output.save(sink);
    std::vector<std::byte> file = sink.release();

    openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
    const pixel_region row{.origin = {0, 5}, .size = {16, 1}, .channel_count = 1};
    std::vector<float> values(16);
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<float>(i) / 16.0F;
    }
    edit.write_pixels(0, std::span<const float>(values), {.region = row});
    EXPECT_EQ(edit.read_pixels<float>(0, {.region = row}), values);
    std::vector<float> read(16);
    edit.read_pixels(0, std::span(read), {.region = row});
    EXPECT_EQ(read, values);
    // Samples that are not const, and samples of another type.
    values[0] = 1.0F;
    edit.write_pixels(0, std::span(values), {.region = row});
    EXPECT_EQ(edit.read_pixels<float>(0, {.region = row}).front(), 1.0F);
    const std::vector<std::uint32_t> integers(16);
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument, [&] {
        edit.write_pixels(0, std::span<const std::uint32_t>(integers), {.region = row});
    }));
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument,
                                              [&] { (void)edit.read_pixels<double>(0, {.region = row}); }));
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument,
                                              [&] { edit.write_pixels(1, std::span<const float>(values)); }));
}

TEST(conformance_editor, verifies_the_blocks_that_it_has_not_written)
{
    // Two images with SHA-1 checksums, the second corrupted: the editor refuses to read it, unless it is read
    // unverified, as a reader does, and to write it, so that commit() never gives a digest to bytes that were not
    // verified; and reads the first, once written, unverified.
    openxisf::write_options options = basic_options();
    options.checksum = openxisf::checksum_algorithm::sha1;
    const std::vector<std::byte> samples = native_samples(frame(), pixel_storage::planar);
    openxisf::writer output(options);
    (void)output.add_image(frame_info(), samples);
    (void)output.add_image(frame_info(), samples);
    openxisf::memory_sink sink;
    output.save(sink);
    std::vector<std::byte> file = sink.release();
    // The last byte of the file is the last of the second block.
    file.back() ^= std::byte{0x01};

    openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch, [&] { (void)edit.read_pixels(1); }));
    EXPECT_EQ(edit.read_pixels(1, {.verify_checksum = false}).size(), samples.size());
    EXPECT_EQ(edit.read_pixels(0), samples);
    const std::vector<std::byte> other = native_samples(frame(), pixel_storage::planar, 30000);
    edit.write_pixels(0, other);
    EXPECT_EQ(edit.read_pixels(0), other);

    const std::vector<std::byte> before = file;
    const pixel_region corner{.origin = {0, 0}, .size = {2, 2}, .channel_count = 1};
    for (int attempt = 0; attempt < 2; ++attempt) {
        EXPECT_TRUE(throws<openxisf::integrity_error>(
            errc::checksum_mismatch, [&] { edit.write_pixels(1, std::vector<std::byte>(8), {.region = corner}); }));
    }
    EXPECT_EQ(file, before);
    edit.commit();
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch, [&] { (void)edit.read_pixels(1); }));
}

// A device over a buffer that counts the bytes read through it.
class counting_device final : public openxisf::random_access_device
{
public:
    explicit counting_device(std::span<std::byte> data) : inner_(data) {}

    std::uint64_t size() const override
    {
        return inner_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        read_ += destination.size();
        inner_.read(offset, destination);
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        inner_.write(offset, data);
    }

    // The bytes read since the last call.
    [[nodiscard]] std::uint64_t take_read() const noexcept
    {
        return std::exchange(read_, 0);
    }

private:
    openxisf::memory_device inner_;
    mutable std::uint64_t read_ = 0;
};

TEST(conformance_editor, verifies_a_block_with_a_checksum_once_before_its_first_write)
{
    // The first write of the reserved frame reads its 1152 bytes to verify them; later writes, before and after a
    // commit, which gives the block a digest of its own, read nothing. A block without a checksum is never read, nor
    // one whose first write the application asks not to verify.
    for (const bool checksum : {true, false}) {
        openxisf::write_options options = basic_options();
        if (checksum) {
            options.checksum = openxisf::checksum_algorithm::sha256;
        }
        std::vector<std::byte> file = reserved_unit(options);
        auto device = std::make_unique<counting_device>(file);
        const counting_device& counting = *device;
        openxisf::editor edit(std::move(device));
        (void)counting.take_read();
        const pixel_region corner{.origin = {0, 0}, .size = {2, 2}, .channel_count = 1};
        const std::vector<std::byte> ones(8, std::byte{1});
        edit.write_pixels(0, ones, {.region = corner});
        EXPECT_EQ(counting.take_read(), checksum ? 1152U : 0U);
        edit.write_pixels(0, ones, {.region = corner});
        EXPECT_EQ(counting.take_read(), 0U);
        edit.commit();
        (void)counting.take_read();
        edit.write_pixels(0, ones, {.region = corner});
        EXPECT_EQ(counting.take_read(), 0U);
    }
    openxisf::write_options options = basic_options();
    options.checksum = openxisf::checksum_algorithm::sha256;
    std::vector<std::byte> file = reserved_unit(options);
    auto device = std::make_unique<counting_device>(file);
    const counting_device& counting = *device;
    openxisf::editor edit(std::move(device));
    (void)counting.take_read();
    const pixel_region corner{.origin = {0, 0}, .size = {2, 2}, .channel_count = 1};
    edit.write_pixels(0, std::vector<std::byte>(8), {.region = corner, .verify_checksum = false});
    edit.write_pixels(0, std::vector<std::byte>(8), {.region = corner});
    EXPECT_EQ(counting.take_read(), 0U);
}

// A device over a buffer that records the range of each read through it: where it starts, and its size.
class recording_device final : public openxisf::random_access_device
{
public:
    explicit recording_device(std::span<std::byte> data) : inner_(data) {}

    std::uint64_t size() const override
    {
        return inner_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        reads_.emplace_back(offset, destination.size());
        inner_.read(offset, destination);
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        inner_.write(offset, data);
    }

    [[nodiscard]] const std::vector<std::pair<std::uint64_t, std::uint64_t>>& reads() const noexcept
    {
        return reads_;
    }

    void forget() const noexcept
    {
        reads_.clear();
    }

private:
    openxisf::memory_device inner_;
    mutable std::vector<std::pair<std::uint64_t, std::uint64_t>> reads_;
};

TEST(conformance_editor, a_region_read_reads_the_bytes_of_its_region_alone)
{
    // Threads may read and write regions that share no sample at once, so a read never touches the bytes between the
    // rows of its region, which another thread may be writing, where a reader reads through gaps this small in one
    // read: the two rows of a corner of the frame, whose rows are 32 bytes apart, are two reads of 4 bytes.
    std::vector<std::byte> file = reserved_unit(basic_options());
    auto device = std::make_unique<recording_device>(file);
    const recording_device& recording = *device;
    const openxisf::editor edit(std::move(device));
    recording.forget();
    const pixel_region corner{.origin = {0, 0}, .size = {2, 2}, .channel_count = 1};
    EXPECT_EQ(edit.read_pixels(0, {.region = corner}).size(), 8U);
    ASSERT_EQ(recording.reads().size(), 2U);
    EXPECT_EQ(recording.reads()[0].second, 4U);
    EXPECT_EQ(recording.reads()[1].second, 4U);
    EXPECT_EQ(recording.reads()[1].first - recording.reads()[0].first, 32U);
}

// A device over a buffer that calls a function of the test once, before the first read at a given offset: a write of
// another thread that lands while a read is under way.
class interrupting_device final : public openxisf::random_access_device
{
public:
    interrupting_device(std::span<std::byte> data, std::uint64_t at) : inner_(data), at_(at) {}

    std::uint64_t size() const override
    {
        return inner_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (offset == at_ && interrupt_) {
            const std::function<void()> interrupt = std::exchange(interrupt_, {});
            interrupt();
        }
        inner_.read(offset, destination);
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        inner_.write(offset, data);
    }

    bool supports_concurrent_reads() const override
    {
        return true;
    }

    bool supports_concurrent_writes() const override
    {
        return true;
    }

    void interrupt_with(std::function<void()> function)
    {
        interrupt_ = std::move(function);
    }

private:
    openxisf::memory_device inner_;
    std::uint64_t at_;
    mutable std::function<void()> interrupt_;
};

TEST(conformance_editor, a_read_that_verifies_a_block_while_it_is_first_written_is_made_again_unverified)
{
    // The first write of the reserved frame, into its lower half, lands while a read of its upper half reads the whole
    // block to verify it: the read finds a mismatch, and reads the frame again, unverified, since it is being written.
    openxisf::write_options options = basic_options();
    options.checksum = openxisf::checksum_algorithm::sha256;
    std::vector<std::byte> file = reserved_unit(options);
    const auto header = openxisf::test::parsed_header(file);
    const std::uint64_t position =
        openxisf::detail::parse_location(openxisf::test::element_at(*header, "Image").attribute("location").value())
            .position;
    auto device = std::make_unique<interrupting_device>(file, position);
    interrupting_device& interrupting = *device;
    openxisf::editor edit(std::move(device));

    const pixel_region upper{.origin = {0, 0}, .size = {16, 6}};
    const pixel_region lower{.origin = {0, 6}, .size = {16, 6}};
    const std::vector<std::byte> stacked = native_samples(frame(), pixel_storage::planar, 30000);
    const std::vector<std::byte> data =
        openxisf::test::region_of(stacked, frame(), 2, pixel_storage::planar, lower, pixel_storage::planar);
    interrupting.interrupt_with([&] { edit.write_pixels(0, data, {.region = lower}); });
    EXPECT_EQ(edit.read_pixels(0, {.region = upper}), std::vector<std::byte>(576));
    EXPECT_EQ(edit.read_pixels(0, {.region = lower}), data);
    edit.commit();
    EXPECT_EQ(openxisf::test::open_unit(file, {.strict = true}).read_pixels(0),
              openxisf::test::with_region(std::vector<std::byte>(1152), frame(), 2, pixel_storage::planar, lower, data,
                                          pixel_storage::planar));
    // A mismatch of a block not written is an error, as for a reader.
    file[position] ^= std::byte{1};
    openxisf::editor reopened(std::make_unique<openxisf::memory_device>(file));
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch,
                                                  [&] { (void)reopened.read_pixels(0, {.region = upper}); }));
}

TEST(conformance_editor, a_failure_of_the_device_at_any_write_passes_through_and_a_retry_completes)
{
    // Each write of the device fails in turn, in each way, while a reserved frame with a checksum is filled in tiles
    // and committed: the failure passes through, and an editor that does it all again, without verifying the block that
    // the failure left stale, leaves the frame complete.
    using openxisf::test::fault;
    openxisf::write_options options = basic_options();
    options.checksum = openxisf::checksum_algorithm::sha256;
    const std::vector<std::byte> reserved = reserved_unit(options);
    const std::vector<std::byte> stacked = native_samples(frame(), pixel_storage::planar, 30000);
    const auto fill = [&stacked](std::unique_ptr<openxisf::random_access_device> device, bool verify) {
        openxisf::editor edit(std::move(device));
        write_in_tiles(edit, 0, pixel_storage::planar, stacked, 8, verify);
        edit.commit();
    };
    for (const fault kind : {fault::error, fault::short_transfer, fault::foreign_exception}) {
        std::size_t calls = 0;
        for (std::size_t failing = 1;; ++failing) {
            std::vector<std::byte> file = reserved;
            openxisf::memory_device inner(file);
            bool failed = false;
            try {
                fill(std::make_unique<openxisf::test::faulty_device>(inner, failing, kind), true);
            } catch (const openxisf::io_error& failure) {
                failed = true;
                EXPECT_EQ(failure.code(), errc::write_failed);
            } catch (const openxisf::test::injected_fault&) {
                failed = true;
            }
            if (!failed) {
                calls = failing - 1;
                break;
            }
            fill(std::make_unique<openxisf::memory_device>(file), false);
            EXPECT_EQ(openxisf::test::open_unit(file, {.strict = true}).read_pixels(0), stacked) << failing;
        }
        // Tiles of 8 x 8 and 8 x 4 pixels of 3 channels, 24 rows of each channel written one by one, then the digest
        // and the flush.
        EXPECT_EQ(calls, (24U * 3U) + 2U);
    }
}

TEST(conformance_editor, opens_with_the_options_of_a_reader)
{
    const std::vector<std::byte> samples = native_samples(frame(), pixel_storage::planar);
    openxisf::writer output(basic_options());
    (void)output.add_image(frame_info(), samples);
    output.properties().set("Test:Values", openxisf::property_value(std::vector<double>(1000, 1.0)));
    openxisf::memory_sink sink;
    output.save(sink);
    std::vector<std::byte> file = sink.release();

    // The limits of the options, and a header read alone.
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::header_too_large, [&] {
        openxisf::editor edit(std::make_unique<openxisf::memory_device>(file), {.limits = {.max_header_size = 100}});
    }));
    openxisf::editor edit(std::make_unique<openxisf::memory_device>(file), {.header_only = true});
    EXPECT_EQ(edit.read_pixels(0), samples);
    // A resolver is never called: a monolithic file has no external blocks.
    bool resolved = false;
    openxisf::read_options with_resolver;
    with_resolver.resolver = [&resolved](const openxisf::external_reference&) {
        resolved = true;
        return std::unique_ptr<openxisf::input_source>();
    };
    const openxisf::editor other(std::make_unique<openxisf::memory_device>(file), std::move(with_resolver));
    EXPECT_FALSE(resolved);
}

TEST(conformance_editor, refuses_a_null_device_and_what_is_not_a_unit)
{
    EXPECT_TRUE(throws<openxisf::usage_error>(
        errc::invalid_argument, [] { openxisf::editor edit{std::unique_ptr<openxisf::random_access_device>()}; }));
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument, [] { openxisf::editor edit(""); }));
    const temp_directory directory;
    EXPECT_TRUE(
        throws<openxisf::io_error>(errc::open_failed, [&] { openxisf::editor edit(directory.file("missing.xisf")); }));
    std::vector<std::byte> text = openxisf::test::bytes("not a unit at all");
    EXPECT_TRUE(throws<openxisf::invalid_data_error>(
        errc::not_an_xisf_unit, [&] { openxisf::editor edit(std::make_unique<openxisf::memory_device>(text)); }));
    std::vector<std::byte> empty;
    EXPECT_TRUE(throws<openxisf::invalid_data_error>(
        errc::not_an_xisf_unit, [&] { openxisf::editor edit(std::make_unique<openxisf::memory_device>(empty)); }));
}

TEST(conformance_editor, can_be_moved)
{
    std::vector<std::byte> file = reserved_unit(basic_options());
    std::vector<std::byte> second = file;
    openxisf::editor first(std::make_unique<openxisf::memory_device>(file));
    openxisf::editor moved(std::move(first));
    const std::vector<std::byte> stacked = native_samples(frame(), pixel_storage::planar, 30000);
    moved.write_pixels(0, stacked);
    openxisf::editor other(std::make_unique<openxisf::memory_device>(second));
    other = std::move(moved);
    other.commit();
    EXPECT_EQ(other.read_pixels(0), stacked);
    EXPECT_EQ(openxisf::test::open_unit(file, {.strict = true}).read_pixels(0), stacked);
}

} // namespace
