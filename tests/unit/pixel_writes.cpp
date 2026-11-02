// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The writes of pixel data in place: every region of an image in each form of block that can be written in place, the
// writes that a region costs, the changes of byte order and storage model, progress, the checks made before anything is
// written, the blocks that cannot be written in place, and the digests rewritten in place.

#include "model/pixel_writes.h"

#include <openxisf/editor.h>
#include <openxisf/error.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>

#include "core/data_encoding.h"
#include "crypto/hash.h"
#include "io/thread_safe_device.h"
#include "model/pixels.h"
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
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::geometry;
using openxisf::pixel_region;
using openxisf::pixel_storage;
using openxisf::pixel_write_options;
using openxisf::detail::block_descriptor;
using openxisf::detail::region_layout;
using openxisf::detail::thread_safe_device;
using openxisf::detail::unit;
using openxisf::test::block_form;
using openxisf::test::describe;
using openxisf::test::descriptor_at;
using openxisf::test::native_samples;
using openxisf::test::row_of;
using openxisf::test::throws;
using openxisf::test::unit_of;

// A write that a device received: where, how many bytes, and from where in memory.
struct recorded_write
{
    std::uint64_t offset = 0;
    std::size_t length = 0;
    const std::byte* from = nullptr;
};

// A device over a buffer of the test, which records the writes it receives.
class recording_device final : public openxisf::random_access_device
{
public:
    explicit recording_device(std::vector<std::byte>& content) : content_(content) {}

