// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The samples of group D. D1: an image with what describes it (spec §11.6 to §11.12). D2: a colour filter array
// image (spec §11.10). D3: typed properties written by PixInsight (spec §8.4.4, §11.1), with the values its script set.
// D4: three images in one unit (spec §11.5), each with its id and a property of its own.

#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>
#include <openxisf/types.h>

#include "model/unit.h"
#include "samples/sample_catalog.h"
#include "support/diagnostics.h"
#include "support/opened_unit.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::date_time;
using openxisf::fits_keyword;
using openxisf::property_list;
using openxisf::property_value;

openxisf::reader open_sample(std::string_view id, openxisf::read_options options = {.strict = true})
{
    return openxisf::reader(openxisf::test::sample_path(openxisf::test::sample_by_id(id)), std::move(options));
}

// The samples of an image read from a sample are those of the pattern image of the samples, computed in double
// precision, within one unit of the last place: channel holds expression(i) at pixel i, in [0, 1], rounded to 16 bits.
template <typename Expression>
testing::AssertionResult has_pattern(const std::vector<std::uint16_t>& samples, std::size_t channel, std::size_t pixels,
                                     Expression expression)
{
    for (std::size_t i = 0; i < pixels; ++i) {
        const double expected = expression(static_cast<double>(i)) * 65535.0;
        if (std::abs(samples[(channel * pixels) + i] - expected) > 1.0) {
            return testing::AssertionFailure() << "channel " << channel << ", pixel " << i << ": "
                                               << samples[(channel * pixels) + i] << " instead of " << expected;
        }
    }
    return testing::AssertionSuccess();
}

// -----------------------------------------------------------------------------------------------------------------
// D1

TEST(samples_d1, opens_strictly_with_only_the_warning_of_every_sample)
{
    const openxisf::reader file = open_sample("D1");
    EXPECT_TRUE(openxisf::test::no_diagnostics(openxisf::test::unexpected_diagnostics(file.diagnostics())));
    ASSERT_EQ(file.images().size(), 1U);
    EXPECT_EQ(file.image(0).geometry, (openxisf::geometry{.dimensions = {37, 23}, .channels = 3}));
    EXPECT_EQ(file.image(0).color_space, openxisf::color_space::rgb);
}

TEST(samples_d1, the_fits_keywords_are_those_of_the_script_in_order)
{
    std::string long68;
    while (long68.size() < 68) {
        long68 += "0123456789";
    }
    long68.resize(68);
    std::string long90;
    for (int i = 0; i < 9; ++i) {
        long90 += "abcdefghij";
    }
    const std::vector<fits_keyword> expected{
        {.name = "OBJECT", .value = "'M31'", .comment = "Name of the object"},
        {.name = "EXPTIME", .value = "300.", .comment = "Exposure time in seconds"},
        {.name = "HISTORY", .value = "", .comment = "Calibrated with a master dark"},
        {.name = "COMMENT", .value = "", .comment = "Written by the OpenXISF sample script"},
        {.name = "CARD68", .value = "'" + long68 + "'", .comment = "The longest string of a FITS card"},
        {.name = "LONGSTR", .value = "'" + long90 + "'", .comment = "Longer than a FITS card"}};
    EXPECT_EQ(open_sample("D1").image(0).fits_keywords, expected);
}

TEST(samples_d1, the_working_space_display_function_and_resolution_are_those_of_the_script)
{
    const openxisf::image_info info = open_sample("D1").image(0);
    // PixInsight holds the values of Adobe RGB (1998) as 32-bit floating point values, and writes them with 17 digits.
    const openxisf::rgb_working_space& space = openxisf::test::value_of(info.rgb_working_space);
    EXPECT_EQ(space.gamma, std::optional(2.2));
    EXPECT_EQ(space.x, (std::array<double, 3>{0.6484310030937195, 0.2301539927721024, 0.1558859944343567}));
    EXPECT_EQ(space.y, (std::array<double, 3>{0.3308559954166412, 0.7015720009803772, 0.06604400277137756}));
    EXPECT_EQ(space.luminance, (std::array<double, 3>{0.3111140131950378, 0.6256620287895203, 0.06322400271892548}));
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(static_cast<float>(space.x[i]), static_cast<float>(std::array{0.648431, 0.230154, 0.155886}[i]));
    }
    EXPECT_EQ(space.name, "");

    EXPECT_EQ(info.display_function, (openxisf::display_function{.midtones = {0.25, 0.3, 0.35, 0.5},
                                                                 .shadows = {0.01, 0.02, 0.03, 0.0},
                                                                 .highlights = {0.9, 0.95, 1.0, 1.0}}));
    EXPECT_EQ(
        info.resolution,
        (openxisf::resolution{.horizontal = 120.0, .vertical = 100.0, .unit = openxisf::resolution_unit::centimeter}));
    EXPECT_FALSE(info.color_filter_array.has_value());
}

