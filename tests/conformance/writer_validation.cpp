// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The writer checks a unit against the specification before it writes any byte of it, and names the object at fault:
// one case for each rule, of properties (spec §8.4.1, §8.4.3, §8.4.4, §11.1), tables (§8.4.4.7, §11.2, §11.3), the
// metadata (§11.4) and reserved identifiers (§11.5.3), images (§8.5.1, §8.5.5, §11.5.1) and their ancillary elements
// (§8.5.4, §8.5.6, §11.6 to §11.12).

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>
#include <openxisf/writer.h>

#include "core/scratch_buffer.h"
#include "support/bytes.h"
#include "support/temp_directory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::image_info;
using openxisf::property;
using openxisf::property_value;
using openxisf::table;

constexpr double nan = std::numeric_limits<double>::quiet_NaN();
constexpr double infinity = std::numeric_limits<double>::infinity();

// The bytes of an ICC profile header: its size, then the signature at byte 36.
std::vector<std::byte> icc_profile()
{
    std::vector<std::byte> profile(132);
    profile[3] = std::byte{132};
    const std::vector<std::byte> signature = openxisf::test::bytes("acsp");
    std::ranges::copy(signature, profile.begin() + 36);
    return profile;
}

openxisf::thumbnail small_thumbnail()
{
    openxisf::thumbnail result;
    result.geometry = {.dimensions = {4, 3}, .channels = 3};
    result.pixels.resize(36);
    return result;
}

// A unit that the writer accepts, with every kind of object, which each case breaks in one place.
struct model
{
    openxisf::write_options options{.creator_application = "OpenXISF tests 1.0",
                                    .creation_time = openxisf::date_time{.year = 2026, .month = 10, .day = 4}};
    openxisf::property_list metadata{};
    openxisf::property_list properties{};
    std::vector<table> tables{};
    std::vector<image_info> images{};

    model()
    {
        metadata.set("XISF:Title", "A test");
        properties.set("Test:Standalone", std::int32_t{1});
        tables.push_back({.id = "Standalone",
                          .fields = {{.id = "name", .type = openxisf::property_type::string}},
                          .rows = {{"Vega"}}});

        // Member by member: GCC 16 warns about copies of nested designated initializers that hold vectors.
        image_info image;
        image.geometry = {.dimensions = {4, 3}, .channels = 3};
        image.sample_format = openxisf::sample_format::float32;
        image.color_space = openxisf::color_space::rgb;
        image.bounds = openxisf::bounds{.lower = 0.0, .upper = 1.0};
        image.id = "first";
        image.properties.set("Instrument:ExposureTime", 300.0F);
        image.properties.set("Image:FrameNumber", std::uint32_t{1});
        image.properties.set("Image:GroupId", "M42_2026-10.L");
        image.properties.set("Image:SubgroupId", "9");
        image.tables.push_back({.id = "Table",
                                .fields = {{.id = "number", .type = openxisf::property_type::uint8},
                                           {.id = "name", .type = openxisf::property_type::string}},
                                .rows = {{std::uint8_t{1}, "Crab Nebula"}}});
        image.fits_keywords = {{.name = "EXPTIME", .value = "300", .comment = "Exposure time in seconds"},
                               {.name = "HISTORY", .comment = "Calibrated"}};
        image.icc_profile = icc_profile();
        image.rgb_working_space = openxisf::rgb_working_space{};
        image.display_function = openxisf::display_function{};
        image.color_filter_array = openxisf::color_filter_array{.pattern = "RGGB", .width = 2, .height = 2};
        image.resolution = openxisf::resolution{.unit = openxisf::resolution_unit::centimeter};
        image.thumbnail = small_thumbnail();
        image.thumbnail->properties.set("Test:Thumbnail", true);
        images.push_back(image);

        image_info second;
        second.geometry = {.dimensions = {5}, .channels = 1};
        second.id = "second";
        images.push_back(second);
    }
};

// Saves a model into a sink, which must stay empty when the save fails.
void save(const model& unit)
{
    openxisf::writer output(unit.options);
    output.metadata() = unit.metadata;
    output.properties() = unit.properties;
    output.tables() = unit.tables;
    std::vector<std::vector<std::byte>> pixels;
    for (const image_info& image : unit.images) {
        pixels.emplace_back(image.data_size());
        (void)output.add_image(image, pixels.back());
    }
    openxisf::memory_sink sink;
    try {
        output.save(sink);
    } catch (...) {
        EXPECT_EQ(sink.position(), 0U) << "bytes were written before the unit failed its validation";
        throw;
    }
}

image_info& first(model& unit)
{
    return unit.images.front();
}

openxisf::thumbnail& small(model& unit)
{
    std::optional<openxisf::thumbnail>& thumbnail = unit.images.front().thumbnail;
    if (!thumbnail) {
        throw std::logic_error("the model has no thumbnail");
    }
    return *thumbnail;
}

// A value outside an enumeration, which a cast can give and the writer refuses.
template <typename Enum> Enum outside_of_enumeration()
{
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): the value is outside on purpose
    return static_cast<Enum>(99);
}

TEST(conformance_writer_validation, the_model_of_the_cases_is_valid)
{
    EXPECT_NO_THROW(save(model{}));
}