    std::uint64_t size() const override
    {
        return content_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        std::copy_n(content_.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(), destination.begin());
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        writes_.push_back({.offset = offset, .length = data.size(), .from = data.data()});
        std::ranges::copy(data, content_.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    [[nodiscard]] const std::vector<recorded_write>& writes() const noexcept
    {
        return writes_;
    }

    void clear() noexcept
    {
        writes_.clear();
    }

private:
    std::vector<std::byte>& content_;
    std::vector<recorded_write> writes_;
};

// A unit in a buffer, opened through a device over it, as an editor opens one.
struct edited_unit
{
    explicit edited_unit(std::vector<std::byte> file, openxisf::read_options options = {})
        : bytes(std::move(file)), recorded(new recording_device(bytes)),
          device(std::unique_ptr<openxisf::random_access_device>(recorded)), opened(device.source(), std::move(options))
    {
        recorded->clear();
    }

    std::vector<std::byte> bytes;
    // Owned by the device.
    recording_device* recorded;
    thread_safe_device device;
    unit opened;
};

// Writes data into the image of the unit with options, as an editor writes it.
void write(const edited_unit& edited, std::span<const std::byte> data, const pixel_write_options& options,
           std::size_t piece_size = openxisf::detail::default_piece_size)
{
    const region_layout layout = openxisf::detail::write_layout(edited.opened, 0, data, options);
    openxisf::detail::write_pixels(edited.opened, edited.device, 0, data, layout, options, piece_size);
}

// The pixel data of the image of the unit, read without verifying them.
std::vector<std::byte> read_back(const edited_unit& edited)
{
    return openxisf::detail::read_pixels(edited.opened, 0, {.verify_checksum = false});
}

TEST(pixel_writes, every_region_of_an_image_is_written_in_place)
{
    // Images of 4 x 3 pixels of 2 channels, and of 3 x 2 x 2 pixels of 2 channels, in each form of block that can be
    // written in place: the samples of every region, given in each model, with rows that follow each other and with a
    // pitch, written in pieces of the default size and of 3 bytes, replace those of the region in the image, and
    // nothing else of the file changes.
    for (const geometry& image :
         {geometry{.dimensions = {4, 3}, .channels = 2}, geometry{.dimensions = {3, 2, 2}, .channels = 2}}) {
        const std::vector<pixel_region> regions = openxisf::test::every_region(image);
        for (const block_form& form :
             {block_form{}, block_form{.storage = pixel_storage::normal}, block_form{.big_endian = true},
              block_form{.storage = pixel_storage::normal, .checksum = true, .big_endian = true},
              block_form{.checksum = true}}) {
            edited_unit edited(unit_of(image, form));
            ASSERT_TRUE(edited.opened.diagnostics.empty()) << describe(form);
            const std::vector<std::byte> original = edited.bytes;
            const std::vector<std::byte> whole = native_samples(image, form.storage);
            const std::vector<std::byte> other = native_samples(image, form.storage, 30000);
            const block_descriptor& descriptor = descriptor_at(edited.opened, "/xisf/Image[1]");
            const auto start = static_cast<std::ptrdiff_t>(descriptor.location.position);
            const auto end = start + static_cast<std::ptrdiff_t>(descriptor.location.size);
            for (const pixel_region& region : regions) {
                for (const pixel_storage storage : {pixel_storage::planar, pixel_storage::normal}) {
                    for (const bool pitched : {false, true}) {
                        const std::uint64_t pitch = pitched ? row_of(region, storage) + 3 : 0;
                        const std::vector<std::byte> data = openxisf::test::region_of(
                            other, image, 2, form.storage, region, storage, pitch, std::byte{0xEE});
                        const std::vector<std::byte> expected =
                            openxisf::test::with_region(whole, image, 2, form.storage, region, data, storage, pitch);
                        for (const std::size_t piece : {openxisf::detail::default_piece_size, std::size_t{3}}) {
                            std::ranges::copy(original, edited.bytes.begin());
                            write(edited, data, {.storage = storage, .region = region, .pitch = pitch}, piece);
                            EXPECT_EQ(read_back(edited), expected) << describe(form) << ", " << describe(region)
                                                                   << " pitch " << pitch << " piece " << piece;
                            EXPECT_TRUE(std::equal(original.begin(), original.begin() + start, edited.bytes.begin()) &&
                                        std::equal(original.begin() + end, original.end(), edited.bytes.begin() + end))
                                << describe(form) << ", " << describe(region);
                        }
                    }
                }
            }
        }
    }
}

// An image of 8 x 6 pixels of 3 channels in the model storage, little-endian unless big_endian.
std::vector<std::byte> frame(pixel_storage storage, bool big_endian = false)
{
    return unit_of(geometry{.dimensions = {8, 6}, .channels = 3},
                   block_form{.storage = storage, .big_endian = big_endian});
}

// The writes as (offset in the block, length, offset in data) triples, for comparisons.
std::vector<std::vector<std::uint64_t>> writes_of(const edited_unit& edited, std::span<const std::byte> data)
{
    const std::uint64_t base = descriptor_at(edited.opened, "/xisf/Image[1]").location.position;
    std::vector<std::vector<std::uint64_t>> found;
    for (const recorded_write& entry : edited.recorded->writes()) {
        const bool inside = entry.from >= data.data() && entry.from < data.data() + data.size();
        found.push_back({entry.offset - base, entry.length,
                         inside ? static_cast<std::uint64_t>(entry.from - data.data()) : std::uint64_t{999999}});
    }
    return found;
}

TEST(pixel_writes, a_band_of_full_rows_is_one_write_per_channel_straight_from_the_data)
{
    // Rows 2 to 4 of a planar image of 8 x 6 pixels: in each plane of 96 bytes, 48 bytes from byte 32.
    const edited_unit edited(frame(pixel_storage::planar));
    const std::vector<std::byte> data = openxisf::test::pattern(144);
    write(edited, data, {.region = pixel_region{.origin = {0, 2}, .size = {8, 3}}});
    EXPECT_EQ(writes_of(edited, data),
              (std::vector<std::vector<std::uint64_t>>{{32, 48, 0}, {128, 48, 48}, {224, 48, 96}}));
    // The whole image is one write.
    edited.recorded->clear();
    const std::vector<std::byte> whole = openxisf::test::pattern(288);
    write(edited, whole, {});
    EXPECT_EQ(writes_of(edited, whole), (std::vector<std::vector<std::uint64_t>>{{0, 288, 0}}));
    EXPECT_EQ(read_back(edited), whole);
}

TEST(pixel_writes, a_subset_of_the_channels_of_normal_storage_is_one_write_per_pixel)
{
    // Channel 1 of the 4 x 2 pixels from (2, 1) of a normal image of 8 x 6 pixels of 3 channels: one sample of 2 bytes
    // at each of 8 pixels, 6 bytes apart.
    const edited_unit edited(frame(pixel_storage::normal));
    const std::vector<std::byte> data = openxisf::test::pattern(16);
    write(edited, data,
          {.region = pixel_region{.origin = {2, 1}, .size = {4, 2}, .first_channel = 1, .channel_count = 1}});
    std::vector<std::vector<std::uint64_t>> expected;
    for (std::uint64_t y = 0; y < 2; ++y) {
        for (std::uint64_t x = 0; x < 4; ++x) {
            expected.push_back({((((1 + y) * 8) + 2 + x) * 6) + 2, 2, ((y * 4) + x) * 2});
        }
    }
    EXPECT_EQ(writes_of(edited, data), expected);
    // Every channel of the same pixels: a row of 4 pixels is one write.
    edited.recorded->clear();
    const std::vector<std::byte> all = openxisf::test::pattern(48);
    write(edited, all, {.region = pixel_region{.origin = {2, 1}, .size = {4, 2}}});
    EXPECT_EQ(writes_of(edited, all), (std::vector<std::vector<std::uint64_t>>{{60, 24, 0}, {108, 24, 24}}));
}

TEST(pixel_writes, the_rows_of_data_with_a_pitch_are_written_one_piece_each)
{
    // 4 x 3 pixels from (2, 1) of channel 0, from rows of 4 samples 10 samples apart; and a band of full rows, which is
    // one run in the block and three pieces in the data.
    const edited_unit edited(frame(pixel_storage::planar));
    const std::vector<std::byte> data = openxisf::test::pattern(48);
    write(edited, data, {.region = pixel_region{.origin = {2, 1}, .size = {4, 3}, .channel_count = 1}, .pitch = 10});
    EXPECT_EQ(writes_of(edited, data), (std::vector<std::vector<std::uint64_t>>{{20, 8, 0}, {36, 8, 20}, {52, 8, 40}}));
    edited.recorded->clear();
    const std::vector<std::byte> band = openxisf::test::pattern(84);
    write(edited, band, {.region = pixel_region{.origin = {0, 1}, .size = {8, 3}, .channel_count = 1}, .pitch = 13});
    EXPECT_EQ(writes_of(edited, band),
              (std::vector<std::vector<std::uint64_t>>{{16, 16, 0}, {32, 16, 26}, {48, 16, 52}}));
}

TEST(pixel_writes, samples_that_change_their_byte_order_go_through_a_staging_buffer)
{
    // Rows 2 to 4 of channel 0 of a big-endian block: one run of 48 bytes, written in parts of at most the piece size,
    // whole samples, from a buffer of the library, swapped there.
    const std::vector<std::byte> data = openxisf::test::pattern(48);
    for (const std::size_t piece : {std::size_t{16}, std::size_t{3}}) {
        const edited_unit edited(frame(pixel_storage::planar, true));
        write(edited, data, {.region = pixel_region{.origin = {0, 2}, .size = {8, 3}, .channel_count = 1}}, piece);
        const std::uint64_t part = piece == 16 ? 16 : 2;
        std::vector<std::vector<std::uint64_t>> expected;
        for (std::uint64_t at = 0; at < 48; at += part) {
            expected.push_back({32 + at, part, 999999});
        }
        EXPECT_EQ(writes_of(edited, data), expected) << piece;
        // The block holds the samples in big-endian byte order, and reads back as they were given.
        const std::vector<std::byte> samples = read_back(edited);
        EXPECT_TRUE(std::equal(data.begin(), data.end(), samples.begin() + 32)) << piece;
        const std::uint64_t base = descriptor_at(edited.opened, "/xisf/Image[1]").location.position;
        const std::vector<std::byte> stored(edited.bytes.begin() + static_cast<std::ptrdiff_t>(base + 32),
                                            edited.bytes.begin() + static_cast<std::ptrdiff_t>(base + 80));
        EXPECT_EQ(stored, openxisf::test::in_byte_order(data, true)) << piece;
    }
}

TEST(pixel_writes, samples_in_the_other_storage_model_are_converted_first)
{
    // A tile of every channel of a planar image, given in the normal model: written from a buffer of the region in the
    // planar model, one run per row and channel; beyond the allocation limit, nothing is written.
    const geometry image{.dimensions = {8, 6}, .channels = 3};
    const pixel_region tile{.origin = {2, 1}, .size = {4, 2}};
    const std::vector<std::byte> other = native_samples(image, pixel_storage::normal, 30000);
    const std::vector<std::byte> data =
        openxisf::test::region_of(other, image, 2, pixel_storage::normal, tile, pixel_storage::normal);
    const edited_unit edited(frame(pixel_storage::planar));
    write(edited, data, {.storage = pixel_storage::normal, .region = tile});
    EXPECT_EQ(edited.recorded->writes().size(), 6U);
    EXPECT_EQ(writes_of(edited, data).front().back(), 999999U);
    EXPECT_EQ(read_back(edited), openxisf::test::with_region(native_samples(image, pixel_storage::planar), image, 2,
                                                             pixel_storage::planar, tile, data, pixel_storage::normal));

    const edited_unit limited(frame(pixel_storage::planar), {.limits = {.max_allocation = 47}});
    EXPECT_TRUE(throws<openxisf::limit_error>(
        errc::allocation_too_large, [&] { write(limited, data, {.storage = pixel_storage::normal, .region = tile}); }));
    EXPECT_TRUE(limited.recorded->writes().empty());
    const edited_unit enough(frame(pixel_storage::planar), {.limits = {.max_allocation = 48}});
    write(enough, data, {.storage = pixel_storage::normal, .region = tile});
}

TEST(pixel_writes, progress_counts_the_bytes_of_the_region_and_can_cancel_the_write)
{
    // The whole image, 288 bytes, in parts of 64 bytes when its progress is reported, and the channel of a tile, 16
    // bytes in 8 writes, which is reported once it is written.
    const edited_unit edited(frame(pixel_storage::planar));
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    const auto record = [&calls](std::uint64_t done, std::uint64_t total) {
        calls.emplace_back(done, total);
        return true;
    };
    write(edited, openxisf::test::pattern(288), {.progress = record}, 64);
    EXPECT_EQ(calls, (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                         {0, 288}, {64, 288}, {128, 288}, {192, 288}, {256, 288}, {288, 288}}));
    EXPECT_EQ(edited.recorded->writes().size(), 5U);
    calls.clear();
    write(edited, openxisf::test::pattern(16),
          {.progress = record,
           .region = pixel_region{.origin = {2, 1}, .size = {4, 2}, .first_channel = 1, .channel_count = 1}},
          64);
    EXPECT_EQ(calls, (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 16}, {16, 16}}));

    // Cancelled at its third report: two parts were written, and the error names the element.
    edited.recorded->clear();
    int reports = 0;
    try {
        write(edited, openxisf::test::pattern(288),
              {.progress = [&reports](std::uint64_t, std::uint64_t) { return ++reports < 3; }}, 64);
        ADD_FAILURE() << "no exception";
    } catch (const openxisf::cancelled_error& failure) {
        EXPECT_EQ(failure.code(), errc::cancelled);
        EXPECT_EQ(failure.context().element, "/xisf/Image[1]");
    }
    EXPECT_EQ(edited.recorded->writes().size(), 2U);
}

