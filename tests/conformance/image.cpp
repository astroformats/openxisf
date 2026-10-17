// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §8.5 and §11.5: Image elements and their pixel data, on constructed units.

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/reader.h>
#include <openxisf/types.h>

#include "codec/compressed_block.h"
#include "codec/compression.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/faulty_io.h"
#include "support/fixture_builder.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::color_space;
using openxisf::errc;
using openxisf::image_info;
using openxisf::pixel_storage;
using openxisf::reader;
using openxisf::sample_format;
using openxisf::severity;
using openxisf::test::bytes;
using openxisf::test::header_xml;
using openxisf::test::no_diagnostics;
using openxisf::test::single_diagnostic;
using openxisf::test::throws;

// An Image element with the given attributes, its pixel data embedded as hexadecimal digits, and content after its Data
// element.
std::string image(std::string_view attributes, std::string_view hex, std::string_view content = {})
{
    return "<Image " + std::string(attributes) + R"( location="embedded"><Data encoding="hex">)" + std::string(hex) +
           "</Data>" + std::string(content) + "</Image>";
}

// A gray 8-bit image of two pixels, 1 and 2, with the given attributes added.
std::string pair_image(std::string_view attributes = {})
{
    return image(R"(geometry="2:1:1" sampleFormat="UInt8" )" + std::string(attributes), "0102");
}

reader open_body(std::string_view body, openxisf::read_options options = {})
{
    return openxisf::test::open_header(header_xml(body), std::move(options));
}

// Opens a unit whose images have attached blocks: {N} in body locates blocks[N].
reader open_attached(std::string_view body, const std::vector<std::vector<std::byte>>& blocks,
                     openxisf::read_options options = {})
{
    return openxisf::test::open_unit(openxisf::test::file_with_attachments(header_xml(body), blocks),
                                     std::move(options));
}

// The only diagnostic is an error with code about the first image, which is left out.
testing::AssertionResult left_out(const reader& file, errc code, std::string_view attribute)
{
    testing::AssertionResult found = single_diagnostic(file.diagnostics(), severity::error, code, "/xisf/Image[1]");
    if (!found) {
        return found;
    }
    if (file.diagnostics().front().context.attribute != attribute) {
        return testing::AssertionFailure()
               << "the diagnostic is about the attribute " << file.diagnostics().front().context.attribute;
    }
    if (!file.images().empty()) {
        return testing::AssertionFailure() << "the image is listed";
    }
    return testing::AssertionSuccess();
}

// The only diagnostic is a warning with code about an attribute of the first image, which is listed.
testing::AssertionResult tolerated(const reader& file, errc code, std::string_view attribute)
{
    testing::AssertionResult found = single_diagnostic(file.diagnostics(), severity::warning, code, "/xisf/Image[1]");
    if (!found) {
        return found;
    }
    if (file.diagnostics().front().context.attribute != attribute) {
        return testing::AssertionFailure()
               << "the diagnostic is about the attribute " << file.diagnostics().front().context.attribute;
    }
    if (file.images().size() != 1) {
        return testing::AssertionFailure() << "the image is not listed";
    }
    return testing::AssertionSuccess();
}

// -----------------------------------------------------------------------------------------------------------------
// Attributes