TEST(samples_d1, the_icc_profile_is_the_srgb_profile_of_pixinsight)
{
    // The bytes of the attachment, as written, are those of the profile of A4.
    const std::vector<std::byte> profile = open_sample("D1").image(0).icc_profile;
    ASSERT_EQ(profile.size(), 3024U);
    EXPECT_EQ(profile, open_sample("A4").image(0).icc_profile);
}

TEST(samples_d1, the_thumbnail_is_that_of_the_global_preferences)
{
    const openxisf::reader file = open_sample("D1");
    const openxisf::thumbnail& small = openxisf::test::value_of(file.image(0).thumbnail);
    EXPECT_EQ(small.geometry, (openxisf::geometry{.dimensions = {400, 248}, .channels = 3}));
    EXPECT_EQ(small.sample_format, openxisf::sample_format::uint8);
    EXPECT_EQ(small.color_space, openxisf::color_space::rgb);
    EXPECT_EQ(small.pixel_storage, openxisf::pixel_storage::planar);
    EXPECT_EQ(small.pixels.size(), 400U * 248U * 3U);
    EXPECT_TRUE(small.properties.empty());
    EXPECT_TRUE(small.fits_keywords.empty());
}

TEST(samples_d1, the_properties_are_those_of_the_script)
{
    const openxisf::reader file = open_sample("D1");
    const property_list& properties = file.image(0).properties;
    // The processing history of PixInsight, and the eleven properties of the script.
    EXPECT_EQ(properties.size(), 12U);
    EXPECT_EQ(properties.at("Observation:Object:Name").value, property_value("M31"));
    EXPECT_EQ(properties.at("Observation:Center:RA").value, property_value(10.684708));
    EXPECT_EQ(properties.at("Observation:Center:Dec").value, property_value(41.26875));
    EXPECT_EQ(properties.at("Observation:Time:Start").value,
              property_value(date_time{.year = 2026, .month = 9, .day = 20, .hour = 23, .minute = 15}));
    EXPECT_EQ(properties.at("Instrument:ExposureTime").value, property_value(300.0F));
    EXPECT_EQ(properties.at("Instrument:Camera:Name").value, property_value("Synthetic camera"));
    EXPECT_EQ(properties.at("Instrument:Telescope:FocalLength").value, property_value(0.53F));
    EXPECT_EQ(properties.at("Instrument:Sensor:Temperature").value, property_value(-10.0F));
    EXPECT_EQ(properties.at("Test:SmallVector").value, property_value(std::vector<double>{1, 2, 3, 4}));
    EXPECT_EQ(properties.at("Test:SmallMatrix").value, property_value::matrix(2, 2, std::vector<double>{1, 2, 3, 4}));
    std::vector<double> attached(100);
    for (std::size_t i = 0; i < attached.size(); ++i) {
        attached[i] = static_cast<double>(i) * 0.25;
    }
    EXPECT_EQ(properties.at("Test:AttachedVector").value, property_value(attached));
}

TEST(samples_d1, the_pixels_are_the_colour_channels_of_the_pattern)
{
    const std::vector<std::uint16_t> samples = open_sample("D1").read_pixels<std::uint16_t>(0);
    constexpr std::size_t pixels = std::size_t{37} * 23;
    ASSERT_EQ(samples.size(), 3 * pixels);
    EXPECT_TRUE(has_pattern(samples, 0, pixels, [](double i) { return i / 850.0; }));
    EXPECT_TRUE(has_pattern(samples, 1, pixels, [](double i) { return 1.0 - (i / 850.0); }));
    EXPECT_TRUE(has_pattern(samples, 2, pixels, [](double i) { return std::fmod(i, 37.0) / 36.0; }));
}