TEST(pixel_writes, the_layout_of_a_write_is_checked_before_anything_is_written)
{
    const edited_unit edited(frame(pixel_storage::planar));
    const std::vector<std::byte> data = openxisf::test::pattern(16);
    const pixel_region tile{.origin = {2, 1}, .size = {4, 2}, .channel_count = 1};
    const auto refused = [&edited](errc code, std::span<const std::byte> given, const pixel_write_options& options) {
        return throws<openxisf::usage_error>(
            code, [&] { (void)openxisf::detail::write_layout(edited.opened, 0, given, options); });
    };
    EXPECT_TRUE(refused(errc::invalid_argument, data, {.region = pixel_region{.origin = {6, 0}, .size = {4, 1}}}));
    EXPECT_TRUE(refused(errc::invalid_argument, data, {.region = tile, .pitch = 3}));
    // Exactly the bytes of the region, or with a pitch at least those that its rows span.
    EXPECT_TRUE(refused(errc::invalid_argument, std::span(data).first(15), {.region = tile}));
    const std::vector<std::byte> more = openxisf::test::pattern(20);
    EXPECT_TRUE(refused(errc::invalid_argument, more, {.region = tile}));
    // Two rows of 4 samples, 5 samples apart: 9 samples.
    EXPECT_TRUE(refused(errc::invalid_argument, std::span(more).first(17), {.region = tile, .pitch = 5}));
    EXPECT_EQ(
        openxisf::detail::write_layout(edited.opened, 0, std::span(more).first(18), {.region = tile, .pitch = 5}).size,
        18U);
    EXPECT_EQ(openxisf::detail::write_layout(edited.opened, 0, more, {.region = tile, .pitch = 5}).size, 18U);
    EXPECT_TRUE(throws<openxisf::usage_error>(
        errc::invalid_argument, [&] { (void)openxisf::detail::write_layout(edited.opened, 1, data, {}); }));
    EXPECT_TRUE(edited.recorded->writes().empty());
}