TEST(conformance_image, an_image_has_the_attributes_of_its_element)
{
    // Spec §11.5.1, §11.5.2. The uuid has uppercase digits, which its canonical form allows.
    const reader file = open_body(image(R"(geometry="2:1:1" sampleFormat="UInt8" colorSpace="Gray" )"
                                        R"(pixelStorage="Normal" imageType="MasterDark" bounds="1:200" offset="1.5" )"
                                        R"(orientation="90;flip" id="M31" uuid="C5C93B6D-9072-4E85-9548-1A5391377683")",
                                        "0102"));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.images().size(), 1U);
    const image_info& info = file.image(0);
    EXPECT_EQ(info.geometry, (openxisf::geometry{.dimensions = {2, 1}, .channels = 1}));
    EXPECT_EQ(info.sample_format, sample_format::uint8);
    EXPECT_EQ(info.color_space, color_space::gray);
    EXPECT_EQ(info.pixel_storage, pixel_storage::normal);
    EXPECT_EQ(info.image_type, openxisf::image_type::master_dark);
    EXPECT_EQ(info.bounds, (openxisf::bounds{.lower = 1.0, .upper = 200.0}));
    EXPECT_EQ(info.offset, 1.5);
    EXPECT_EQ(info.orientation, openxisf::orientation::rotate_90_flip);
    EXPECT_EQ(info.id, "M31");
    EXPECT_EQ(info.uuid, "c5c93b6d-9072-4e85-9548-1a5391377683");
}

TEST(conformance_image, optional_attributes_have_the_defaults_of_the_specification)
{
    const reader file = open_body(image(R"(geometry="2:1:1" sampleFormat="UInt16")", "01000200"));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.images().size(), 1U);
    EXPECT_EQ(file.image(0),
              (image_info{.geometry = {.dimensions = {2, 1}, .channels = 1}, .sample_format = sample_format::uint16}));
    EXPECT_EQ(file.image(0).representable_range(), (openxisf::bounds{.lower = 0.0, .upper = 65535.0}));
}

TEST(conformance_image, attributes_that_the_specification_does_not_define_are_ignored)
{
    // Spec §7. Attributes are matched by name, so a prefixed geometry is another attribute.
    const reader file = open_body(pair_image(R"(xmlns:x="urn:x" x:geometry="1:1:1" Geometry="9:9:9" foo="bar")"));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.image(0).geometry.dimensions, (std::vector<std::uint64_t>{2, 1}));
}

TEST(conformance_image, an_image_has_its_properties)
{
    const reader file = open_body(image(R"(geometry="2:1:1" sampleFormat="UInt8")", "0102",
                                        R"(<Property id="Instrument:ExposureTime" type="Float32" value="300"/>)"));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.image(0).properties.at("Instrument:ExposureTime").value, openxisf::property_value(300.0F));
}

TEST(conformance_image, an_image_without_geometry_or_sample_format_is_left_out)
{
    // Spec §11.5.1: both are required.
    EXPECT_TRUE(left_out(open_body(image(R"(sampleFormat="UInt8")", "0102")), errc::invalid_image, "geometry"));
    EXPECT_TRUE(left_out(open_body(image(R"(geometry="2:1:1")", "0102")), errc::invalid_image, "sampleFormat"));
}

TEST(conformance_image, a_geometry_that_cannot_be_read_leaves_the_image_out)
{
    // Spec §11.5.1: lengths and a number of channels above zero, in the plain text grammar of unsigned integers. The
    // geometries refused are in unit/image_attributes.cpp.
    EXPECT_TRUE(left_out(open_body(image(R"(sampleFormat="UInt8" geometry="2:0:1")", "0102")), errc::invalid_geometry,
                         "geometry"));
    const reader hexadecimal = open_body(image(R"(geometry="0x2:1:1" sampleFormat="UInt8")", "0102"));
    EXPECT_TRUE(no_diagnostics(hexadecimal.diagnostics()));
}

TEST(conformance_image, pixel_data_that_64_bits_cannot_count_leave_the_image_out)
{
    // 2^32 x 2^31 samples fit in 64 bits, but not their 2^64 bytes in UInt16.
    const reader file = open_body(image(R"(geometry="4294967296:2147483648:1" sampleFormat="UInt16")", "0102"));
    EXPECT_TRUE(left_out(file, errc::invalid_geometry, "geometry"));
}