TEST(samples_d1, a_header_only_open_leaves_the_data_blocks_until_they_are_loaded)
{
    openxisf::reader file = open_sample("D1", {.strict = true, .header_only = true});
    EXPECT_FALSE(file.ancillary_data_loaded());
    EXPECT_TRUE(openxisf::test::no_diagnostics(openxisf::test::unexpected_diagnostics(file.diagnostics())));
    const openxisf::image_info& described = file.image(0);
    EXPECT_TRUE(described.icc_profile.empty());
    EXPECT_TRUE(openxisf::test::value_of(described.thumbnail).pixels.empty());
    // The three properties in data blocks are left out.
    EXPECT_EQ(described.properties.size(), 9U);
    EXPECT_FALSE(described.properties.contains("Test:SmallVector"));
    EXPECT_EQ(described.fits_keywords.size(), 6U);

    file.load_ancillary_data();
    EXPECT_TRUE(file.ancillary_data_loaded());
    const openxisf::reader full = open_sample("D1");
    EXPECT_EQ(file.image(0), full.image(0));
    EXPECT_EQ(file.diagnostics().size(), full.diagnostics().size());
}

// -----------------------------------------------------------------------------------------------------------------
// D2

TEST(samples_d2, a_light_frame_with_its_colour_filter_array_and_camera_keywords)
{
    const openxisf::reader file = open_sample("D2");
    EXPECT_TRUE(openxisf::test::no_diagnostics(openxisf::test::unexpected_diagnostics(file.diagnostics())));
    ASSERT_EQ(file.images().size(), 1U);
    const openxisf::image_info& info = file.image(0);
    EXPECT_EQ(info.geometry, (openxisf::geometry{.dimensions = {64, 48}, .channels = 1}));
    EXPECT_EQ(info.sample_format, openxisf::sample_format::uint16);
    EXPECT_EQ(info.color_space, openxisf::color_space::gray);
    EXPECT_EQ(info.image_type, openxisf::image_type::light);
    EXPECT_EQ(info.color_filter_array,
              (openxisf::color_filter_array{.pattern = "RGGB", .width = 2, .height = 2, .name = "RGGB Bayer filter"}));
    EXPECT_EQ(info.resolution, openxisf::resolution{});
    EXPECT_FALSE(info.thumbnail.has_value());
    EXPECT_TRUE(info.icc_profile.empty());
    const std::vector<fits_keyword> expected{
        {.name = "INSTRUME", .value = "'Synthetic CFA camera'", .comment = "Camera"},
        {.name = "BAYERPAT", .value = "'RGGB'", .comment = "Bayer color pattern"},
        {.name = "XBAYROFF", .value = "0", .comment = "X offset of the Bayer pattern"},
        {.name = "YBAYROFF", .value = "0", .comment = "Y offset of the Bayer pattern"},
        {.name = "EXPTIME", .value = "120.", .comment = "Exposure time in seconds"},
        {.name = "GAIN", .value = "120", .comment = "Sensor gain"},
        {.name = "XPIXSZ", .value = "2.4", .comment = "Pixel width in microns"},
        {.name = "YPIXSZ", .value = "2.4", .comment = "Pixel height in microns"},
        {.name = "CCD-TEMP", .value = "-10.", .comment = "Sensor temperature in degrees C"}};
    EXPECT_EQ(info.fits_keywords, expected);
}

TEST(samples_d2, the_samples_increase_in_storage_order)
{
    const std::vector<std::uint16_t> samples = open_sample("D2").read_pixels<std::uint16_t>(0);
    ASSERT_EQ(samples.size(), 64U * 48U);
    EXPECT_TRUE(has_pattern(samples, 0, samples.size(), [](double i) { return i / 3071.0; }));
    EXPECT_TRUE(std::ranges::is_sorted(samples));
}

// -----------------------------------------------------------------------------------------------------------------
// D3

openxisf::detail::unit open_d3()
{
    return {std::make_unique<openxisf::file_source>(openxisf::test::sample_path(openxisf::test::sample_by_id("D3"))),
            {.strict = true}};
}

// The largest integer that JavaScript numbers hold exactly, 2^53 - 1, to which the script limits 64-bit values.
constexpr std::int64_t largest_exact = 9'007'199'254'740'991;

TEST(samples_d3, opens_strictly_with_only_the_warning_of_every_sample)
{
    const openxisf::detail::unit opened = open_d3();
    EXPECT_TRUE(openxisf::test::no_diagnostics(openxisf::test::unexpected_diagnostics(opened.diagnostics)));
    // The processing history of PixInsight, and the 31 properties of the script.
    EXPECT_EQ(openxisf::test::properties_of(opened, "/xisf/Image[1]").size(), 32U);
}