struct rule
{
    std::string name{};
    std::function<void(model&)> breaks{};
    errc code = errc::invalid_argument;
    std::string element{};
    std::string attribute{};
};

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const rule& value, std::ostream* output)
{
    *output << value.name;
}

std::vector<rule> rules()
{
    const std::string image = "/xisf/Image[1]";
    const std::string application = "/xisf/Metadata/Property[@id='XISF:CreatorApplication']";
    std::vector<rule> cases{
        // Mandatory metadata (spec §11.4.1).
        {.name = "no_creator_application",
         .breaks = [](model& unit) { unit.options.creator_application.clear(); },
         .code = errc::invalid_metadata,
         .element = application,
         .attribute = "value"},
        {.name = "creator_application_without_version",
         .breaks = [](model& unit) { unit.options.creator_application = "MyCapture"; },
         .code = errc::invalid_metadata,
         .element = application,
         .attribute = "value"},
        {.name = "creator_application_without_name",
         .breaks = [](model& unit) { unit.options.creator_application = "2.1"; },
         .code = errc::invalid_metadata,
         .element = application,
         .attribute = "value"},
        {.name = "creator_application_with_a_control_character",
         .breaks =
             [](model& unit) {
                 unit.options.creator_application = "My\x01"
                                                    "Capture 2.1";
             },
         .code = errc::invalid_character,
         .element = application,
         .attribute = "value"},
        {.name = "creator_application_not_utf8",
         .breaks =
             [](model& unit) {
                 unit.options.creator_application = "My\xFF"
                                                    "Capture 2.1";
             },
         .code = errc::invalid_utf8,
         .element = application,
         .attribute = "value"},
        {.name = "creation_time_out_of_range",
         .breaks = [](model& unit) { unit.options.creation_time = openxisf::date_time{.year = 2026, .month = 13}; },
         .code = errc::invalid_time_point,
         .element = "/xisf/Metadata/Property[@id='XISF:CreationTime']",
         .attribute = "value"},
        {.name = "metadata_outside_the_xisf_namespace",
         .breaks = [](model& unit) { unit.metadata.set("Title", "A test"); },
         .code = errc::invalid_metadata,
         .element = "/xisf/Metadata/Property[@id='Title']",
         .attribute = "id"},
        {.name = "metadata_of_a_reserved_type",
         .breaks = [](model& unit) { unit.metadata.set("XISF:Title", std::uint8_t{1}); },
         .code = errc::reserved_property_type,
         .element = "/xisf/Metadata/Property[@id='XISF:Title']",
         .attribute = "type"},

        // Properties (spec §8.4, §11.1).
        {.name = "property_identifier",
         .breaks = [](model& unit) { unit.properties.set("1st", true); },
         .code = errc::invalid_property_id,
         .element = "/xisf/Property[2]",
         .attribute = "id"},
        {.name = "property_identifier_with_an_empty_namespace",
         .breaks = [](model& unit) { unit.properties.set("Test::Empty", true); },
         .code = errc::invalid_property_id,
         .element = "/xisf/Property[2]",
         .attribute = "id"},
        {.name = "reserved_property_type",
         .breaks = [](model& unit) { first(unit).properties.set("Instrument:ExposureTime", "300 s"); },
         .code = errc::reserved_property_type,
         .element = image + "/Property[@id='Instrument:ExposureTime']",
         .attribute = "type"},
        // The values of the Image namespace (spec §11.5.3.5).
        {.name = "frame_number_zero",
         .breaks = [](model& unit) { first(unit).properties.set("Image:FrameNumber", std::uint32_t{0}); },
         .code = errc::invalid_property,
         .element = image + "/Property[@id='Image:FrameNumber']",
         .attribute = "value"},
        {.name = "group_identifier_with_a_space",
         .breaks = [](model& unit) { first(unit).properties.set("Image:GroupId", "M42 L"); },
         .code = errc::invalid_property,
         .element = image + "/Property[@id='Image:GroupId']",
         .attribute = "value"},
        {.name = "group_identifier_starting_with_a_hyphen",
         .breaks = [](model& unit) { first(unit).properties.set("Image:GroupId", "-M42"); },
         .code = errc::invalid_property,
         .element = image + "/Property[@id='Image:GroupId']",
         .attribute = "value"},
        {.name = "empty_subgroup_identifier",
         .breaks = [](model& unit) { first(unit).properties.set("Image:SubgroupId", ""); },
         .code = errc::invalid_property,
         .element = image + "/Property[@id='Image:SubgroupId']",
         .attribute = "value"},
        {.name = "subgroup_without_a_group",
         .breaks = [](model& unit) { first(unit).properties.erase("Image:GroupId"); },
         .code = errc::invalid_property,
         .element = image + "/Property[@id='Image:SubgroupId']",
         .attribute = "id"},
        {.name = "thumbnail_frame_number_zero",
         .breaks = [](model& unit) { small(unit).properties.set("Image:FrameNumber", std::uint32_t{0}); },
         .code = errc::invalid_property,
         .element = image + "/Thumbnail/Property[@id='Image:FrameNumber']",
         .attribute = "value"},
        {.name = "string_not_utf8",
         .breaks = [](model& unit) { unit.properties.set("Test:String", "\xC3("); },
         .code = errc::invalid_utf8,
         .element = "/xisf/Property[@id='Test:String']",
         .attribute = "value"},
        {.name = "string_with_u0000",
         .breaks = [](model& unit) { unit.properties.set("Test:String", std::string("a\0b", 3)); },
         .code = errc::invalid_utf8,
         .element = "/xisf/Property[@id='Test:String']",
         .attribute = "value"},
        {.name = "time_point_out_of_range",
         .breaks = [](model& unit) { unit.properties.set("Test:Time", openxisf::date_time{.year = 10000}); },
         .code = errc::invalid_time_point,
         .element = "/xisf/Property[@id='Test:Time']",
         .attribute = "value"},
        {.name = "format_of_a_time_point",
         .breaks =
             [](model& unit) {
                 unit.properties.set({.id = "Test:Time",
                                      .value = openxisf::date_time{},
                                      .format = openxisf::property_format{.width = 30}});
             },
         .code = errc::invalid_format_specifier,
         .element = "/xisf/Property[@id='Test:Time']",
         .attribute = "format"},
        {.name = "format_fill_semicolon",
         .breaks =
             [](model& unit) {
                 unit.properties.set(
                     {.id = "Test:Value", .value = 1.0, .format = openxisf::property_format{.fill = ';'}});
             },
         .code = errc::invalid_format_specifier,
         .element = "/xisf/Property[@id='Test:Value']",
         .attribute = "format"},
        {.name = "format_fill_tab",
         .breaks =
             [](model& unit) {
                 unit.properties.set(
                     {.id = "Test:Value", .value = 1.0, .format = openxisf::property_format{.fill = '\t'}});
             },
         .code = errc::invalid_format_specifier,
         .element = "/xisf/Property[@id='Test:Value']",
         .attribute = "format"},
        {.name = "format_unit_with_a_space",
         .breaks =
             [](model& unit) {
                 unit.properties.set(
                     {.id = "Test:Value", .value = 1.0, .format = openxisf::property_format{.unit = "km s-1"}});
             },
         .code = errc::invalid_format_specifier,
         .element = "/xisf/Property[@id='Test:Value']",
         .attribute = "format"},
        {.name = "format_unit_with_a_semicolon",
         .breaks =
             [](model& unit) {
                 unit.properties.set(
                     {.id = "Test:Value", .value = 1.0, .format = openxisf::property_format{.unit = "m;s"}});
             },
         .code = errc::invalid_format_specifier,
         .element = "/xisf/Property[@id='Test:Value']",
         .attribute = "format"},
        {.name = "format_enumerator",
         .breaks =
             [](model& unit) {
                 unit.properties.set(
                     {.id = "Test:Value",
                      .value = 1.0,
                      .format = openxisf::property_format{.align = outside_of_enumeration<openxisf::format_align>()}});
             },
         .code = errc::invalid_format_specifier,
         .element = "/xisf/Property[@id='Test:Value']",
         .attribute = "format"},
        {.name = "property_comment",
         .breaks = [](model& unit) { unit.properties.set({.id = "Test:Value", .value = 1.0, .comment = "a\x02"}); },
         .code = errc::invalid_character,
         .element = "/xisf/Property[@id='Test:Value']",
         .attribute = "comment"},

        // Tables (spec §11.2, §11.3).
        {.name = "table_named_like_a_property",
         .breaks = [](model& unit) { first(unit).tables.front().id = "Instrument:ExposureTime"; },
         .code = errc::duplicate_property_id,
         .element = image + "/Table[@id='Instrument:ExposureTime']",
         .attribute = "id"},
        {.name = "two_tables_of_one_name",
         .breaks = [](model& unit) { unit.tables.push_back(unit.tables.front()); },
         .code = errc::duplicate_property_id,
         .element = "/xisf/Table[@id='Standalone']",
         .attribute = "id"},
        {.name = "table_identifier",
         .breaks = [](model& unit) { first(unit).tables.front().id = "a-b"; },
         .code = errc::invalid_property_id,
         .element = image + "/Table[1]",
         .attribute = "id"},
        {.name = "table_without_fields",
         .breaks =
             [](model& unit) {
                 unit.tables.front().fields.clear();
                 unit.tables.front().rows.clear();
             },
         .code = errc::invalid_table,
         .element = "/xisf/Table[@id='Standalone']/Structure"},
        {.name = "field_identifier",
         .breaks = [](model& unit) { first(unit).tables.front().fields[1].id = "2nd"; },
         .code = errc::invalid_property_id,
         .element = image + "/Table[@id='Table']/Structure/Field[2]",
         .attribute = "id"},
        {.name = "two_fields_of_one_name",
         .breaks = [](model& unit) { first(unit).tables.front().fields[1].id = "number"; },
         .code = errc::invalid_table,
         .element = image + "/Table[@id='Table']/Structure/Field[2]",
         .attribute = "id"},
        {.name = "field_type",
         .breaks =
             [](model& unit) {
                 first(unit).tables.front().fields[0].type = outside_of_enumeration<openxisf::property_type>();
             },
         .code = errc::unsupported_property_type,
         .element = image + "/Table[@id='Table']/Structure/Field[1]",
         .attribute = "type"},
        {.name = "field_header",
         .breaks = [](model& unit) { first(unit).tables.front().fields[1].header = "\x7F ok, \x1B not"; },
         .code = errc::invalid_character,
         .element = image + "/Table[@id='Table']/Structure/Field[2]",
         .attribute = "header"},
        {.name = "field_format",
         .breaks =
             [](model& unit) { first(unit).tables.front().fields[0].format = openxisf::property_format{.unit = " "}; },
         .code = errc::invalid_format_specifier,
         .element = image + "/Table[@id='Table']/Structure/Field[1]",
         .attribute = "format"},
        {.name = "row_without_a_cell_for_each_field",
         .breaks = [](model& unit) { first(unit).tables.front().rows.front().pop_back(); },
         .code = errc::invalid_table,
         .element = image + "/Table[@id='Table']/Row[1]"},
        {.name = "cell_of_another_type",
         .breaks = [](model& unit) { first(unit).tables.front().rows.front()[1] = std::int32_t{1}; },
         .code = errc::invalid_table,
         .element = image + "/Table[@id='Table']/Row[1]/Cell[2]"},
        {.name = "cell_value",
         .breaks = [](model& unit) { first(unit).tables.front().rows.front()[1] = "\xFF"; },
         .code = errc::invalid_utf8,
         .element = image + "/Table[@id='Table']/Row[1]/Cell[2]",
         .attribute = "value"},
        {.name = "table_caption",
         .breaks = [](model& unit) { unit.tables.front().caption = "\x0B"; },
         .code = errc::invalid_character,
         .element = "/xisf/Table[@id='Standalone']",
         .attribute = "caption"},
        {.name = "table_comment",
         .breaks = [](model& unit) { unit.tables.front().comment = "\xEF\xBF\xBF"; },
         .code = errc::invalid_character,
         .element = "/xisf/Table[@id='Standalone']",
         .attribute = "comment"},

        // Images (spec §8.5, §11.5).
        {.name = "geometry_without_dimensions",
         .breaks = [](model& unit) { first(unit).geometry.dimensions.clear(); },
         .code = errc::invalid_geometry,
         .element = image,
         .attribute = "geometry"},
        {.name = "geometry_of_length_zero",
         .breaks = [](model& unit) { first(unit).geometry.dimensions = {4, 0}; },
         .code = errc::invalid_geometry,
         .element = image,
         .attribute = "geometry"},
        {.name = "geometry_without_channels",
         .breaks = [](model& unit) { first(unit).geometry.channels = 0; },
         .code = errc::invalid_geometry,
         .element = image,
         .attribute = "geometry"},
        {.name = "fewer_channels_than_the_colour_space",
         .breaks = [](model& unit) { first(unit).geometry.channels = 2; },
         .code = errc::invalid_image,
         .element = image,
         .attribute = "geometry"},
        {.name = "sample_format",
         .breaks = [](model& unit) { first(unit).sample_format = outside_of_enumeration<openxisf::sample_format>(); },
         .code = errc::unsupported_sample_format,
         .element = image,
         .attribute = "sampleFormat"},
        {.name = "color_space",
         .breaks = [](model& unit) { first(unit).color_space = outside_of_enumeration<openxisf::color_space>(); },
         .code = errc::unsupported_color_space,
         .element = image,
         .attribute = "colorSpace"},
        {.name = "pixel_storage",
         .breaks = [](model& unit) { first(unit).pixel_storage = outside_of_enumeration<openxisf::pixel_storage>(); },
         .code = errc::invalid_image,
         .element = image,
         .attribute = "pixelStorage"},
        {.name = "image_type",
         .breaks = [](model& unit) { first(unit).image_type = outside_of_enumeration<openxisf::image_type>(); },
         .code = errc::invalid_image,
         .element = image,
         .attribute = "imageType"},
        {.name = "orientation",
         .breaks = [](model& unit) { first(unit).orientation = outside_of_enumeration<openxisf::orientation>(); },
         .code = errc::invalid_image,
         .element = image,
         .attribute = "orientation"},
        {.name = "floating_point_image_without_bounds",
         .breaks = [](model& unit) { first(unit).bounds.reset(); },
         .code = errc::invalid_image,
         .element = image,
         .attribute = "bounds"},
        {.name = "bounds_in_decreasing_order",
         .breaks = [](model& unit) { first(unit).bounds = openxisf::bounds{.lower = 1.0, .upper = 1.0}; },
         .code = errc::invalid_image,
         .element = image,
         .attribute = "bounds"},
        {.name = "bounds_not_finite",
         .breaks = [](model& unit) { unit.images[1].bounds = openxisf::bounds{.lower = 0.0, .upper = infinity}; },
         .code = errc::invalid_image,
         .element = "/xisf/Image[2]",
         .attribute = "bounds"},
        {.name = "negative_offset",
         .breaks = [](model& unit) { first(unit).offset = -0.5; },
         .code = errc::invalid_image,
         .element = image,
         .attribute = "offset"},
        {.name = "offset_not_finite",
         .breaks = [](model& unit) { first(unit).offset = nan; },
         .code = errc::invalid_image,
         .element = image,
         .attribute = "offset"},
        {.name = "image_identifier",
         .breaks = [](model& unit) { first(unit).id = "1st"; },
         .code = errc::invalid_image_id,
         .element = image,
         .attribute = "id"},
        {.name = "image_identifier_with_a_colon",
         .breaks = [](model& unit) { first(unit).id = "a:b"; },
         .code = errc::invalid_image_id,
         .element = image,
         .attribute = "id"},
        {.name = "two_images_of_one_identifier",
         .breaks = [](model& unit) { unit.images[1].id = "first"; },
         .code = errc::duplicate_image_id,
         .element = "/xisf/Image[2]",
         .attribute = "id"},
        {.name = "uuid_not_canonical",
         .breaks = [](model& unit) { first(unit).uuid = "{c5c93b6d-9072-4e85-9548-1a5391377683}"; },
         .code = errc::invalid_uuid,
         .element = image,
         .attribute = "uuid"},
        {.name = "uuid_of_version_1",
         .breaks = [](model& unit) { first(unit).uuid = "c5c93b6d-9072-1e85-9548-1a5391377683"; },
         .code = errc::invalid_uuid,
         .element = image,
         .attribute = "uuid"},

        // FITS keywords (spec §11.6).
        {.name = "keyword_name_in_lowercase",
         .breaks = [](model& unit) { first(unit).fits_keywords[0].name = "exptime"; },
         .code = errc::invalid_fits_keyword,
         .element = image + "/FITSKeyword[1]",
         .attribute = "name"},
        {.name = "keyword_name_padded",
         .breaks = [](model& unit) { first(unit).fits_keywords[0].name = "EXPTIME "; },
         .code = errc::invalid_fits_keyword,
         .element = image + "/FITSKeyword[1]",
         .attribute = "name"},
        {.name = "keyword_name_too_long",
         .breaks = [](model& unit) { first(unit).fits_keywords[0].name = "EXPOSURET"; },
         .code = errc::invalid_fits_keyword,
         .element = image + "/FITSKeyword[1]",
         .attribute = "name"},
        {.name = "history_keyword_with_a_value",
         .breaks = [](model& unit) { first(unit).fits_keywords[1].value = "1"; },
         .code = errc::invalid_fits_keyword,
         .element = image + "/FITSKeyword[2]",
         .attribute = "value"},
        // FITS has a blank keyword, but the XML schema of XISF requires a name of one to eight characters.
        {.name = "blank_keyword",
         .breaks = [](model& unit) { first(unit).fits_keywords.push_back({.comment = "A blank keyword"}); },
         .code = errc::invalid_fits_keyword,
         .element = image + "/FITSKeyword[3]",
         .attribute = "name"},
        {.name = "keyword_value_not_ascii",
         .breaks =
             [](model& unit) {
                 first(unit).fits_keywords[0].value = "'\xC3\x91"
                                                      "and\xC3\xBA'";
             },
         .code = errc::invalid_fits_keyword,
         .element = image + "/FITSKeyword[1]",
         .attribute = "value"},
        {.name = "keyword_comment_with_a_tab",
         .breaks = [](model& unit) { first(unit).fits_keywords[0].comment = "in\tseconds"; },
         .code = errc::invalid_fits_keyword,
         .element = image + "/FITSKeyword[1]",
         .attribute = "comment"},

        // ICC profiles (spec §11.7).
        {.name = "icc_profile_too_short",
         .breaks = [](model& unit) { first(unit).icc_profile.resize(127); },
         .code = errc::invalid_icc_profile,
         .element = image + "/ICCProfile"},
        {.name = "icc_profile_without_signature",
         .breaks = [](model& unit) { first(unit).icc_profile[36] = std::byte{'b'}; },
         .code = errc::invalid_icc_profile,
         .element = image + "/ICCProfile"},

        // RGB working spaces (spec §8.5.4.1, §11.8).
        {.name = "gamma_of_zero",
         .breaks = [](model& unit) { first(unit).rgb_working_space->gamma = 0.0; },
         .code = errc::invalid_rgb_working_space,
         .element = image + "/RGBWorkingSpace",
         .attribute = "gamma"},
        {.name = "gamma_not_finite",
         .breaks = [](model& unit) { first(unit).rgb_working_space->gamma = infinity; },
         .code = errc::invalid_rgb_working_space,
         .element = image + "/RGBWorkingSpace",
         .attribute = "gamma"},
        {.name = "chromaticity_out_of_range",
         .breaks = [](model& unit) { first(unit).rgb_working_space->x[1] = 1.5; },
         .code = errc::invalid_rgb_working_space,
         .element = image + "/RGBWorkingSpace"},
        {.name = "chromaticities_without_a_working_space",
         .breaks = [](model& unit) { first(unit).rgb_working_space->y = {0.3, 0.3, 0.0}; },
         .code = errc::invalid_rgb_working_space,
         .element = image + "/RGBWorkingSpace"},
        {.name = "luminance_of_another_white",
         .breaks = [](model& unit) { first(unit).rgb_working_space->luminance = {0.2126, 0.7152, 0.0722}; },
         .code = errc::invalid_rgb_working_space,
         .element = image + "/RGBWorkingSpace",
         .attribute = "Y"},
        {.name = "luminance_that_the_chromaticities_give_below_0",
         .breaks =
             [](model& unit) {
                 // D50 lies just outside the triangle of these primaries: they give the green coefficient -5.6e-5,
                 // within the tolerance of the 0 given, and the writer would write that one.
                 first(unit).rgb_working_space->x = {0.64, 0.30, 0.0};
                 first(unit).rgb_working_space->y = {0.33, 0.60, 0.39198};
                 first(unit).rgb_working_space->luminance = {0.4972, 0.0, 0.5029};
             },
         .code = errc::invalid_rgb_working_space,
         .element = image + "/RGBWorkingSpace",
         .attribute = "Y"},
        {.name = "working_space_name",
         .breaks = [](model& unit) { first(unit).rgb_working_space->name = "\x01"; },
         .code = errc::invalid_character,
         .element = image + "/RGBWorkingSpace",
         .attribute = "name"},

        // Display functions (spec §8.5.6, §11.9).
        {.name = "midtones_out_of_range",
         .breaks = [](model& unit) { first(unit).display_function->midtones[3] = 1.5; },
         .code = errc::invalid_display_function,
         .element = image + "/DisplayFunction"},
        {.name = "shadows_above_highlights",
         .breaks =
             [](model& unit) {
                 first(unit).display_function->shadows[0] = 0.8;
                 first(unit).display_function->highlights[0] = 0.7;
             },
         .code = errc::invalid_display_function,
         .element = image + "/DisplayFunction"},
        {.name = "shadows_expansion_above_zero",
         .breaks = [](model& unit) { first(unit).display_function->shadows_expansion[1] = 0.1; },
         .code = errc::invalid_display_function,
         .element = image + "/DisplayFunction"},
        {.name = "highlights_expansion_below_one",
         .breaks = [](model& unit) { first(unit).display_function->highlights_expansion[2] = 0.9; },
         .code = errc::invalid_display_function,
         .element = image + "/DisplayFunction"},
        {.name = "expansion_not_finite",
         .breaks = [](model& unit) { first(unit).display_function->shadows_expansion[0] = -infinity; },
         .code = errc::invalid_display_function,
         .element = image + "/DisplayFunction"},
        {.name = "display_function_name",
         .breaks = [](model& unit) { first(unit).display_function->name = "\xFF"; },
         .code = errc::invalid_utf8,
         .element = image + "/DisplayFunction",
         .attribute = "name"},

        // Colour filter arrays (spec §11.10).
        {.name = "cfa_element",
         .breaks = [](model& unit) { first(unit).color_filter_array->pattern = "RGGX"; },
         .code = errc::invalid_color_filter_array,
         .element = image + "/ColorFilterArray",
         .attribute = "pattern"},
        {.name = "cfa_pattern_length",
         .breaks = [](model& unit) { first(unit).color_filter_array->pattern = "RGG"; },
         .code = errc::invalid_color_filter_array,
         .element = image + "/ColorFilterArray",
         .attribute = "pattern"},
        {.name = "cfa_of_width_zero",
         .breaks =
             [](model& unit) {
                 first(unit).color_filter_array = openxisf::color_filter_array{.pattern = "", .width = 0, .height = 2};
             },
         .code = errc::invalid_color_filter_array,
         .element = image + "/ColorFilterArray",
         .attribute = "pattern"},
        {.name = "cfa_of_a_three_dimensional_image",
         .breaks = [](model& unit) { first(unit).geometry.dimensions = {2, 3, 2}; },
         .code = errc::invalid_color_filter_array,
         .element = image + "/ColorFilterArray"},
        {.name = "cfa_name",
         .breaks = [](model& unit) { first(unit).color_filter_array->name = "\x1F"; },
         .code = errc::invalid_character,
         .element = image + "/ColorFilterArray",
         .attribute = "name"},

        // Resolutions (spec §11.11).
        {.name = "resolution_of_zero",
         .breaks = [](model& unit) { first(unit).resolution->horizontal = 0.0; },
         .code = errc::invalid_resolution,
         .element = image + "/Resolution"},
        {.name = "resolution_not_finite",
         .breaks = [](model& unit) { first(unit).resolution->vertical = infinity; },
         .code = errc::invalid_resolution,
         .element = image + "/Resolution"},
        {.name = "resolution_unit",
         .breaks =
             [](model& unit) { first(unit).resolution->unit = outside_of_enumeration<openxisf::resolution_unit>(); },
         .code = errc::invalid_resolution,
         .element = image + "/Resolution",
         .attribute = "unit"},

        // Thumbnails (spec §11.12).
        {.name = "thumbnail_of_three_dimensions",
         .breaks = [](model& unit) { small(unit).geometry.dimensions = {2, 2, 3}; },
         .code = errc::invalid_thumbnail,
         .element = image + "/Thumbnail"},
        {.name = "thumbnail_of_floating_point_samples",
         .breaks =
             [](model& unit) {
                 small(unit).sample_format = openxisf::sample_format::float32;
                 small(unit).pixels.resize(144);
             },
         .code = errc::invalid_thumbnail,
         .element = image + "/Thumbnail"},
        {.name = "thumbnail_in_cielab",
         .breaks = [](model& unit) { small(unit).color_space = openxisf::color_space::cie_lab; },
         .code = errc::invalid_thumbnail,
         .element = image + "/Thumbnail"},
        {.name = "thumbnail_with_two_alpha_channels",
         .breaks =
             [](model& unit) {
                 small(unit).geometry.channels = 5;
                 small(unit).pixels.resize(60);
             },
         .code = errc::invalid_thumbnail,
         .element = image + "/Thumbnail"},
        {.name = "thumbnail_beyond_2_64_bytes",
         .breaks =
             [](model& unit) { small(unit).geometry.dimensions = {std::uint64_t{1} << 32, std::uint64_t{1} << 32}; },
         .code = errc::invalid_geometry,
         .element = image + "/Thumbnail",
         .attribute = "geometry"},
        {.name = "thumbnail_pixel_data",
         .breaks = [](model& unit) { small(unit).pixels.pop_back(); },
         .code = errc::pixel_data_size_mismatch,
         .element = image + "/Thumbnail"},
        {.name = "thumbnail_identifier",
         .breaks = [](model& unit) { small(unit).id = "-"; },
         .code = errc::invalid_image_id,
         .element = image + "/Thumbnail",
         .attribute = "id"},
        {.name = "thumbnail_property",
         .breaks = [](model& unit) { small(unit).properties.set("Instrument:Filter:Name", 1.0); },
         .code = errc::reserved_property_type,
         .element = image + "/Thumbnail/Property[@id='Instrument:Filter:Name']",
         .attribute = "type"},
        {.name = "thumbnail_keyword",
         .breaks = [](model& unit) { small(unit).fits_keywords.push_back({.name = "A B"}); },
         .code = errc::invalid_fits_keyword,
         .element = image + "/Thumbnail/FITSKeyword[1]",
         .attribute = "name"},
        {.name = "thumbnail_icc_profile",
         .breaks = [](model& unit) { small(unit).icc_profile = std::vector<std::byte>(10); },
         .code = errc::invalid_icc_profile,
         .element = image + "/Thumbnail/ICCProfile"},
        {.name = "thumbnail_offset",
         .breaks = [](model& unit) { small(unit).offset = -1.0; },
         .code = errc::invalid_image,
         .element = image + "/Thumbnail",
         .attribute = "offset"},
    };
    return cases;
}

