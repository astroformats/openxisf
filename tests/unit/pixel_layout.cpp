// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/pixel_layout.h"

#include <openxisf/error.h>
#include <openxisf/types.h>

#include "container/block_attributes.h"
#include "support/bytes.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <bit>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::pixel_storage;
using openxisf::sample_format;
using openxisf::detail::byte_order;
using openxisf::detail::convert_storage;
using openxisf::detail::to_native_byte_order;
using namespace std::string_view_literals;

// The bytes of value in big-endian order, written out independently of the host.
std::vector<std::byte> big_endian(std::uint64_t value, std::size_t size)
{
    std::vector<std::byte> bytes(size);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[size - 1 - i] = static_cast<std::byte>((value >> (8 * i)) & 0xFFU);
    }
    return bytes;
}

std::vector<std::byte> reversed(std::vector<std::byte> bytes)
{
    return {bytes.rbegin(), bytes.rend()};
}

template <typename T> T load(const std::vector<std::byte>& bytes)
{
    T value{};
    std::memcpy(&value, bytes.data(), sizeof(T));
    return value;
}

TEST(pixel_layout, the_byte_order_of_a_complex_sample_applies_to_each_part)
{
    // Spec §10.4: the byte order is that of the scalars that make up the data.
    EXPECT_EQ(openxisf::detail::byte_order_item_size(sample_format::uint8), 1U);
    EXPECT_EQ(openxisf::detail::byte_order_item_size(sample_format::uint16), 2U);
    EXPECT_EQ(openxisf::detail::byte_order_item_size(sample_format::uint32), 4U);
    EXPECT_EQ(openxisf::detail::byte_order_item_size(sample_format::uint64), 8U);
    EXPECT_EQ(openxisf::detail::byte_order_item_size(sample_format::float32), 4U);
    EXPECT_EQ(openxisf::detail::byte_order_item_size(sample_format::float64), 8U);
    EXPECT_EQ(openxisf::detail::byte_order_item_size(sample_format::complex32), 4U);
    EXPECT_EQ(openxisf::detail::byte_order_item_size(sample_format::complex64), 8U);
}

TEST(pixel_layout, samples_come_to_native_byte_order_from_either_order)
{
    std::vector<std::byte> big = big_endian(0x0102030405060708U, 8);
    std::vector<std::byte> little = reversed(big);
    to_native_byte_order(big, sample_format::uint64, byte_order::big);
    to_native_byte_order(little, sample_format::uint64, byte_order::little);
    EXPECT_EQ(load<std::uint64_t>(big), 0x0102030405060708U);
    EXPECT_EQ(load<std::uint64_t>(little), 0x0102030405060708U);

    // 8-bit samples have no byte order.
    std::vector<std::byte> bytes = openxisf::test::bytes("\x01\x02");
    to_native_byte_order(bytes, sample_format::uint8, byte_order::big);
    EXPECT_EQ(bytes, openxisf::test::bytes("\x01\x02"));
}

TEST(pixel_layout, the_parts_of_a_complex_sample_stay_in_their_order)
{
    // The real part comes first in either byte order; only the bytes of each part are reversed.
    std::vector<std::byte> big = big_endian(std::bit_cast<std::uint32_t>(1.5F), 4);
    const std::vector<std::byte> imag = big_endian(std::bit_cast<std::uint32_t>(-2.25F), 4);
    big.insert(big.end(), imag.begin(), imag.end());
    to_native_byte_order(big, sample_format::complex32, byte_order::big);
    EXPECT_EQ(load<std::complex<float>>(big), std::complex<float>(1.5F, -2.25F));
}

// The sample of channel c at pixel p of a test image: its coordinates in the digits, so that a sample in a wrong place
// shows.
std::uint16_t sample_value(std::size_t c, std::size_t p)
{
    return static_cast<std::uint16_t>((1000 * c) + p);
}

// The samples of an image with the given pixel count and channels, 16-bit, in a storage model: spec §8.5.3 orders the
// pixels by their coordinates, the first varying fastest, which is the order of their index p; the planar model stores
// channel by channel, the normal model pixel by pixel.
std::vector<std::byte> samples(std::size_t pixels, std::size_t channels, pixel_storage storage)
{
    std::vector<std::uint16_t> values(pixels * channels);
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t p = 0; p < pixels; ++p) {
            values[storage == pixel_storage::planar ? (c * pixels) + p : (p * channels) + c] = sample_value(c, p);
        }
    }
    std::vector<std::byte> bytes(values.size() * 2);
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
}