// The diagnostic that check_editable() throws for the image at index of the unit in file, or a failure.
testing::AssertionResult not_editable(const std::vector<std::byte>& file, std::size_t index = 0)
{
    const unit opened = openxisf::test::open_internal(file);
    const std::vector<bool> overlapping = openxisf::detail::overlapping_blocks(opened.blocks);
    return throws<openxisf::unsupported_error>(errc::not_editable,
                                               [&] { openxisf::detail::check_editable(opened, index, overlapping); });
}

TEST(pixel_writes, a_block_that_cannot_be_written_in_place_is_not_editable)
{
    const geometry image{.dimensions = {4, 3}, .channels = 2};
    // An embedded block, a compressed one, and one whose codec is unknown, which is unavailable.
    EXPECT_TRUE(not_editable(unit_of(image, block_form{.embedded = true})));
    EXPECT_TRUE(not_editable(unit_of(image, block_form{.compressed = true})));
    const std::string attributes = openxisf::test::image_attributes(image, pixel_storage::planar);
    EXPECT_TRUE(not_editable(openxisf::test::file_with_attachments(
        openxisf::test::header_xml("<Image " + attributes + R"( compression="rle:48" location="attachment:{0}"/>)"),
        {openxisf::test::pattern(48)})));
    // A checksum whose digest is written with a character reference, which a digest of its size would not replace.
    const std::vector<std::byte> samples = native_samples(image, pixel_storage::planar);
    const std::string digest =
        openxisf::detail::encode_hex(openxisf::detail::compute_digest(openxisf::detail::hash_algorithm::sha1, samples));
    EXPECT_TRUE(not_editable(openxisf::test::file_with_attachments(
        openxisf::test::header_xml("<Image " + attributes + R"( checksum="sha1:&#x)" +
                                   openxisf::detail::encode_hex(std::as_bytes(std::span(digest).first(1))) + ";" +
                                   digest.substr(1) + R"(" location="attachment:{0}"/>)"),
        {samples})));
    // Blocks that share bytes: two images of one attachment, and a property inside the attachment of an image.
    const std::vector<std::byte> shared = openxisf::test::file_with_attachments(
        openxisf::test::header_xml("<Image " + attributes + R"( location="attachment:{0}"/>)" + "<Image " + attributes +
                                   R"( location="attachment:{0}"/>)"),
        {samples});
    EXPECT_TRUE(not_editable(shared, 0));
    EXPECT_TRUE(not_editable(shared, 1));
    std::vector<std::byte> inside = openxisf::test::file_with_attachments(
        openxisf::test::header_xml("<Image " + attributes + R"( location="attachment:{0}"/>)" +
                                   R"(<Property id="Test:Inside" type="ByteArray" location="attachment:4100:4"/>)"),
        {samples});
    EXPECT_TRUE(not_editable(inside));

    // An image that a Reference lists again shares its block, which is no other block.
    const unit referenced = openxisf::test::open_internal(openxisf::test::file_with_attachments(
        openxisf::test::header_xml("<Image uid=\"first\" " + attributes + R"( location="attachment:{0}"/>)" +
                                   R"(<Reference ref="first"/>)"),
        {samples}));
    const std::vector<bool> overlapping = openxisf::detail::overlapping_blocks(referenced.blocks);
    openxisf::detail::check_editable(referenced, 0, overlapping);
    openxisf::detail::check_editable(referenced, 1, overlapping);
    // As are every block that the writer writes uncompressed, with or without checksum.
    for (const block_form& form : {block_form{}, block_form{.checksum = true, .big_endian = true}}) {
        const unit opened = openxisf::test::open_internal(unit_of(image, form));
        openxisf::detail::check_editable(opened, 0, openxisf::detail::overlapping_blocks(opened.blocks));
    }
}