class conformance_writer_validation_rule : public testing::TestWithParam<rule>
{};

TEST_P(conformance_writer_validation_rule, fails_before_anything_is_written)
{
    const rule& tested = GetParam();
    model unit;
    tested.breaks(unit);
    try {
        save(unit);
        ADD_FAILURE() << "the unit was written";
    } catch (const openxisf::validation_error& failure) {
        EXPECT_EQ(failure.code(), tested.code) << failure.what();
        EXPECT_EQ(failure.context().element, tested.element) << failure.what();
        EXPECT_EQ(failure.context().attribute, tested.attribute) << failure.what();
    }
}

INSTANTIATE_TEST_SUITE_P(rules, conformance_writer_validation_rule, testing::ValuesIn(rules()),
                         [](const testing::TestParamInfo<rule>& parameter) { return parameter.param.name; });

TEST(conformance_writer_validation, the_metadata_that_the_writer_writes_itself_is_not_checked)
{
    // As a reader returns it from a PixInsight file, whose XISF:CreationTime is a String.
    model unit;
    unit.metadata.set("XISF:CreationTime", "2026-10-04T12:00:00Z");
    unit.metadata.set("XISF:CompressionLevel", "\x01");
    EXPECT_NO_THROW(save(unit));
}

TEST(conformance_writer_validation, a_creator_application_longer_than_1_mib_is_refused)
{
    // The writer writes the metadata that it generates in the header, which has no data block for them, while a String
    // longer than 1 MiB goes to a data block. The creator application then named a block of the unit: the pixel data of
    // an image, the value of an inline property, or none, which crashed the save.
    image_info image;
    image.geometry = {.dimensions = {4, 4}, .channels = 1};
    const std::vector<std::byte> pixels(image.data_size(), std::byte{'Z'});
    const auto writer_of = [&image, &pixels](const std::string& application, std::string_view blocks) {
        openxisf::write_options options = model{}.options;
        options.creator_application = application;
        openxisf::writer output(options);
        if (blocks == "attached") {
            (void)output.add_image(image, pixels);
        } else if (blocks == "inline") {
            output.properties().set("Test:Vector", std::vector<std::uint8_t>{'h', 'i', '!'});
        }
        return output;
    };
    const std::string longest = "MyCapture 2.1 " + std::string((std::size_t{1} << 20) - 14, 'a');
    for (const std::string_view blocks : {"attached", "inline", "none"}) {
        SCOPED_TRACE(blocks);
        openxisf::memory_sink written;
        writer_of(longest, blocks).save(written);
        const openxisf::reader file(std::make_unique<openxisf::memory_source>(written.release()));
        const property* application = file.metadata().find("XISF:CreatorApplication");
        ASSERT_NE(application, nullptr);
        // Compared as a whole: a message with the two values would hold 1 MiB.
        EXPECT_TRUE(application->value == property_value(longest)) << "another creator application was read back";

        openxisf::memory_sink refused;
        try {
            writer_of(longest + "a", blocks).save(refused);
            ADD_FAILURE() << "the unit was written";
        } catch (const openxisf::validation_error& failure) {
            EXPECT_EQ(failure.code(), errc::invalid_metadata) << failure.what();
            EXPECT_EQ(failure.context().element, "/xisf/Metadata/Property[@id='XISF:CreatorApplication']");
            EXPECT_EQ(failure.context().attribute, "value");
        }
        EXPECT_EQ(refused.position(), 0U);
    }
}