TEST(samples_d3, scalars)
{
    const openxisf::detail::unit opened = open_d3();
    const property_list& image = openxisf::test::properties_of(opened, "/xisf/Image[1]");
    EXPECT_EQ(image.at("Test:Boolean").value, property_value(true));
    EXPECT_EQ(image.at("Test:Int8").value, property_value(std::int8_t{-128}));
    EXPECT_EQ(image.at("Test:UInt8").value, property_value(std::uint8_t{255}));
    EXPECT_EQ(image.at("Test:Int16").value, property_value(std::int16_t{-32768}));
    EXPECT_EQ(image.at("Test:UInt16").value, property_value(std::uint16_t{65535}));
    EXPECT_EQ(image.at("Test:Int32").value, property_value(std::numeric_limits<std::int32_t>::min()));
    EXPECT_EQ(image.at("Test:UInt32").value, property_value(std::numeric_limits<std::uint32_t>::max()));
    EXPECT_EQ(image.at("Test:Int64").value, property_value(-largest_exact));
    EXPECT_EQ(image.at("Test:UInt64").value, property_value(std::uint64_t{largest_exact}));
    EXPECT_EQ(image.at("Test:Float32").value, property_value(0.1F));
    EXPECT_EQ(image.at("Test:Float64").value, property_value(1e-300));
    EXPECT_EQ(
        image.at("Test:TimePoint").value,
        property_value(date_time{
            .year = 2026, .month = 3, .day = 14, .hour = 1, .minute = 59, .second = 26, .nanosecond = 535'000'000}));
}

TEST(samples_d3, strings_keep_their_spaces_and_their_length)
{
    const openxisf::detail::unit opened = open_d3();
    const property_list& image = openxisf::test::properties_of(opened, "/xisf/Image[1]");
    EXPECT_EQ(image.at("Test:String").value, property_value("  Ñandú — 星雲 ✓  "));
    std::string digits;
    for (int i = 0; i < 1000; ++i) {
        digits += "0123456789";
    }
    EXPECT_EQ(image.at("Test:LongString").value, property_value(digits));
}

TEST(samples_d3, vectors_hold_the_extremes_of_their_elements)
{
    const openxisf::detail::unit opened = open_d3();
    const property_list& image = openxisf::test::properties_of(opened, "/xisf/Image[1]");
    EXPECT_EQ(image.at("Test:I8Vector").value, property_value(std::vector<std::int8_t>{-128, -1, 1, 127}));
    EXPECT_EQ(image.at("Test:UI8Vector").value, property_value(std::vector<std::uint8_t>{0, 0, 1, 255}));
    EXPECT_EQ(image.at("Test:I16Vector").value, property_value(std::vector<std::int16_t>{-32768, -1, 1, 32767}));
    EXPECT_EQ(image.at("Test:UI16Vector").value, property_value(std::vector<std::uint16_t>{0, 0, 1, 65535}));
    EXPECT_EQ(image.at("Test:I32Vector").value,
              property_value(std::vector<std::int32_t>{std::numeric_limits<std::int32_t>::min(), -1, 1,
                                                       std::numeric_limits<std::int32_t>::max()}));
    EXPECT_EQ(image.at("Test:UI32Vector").value,
              property_value(std::vector<std::uint32_t>{0, 0, 1, std::numeric_limits<std::uint32_t>::max()}));
    EXPECT_EQ(image.at("Test:I64Vector").value,
              property_value(std::vector<std::int64_t>{-largest_exact, -1, 1, largest_exact}));
    EXPECT_EQ(image.at("Test:UI64Vector").value, property_value(std::vector<std::uint64_t>{0, 0, 1, largest_exact}));
    constexpr float max32 = std::numeric_limits<float>::max();
    EXPECT_EQ(image.at("Test:F32Vector").value, property_value(std::vector<float>{-max32, -1, 1, max32}));
    constexpr double max64 = std::numeric_limits<double>::max();
    EXPECT_EQ(image.at("Test:F64Vector").value, property_value(std::vector<double>{-max64, -1, 1, max64}));
    EXPECT_EQ(image.at("Test:C32Vector").value,
              property_value(std::vector<std::complex<float>>{{1.0F, 2.0F}, {-3.0F, 4.0F}}));
    EXPECT_EQ(image.at("Test:C64Vector").value,
              property_value(std::vector<std::complex<double>>{{1.0, 2.0}, {-3.0, 4.0}}));
    EXPECT_EQ(image.at("Test:EmptyVector").value, property_value(std::vector<double>{}));
}

