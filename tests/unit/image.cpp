// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/types.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <complex>
#include <cstdint>
#include <limits>
#include <optional>

namespace {

using openxisf::bounds;
using openxisf::color_space;
using openxisf::errc;
using openxisf::geometry;
using openxisf::image_info;
using openxisf::sample_format;
using openxisf::usage_error;
using openxisf::test::throws;

constexpr std::uint64_t max64 = std::numeric_limits<std::uint64_t>::max();

TEST(image, the_names_of_the_specification)
{
    EXPECT_EQ(openxisf::sample_format_name(sample_format::uint8), "UInt8");
    EXPECT_EQ(openxisf::sample_format_name(sample_format::complex64), "Complex64");
    EXPECT_EQ(openxisf::color_space_name(color_space::cie_lab), "CIELab");
    EXPECT_EQ(openxisf::pixel_storage_name(openxisf::pixel_storage::normal), "Normal");
    EXPECT_EQ(openxisf::image_type_name(openxisf::image_type::binary_rejection_map_low), "BinaryRejectionMapLow");
    EXPECT_EQ(openxisf::orientation_name(openxisf::orientation::none), "0");
    EXPECT_EQ(openxisf::orientation_name(openxisf::orientation::rotate_minus_90_flip), "-90;flip");
}

TEST(image, a_value_outside_an_enumeration_has_no_name)
{
    // A cast gives such a value, as in a model that an application fills from numbers of its own.
    // NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange): the values are outside on purpose
    EXPECT_EQ(openxisf::sample_format_name(static_cast<sample_format>(8)), "");
    EXPECT_EQ(openxisf::color_space_name(static_cast<color_space>(3)), "");
    EXPECT_EQ(openxisf::pixel_storage_name(static_cast<openxisf::pixel_storage>(2)), "");
    EXPECT_EQ(openxisf::image_type_name(static_cast<openxisf::image_type>(15)), "");
    EXPECT_EQ(openxisf::orientation_name(static_cast<openxisf::orientation>(8)), "");
    EXPECT_EQ(openxisf::resolution_unit_name(static_cast<openxisf::resolution_unit>(2)), "");
    // NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange)
}

TEST(image, the_size_and_the_cpp_type_of_each_sample_format)
{
    static_assert(openxisf::sample_size(sample_format::uint8) == 1);
    static_assert(openxisf::sample_size(sample_format::uint16) == 2);
    static_assert(openxisf::sample_size(sample_format::uint32) == 4);
    static_assert(openxisf::sample_size(sample_format::uint64) == 8);
    static_assert(openxisf::sample_size(sample_format::float32) == 4);
    static_assert(openxisf::sample_size(sample_format::float64) == 8);
    static_assert(openxisf::sample_size(sample_format::complex32) == 8);
    static_assert(openxisf::sample_size(sample_format::complex64) == 16);

    static_assert(openxisf::sample_format_of<std::uint8_t>() == sample_format::uint8);
    static_assert(openxisf::sample_format_of<std::uint16_t>() == sample_format::uint16);
    static_assert(openxisf::sample_format_of<std::uint32_t>() == sample_format::uint32);
    static_assert(openxisf::sample_format_of<std::uint64_t>() == sample_format::uint64);
    static_assert(openxisf::sample_format_of<float>() == sample_format::float32);
    static_assert(openxisf::sample_format_of<double>() == sample_format::float64);
    static_assert(openxisf::sample_format_of<std::complex<float>>() == sample_format::complex32);
    static_assert(openxisf::sample_format_of<std::complex<double>>() == sample_format::complex64);
    static_assert(!openxisf::pixel_sample<std::int16_t> && !openxisf::pixel_sample<std::byte>);
    SUCCEED();
}

TEST(image, colour_spaces_have_their_nominal_channels)
{
    // Spec §8.5.1, §8.5.4.
    EXPECT_EQ(openxisf::nominal_channels(color_space::gray), 1U);
    EXPECT_EQ(openxisf::nominal_channels(color_space::rgb), 3U);
    EXPECT_EQ(openxisf::nominal_channels(color_space::cie_lab), 3U);
}

TEST(image, a_geometry_counts_its_pixels_and_samples)
{
    // Spec §8.5.1: the product of the lengths, and that times the channels.
    const geometry rgb{.dimensions = {960, 540}, .channels = 3};
    EXPECT_EQ(rgb.pixel_count(), 518'400U);
    EXPECT_EQ(rgb.sample_count(), 1'555'200U);
    EXPECT_EQ((geometry{.dimensions = {2, 3, 4}, .channels = 5}).sample_count(), 120U);
    EXPECT_EQ((geometry{.dimensions = {max64}, .channels = 1}).sample_count(), max64);
}

TEST(image, a_count_beyond_64_bits_is_a_usage_error)
{
    const geometry pixels{.dimensions = {std::uint64_t{1} << 32U, std::uint64_t{1} << 32U}, .channels = 1};
    EXPECT_TRUE(throws<usage_error>(errc::arithmetic_overflow, [&pixels] { (void)pixels.pixel_count(); }));
    const geometry samples{.dimensions = {max64}, .channels = 2};
    EXPECT_TRUE(throws<usage_error>(errc::arithmetic_overflow, [&samples] { (void)samples.sample_count(); }));
    const image_info bytes{.geometry = {.dimensions = {max64}, .channels = 1}, .sample_format = sample_format::uint16};
    EXPECT_TRUE(throws<usage_error>(errc::arithmetic_overflow, [&bytes] { (void)bytes.data_size(); }));
}

TEST(image, the_data_size_is_the_number_of_samples_times_their_size)
{
    // Spec §8.5.3.
    const image_info info{.geometry = {.dimensions = {37, 23}, .channels = 3},
                          .sample_format = sample_format::complex32};
    EXPECT_EQ(info.data_size(), 37U * 23 * 3 * 8);
}

TEST(image, the_representable_range_of_an_integer_image_defaults_to_the_range_of_its_samples)
{
    // Spec §8.5.5: [0, 2^k - 1] for k-bit unsigned integers.
    const auto range = [](sample_format format) { return image_info{.sample_format = format}.representable_range(); };
    EXPECT_EQ(range(sample_format::uint8), (bounds{.lower = 0.0, .upper = 255.0}));
    EXPECT_EQ(range(sample_format::uint16), (bounds{.lower = 0.0, .upper = 65535.0}));
    EXPECT_EQ(range(sample_format::uint32), (bounds{.lower = 0.0, .upper = 4294967295.0}));
    EXPECT_EQ(range(sample_format::uint64), (bounds{.lower = 0.0, .upper = 18446744073709551615.0}));
}

TEST(image, floating_point_and_complex_images_have_no_default_range)
{
    for (const sample_format format :
         {sample_format::float32, sample_format::float64, sample_format::complex32, sample_format::complex64}) {
        EXPECT_EQ(image_info{.sample_format = format}.representable_range(), std::nullopt);
    }
}

TEST(image, bounds_are_the_representable_range_of_any_image)
{
    const bounds range{.lower = -1.0, .upper = 2.0};
    for (const sample_format format : {sample_format::uint8, sample_format::float32, sample_format::complex64}) {
        EXPECT_EQ((image_info{.sample_format = format, .bounds = range}).representable_range(), range);
    }
}

} // namespace