TEST(conformance_writer_validation, strings_that_xml_cannot_hold_are_valid_values)
{
    // They are written in data blocks (spec §11.1.6).
    model unit;
    unit.properties.set("Test:Control", "\x01\x02 \xEF\xBF\xBE");
    first(unit).tables.front().rows.front()[1] = "\x1B[0m";
    EXPECT_NO_THROW(save(unit));
}

// A save of output that the writer refuses with header_too_large before any byte is written, and before the progress
// function, which counts its calls, hears of it. distributed picks save_distributed() over save(), and rewritable
// memory sinks over sinks that only append.
void expect_header_too_large(const openxisf::writer& output, const int& progress_calls, bool distributed,
                             bool rewritable)
{
    SCOPED_TRACE(std::string(distributed ? "distributed" : "monolithic") + (rewritable ? ", rewritable" : ""));
    std::uint64_t appended = 0;
    const auto append = [&appended](std::span<const std::byte> data) { appended += data.size(); };
    openxisf::memory_sink header;
    openxisf::memory_sink blocks;
    openxisf::callback_sink appended_header(append);
    openxisf::callback_sink appended_blocks(append);
    openxisf::output_sink& header_sink = rewritable ? static_cast<openxisf::output_sink&>(header) : appended_header;
    openxisf::output_sink& blocks_sink = rewritable ? static_cast<openxisf::output_sink&>(blocks) : appended_blocks;
    try {
        if (distributed) {
            output.save_distributed(header_sink, blocks_sink, "unit.xisb");
        } else {
            output.save(header_sink);
        }
        ADD_FAILURE() << "the unit was written";
    } catch (const openxisf::validation_error& failure) {
        EXPECT_EQ(failure.code(), errc::header_too_large) << failure.what();
        EXPECT_EQ(failure.context().element, "/xisf");
    }
    EXPECT_EQ(header.position() + blocks.position() + appended, 0U);
    EXPECT_EQ(progress_calls, 0);
}