TEST(pixel_writes, blocks_that_share_bytes_are_found_in_any_order)
{
    // Attachments [0, 10), [2, 3) and [5, 6) inside it, [20, 30) and [29, 31) that meet it, [40, 41) alone, an empty
    // one, and blocks that are not attached.
    using openxisf::detail::location_kind;
    const auto stored = [](location_kind kind, std::uint64_t position, std::uint64_t size) {
        openxisf::detail::data_block block;
        block.descriptor = block_descriptor{.location = {.kind = kind, .position = position, .size = size}};
        return block;
    };
    const auto attached = [&stored](std::uint64_t position, std::uint64_t size) {
        return stored(location_kind::attachment, position, size);
    };
    const std::vector<openxisf::detail::data_block> blocks{attached(29, 2),
                                                           attached(5, 1),
                                                           attached(40, 1),
                                                           attached(0, 10),
                                                           attached(2, 1),
                                                           attached(20, 10),
                                                           attached(41, 0),
                                                           openxisf::detail::data_block{},
                                                           stored(location_kind::embedded, 0, 50)};
    EXPECT_EQ(openxisf::detail::overlapping_blocks(blocks),
              (std::vector<bool>{true, true, false, true, true, true, false, false, false}));
}

TEST(pixel_writes, the_digest_of_a_block_written_is_rewritten_in_place)
{
    // A planar block with a SHA-1 checksum, in uppercase: the new digest replaces it, in lowercase, at its place, and
    // the descriptor holds it, so that the unit reads back verified.
    const geometry image{.dimensions = {4, 3}, .channels = 2};
    const std::vector<std::byte> samples = native_samples(image, pixel_storage::planar);
    std::string digest = openxisf::detail::encode_hex(
        openxisf::detail::compute_digest(openxisf::detail::hash_algorithm::sha256, samples));
    std::ranges::transform(digest, digest.begin(),
                           [](char c) { return c >= 'a' && c <= 'f' ? static_cast<char>(c - 32) : c; });
    edited_unit edited(openxisf::test::file_with_attachments(
        openxisf::test::header_xml("<Image " + openxisf::test::image_attributes(image, pixel_storage::planar) +
                                   R"( checksum="sha256:)" + digest + R"(" location="attachment:{0}"/>)"),
        {samples}));
    const std::vector<std::byte> data = native_samples(image, pixel_storage::planar, 30000);
    write(edited, data, {});
    const std::vector<std::byte> before = edited.bytes;

    std::optional<block_descriptor>& held = edited.opened.blocks.front().descriptor;
    if (!held) {
        FAIL() << "the block is unavailable";
    }
    block_descriptor& descriptor = *held;
    const std::uint64_t place = descriptor.digest_offset.value_or(0);
    ASSERT_NE(place, 0U);
    for (const std::size_t piece : {openxisf::detail::default_piece_size, std::size_t{5}}) {
        openxisf::detail::refresh_digest(edited.device, descriptor, piece);
        const std::vector<std::byte> expected =
            openxisf::detail::compute_digest(openxisf::detail::hash_algorithm::sha256, data);
        EXPECT_EQ(openxisf::test::value_of(descriptor.checksum).digest, expected);
        const std::string written = openxisf::test::text(std::span(edited.bytes).subspan(place, expected.size() * 2));
        EXPECT_EQ(written, openxisf::detail::encode_hex(expected));
    }
    // Nothing else changed, and the file reads back verified.
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (i < place || i >= place + 64) {
            ASSERT_EQ(edited.bytes[i], before[i]) << i;
        }
    }
    const openxisf::reader reread = openxisf::test::open_unit(edited.bytes, {.strict = true});
    EXPECT_EQ(reread.read_pixels(0), data);
    EXPECT_EQ(openxisf::detail::read_pixels(edited.opened, 0, {}), data);
}

