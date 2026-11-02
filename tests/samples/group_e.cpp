// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The samples of group E, written by PixInsight 1.9.6 under Revision 2 of the specification: a block of two subblocks,
// the second of which no codec makes shorter and which is stored as it is (spec §10.6.1), with zstd+sh, lz4+sh, zstd,
// and zstd+sh with SHA-256 checksums; and a block that no codec makes shorter, written without compression.

#include <openxisf/io.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>
#include <openxisf/types.h>

#include "codec/compression.h"
#include "container/data_block.h"
#include "model/unit.h"
#include "samples/sample_catalog.h"
#include "support/opened_unit.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openxisf::detail::block_compression;
using openxisf::detail::compression_codec;
using openxisf::detail::subblock;
using openxisf::detail::unit;
using openxisf::test::sample;
using openxisf::test::sample_by_id;

// The ramp image: 2048 x 1025 samples of 16 bits, whose last two rows, 4096 samples, are noise.
constexpr std::uint64_t width = 2048;
constexpr std::uint64_t height = 1025;
constexpr std::uint64_t ramp_size = width * height * 2;
// PixInsight divides the block into a subblock of 4 MiB and the rest, 4096 bytes, which it stores as they are.
constexpr std::uint64_t first_subblock = std::uint64_t{4} << 20;
constexpr std::uint64_t stored_subblock = ramp_size - first_subblock;

// How PixInsight compressed the block of a sample of the ramp: the codec and the shuffling, the compressed size of its
// first subblock, and its checksum algorithm.
struct stored_image
{
    std::string_view id{};
    openxisf::codec codec = openxisf::codec::zstd;
    compression_codec internal = compression_codec::zstd;
    bool shuffled = false;
    std::uint64_t first_compressed = 0;
    std::optional<openxisf::checksum_algorithm> checksum{};
};

const std::vector<stored_image>& stored_images()
{
    static const std::vector<stored_image> images{
        {.id = "E1",
         .codec = openxisf::codec::zstd,
         .internal = compression_codec::zstd,
         .shuffled = true,
         .first_compressed = 8284},
        {.id = "E2",
         .codec = openxisf::codec::lz4,
         .internal = compression_codec::lz4,
         .shuffled = true,
         .first_compressed = 24006},
        {.id = "E3",
         .codec = openxisf::codec::zstd,
         .internal = compression_codec::zstd,
         .shuffled = false,
         .first_compressed = 8592},
        {.id = "E4",
         .codec = openxisf::codec::zstd,
         .internal = compression_codec::zstd,
         .shuffled = true,
         .first_compressed = 8284,
         .checksum = openxisf::checksum_algorithm::sha256},
    };
    return images;
}

const stored_image& image_of(const sample& unit)
{
    for (const stored_image& image : stored_images()) {
        if (image.id == unit.id) {
            return image;
        }
    }
    throw std::logic_error("no ramp image for sample " + std::string(unit.id));
}

std::vector<sample> ramp_samples()
{
    std::vector<sample> found;
    for (const stored_image& image : stored_images()) {
        found.push_back(sample_by_id(image.id));
    }
    return found;
}

unit open_sample(const sample& unit)
{
    return {std::make_unique<openxisf::file_source>(openxisf::test::sample_path(unit)), {.strict = true}};
}

// The little-endian 16-bit sample at index of a block of samples.
std::uint32_t sample_at(const std::vector<std::byte>& pixels, std::size_t index)
{
    return std::to_integer<std::uint32_t>(pixels[2 * index]) |
           (std::to_integer<std::uint32_t>(pixels[(2 * index) + 1]) << 8U);
}

// The value of the ramp at x, as PixInsight rounds x/2047 to 16 bits.
double ramp_at(std::uint64_t x)
{
    return std::round(static_cast<double>(x) / 2047.0 * 65535.0);
}

class samples_group_e_ramp : public testing::TestWithParam<sample>
{};

TEST_P(samples_group_e_ramp, the_block_has_a_subblock_of_4_mib_and_one_of_4096_bytes_stored_as_it_is)
{
    const stored_image& expected = image_of(GetParam());
    const openxisf::reader file(openxisf::test::sample_path(GetParam()), {.strict = true});
    EXPECT_EQ(file.block(0), (openxisf::block_info{.location = openxisf::block_location::attachment,
                                                   .codec = expected.codec,
                                                   .byte_shuffle = expected.shuffled,
                                                   .subblock_size = first_subblock,
                                                   .subblock_count = 2,
                                                   .checksum = expected.checksum,
                                                   .stored_size = expected.first_compressed + stored_subblock,
                                                   .uncompressed_size = ramp_size}));

    // Spec §10.6.1: the second subblock has equal sizes, so it holds its bytes as they are.
    const unit opened = open_sample(GetParam());
    const block_compression& compression =
        openxisf::test::value_of(openxisf::test::descriptor_at(opened, "/xisf/Image[1]").compression);
    EXPECT_EQ(compression.codec, expected.internal);
    EXPECT_EQ(compression.uncompressed_size, ramp_size);
    EXPECT_EQ(compression.item_size, expected.shuffled ? 2U : 0U);
    EXPECT_EQ(
        compression.subblocks,
        (std::vector<subblock>{{.compressed_size = expected.first_compressed, .uncompressed_size = first_subblock},
                               {.compressed_size = stored_subblock, .uncompressed_size = stored_subblock}}));
}