TEST(samples_d3, a_large_vector_in_an_attached_block)
{
    const openxisf::detail::unit opened = open_d3();
    const property_value& value = openxisf::test::properties_of(opened, "/xisf/Image[1]").at("Test:LargeVector").value;
    const std::span<const float> elements = value.elements<float>();
    ASSERT_EQ(elements.size(), 100'000U);
    for (std::size_t i = 0; i < elements.size(); ++i) {
        ASSERT_EQ(elements[i], static_cast<float>(i) * 0.5F) << i;
    }
}

TEST(samples_d3, matrices_in_row_order)
{
    const openxisf::detail::unit opened = open_d3();
    const property_list& image = openxisf::test::properties_of(opened, "/xisf/Image[1]");
    std::vector<double> elements;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            elements.push_back((10.0 * r) + c);
        }
    }
    EXPECT_EQ(image.at("Test:F64Matrix").value, property_value::matrix(3, 4, elements));
    EXPECT_EQ(image.at("Test:UI16Matrix").value, property_value::matrix(2, 2, std::vector<std::uint16_t>{1, 2, 3, 4}));
    EXPECT_EQ(image.at("Test:EmptyMatrix").value, property_value::matrix(0, 0, std::vector<double>{}));
}

// -----------------------------------------------------------------------------------------------------------------
// D4

TEST(samples_d4, three_images_with_their_ids_and_one_property_each)
{
    const openxisf::reader file = open_sample("D4");
    EXPECT_TRUE(openxisf::test::no_diagnostics(openxisf::test::unexpected_diagnostics(file.diagnostics())));
    ASSERT_EQ(file.images().size(), 3U);

    const openxisf::image_info& first = file.image(0);
    EXPECT_EQ(first.id, "first");
    EXPECT_EQ(first.geometry, (openxisf::geometry{.dimensions = {37, 23}, .channels = 1}));
    EXPECT_EQ(first.sample_format, openxisf::sample_format::uint16);
    EXPECT_EQ(first.properties, property_list({{.id = "Test:First", .value = property_value(std::uint16_t{1})}}));

    const openxisf::image_info& second = file.image(1);
    EXPECT_EQ(second.id, "second");
    EXPECT_EQ(second.geometry, (openxisf::geometry{.dimensions = {37, 23}, .channels = 3}));
    EXPECT_EQ(second.sample_format, openxisf::sample_format::float32);
    EXPECT_EQ(second.color_space, openxisf::color_space::rgb);
    EXPECT_EQ(second.bounds, (openxisf::bounds{.lower = 0.0, .upper = 1.0}));
    EXPECT_EQ(second.properties, property_list({{.id = "Test:Second", .value = property_value("second image")}}));

    const openxisf::image_info& third = file.image(2);
    EXPECT_EQ(third.id, "third");
    EXPECT_EQ(third.geometry, (openxisf::geometry{.dimensions = {11, 7}, .channels = 1}));
    EXPECT_EQ(third.sample_format, openxisf::sample_format::uint8);
    EXPECT_EQ(third.properties, property_list({{.id = "Test:Third", .value = property_value(3.5)}}));
}

TEST(samples_d4, the_first_two_images_are_those_of_a2_and_a4)
{
    const openxisf::reader file = open_sample("D4");
    EXPECT_EQ(file.read_pixels(0), open_sample("A2").read_pixels(0));
    EXPECT_EQ(file.read_pixels(1), open_sample("A4").read_pixels(0));
}

TEST(samples_d4, each_image_has_a_block_of_its_own)
{
    // PixInsight embeds a block of up to 3072 bytes in the header, as for the samples of group A.
    const openxisf::reader file = open_sample("D4");
    EXPECT_EQ(file.block(0),
              (openxisf::block_info{
                  .location = openxisf::block_location::embedded, .stored_size = 1702, .uncompressed_size = 1702}));
    EXPECT_EQ(file.block(1),
              (openxisf::block_info{
                  .location = openxisf::block_location::attachment, .stored_size = 10212, .uncompressed_size = 10212}));
    EXPECT_EQ(file.block(2),
              (openxisf::block_info{
                  .location = openxisf::block_location::embedded, .stored_size = 77, .uncompressed_size = 77}));
}

TEST(samples_d4, the_third_image_increases_in_storage_order)
{
    // The expression (x() + 11*y())/76 gives pixel i the value i / 76, rounded to 8 bits.
    const std::vector<std::uint8_t> samples = open_sample("D4").read_pixels<std::uint8_t>(2);
    ASSERT_EQ(samples.size(), 77U);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        EXPECT_LE(std::abs(samples[i] - (static_cast<double>(i) / 76.0 * 255.0)), 1.0) << i;
        if (i > 0) {
            EXPECT_GT(samples[i], samples[i - 1]) << i;
        }
    }
}

} // namespace