TEST(pixel_layout, planar_and_normal_storage_order_the_samples_of_a_two_dimensional_image)
{
    // An RGB image of 3 x 2 pixels, written out: R(0,0) R(1,0) R(2,0) R(0,1) R(1,1) R(2,1) G... in the planar model,
    // and R(0,0) G(0,0) B(0,0) R(1,0) ... in the normal one, where the sample of channel c at (x, y) is 100c + 10y + x.
    const std::vector<std::byte> planar = openxisf::test::bytes("\x00\x01\x02\x0A\x0B\x0C"
                                                                "\x64\x65\x66\x6E\x6F\x70"
                                                                "\xC8\xC9\xCA\xD2\xD3\xD4"sv);
    const std::vector<std::byte> normal = openxisf::test::bytes("\x00\x64\xC8\x01\x65\xC9\x02\x66\xCA"
                                                                "\x0A\x6E\xD2\x0B\x6F\xD3\x0C\x70\xD4"sv);
    std::vector<std::byte> converted(planar.size());
    convert_storage(planar, converted, pixel_storage::planar, 6, 3, 1);
    EXPECT_EQ(converted, normal);
    convert_storage(normal, converted, pixel_storage::normal, 6, 3, 1);
    EXPECT_EQ(converted, planar);
}

TEST(pixel_layout, one_and_three_dimensional_images_have_the_same_order_by_pixel)
{
    // 1-D: 5 pixels of 2 channels. 3-D: 2 x 3 x 2 pixels of 4 channels.
    for (const auto& [pixels, channels] : {std::pair<std::size_t, std::size_t>{5, 2}, {12, 4}}) {
        const std::vector<std::byte> planar = samples(pixels, channels, pixel_storage::planar);
        const std::vector<std::byte> normal = samples(pixels, channels, pixel_storage::normal);
        std::vector<std::byte> converted(planar.size());
        convert_storage(planar, converted, pixel_storage::planar, pixels, channels, 2);
        EXPECT_EQ(converted, normal) << pixels;
        convert_storage(normal, converted, pixel_storage::normal, pixels, channels, 2);
        EXPECT_EQ(converted, planar) << pixels;
    }
}

TEST(pixel_layout, a_single_channel_is_stored_the_same_way_in_both_models)
{
    const std::vector<std::byte> data = openxisf::test::pattern(30);
    std::vector<std::byte> converted(data.size());
    convert_storage(data, converted, pixel_storage::planar, 15, 1, 2);
    EXPECT_EQ(converted, data);
}

TEST(pixel_layout, a_large_image_is_converted_piece_by_piece)
{
    // Images are converted in pieces of about 1 MiB, and this one spans several: 3 channels of 32-bit samples whose
    // values are their channel and pixel.
    constexpr std::size_t pixels = 250'003;
    constexpr std::size_t channels = 3;
    std::vector<std::uint32_t> planar(pixels * channels);
    std::vector<std::uint32_t> normal(pixels * channels);
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t p = 0; p < pixels; ++p) {
            const auto value = static_cast<std::uint32_t>((c << 24U) | p);
            planar[(c * pixels) + p] = value;
            normal[(p * channels) + c] = value;
        }
    }
    std::vector<std::uint32_t> converted(planar.size());
    convert_storage(std::as_bytes(std::span(planar)), std::as_writable_bytes(std::span(converted)),
                    pixel_storage::planar, pixels, channels, 4);
    EXPECT_EQ(converted, normal);
    convert_storage(std::as_bytes(std::span(normal)), std::as_writable_bytes(std::span(converted)),
                    pixel_storage::normal, pixels, channels, 4);
    EXPECT_EQ(converted, planar);
}

// A run as a pair: where it starts in the block, and its length.
using run_list = std::vector<std::pair<std::uint64_t, std::uint64_t>>;

// The runs, walked with a cursor, each checked against its random access.
run_list runs_of(const openxisf::detail::region_runs& runs)
{
    run_list list;
    for (openxisf::detail::region_runs::cursor run = runs.at(0); !run.done(); run.next()) {
        EXPECT_EQ(run.offset(), runs.offset(run.index())) << run.index();
        EXPECT_EQ(run.position(), run.index() * runs.length()) << run.index();
        list.emplace_back(run.offset(), runs.length());
    }
    EXPECT_EQ(list.size(), runs.count());
    return list;
}