TEST(pixel_writes, the_digest_of_a_block_is_verified_in_pieces_before_it_is_written)
{
    // A planar block with a SHA-256 checksum verifies as it is, in pieces of any size, and fails with the element of
    // its block once a byte changes; a block without a checksum is not read.
    const geometry image{.dimensions = {4, 3}, .channels = 2};
    const std::vector<std::byte> samples = native_samples(image, pixel_storage::planar);
    const std::string attributes = "<Image " + openxisf::test::image_attributes(image, pixel_storage::planar);
    const std::string header = openxisf::test::header_xml(attributes + R"( checksum="sha256:)" +
                                                          openxisf::detail::encode_hex(openxisf::detail::compute_digest(
                                                              openxisf::detail::hash_algorithm::sha256, samples)) +
                                                          R"(" location="attachment:{0}"/>)");
    const edited_unit sound(openxisf::test::file_with_attachments(header, {samples}));
    for (const std::size_t piece : {openxisf::detail::default_piece_size, std::size_t{5}}) {
        openxisf::detail::verify_digest(sound.device, sound.opened.blocks.front(), piece);
    }

    std::vector<std::byte> corrupt = samples;
    corrupt.back() ^= std::byte{0x01};
    const edited_unit broken(openxisf::test::file_with_attachments(header, {corrupt}));
    try {
        openxisf::detail::verify_digest(broken.device, broken.opened.blocks.front(), 5);
        ADD_FAILURE() << "no error";
    } catch (const openxisf::integrity_error& failure) {
        EXPECT_EQ(failure.code(), errc::checksum_mismatch);
        EXPECT_EQ(failure.context().element, "/xisf/Image[1]");
    }
    const edited_unit plain(openxisf::test::file_with_attachments(
        openxisf::test::header_xml(attributes + R"( location="attachment:{0}"/>)"), {corrupt}));
    openxisf::detail::verify_digest(plain.device, plain.opened.blocks.front());
}

