// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The samples of group A: the pattern image of make_group_a.js, written by PixInsight in each sample format and colour
// space of the group. Their pixels are compared with the expressions that created them, within one unit of the last
// place of the format, so that no decoder is trusted.

#include <openxisf/image.h>
#include <openxisf/reader.h>
#include <openxisf/types.h>

#include "samples/sample_catalog.h"
#include "support/diagnostics.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openxisf::color_space;
using openxisf::pixel_storage;
using openxisf::reader;
using openxisf::sample_format;
using openxisf::test::sample;

constexpr std::size_t width = 37;
constexpr std::size_t height = 23;

// The value in [0, 1] of channel c at (x, y), as the PixelMath expressions of make_group_a.js compute it: red increases
// in storage order, green decreases, blue follows x and alpha follows y.
double pattern(std::size_t channel, std::size_t x, std::size_t y)
{
    const auto index = static_cast<double>(x + (width * y));
    switch (channel) {
    case 0:
        return index / 850.0;
    case 1:
        return 1.0 - (index / 850.0);
    case 2:
        return static_cast<double>(x) / 36.0;
    default:
        return static_cast<double>(y) / 22.0;
    }
}

// What the catalog says of the image of a sample.
struct pattern_image
{
    std::string_view id{};
    sample_format format = sample_format::uint8;
    color_space space = color_space::gray;
    std::uint64_t channels = 1;
};

const std::vector<pattern_image>& pattern_images()
{
    static const std::vector<pattern_image> images{
        {.id = "A1", .format = sample_format::uint8},
        {.id = "A2", .format = sample_format::uint16},
        {.id = "A3", .format = sample_format::uint32},
        {.id = "A4", .format = sample_format::float32, .space = color_space::rgb, .channels = 3},
        {.id = "A5", .format = sample_format::float64},
        {.id = "A6", .format = sample_format::uint16, .space = color_space::rgb, .channels = 4},
    };
    return images;
}

const pattern_image& image_of(const sample& unit)
{
    for (const pattern_image& image : pattern_images()) {
        if (image.id == unit.id) {
            return image;
        }
    }
    ADD_FAILURE() << "no pattern image for " << unit.id;
    static const pattern_image none{};
    return none;
}

// The tolerance of a format, and the value that stands for 1: integer formats hold PixInsight's rounding of the values
// in [0, 1] to their range, and floating point formats the values themselves. PixelMath computes the values of a
// 32-bit integer image as 32-bit floating point values before it rounds them, which moves them by up to 128 units.
struct scale
{
    double one = 1.0;
    double tolerance = 0.0;
    bool through_float32 = false;
};

scale scale_of(sample_format format)
{
    switch (format) {
    case sample_format::uint8:
        return {.one = 255.0, .tolerance = 1.0};
    case sample_format::uint16:
        return {.one = 65535.0, .tolerance = 1.0};
    case sample_format::uint32:
        return {.one = 4294967295.0, .tolerance = 1.0, .through_float32 = true};
    case sample_format::float32:
        return {.one = 1.0, .tolerance = 1e-6};
    case sample_format::float64:
        return {.one = 1.0, .tolerance = 1e-12};
    default:
        return {};
    }
}

// Compares every sample of the image of file, read as T in the given storage model, with the pattern. Returns the
// number of samples that differ, after reporting the first few; all of them when the image has another number.
template <typename T> int count_differences(const reader& file, const pattern_image& image, pixel_storage storage)
{
    const std::vector<T> samples = file.read_pixels<T>(0, {.storage = storage});
    const scale expected = scale_of(image.format);
    const std::size_t channels = image.channels;
    if (samples.size() != channels * width * height) {
        ADD_FAILURE() << image.id << ": " << samples.size() << " samples instead of " << channels * width * height;
        return static_cast<int>(channels * width * height);
    }
    int differences = 0;
    for (std::size_t c = 0; c < channels; ++c) {
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const std::size_t pixel = x + (width * y);
                const std::size_t index =
                    storage == pixel_storage::planar ? (c * width * height) + pixel : (pixel * channels) + c;
                const auto value = static_cast<double>(samples[index]);
                const double exact = pattern(c, x, y);
                const double computed =
                    expected.through_float32 ? static_cast<double>(static_cast<float>(exact)) : exact;
                if (std::abs(value - (computed * expected.one)) > expected.tolerance && ++differences <= 5) {
                    ADD_FAILURE() << image.id << ": channel " << c << " at (" << x << ", " << y << ") is " << value;
                }
            }
        }
    }
    return differences;
}

int count_differences(const reader& file, const pattern_image& image, pixel_storage storage)
{
    switch (image.format) {
    case sample_format::uint8:
        return count_differences<std::uint8_t>(file, image, storage);
    case sample_format::uint16:
        return count_differences<std::uint16_t>(file, image, storage);
    case sample_format::uint32:
        return count_differences<std::uint32_t>(file, image, storage);
    case sample_format::float32:
        return count_differences<float>(file, image, storage);
    case sample_format::float64:
        return count_differences<double>(file, image, storage);
    default:
        ADD_FAILURE() << "no pattern image in " << openxisf::sample_format_name(image.format);
        return -1;
    }
}

class samples_group_a : public testing::TestWithParam<sample>
{};

TEST_P(samples_group_a, the_image_has_the_attributes_of_the_pattern)
{
    const pattern_image& expected = image_of(GetParam());
    const reader file(openxisf::test::sample_path(GetParam()), {.strict = true});
    ASSERT_EQ(file.images().size(), 1U);
    const openxisf::image_info& info = file.image(0);
    EXPECT_EQ(info.geometry, (openxisf::geometry{.dimensions = {width, height}, .channels = expected.channels}));
    EXPECT_EQ(info.sample_format, expected.format);
    EXPECT_EQ(info.color_space, expected.space);
    EXPECT_EQ(info.pixel_storage, pixel_storage::planar);
    // PixInsight writes bounds for floating point images only, and names each image after its view.
    const bool real = expected.format == sample_format::float32 || expected.format == sample_format::float64;
    EXPECT_EQ(info.bounds, real ? std::optional(openxisf::bounds{.lower = 0.0, .upper = 1.0}) : std::nullopt);
    EXPECT_EQ(info.id, "openxisf_sample_" + std::string(1, GetParam().id[1]));
    EXPECT_TRUE(info.uuid.empty());
}

TEST_P(samples_group_a, the_pixels_are_the_pattern_in_either_storage_model)
{
    const pattern_image& expected = image_of(GetParam());
    const reader file(openxisf::test::sample_path(GetParam()), {.strict = true});
    EXPECT_EQ(count_differences(file, expected, pixel_storage::planar), 0);
    EXPECT_EQ(count_differences(file, expected, pixel_storage::normal), 0);
}

INSTANTIATE_TEST_SUITE_P(pixinsight, samples_group_a, testing::ValuesIn(openxisf::test::samples_of_group('A')),
                         [](const testing::TestParamInfo<sample>& parameter) {
                             return std::string(parameter.param.id);
                         });

} // namespace