// The groups as pairs: where each starts in the block, and where it ends.
run_list groups_of(const openxisf::detail::region_runs& runs)
{
    run_list list;
    for (std::uint64_t g = 0; g < runs.groups(); ++g) {
        list.emplace_back(runs.group_start(g), runs.group_end(g));
    }
    return list;
}

// The runs of a region of the image of 4 x 3 pixels and 2 channels, of samples of sample_size bytes. Channel c of pixel
// (x, y) is at c × 12 + y × 4 + x in planar storage, and at (y × 4 + x) × 2 + c in normal storage, in samples.
openxisf::detail::region_runs runs_in_4x3x2(const openxisf::pixel_region& region, pixel_storage storage,
                                            std::size_t sample_size = 1)
{
    const openxisf::geometry image{.dimensions = {4, 3}, .channels = 2};
    return {image, sample_size, storage, openxisf::detail::resolve_region(region, image)};
}

TEST(pixel_layout, the_runs_of_a_region_in_planar_storage_are_its_rows_channel_by_channel)
{
    const auto runs = runs_in_4x3x2({.origin = {1, 1}, .size = {2, 2}}, pixel_storage::planar);
    EXPECT_EQ(runs_of(runs), (run_list{{5, 2}, {9, 2}, {17, 2}, {21, 2}}));
    // Each channel is a group of its own.
    EXPECT_EQ(groups_of(runs), (run_list{{5, 11}, {17, 23}}));
    // With 16-bit samples, in bytes.
    const auto wide = runs_in_4x3x2({.origin = {1, 1}, .size = {2, 2}}, pixel_storage::planar, 2);
    EXPECT_EQ(runs_of(wide), (run_list{{10, 4}, {18, 4}, {34, 4}, {42, 4}}));
    EXPECT_EQ(groups_of(wide), (run_list{{10, 22}, {34, 46}}));
}

TEST(pixel_layout, a_channel_subset_in_planar_storage_has_the_runs_of_its_channels)
{
    const auto runs = runs_in_4x3x2({.origin = {1, 1}, .size = {2, 2}, .first_channel = 1, .channel_count = 1},
                                    pixel_storage::planar);
    EXPECT_EQ(runs_of(runs), (run_list{{17, 2}, {21, 2}}));
    EXPECT_EQ(groups_of(runs), (run_list{{17, 23}}));
}

TEST(pixel_layout, a_band_of_full_rows_in_planar_storage_is_one_run_per_channel)
{
    const auto runs = runs_in_4x3x2({.origin = {0, 1}, .size = {4, 2}}, pixel_storage::planar);
    EXPECT_EQ(runs_of(runs), (run_list{{4, 8}, {16, 8}}));
    EXPECT_EQ(groups_of(runs), (run_list{{4, 12}, {16, 24}}));
    // Whole planes merge across channels, into one group.
    const auto planes = runs_in_4x3x2({.first_channel = 1}, pixel_storage::planar);
    EXPECT_EQ(runs_of(planes), (run_list{{12, 12}}));
    EXPECT_EQ(groups_of(planes), (run_list{{12, 24}}));
}

TEST(pixel_layout, the_runs_of_a_region_in_normal_storage_are_its_rows_of_pixels)
{
    const auto runs = runs_in_4x3x2({.origin = {1, 1}, .size = {2, 2}}, pixel_storage::normal);
    EXPECT_EQ(runs_of(runs), (run_list{{10, 4}, {18, 4}}));
    // The region is one group.
    EXPECT_EQ(groups_of(runs), (run_list{{10, 22}}));
    const auto band = runs_in_4x3x2({.origin = {0, 1}, .size = {4, 2}}, pixel_storage::normal);
    EXPECT_EQ(runs_of(band), (run_list{{8, 16}}));
}

TEST(pixel_layout, a_channel_subset_in_normal_storage_is_one_run_per_pixel)
{
    const auto runs = runs_in_4x3x2({.origin = {1, 1}, .size = {2, 2}, .first_channel = 1, .channel_count = 1},
                                    pixel_storage::normal);
    EXPECT_EQ(runs_of(runs), (run_list{{11, 1}, {13, 1}, {19, 1}, {21, 1}}));
    EXPECT_EQ(groups_of(runs), (run_list{{11, 22}}));
}

TEST(pixel_layout, the_whole_image_is_one_run_in_either_model)
{
    for (const pixel_storage storage : {pixel_storage::planar, pixel_storage::normal}) {
        const auto runs = runs_in_4x3x2({}, storage);
        EXPECT_EQ(runs_of(runs), (run_list{{0, 24}}));
        EXPECT_EQ(groups_of(runs), (run_list{{0, 24}}));
    }
}