TEST(pixel_writes, a_failure_of_the_device_stops_a_write_there_and_passes_through)
{
    // Each write of the rows of a tile of the 8 x 6 image fails in turn, in each way.
    using openxisf::test::fault;
    const std::vector<std::byte> data = openxisf::test::pattern(16);
    const pixel_write_options options{.region = pixel_region{.origin = {2, 1}, .size = {4, 2}, .channel_count = 1}};
    for (const fault kind : {fault::error, fault::short_transfer, fault::foreign_exception}) {
        for (std::size_t failing = 1; failing <= 2; ++failing) {
            std::vector<std::byte> file = frame(pixel_storage::planar);
            openxisf::memory_device inner(file);
            const thread_safe_device device(std::make_unique<openxisf::test::faulty_device>(inner, failing, kind));
            const unit opened(device.source(), {});
            const region_layout layout = openxisf::detail::write_layout(opened, 0, data, options);
            const auto attempt = [&] { openxisf::detail::write_pixels(opened, device, 0, data, layout, options); };
            if (kind == fault::foreign_exception) {
                EXPECT_THROW(attempt(), openxisf::test::injected_fault);
            } else {
                EXPECT_TRUE(throws<openxisf::io_error>(errc::write_failed, attempt)) << failing;
            }
        }
    }
}

} // namespace