TEST(conformance_image, an_image_in_a_sample_format_or_colour_space_outside_the_specification_is_unavailable)
{
    // Spec §7: the image is unavailable, and the rest of the unit stays readable.
    const reader file = open_body(image(R"(geometry="2:1:1" sampleFormat="Int16")", "01000200") + pair_image() +
                                  pair_image(R"(colorSpace="CMYK")"));
    ASSERT_EQ(file.diagnostics().size(), 2U) << openxisf::test::describe(file.diagnostics());
    EXPECT_EQ(file.diagnostics()[0].code, errc::unsupported_sample_format);
    EXPECT_EQ(file.diagnostics()[0].context.element, "/xisf/Image[1]");
    EXPECT_EQ(file.diagnostics()[1].code, errc::unsupported_color_space);
    EXPECT_EQ(file.diagnostics()[1].context.element, "/xisf/Image[3]");
    ASSERT_EQ(file.images().size(), 1U);
    EXPECT_EQ(file.read_pixels<std::uint8_t>(0), (std::vector<std::uint8_t>{1, 2}));

    // Strict reading refuses the unit with an unsupported_error.
    EXPECT_TRUE(throws<openxisf::unsupported_error>(
        errc::unsupported_color_space, [] { (void)open_body(pair_image(R"(colorSpace="CMYK")"), {.strict = true}); }));
}