TEST_P(samples_group_e_ramp, the_image_is_the_ramp_with_the_noise_of_the_stored_subblock)
{
    // Every sample of the ramp holds the same pixels: rows 0 to 1022 are x/2047 as PixInsight rounds it, the last two
    // rows noise, and the stored subblock, the last 4096 bytes of the block as stored, holds the high bytes of the
    // noise with byte shuffling, which puts the low bytes of every sample first, or its last 2048 samples without it.
    const stored_image& expected = image_of(GetParam());
    const unit opened = open_sample(GetParam());
    const std::vector<std::byte> pixels = openxisf::test::block_data(opened, "/xisf/Image[1]");
    ASSERT_EQ(pixels.size(), ramp_size);
    for (std::uint64_t y = 0; y < height - 2; y += 93) {
        for (std::uint64_t x = 0; x < width; x += 7) {
            const std::size_t index = (y * width) + x;
            EXPECT_LE(std::abs(static_cast<double>(sample_at(pixels, index)) - ramp_at(x)), 1.0)
                << "sample at " << x << ", " << y;
        }
    }
    const std::size_t count = width * height;
    std::size_t noise = 0;
    for (std::size_t index = count - stored_subblock; index < count; ++index) {
        if (std::abs(static_cast<double>(sample_at(pixels, index)) - ramp_at(index % width)) > 1.0) {
            ++noise;
        }
    }
    EXPECT_GT(noise, 4000U);
    EXPECT_EQ(pixels, openxisf::test::block_data(open_sample(sample_by_id("E1")), "/xisf/Image[1]"));

    const std::vector<std::byte> stored = openxisf::test::stored_block(opened, "/xisf/Image[1]");
    ASSERT_EQ(stored.size(), expected.first_compressed + stored_subblock);
    const std::span<const std::byte> last = std::span(stored).last(stored_subblock);
    if (expected.shuffled) {
        for (std::size_t i = 0; i < last.size(); ++i) {
            EXPECT_EQ(last[i], pixels[(2 * (count - stored_subblock + i)) + 1]) << "byte " << i;
        }
    } else {
        EXPECT_TRUE(std::ranges::equal(last, std::span(pixels).last(stored_subblock)));
    }
}

TEST_P(samples_group_e_ramp, the_noise_rows_are_read_as_a_region_from_the_stored_subblock)
{
    const openxisf::reader file(openxisf::test::sample_path(GetParam()), {.strict = true});
    const std::vector<std::uint16_t> whole = file.read_pixels<std::uint16_t>(0);
    openxisf::pixel_region region{.channel_count = 1};
    region.origin = {0, height - 2};
    region.size = {width, 2};
    const std::vector<std::uint16_t> rows = file.read_pixels<std::uint16_t>(0, {.region = region});
    ASSERT_EQ(rows.size(), 2 * width);
    EXPECT_TRUE(std::ranges::equal(rows, std::span(whole).last(rows.size())));
}

INSTANTIATE_TEST_SUITE_P(pixinsight, samples_group_e_ramp, testing::ValuesIn(ramp_samples()),
                         [](const testing::TestParamInfo<sample>& parameter) {
                             return std::string(parameter.param.id);
                         });

TEST(samples_group_e, a_block_that_no_codec_makes_shorter_is_written_uncompressed)
{
    // E5: 64 x 64 samples of noise, saved with zstd+sh. The block is attached without compression, while the thumbnail
    // and the processing history are compressed, which XISF:CompressionCodecs names.
    constexpr std::uint64_t noise_size = std::uint64_t{64} * 64 * 2;
    const sample& e5 = sample_by_id("E5");
    const openxisf::reader file(openxisf::test::sample_path(e5), {.strict = true});
    EXPECT_EQ(file.block(0), (openxisf::block_info{.location = openxisf::block_location::attachment,
                                                   .stored_size = noise_size,
                                                   .uncompressed_size = noise_size}));
    EXPECT_EQ(file.metadata().at("XISF:CompressionCodecs").value, openxisf::property_value("zstd"));
    EXPECT_EQ(file.read_pixels<std::uint16_t>(0).size(), noise_size / 2);
}

} // namespace
