// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The large samples, which are not versioned: the tests find them in the directory that the environment variable
// OPENXISF_LARGE_SAMPLES_DIR names, and skip without it. L1 is an RGB Float32 image above 4 GiB that PixInsight
// compressed with zlib, in subblocks of its own choice (spec §10.6).

#include <openxisf/io.h>
#include <openxisf/reader.h>
#include <openxisf/writer.h>

#include "codec/compression.h"
#include "container/data_block.h"
#include "model/unit.h"
#include "samples/sample_catalog.h"
#include "support/environment.h"
#include "support/opened_unit.h"
#include "support/temp_directory.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using openxisf::detail::compression_codec;
using openxisf::detail::subblock;
using openxisf::detail::unit;

constexpr std::size_t width = 22000;
constexpr std::size_t height = 17000;

// The value of channel c at (x, y), as make_large_l1.js computes it.
double expected_value(std::size_t channel, std::size_t x, std::size_t y)
{
    switch (channel) {
    case 0:
        return static_cast<double>(x) / static_cast<double>(width - 1);
    case 1:
        return static_cast<double>(y) / static_cast<double>(height - 1);
    default:
        return static_cast<double>(x + y) / static_cast<double>(width + height - 2);
    }
}

// The little-endian Float32 sample at index of a planar image.
double sample_at(const std::vector<std::byte>& pixels, std::size_t index)
{
    std::uint32_t bits = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        bits |= std::to_integer<std::uint32_t>(pixels[(4 * index) + i]) << (8 * i);
    }
    return static_cast<double>(std::bit_cast<float>(bits));
}

// The path of the file of L1, or why it is not there.
struct l1_file
{
    std::string path{};
    std::string missing{};
};

l1_file find_l1()
{
    const std::optional<std::string> directory = openxisf::test::environment_variable("OPENXISF_LARGE_SAMPLES_DIR");
    if (!directory) {
        return {.missing = "OPENXISF_LARGE_SAMPLES_DIR is not set"};
    }
    std::string path = *directory + "/" + std::string(openxisf::test::large_samples().front().file);
    if (!std::filesystem::exists(openxisf::test::path_of(path))) {
        return {.missing = path + " does not exist"};
    }
    return {.path = std::move(path)};
}

TEST(samples_large, l1_decompresses_with_the_subblocks_of_pixinsight)
{
    const l1_file found = find_l1();
    if (!found.missing.empty()) {
        GTEST_SKIP() << found.missing;
    }
    const openxisf::test::sample& l1 = openxisf::test::large_samples().front();

    const unit opened(std::make_unique<openxisf::file_source>(found.path), {.strict = true});
    EXPECT_EQ(openxisf::test::block_paths(opened), l1.blocks);
    // The first subblock is as large as PixInsight makes a zlib subblock, and did not compress, so it is stored as it
    // is: its two sizes are equal.
    const openxisf::detail::block_compression& compression =
        openxisf::test::value_of(openxisf::test::descriptor_at(opened, "/xisf/Image[1]").compression);
    EXPECT_EQ(compression.codec, compression_codec::zlib);
    EXPECT_EQ(compression.uncompressed_size, std::uint64_t{width} * height * 3 * 4);
    EXPECT_EQ(compression.subblocks,
              (std::vector<subblock>{{.compressed_size = 4'294'967'294, .uncompressed_size = 4'294'967'294},
                                     {.compressed_size = 159'192'850, .uncompressed_size = 193'032'706}}));

    const std::vector<std::byte> pixels = openxisf::test::block_data(opened, "/xisf/Image[1]");
    ASSERT_EQ(pixels.size(), compression.uncompressed_size);
    // The corners of each channel, and the samples around the subblock boundary, at byte 4,294,967,294 of the block:
    // the sample at index 1,073,741,823 has two bytes in each subblock.
    std::vector<std::size_t> indexes;
    for (std::size_t channel = 0; channel < 3; ++channel) {
        indexes.push_back(channel * width * height);
        indexes.push_back(((channel + 1) * width * height) - 1);
    }
    for (std::size_t index = 1'073'741'820; index < 1'073'741'828; ++index) {
        indexes.push_back(index);
    }
    for (const std::size_t index : indexes) {
        const std::size_t channel = index / (width * height);
        const std::size_t x = (index % (width * height)) % width;
        const std::size_t y = (index % (width * height)) / width;
        EXPECT_LE(std::abs(sample_at(pixels, index) - expected_value(channel, x, y)), 1e-6) << "sample " << index;
    }
}

TEST(samples_large, the_block_of_l1_has_its_two_subblocks_the_first_one_the_largest)
{
    // The facts of the block come from the header, which a header-only open reads alone.
    const l1_file found = find_l1();
    if (!found.missing.empty()) {
        GTEST_SKIP() << found.missing;
    }
    const openxisf::reader l1(found.path, {.strict = true, .header_only = true});
    EXPECT_EQ(l1.block(0), (openxisf::block_info{.location = openxisf::block_location::attachment,
                                                 .codec = openxisf::codec::zlib,
                                                 .subblock_size = 4'294'967'294,
                                                 .subblock_count = 2,
                                                 .stored_size = 4'294'967'294 + 159'192'850,
                                                 .uncompressed_size = std::uint64_t{width} * height * 3 * 4}));
}

} // namespace