TEST(conformance_image, an_rgb_or_cielab_image_has_its_three_nominal_channels)
{
    // Spec §8.5.1: the channels after the nominal ones are alpha channels.
    const reader rgba = open_body(image(R"(geometry="1:1:4" sampleFormat="UInt8" colorSpace="RGB")", "01020304"));
    EXPECT_TRUE(no_diagnostics(rgba.diagnostics()));
    EXPECT_EQ(rgba.image(0).color_space, color_space::rgb);
    const reader lab = open_body(image(R"(geometry="1:1:3" sampleFormat="UInt8" colorSpace="CIELab")", "010203"));
    EXPECT_TRUE(no_diagnostics(lab.diagnostics()));
    EXPECT_EQ(lab.image(0).color_space, color_space::cie_lab);

    for (const std::string_view space : {"RGB", "CIELab"}) {
        const reader file = open_body(
            image(R"(geometry="2:1:2" sampleFormat="UInt8" colorSpace=")" + std::string(space) + '"', "01020304"));
        EXPECT_TRUE(left_out(file, errc::invalid_image, "colorSpace")) << space;
    }
}

TEST(conformance_image, a_pixel_storage_outside_the_specification_leaves_the_image_out)
{
    // Spec §11.5.2: the storage model says where each sample is.
    EXPECT_TRUE(left_out(open_body(pair_image(R"(pixelStorage="planar")")), errc::invalid_image, "pixelStorage"));
}

TEST(conformance_image, an_offset_is_a_finite_value_of_at_least_zero)
{
    // Spec §11.5.2. A negative offset still says what was added to the samples, and is kept with a warning; a value
    // that cannot be read could mean anything.
    EXPECT_EQ(open_body(pair_image(R"(offset="2.5")")).image(0).offset, 2.5);
    const reader negative = open_body(pair_image(R"(offset="-1")"));
    EXPECT_TRUE(tolerated(negative, errc::invalid_image, "offset"));
    EXPECT_EQ(negative.image(0).offset, -1.0);
    // The offsets refused are in unit/image_attributes.cpp.
    EXPECT_TRUE(left_out(open_body(pair_image(R"(offset="NaN")")), errc::invalid_image, "offset"));
}

TEST(conformance_image, a_floating_point_image_needs_bounds)
{
    // Spec §8.5.5, §11.5.1: there is no default range for floating point samples. The samples are still read.
    const reader file = open_body(image(R"(geometry="1:1:1" sampleFormat="Float32")", "0000c03f"));
    EXPECT_TRUE(tolerated(file, errc::invalid_image, "bounds"));
    EXPECT_EQ(file.image(0).bounds, std::nullopt);
    EXPECT_EQ(file.image(0).representable_range(), std::nullopt);
    EXPECT_EQ(file.read_pixels<float>(0), std::vector<float>{1.5F});
    const reader binary64 = open_body(image(R"(geometry="1:1:1" sampleFormat="Float64")", "000000000000f83f"));
    EXPECT_TRUE(tolerated(binary64, errc::invalid_image, "bounds"));

    // Bounds are optional for complex images, whose range is undefined.
    const reader complex = open_body(image(R"(geometry="1:1:1" sampleFormat="Complex32")", "0000c03f00000040"));
    EXPECT_TRUE(no_diagnostics(complex.diagnostics()));
    EXPECT_EQ(complex.image(0).representable_range(), std::nullopt);
}

TEST(conformance_image, bounds_that_cannot_be_read_are_ignored)
{
    // They only say how to show the samples. The bounds refused are in unit/image_attributes.cpp.
    const reader file = open_body(pair_image(R"(bounds="1:0")"));
    EXPECT_TRUE(tolerated(file, errc::invalid_image, "bounds"));
    EXPECT_EQ(file.image(0).bounds, std::nullopt);
}

TEST(conformance_image, an_unknown_image_type_or_orientation_is_ignored)
{
    const reader type = open_body(pair_image(R"(imageType="Science")"));
    EXPECT_TRUE(tolerated(type, errc::invalid_image, "imageType"));
    EXPECT_EQ(type.image(0).image_type, std::nullopt);
    const reader turn = open_body(pair_image(R"(orientation="270")"));
    EXPECT_TRUE(tolerated(turn, errc::invalid_image, "orientation"));
    EXPECT_EQ(turn.image(0).orientation, openxisf::orientation::none);
}

TEST(conformance_image, image_ids_are_unique_and_of_their_grammar)
{
    // Spec §11.5.2. An id only names its image, so every image is kept with its id.
    const reader file = open_body(pair_image(R"(id="M31")") + pair_image(R"(id="M31")") + pair_image(R"(id="1st")"));
    ASSERT_EQ(file.diagnostics().size(), 2U) << openxisf::test::describe(file.diagnostics());
    EXPECT_EQ(file.diagnostics()[0].severity, severity::warning);
    EXPECT_EQ(file.diagnostics()[0].code, errc::duplicate_image_id);
    EXPECT_EQ(file.diagnostics()[0].context.element, "/xisf/Image[2]");
    EXPECT_EQ(file.diagnostics()[1].severity, severity::warning);
    EXPECT_EQ(file.diagnostics()[1].code, errc::invalid_image_id);
    EXPECT_EQ(file.diagnostics()[1].context.element, "/xisf/Image[3]");
    ASSERT_EQ(file.images().size(), 3U);
    EXPECT_EQ(file.image(1).id, "M31");
    EXPECT_EQ(file.image(2).id, "1st");
}

TEST(conformance_image, a_uuid_is_a_version_4_uuid_in_canonical_form)
{
    // Spec §11.5.2. A malformed uuid identifies nothing; another version of UUID still identifies the image.
    const reader malformed = open_body(pair_image(R"(uuid="c5c93b6d90724e8595481a5391377683")"));
    EXPECT_TRUE(tolerated(malformed, errc::invalid_uuid, "uuid"));
    EXPECT_EQ(malformed.image(0).uuid, "");
    const reader version_1 = open_body(pair_image(R"(uuid="c5c93b6d-9072-1e85-9548-1a5391377683")"));
    EXPECT_TRUE(tolerated(version_1, errc::invalid_uuid, "uuid"));
    EXPECT_EQ(version_1.image(0).uuid, "c5c93b6d-9072-1e85-9548-1a5391377683");
    // Version 4 in another variant than that of RFC 9562, whose two high bits of the ninth byte are 10.
    const reader variant = open_body(pair_image(R"(uuid="c5c93b6d-9072-4e85-c548-1a5391377683")"));
    EXPECT_TRUE(tolerated(variant, errc::invalid_uuid, "uuid"));
}

// -----------------------------------------------------------------------------------------------------------------
// Pixel data

TEST(conformance_image, an_image_without_a_data_block_is_left_out)
{
    // Spec §11.5: the pixel data are a data block.
    const reader file = open_body(R"(<Image geometry="2:1:1" sampleFormat="UInt8"/>)");
    EXPECT_TRUE(left_out(file, errc::invalid_image, "location"));
}

TEST(conformance_image, the_data_block_holds_exactly_the_samples_of_the_geometry)
{
    // Spec §8.5.3: n1 x ... x nN x channels x sample size bytes.
    for (const std::string_view hex : {"01", "010203"}) {
        const reader file = open_body(image(R"(geometry="2:1:1" sampleFormat="UInt8")", hex));
        EXPECT_TRUE(left_out(file, errc::pixel_data_size_mismatch, "geometry")) << hex;
    }
    // A compressed block holds its uncompressed size, which is compared without decompressing anything.
    const reader compressed = open_attached(
        R"(<Image geometry="2:1:1" sampleFormat="UInt8" compression="zlib:3" location="attachment:{0}"/>)",
        {bytes("not zlib")});
    EXPECT_TRUE(left_out(compressed, errc::pixel_data_size_mismatch, "geometry"));
}

TEST(conformance_image, an_image_whose_data_block_is_unavailable_is_listed_without_its_pixels)
{
    // Spec §7: the unsupported object is the data block; reading the pixels throws its error.
    const reader file =
        open_attached(R"(<Image geometry="2:1:1" sampleFormat="UInt8" compression="rle:2" location="attachment:{0}"/>)",
                      {bytes("ab")});
    EXPECT_TRUE(
        single_diagnostic(file.diagnostics(), severity::error, errc::unsupported_compression, "/xisf/Image[1]"));
    ASSERT_EQ(file.images().size(), 1U);
    EXPECT_TRUE(
        throws<openxisf::unsupported_error>(errc::unsupported_compression, [&file] { (void)file.read_pixels(0); }));
}

TEST(conformance_image, an_inline_data_block_is_read_with_a_warning)
{
    // Spec §11.5 forbids it, since an Image element can have child elements, but the data are unambiguous, and a
    // baseline decoder reads pixel data from inline blocks (spec §7.2).
    const reader file = open_body(R"(<Image geometry="2:1:1" sampleFormat="UInt8" location="inline:hex">0102</Image>)");
    EXPECT_TRUE(tolerated(file, errc::invalid_location, "location"));
    EXPECT_EQ(file.read_pixels<std::uint8_t>(0), (std::vector<std::uint8_t>{1, 2}));
}

TEST(conformance_image, images_are_listed_in_document_order_and_can_share_a_data_block)
{
    // Each image reads the shared bytes with its own attributes.
    const reader file = open_attached(
        R"(<Image id="a" geometry="2:1:1" sampleFormat="UInt16" location="attachment:{0}"/>)"
        R"(<Image id="b" geometry="4:1:1" sampleFormat="UInt8" location="attachment:{0}"/>)"
        R"(<Image id="c" geometry="2:1:1" sampleFormat="UInt16" byteOrder="big" location="attachment:{0}"/>)",
        {bytes("\x01\x02\x03\x04")});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.images().size(), 3U);
    EXPECT_EQ(file.image(0).id, "a");
    EXPECT_EQ(file.read_pixels<std::uint16_t>(0), (std::vector<std::uint16_t>{0x0201, 0x0403}));
    EXPECT_EQ(file.read_pixels<std::uint8_t>(1), (std::vector<std::uint8_t>{1, 2, 3, 4}));
    EXPECT_EQ(file.read_pixels<std::uint16_t>(2), (std::vector<std::uint16_t>{0x0102, 0x0304}));
}