TEST(conformance_writer_validation, a_header_longer_than_its_length_can_count_is_refused)
{
    // Spec §9.2: the header length has 32 bits. A subblock takes at least four characters of the header ("1,1:"), so
    // the 2^30 subblocks of one byte of two images of 512 MiB cannot be listed, though those of each could be; the
    // writer finds it before it compresses anything, so the memory of the pixel data, which the images share, is never
    // written or read.
    image_info image;
    image.geometry = {.dimensions = {std::uint64_t{1} << 15, std::uint64_t{1} << 14}, .channels = 1};
    image.sample_format = openxisf::sample_format::uint8;
    const openxisf::detail::scratch_buffer pixels(image.data_size());
    openxisf::write_options options = model{}.options;
    options.codec = openxisf::codec::lz4;
    options.subblock_size = 1;
    int calls = 0;
    options.progress = [&calls](std::uint64_t, std::uint64_t) {
        ++calls;
        return false;
    };
    openxisf::writer output(options);
    (void)output.add_image(image, pixels.bytes());
    (void)output.add_image(image, pixels.bytes());
    for (const bool distributed : {false, true}) {
        for (const bool rewritable : {false, true}) {
            expect_header_too_large(output, calls, distributed, rewritable);
        }
    }
}

TEST(conformance_writer_validation, a_header_without_room_for_its_longest_form_is_refused_before_it_is_built)
{
    // Sinks that can rewrite get a compressed unit in one pass, and its header once its blocks are written: after room
    // for the longest header that the blocks can give in a monolithic file, and after the data blocks file of a
    // distributed unit. The longest header has subblocks of 20 digits. The 2^27 subblocks of one byte of an image of
    // 128 MiB would fit in 2^32 - 1 bytes at four characters each, but not at that width, over 5 GiB: the writer
    // refuses the unit before it lists them, and before the progress function hears of the save, which it would cancel.
    image_info image;
    image.geometry = {.dimensions = {std::uint64_t{1} << 14, std::uint64_t{1} << 13}, .channels = 1};
    image.sample_format = openxisf::sample_format::uint8;
    const openxisf::detail::scratch_buffer pixels(image.data_size());
    openxisf::write_options options = model{}.options;
    options.codec = openxisf::codec::lz4;
    options.subblock_size = 1;
    int calls = 0;
    options.progress = [&calls](std::uint64_t, std::uint64_t) {
        ++calls;
        return false;
    };
    openxisf::writer output(options);
    (void)output.add_image(image, pixels.bytes());
    for (const bool distributed : {false, true}) {
        expect_header_too_large(output, calls, distributed, true);
    }
}

TEST(conformance_writer_validation, a_save_to_a_path_creates_no_file_for_an_invalid_unit)
{
    const openxisf::test::temp_directory directory;
    model unit;
    unit.options.creator_application.clear();
    openxisf::writer output(unit.options);
    const std::string path = directory.file("invalid.xisf");
    EXPECT_THROW(output.save(path), openxisf::validation_error);
    EXPECT_EQ(directory.entries(), 0U);
}

} // namespace