TEST(pixel_layout, the_runs_of_a_three_dimensional_image_repeat_along_each_axis)
{
    // 2 x 2 x 2 pixels of one channel: pixel (x, y, z) at z × 4 + y × 2 + x.
    const openxisf::geometry image{.dimensions = {2, 2, 2}, .channels = 1};
    const auto runs_of_box = [&image](const openxisf::pixel_region& region) {
        return runs_of({image, 1, pixel_storage::planar, openxisf::detail::resolve_region(region, image)});
    };
    EXPECT_EQ(runs_of_box({.origin = {0, 0, 1}, .size = {2, 2, 1}}), (run_list{{4, 4}}));
    EXPECT_EQ(runs_of_box({.origin = {0, 1, 0}, .size = {2, 1, 2}}), (run_list{{2, 2}, {6, 2}}));
    EXPECT_EQ(runs_of_box({.origin = {1, 0, 0}, .size = {1, 2, 2}}), (run_list{{1, 1}, {3, 1}, {5, 1}, {7, 1}}));
}

TEST(pixel_layout, a_cursor_finds_the_run_that_ends_after_a_byte)
{
    const auto runs = runs_in_4x3x2({.origin = {1, 1}, .size = {2, 2}}, pixel_storage::planar);
    // The runs (5, 2), (9, 2), (17, 2) and (21, 2).
    for (const auto& [position, run] : std::vector<std::pair<std::uint64_t, std::uint64_t>>{
             {0, 0}, {6, 0}, {7, 1}, {10, 1}, {11, 2}, {18, 2}, {19, 3}, {22, 3}, {23, 4}, {100, 4}}) {
        EXPECT_EQ(runs.first_ending_after(position), run) << position;
    }
    openxisf::detail::region_runs::cursor cursor = runs.at(2);
    EXPECT_EQ(cursor.offset(), 17U);
    cursor.move_to(1);
    EXPECT_EQ(cursor.offset(), 9U);
    cursor.next();
    cursor.next();
    EXPECT_EQ(cursor.offset(), 21U);
    cursor.next();
    EXPECT_TRUE(cursor.done());
}

TEST(pixel_layout, the_rows_of_a_region_in_a_destination_follow_its_storage_model)
{
    // 2 x 2 pixels of 2 channels, 16-bit: rows of one channel in the planar model, of both in the normal one.
    const auto box =
        openxisf::detail::resolve_region({.origin = {1, 1}, .size = {2, 2}}, {.dimensions = {4, 3}, .channels = 2});
    const openxisf::detail::region_rows planar = openxisf::detail::rows_of(box, 2, pixel_storage::planar, 0);
    EXPECT_EQ(planar.row, 4U);
    EXPECT_EQ(planar.count, 4U);
    EXPECT_EQ(planar.pitch, 4U);
    EXPECT_EQ(planar.span(), 16U);
    const openxisf::detail::region_rows normal = openxisf::detail::rows_of(box, 2, pixel_storage::normal, 0);
    EXPECT_EQ(normal.row, 8U);
    EXPECT_EQ(normal.count, 2U);
    EXPECT_EQ(normal.span(), 16U);
    // A pitch of 10 samples: the last row ends after a row of the samples of the region.
    const openxisf::detail::region_rows pitched = openxisf::detail::rows_of(box, 2, pixel_storage::normal, 10);
    EXPECT_EQ(pitched.pitch, 20U);
    EXPECT_EQ(pitched.span(), 28U);
    // A single channel is stored the same way in both models.
    const auto gray =
        openxisf::detail::resolve_region({.size = {2, 2}, .channel_count = 1}, {.dimensions = {4, 3}, .channels = 2});
    EXPECT_EQ(openxisf::detail::rows_of(gray, 1, pixel_storage::normal, 0).row, 2U);
    EXPECT_EQ(openxisf::detail::rows_of(gray, 1, pixel_storage::normal, 0).count, 2U);
}