TEST(conformance_image, pixels_are_read_in_the_storage_model_asked_for)
{
    // Spec §8.5.3: an RGB image of 3 x 2 UInt8 samples, where the sample of channel c at (x, y) is 100c + 10y + x,
    // stored in each model.
    const std::string planar_hex = "0001020a0b0c6465666e6f70c8c9cad2d3d4";
    const std::string normal_hex = "0064c80165c90266ca0a6ed20b6fd30c70d4";
    const std::vector<std::uint8_t> planar{0,   1,   2,   10,  11,  12,  100, 101, 102,
                                           110, 111, 112, 200, 201, 202, 210, 211, 212};
    const std::vector<std::uint8_t> normal{0,  100, 200, 1,  101, 201, 2,  102, 202,
                                           10, 110, 210, 11, 111, 211, 12, 112, 212};
    const std::string attributes = R"(geometry="3:2:3" sampleFormat="UInt8" colorSpace="RGB")";
    const reader file =
        open_body(image(attributes, planar_hex) + image(attributes + R"( pixelStorage="Normal")", normal_hex));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    for (std::size_t i = 0; i < 2; ++i) {
        // As stored, then in each model.
        EXPECT_EQ(file.read_pixels<std::uint8_t>(i), i == 0 ? planar : normal) << i;
        EXPECT_EQ(file.read_pixels<std::uint8_t>(i, {.storage = pixel_storage::planar}), planar) << i;
        EXPECT_EQ(file.read_pixels<std::uint8_t>(i, {.storage = pixel_storage::normal}), normal) << i;
    }
}

// A geometry, and the pixel count it gives.
struct dimensions_case
{
    std::string_view geometry{};
    std::size_t pixels = 0;
};

// The dimensionality: CTest names the tests after it. The name is fixed by GoogleTest.
// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const dimensions_case& value, std::ostream* output)
{
    *output << std::ranges::count(value.geometry, ':') + 1 << 'd';
}

class conformance_image_dimensions : public testing::TestWithParam<dimensions_case>
{};

TEST_P(conformance_image_dimensions, an_image_of_any_dimensionality_orders_its_pixels_by_coordinates)
{
    // Spec §8.5.1, §8.5.3: N >= 1 dimensions, the first coordinate varying fastest; two channels of UInt16 samples,
    // whose value is 1000c + p for pixel p of channel c.
    const std::size_t pixels = GetParam().pixels;
    std::vector<std::uint16_t> planar;
    std::vector<std::uint16_t> normal(2 * pixels);
    std::string hex;
    for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t p = 0; p < pixels; ++p) {
            const auto value = static_cast<std::uint16_t>((1000 * c) + p);
            planar.push_back(value);
            normal[(2 * p) + c] = value;
            constexpr std::string_view digits = "0123456789abcdef";
            const unsigned int bits = value;
            for (const unsigned int byte : {bits & 0xFFU, bits >> 8U}) {
                hex += digits[byte >> 4U];
                hex += digits[byte & 0xFU];
            }
        }
    }
    const reader file =
        open_body(image(R"(sampleFormat="UInt16" geometry=")" + std::string(GetParam().geometry) + ":2\"", hex));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.image(0).geometry.pixel_count(), pixels);
    EXPECT_EQ(file.read_pixels<std::uint16_t>(0), planar);
    EXPECT_EQ(file.read_pixels<std::uint16_t>(0, {.storage = pixel_storage::normal}), normal);
}

INSTANTIATE_TEST_SUITE_P(conformance, conformance_image_dimensions,
                         testing::Values(dimensions_case{.geometry = "5", .pixels = 5},
                                         dimensions_case{.geometry = "3:2", .pixels = 6},
                                         dimensions_case{.geometry = "2:3:2", .pixels = 12},
                                         dimensions_case{.geometry = "1:2:1:3", .pixels = 6}),
                         [](const testing::TestParamInfo<dimensions_case>& parameter) {
                             return std::to_string(std::ranges::count(parameter.param.geometry, ':') + 1) + "d";
                         });

TEST(conformance_image, pixels_are_read_into_memory_of_the_application)
{
    const reader file = open_body(image(R"(geometry="2:1:1" sampleFormat="UInt16")", "01000200"));
    std::vector<std::uint16_t> samples(2);
    file.read_pixels(0, std::span(samples));
    EXPECT_EQ(samples, (std::vector<std::uint16_t>{1, 2}));
    std::vector<std::byte> data(4);
    file.read_pixels(0, data);
    EXPECT_EQ(data, file.read_pixels(0));
}