TEST(pixel_layout, a_pitch_shorter_than_a_row_or_too_long_to_count_is_a_usage_error)
{
    const auto box = openxisf::detail::resolve_region({.size = {3, 2}}, {.dimensions = {4, 3}, .channels = 1});
    EXPECT_TRUE(openxisf::test::throws<openxisf::usage_error>(openxisf::errc::invalid_argument, [&box] {
        (void)openxisf::detail::rows_of(box, 1, pixel_storage::planar, 2);
    }));
    EXPECT_TRUE(openxisf::test::throws<openxisf::usage_error>(openxisf::errc::arithmetic_overflow, [&box] {
        (void)openxisf::detail::rows_of(box, 4, pixel_storage::planar, std::uint64_t{1} << 62);
    }));
    // The pitch of a single row does not matter.
    const auto row = openxisf::detail::resolve_region({.size = {3, 1}}, {.dimensions = {4, 3}, .channels = 1});
    EXPECT_EQ(openxisf::detail::rows_of(row, 4, pixel_storage::planar, std::uint64_t{1} << 62).span(), 12U);
}

TEST(pixel_layout, a_sink_places_the_bytes_of_a_region_in_its_rows)
{
    // Rows of 2 bytes, 4 bytes apart.
    std::vector<std::byte> destination(10, std::byte{0xEE});
    const openxisf::detail::region_sink sink(destination, 2, 4);
    sink.put(1, openxisf::test::bytes("abc"));
    EXPECT_EQ(destination, openxisf::test::bytes("\xEE"
                                                 "a\xEE\xEE"
                                                 "bc\xEE\xEE\xEE\xEE"sv));
    sink.at(5) = std::byte{'z'};
    EXPECT_EQ(destination[9], std::byte{'z'});
    EXPECT_EQ(sink.contiguous(2, 2).data(), destination.data() + 4);
    EXPECT_TRUE(sink.contiguous(1, 2).empty());
    // Rows that follow each other are one piece.
    const openxisf::detail::region_sink whole(destination, 2, 2);
    EXPECT_EQ(whole.contiguous(1, 6).data(), destination.data() + 1);
}

TEST(pixel_layout, a_region_is_converted_between_rows_with_a_pitch)
{
    // 2 x 2 pixels of 2 channels, 8-bit: channel c of pixel p is 10c + p.
    const std::vector<std::byte> planar = openxisf::test::bytes("\x00\x01\x02\x03\x0A\x0B\x0C\x0D"sv);
    const std::vector<std::byte> normal = openxisf::test::bytes("\x00\x0A\x01\x0B\x02\x0C\x03\x0D"sv);
    // Into the normal model, rows of 4 bytes, 6 bytes apart, from planar rows of 2 bytes that follow each other.
    const std::vector<std::byte> pitched_normal = openxisf::test::bytes("\x00\x0A\x01\x0B\xEE\xEE\x02\x0C\x03\x0D"sv);
    std::vector<std::byte> pixels(10, std::byte{0xEE});
    convert_storage(planar, pixels, pixel_storage::planar, 4, 2, 1, 2, 2, 6);
    EXPECT_EQ(pixels, pitched_normal);
    // Into the planar model, rows of 2 bytes, 3 bytes apart, those of channel 1 after those of channel 0.
    const std::vector<std::byte> pitched_planar =
        openxisf::test::bytes("\x00\x01\xEE\x02\x03\xEE\x0A\x0B\xEE\x0C\x0D"sv);
    std::vector<std::byte> channels(11, std::byte{0xEE});
    convert_storage(normal, channels, pixel_storage::normal, 4, 2, 1, 2, 4, 3);
    EXPECT_EQ(channels, pitched_planar);
    // From rows with a pitch into rows that follow each other, as a write of a region converts its data.
    std::vector<std::byte> to_normal(8);
    convert_storage(pitched_planar, to_normal, pixel_storage::planar, 4, 2, 1, 2, 3, 4);
    EXPECT_EQ(to_normal, normal);
    std::vector<std::byte> to_planar(8);
    convert_storage(pitched_normal, to_planar, pixel_storage::normal, 4, 2, 1, 2, 6, 2);
    EXPECT_EQ(to_planar, planar);
}

TEST(pixel_layout, samples_move_as_a_whole)
{
    // Complex64 samples, and samples of a size that no sample format has: 3 pixels of 2 channels.
    for (const std::size_t size : {16U, 3U}) {
        const std::vector<std::byte> planar = openxisf::test::pattern(std::size_t{3} * 2 * size);
        std::vector<std::byte> normal(planar.size());
        convert_storage(planar, normal, pixel_storage::planar, 3, 2, size);
        for (std::size_t p = 0; p < 3; ++p) {
            for (std::size_t c = 0; c < 2; ++c) {
                for (std::size_t b = 0; b < size; ++b) {
                    EXPECT_EQ(normal[(((p * 2) + c) * size) + b], planar[(((c * 3) + p) * size) + b])
                        << size << p << c << b;
                }
            }
        }
    }
}

} // namespace