TEST(conformance_image, reads_need_an_image_and_memory_of_its_size_and_type)
{
    const reader file = open_body(image(R"(geometry="2:1:1" sampleFormat="UInt16")", "01000200"));
    const auto usage = [](auto&& read) {
        return throws<openxisf::usage_error>(errc::invalid_argument, std::forward<decltype(read)>(read));
    };
    EXPECT_TRUE(usage([&file] { (void)file.image(1); }));
    EXPECT_TRUE(usage([&file] { (void)file.read_pixels(1); }));
    EXPECT_TRUE(usage([&file] { (void)file.read_pixels<std::uint8_t>(0); }));
    std::vector<std::byte> short_data(3);
    EXPECT_TRUE(usage([&file, &short_data] { file.read_pixels(0, short_data); }));
    std::vector<std::uint16_t> long_samples(3);
    EXPECT_TRUE(usage([&file, &long_samples] { file.read_pixels(0, std::span(long_samples)); }));
    std::vector<float> floats(1);
    EXPECT_TRUE(usage([&file, &floats] { file.read_pixels(0, std::span(floats)); }));
}

TEST(conformance_image, pixel_data_that_fail_their_checksum_are_not_returned)
{
    // Spec §10.5: an attached block is verified when it is read.
    const reader file = open_attached(R"(<Image geometry="2:1:1" sampleFormat="UInt8" location="attachment:{0}" )"
                                      R"(checksum="sha1:0000000000000000000000000000000000000000"/>)",
                                      {bytes("\x01\x02")});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch, [&file] { (void)file.read_pixels(0); }));
}

TEST(conformance_image, compressed_pixel_data_are_decompressed)
{
    // Spec §10.6: decompressed, then unshuffled, then put in native byte order.
    std::vector<std::byte> samples;
    for (std::uint16_t value = 0; value < 300; ++value) {
        samples.push_back(static_cast<std::byte>(value >> 8U));
        samples.push_back(static_cast<std::byte>(value & 0xFFU));
    }
    const openxisf::detail::compressed_block block =
        openxisf::detail::compress_block(samples, {.codec = openxisf::detail::compression_codec::zstd, .item_size = 2});
    const reader file = open_attached(R"(<Image geometry="300:1:1" sampleFormat="UInt16" byteOrder="big" )"
                                      R"(compression="zstd+sh:600:2" location="attachment:{0}"/>)",
                                      {block.data});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    const std::vector<std::uint16_t> values = file.read_pixels<std::uint16_t>(0);
    ASSERT_EQ(values.size(), 300U);
    for (std::uint16_t value = 0; value < 300; ++value) {
        EXPECT_EQ(values[value], value);
    }
}

TEST(conformance_image, a_typed_read_checks_the_allocation_limit_before_it_allocates)
{
    // A compressed block can declare up to 1032 times its stored bytes with zlib; nothing of that size is allocated
    // beyond the limit.
    const openxisf::read_options options{.limits = {.max_allocation = 1000}};
    const reader file =
        open_attached(R"(<Image geometry="1000000:1:1" sampleFormat="UInt8" compression="zlib:1000000" )"
                      R"(location="attachment:{0}"/>)",
                      {openxisf::test::pattern(1000)}, options);
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    // The typed read refuses before it reads anything, which the progress function would see.
    int calls = 0;
    const openxisf::progress_function count = [&calls](std::uint64_t, std::uint64_t) { return ++calls > 0; };
    EXPECT_TRUE(throws<openxisf::limit_error>(
        errc::allocation_too_large, [&file, &count] { (void)file.read_pixels<std::uint8_t>(0, {.progress = count}); }));
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::allocation_too_large, [&file] { (void)file.read_pixels(0); }));

    // A read into memory of the caller allocates nothing, typed or not, so the limit does not apply to it.
    const std::vector<std::byte> stored = openxisf::test::pattern(1001);
    const reader plain = open_attached(R"(<Image geometry="1001:1:1" sampleFormat="UInt8" location="attachment:{0}"/>)",
                                       {stored}, options);
    EXPECT_TRUE(no_diagnostics(plain.diagnostics()));
    std::vector<std::uint8_t> samples(stored.size());
    plain.read_pixels(0, std::span(samples));
    EXPECT_TRUE(std::ranges::equal(std::as_bytes(std::span(samples)), stored));
    std::vector<std::byte> data(stored.size());
    plain.read_pixels(0, std::span(data));
    EXPECT_EQ(data, stored);
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::allocation_too_large,
                                              [&plain] { (void)plain.read_pixels<std::uint8_t>(0); }));
}

// -----------------------------------------------------------------------------------------------------------------
// Progress and cancellation

// The calls of a progress function, which returns false at the call numbered stop (from 1), if any.
struct progress_log
{
    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls{};
    std::size_t stop = 0;

    [[nodiscard]] openxisf::progress_function function()
    {
        return [this](std::uint64_t done, std::uint64_t total) {
            calls.emplace_back(done, total);
            return calls.size() != stop;
        };
    }
};

TEST(conformance_image, progress_counts_the_bytes_read_and_decompressed)
{
    const std::vector<std::byte> samples = openxisf::test::pattern(1000);
    const openxisf::detail::compressed_block block =
        openxisf::detail::compress_block(samples, {.codec = openxisf::detail::compression_codec::zlib});
    const reader file = open_attached(
        R"(<Image geometry="1000:1:1" sampleFormat="UInt8" location="attachment:{0}"/>)"
        R"(<Image geometry="1000:1:1" sampleFormat="UInt8" compression="zlib:1000" location="attachment:{1}"/>)",
        {samples, block.data});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));

    progress_log uncompressed;
    EXPECT_EQ(file.read_pixels(0, {.progress = uncompressed.function()}), samples);
    EXPECT_EQ(uncompressed.calls, (std::vector<std::pair<std::uint64_t, std::uint64_t>>{{0, 1000}, {1000, 1000}}));

    // The compressed bytes are read, then decompressed.
    progress_log compressed;
    EXPECT_EQ(file.read_pixels(1, {.progress = compressed.function()}), samples);
    const std::uint64_t stored = block.data.size();
    EXPECT_EQ(compressed.calls, (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                                    {0, stored + 1000}, {stored, stored + 1000}, {stored + 1000, stored + 1000}}));
}

TEST(conformance_image, a_progress_function_that_returns_false_cancels_the_read)
{
    const reader file = open_body(pair_image());
    for (const std::size_t stop : {1U, 2U}) {
        progress_log log{.stop = stop};
        std::vector<std::byte> destination(2, std::byte{0xEE});
        EXPECT_TRUE(throws<openxisf::cancelled_error>(errc::cancelled, [&] {
            file.read_pixels(0, destination, {.progress = log.function()});
        })) << stop;
        EXPECT_EQ(log.calls.size(), stop);
    }
    // A read cancelled before it starts leaves the destination as it was.
    progress_log log{.stop = 1};
    std::vector<std::byte> destination(2, std::byte{0xEE});
    EXPECT_THROW(file.read_pixels(0, destination, {.progress = log.function()}), openxisf::cancelled_error);
    EXPECT_EQ(destination, std::vector<std::byte>(2, std::byte{0xEE}));
}

TEST(conformance_image, the_exceptions_of_a_progress_function_pass_through)
{
    const reader file = open_body(pair_image());
    const openxisf::progress_function failing = [](std::uint64_t, std::uint64_t) -> bool {
        throw openxisf::test::injected_fault();
    };
    EXPECT_THROW((void)file.read_pixels(0, {.progress = failing}), openxisf::test::injected_fault);
}

} // namespace
