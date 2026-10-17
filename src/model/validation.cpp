// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/validation.h"

#include <openxisf/error.h>

#include "core/quote.h"
#include "core/utc_time.h"
#include "core/utf8.h"
#include "core/uuid.h"
#include "model/ancillary_attributes.h"
#include "model/format_specifier.h"
#include "model/header_tree.h"
#include "model/image_attributes.h"
#include "model/outline.h"
#include "model/property_catalog.h"
#include "model/property_text.h"
#include "model/property_types.h"
#include "xml/xml_writer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace openxisf::detail {

namespace {

[[noreturn]] void fail(errc code, const std::string& message, const std::string& element,
                       std::string_view attribute = {})
{
    throw validation_error(code, message, {.element = element, .attribute = std::string(attribute)});
}

// The step of a path to a property or a table: by its identifier, or by its position when the identifier itself is
// at fault, since it could hold anything.
std::string id_step(std::string_view name, std::string_view id, std::size_t position)
{
    if (is_property_id(id)) {
        return std::string(name) + "[@id='" + std::string(id) + "']";
    }
    return std::string(name) + "[" + std::to_string(position) + "]";
}

// Text written in an attribute or as character data, which only valid UTF-8 of the characters of XML can be.
void check_text(std::string_view text, const std::string& element, std::string_view attribute, std::string_view what)
{
    if (!is_valid_utf8(text)) {
        fail(errc::invalid_utf8, std::string(what) + " is not valid UTF-8 without U+0000", element, attribute);
    }
    if (!is_xml_text(text)) {
        fail(errc::invalid_character,
             std::string(what) + " holds a character that XML cannot hold, such as a control character", element,
             attribute);
    }
}

// A format is written as the tokens that differ from the defaults, and must read back the same: that takes a fill
// that is a printable ASCII character other than the semicolon, a unit without white space or semicolons, and
// enumerators of the API.
void check_format(const property_format& format, property_type type, const std::string& element,
                  std::string_view attribute)
{
    if (type == property_type::time_point) {
        fail(errc::invalid_format_specifier, "a TimePoint has no format specifier (spec §8.4.3)", element, attribute);
    }
    check_text(format.unit, element, attribute, "the unit of the format");
    const std::string text = format_specifier_text(format);
    bool reads_back = text.empty();
    if (!reads_back) {
        try {
            reads_back = parse_format_specifier(text) == format;
        } catch (const invalid_data_error&) {
            reads_back = false;
        }
    }
    if (!reads_back) {
        fail(errc::invalid_format_specifier,
             "the format cannot be written as a format specifier (spec §8.4.3): its fill must be a printable ASCII "
             "character other than the semicolon, and its unit cannot hold white space or semicolons",
             element, attribute);
    }
}

void check_value(const property_value& value, const std::string& element, std::string_view attribute)
{
    switch (category_of(value.type())) {
    case type_category::string:
        if (!is_valid_utf8(value.get<std::string>())) {
            fail(errc::invalid_utf8, "the String is not valid UTF-8 without U+0000 (spec §8.4.4.3)", element,
                 attribute);
        }
        break;
    case type_category::time_point:
        if (!is_valid_time_point(value.get<date_time>())) {
            fail(errc::invalid_time_point, "the TimePoint is not a valid date and time from year 0 to 9999", element,
                 attribute);
        }
        break;
    case type_category::scalar:
    case type_category::complex:
    case type_category::vector:
    case type_category::matrix:
        break;
    }
}

// The properties of an astrometric solution of another revision than 1.x are kept as they are, whatever their types
// (spec §11.5.3.7.6): foreign_solution says that the solution of the object of item is such.
void check_property(const property& item, std::size_t position, const std::string& parent, bool foreign_solution)
{
    const std::string element = parent + "/" + id_step("Property", item.id, position);
    if (!is_property_id(item.id)) {
        fail(errc::invalid_property_id, quote(item.id) + " is not a property identifier (spec §8.4.1)", element, "id");
    }
    if (const std::optional<property_type> reserved = reserved_property_type(item.id);
        reserved && *reserved != item.value.type() && !(foreign_solution && is_astrometric_solution_id(item.id))) {
        fail(errc::reserved_property_type,
             "the property " + item.id + " is a " + std::string(type_name(item.value.type())) +
                 ", and the specification makes it a " + std::string(type_name(*reserved)),
             element, "type");
    }
    check_value(item.value, element, "value");
    if (item.format) {
        check_format(*item.format, item.value.type(), element, "format");
    }
    check_text(item.comment, element, "comment", "the comment");
}

void check_table(const table& item, std::size_t position, const std::string& parent)
{
    const std::string element = parent + "/" + id_step("Table", item.id, position);
    if (!is_property_id(item.id)) {
        fail(errc::invalid_property_id, quote(item.id) + " is not a property identifier (spec §8.4.1)", element, "id");
    }
    check_text(item.caption, element, "caption", "the caption");
    check_text(item.comment, element, "comment", "the comment");
    if (item.fields.empty()) {
        fail(errc::invalid_table, "the table has no field (spec §11.2)", element + "/Structure");
    }
    std::unordered_set<std::string_view> ids;
    for (std::size_t i = 0; i < item.fields.size(); ++i) {
        const table_field& field = item.fields[i];
        const std::string field_element = element + "/Structure/Field[" + std::to_string(i + 1) + "]";
        if (!is_property_id(field.id)) {
            fail(errc::invalid_property_id, quote(field.id) + " is not a field identifier (spec §11.2.1)",
                 field_element, "id");
        }
        if (!ids.insert(field.id).second) {
            fail(errc::invalid_table, "another field of the table has the identifier " + field.id, field_element, "id");
        }
        if (!is_property_type(field.type)) {
            fail(errc::unsupported_property_type,
                 "the field has the type " + std::to_string(static_cast<int>(field.type)) +
                     ", which is no property type of the specification (spec §8.4.4)",
                 field_element, "type");
        }
        check_text(field.header, field_element, "header", "the header");
        if (field.format) {
            check_format(*field.format, field.type, field_element, "format");
        }
    }
    for (std::size_t r = 0; r < item.rows.size(); ++r) {
        const std::string row_element = element + "/Row[" + std::to_string(r + 1) + "]";
        if (item.rows[r].size() != item.fields.size()) {
            fail(errc::invalid_table,
                 "the row has " + std::to_string(item.rows[r].size()) + " cells, and the structure of the table " +
                     std::to_string(item.fields.size()) + " fields (spec §11.3.3)",
                 row_element);
        }
        for (std::size_t c = 0; c < item.fields.size(); ++c) {
            const property_value& cell = item.rows[r][c];
            const std::string cell_element = row_element + "/Cell[" + std::to_string(c + 1) + "]";
            if (cell.type() != item.fields[c].type) {
                fail(errc::invalid_table,
                     "the cell is a " + std::string(type_name(cell.type())) + ", and its field " + item.fields[c].id +
                         " a " + std::string(type_name(item.fields[c].type)),
                     cell_element);
            }
            check_value(cell, cell_element, "value");
        }
    }
}

// The properties and tables of one object, whose identifiers are unique among both.
void check_properties(const property_list& properties, const std::vector<table>& tables, const std::string& parent)
{
    std::unordered_set<std::string_view> ids;
    std::size_t position = 0;
    const bool foreign_solution = has_foreign_astrometric_solution(properties);
    for (const property& item : properties) {
        check_property(item, ++position, parent, foreign_solution);
        ids.insert(item.id);
    }
    for (std::size_t i = 0; i < tables.size(); ++i) {
        check_table(tables[i], i + 1, parent);
        if (!ids.insert(tables[i].id).second) {
            fail(errc::duplicate_property_id, "a property or another table has the identifier " + tables[i].id,
                 parent + "/" + id_step("Table", tables[i].id, i + 1), "id");
        }
    }
}

// The values of the Image namespace that an image or a thumbnail has, which spec §11.5.3.5 restricts.
void check_image_values(const property_list& properties, const std::string& parent)
{
    std::size_t position = 0;
    for (const property& item : properties) {
        ++position;
        if (const std::optional<std::string> problem = forbidden_image_value(item)) {
            fail(errc::invalid_property, *problem, parent + "/" + id_step("Property", item.id, position), "value");
        }
    }
    if (properties.contains("Image:SubgroupId") && !properties.contains("Image:GroupId")) {
        fail(errc::invalid_property, std::string(subgroup_without_group),
             parent + "/" + id_step("Property", "Image:SubgroupId", 0), "id");
    }
}

void check_metadata(const property_list& metadata, const write_options& options)
{
    const std::string element = "/xisf/Metadata";
    const std::string application = element + "/Property[@id='XISF:CreatorApplication']";
    const std::string& name = options.creator_application;
    // The writer writes it in the header, as character data, where a String of that length fits.
    if (name.size() > max_character_data) {
        fail(errc::invalid_metadata,
             "XISF:CreatorApplication has " + std::to_string(name.size()) + " bytes, and the writer writes at most " +
                 std::to_string(max_character_data) + " (1 MiB) in the header",
             application, "value");
    }
    check_text(name, application, "value", "the creator application");
    const bool has_digit = std::ranges::any_of(name, [](char c) { return c >= '0' && c <= '9'; });
    // A letter of ASCII, or a byte of a character beyond it.
    const bool has_name = std::ranges::any_of(name, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || static_cast<unsigned char>(c) >= 0x80U;
    });
    if (!has_digit || !has_name) {
        fail(errc::invalid_metadata,
             "XISF:CreatorApplication must name the application and its version (spec §11.4.1), and " + quote(name) +
                 " has no name or no version number",
             application, "value");
    }
    if (options.creation_time && !is_valid_time_point(*options.creation_time)) {
        fail(errc::invalid_time_point, "the creation time is not a valid date and time from year 0 to 9999",
             element + "/Property[@id='XISF:CreationTime']", "value");
    }

    std::size_t position = 0;
    for (const property& item : metadata) {
        ++position;
        if (is_generated_metadata(item.id)) {
            continue;
        }
        if (!is_metadata_id(item.id)) {
            fail(errc::invalid_metadata, "the property " + quote(item.id) + " of the unit is not in the XISF namespace",
                 element + "/" + id_step("Property", item.id, position), "id");
        }
        check_property(item, position, element, false);
    }
}

void check_geometry(const geometry& size, color_space space, const std::string& element)
{
    if (size.dimensions.empty() || std::ranges::find(size.dimensions, std::uint64_t{0}) != size.dimensions.end() ||
        size.channels == 0) {
        fail(errc::invalid_geometry, "the geometry needs one dimension or more, and no length or channel count of 0",
             element, "geometry");
    }
    if (size.channels < nominal_channels(space)) {
        fail(errc::invalid_image,
             "the image has " + std::to_string(size.channels) + " channels, fewer than " +
                 std::to_string(nominal_channels(space)) + " of the " + std::string(color_space_name(space)) +
                 " colour space (spec §8.5.1)",
             element, "geometry");
    }
}

void check_keywords(const std::vector<fits_keyword>& keywords, const std::string& parent)
{
    for (std::size_t i = 0; i < keywords.size(); ++i) {
        const fits_keyword& keyword = keywords[i];
        const std::string element = parent + "/FITSKeyword[" + std::to_string(i + 1) + "]";
        // FITS has a blank keyword, whose name is empty without its padding, but the XML schema of XISF requires one to
        // eight characters, so the writer has no blank keyword.
        if (keyword.name.empty() || !is_fits_keyword_name(keyword.name)) {
            fail(errc::invalid_fits_keyword,
                 quote(keyword.name) +
                     " is not a FITS keyword name: one to eight upper-case letters, digits, hyphens and underscores, "
                     "without padding (spec §11.6.1)",
                 element, "name");
        }
        if (is_commentary_keyword(keyword.name) && !keyword.value.empty()) {
            fail(errc::invalid_fits_keyword, "a " + keyword.name + " keyword has no value (spec §11.6.1)", element,
                 "value");
        }
        if (!is_fits_keyword_text(keyword.value)) {
            fail(errc::invalid_fits_keyword, "the value of a FITS keyword is printable ASCII text (FITS 4.0 §4.1.1)",
                 element, "value");
        }
        if (!is_fits_keyword_text(keyword.comment)) {
            fail(errc::invalid_fits_keyword, "the comment of a FITS keyword is printable ASCII text (FITS 4.0 §4.1.1)",
                 element, "comment");
        }
    }
}

void check_icc_profile(const std::vector<std::byte>& profile, const std::string& parent)
{
    if (!profile.empty() && !has_icc_profile_header(profile)) {
        fail(errc::invalid_icc_profile,
             "the ICC profile does not start with the header of an ICC profile, 128 bytes with the signature 'acsp'",
             parent + "/ICCProfile");
    }
}

void check_working_space(const std::optional<rgb_working_space>& space, const std::string& parent)
{
    if (!space) {
        return;
    }
    const std::string element = parent + "/RGBWorkingSpace";
    if (space->gamma && (!std::isfinite(*space->gamma) || *space->gamma <= 0.0)) {
        fail(errc::invalid_rgb_working_space, "the gamma is not a finite value above zero (spec §11.8.1)", element,
             "gamma");
    }
    std::array<double, 3> derived{};
    try {
        derived = check_rgb_working_space(*space);
    } catch (const invalid_data_error& failure) {
        fail(errc::invalid_rgb_working_space, failure.what(), element);
    }
    for (std::size_t i = 0; i < 3; ++i) {
        if (std::abs(space->luminance[i] - derived[i]) > luminance_tolerance) {
            fail(errc::invalid_rgb_working_space,
                 "the luminance coefficients are not those of the chromaticities of the primaries and the D50 "
                 "reference white (spec §8.5.4.1)",
                 element, "Y");
        }
    }
    // The coefficients that the chromaticities give are written.
    if (!derived_luminance_in_range(derived)) {
        fail(errc::invalid_rgb_working_space,
             "the luminance coefficients that the chromaticities give, which the writer writes, are not in [0, 1] "
             "(spec §8.5.4.1)",
             element, "Y");
    }
    check_text(space->name, element, "name", "the name");
}

void check_display(const std::optional<display_function>& function, const std::string& parent)
{
    if (!function) {
        return;
    }
    const std::string element = parent + "/DisplayFunction";
    for (const std::array<double, 4>* values : {&function->midtones, &function->shadows, &function->highlights,
                                                &function->shadows_expansion, &function->highlights_expansion}) {
        if (!std::ranges::all_of(*values, [](double value) { return std::isfinite(value); })) {
            fail(errc::invalid_display_function, "the parameters of a display function are finite values", element);
        }
    }
    try {
        check_display_function(*function);
    } catch (const invalid_data_error& failure) {
        fail(errc::invalid_display_function, failure.what(), element);
    }
    check_text(function->name, element, "name", "the name");
}

void check_resolution(const std::optional<resolution>& value, const std::string& parent)
{
    if (!value) {
        return;
    }
    const std::string element = parent + "/Resolution";
    if (!std::isfinite(value->horizontal) || value->horizontal <= 0.0 || !std::isfinite(value->vertical) ||
        value->vertical <= 0.0) {
        fail(errc::invalid_resolution, "the resolution is not finite and above zero (spec §11.11.1)", element);
    }
    if (value->unit != resolution_unit::inch && value->unit != resolution_unit::centimeter) {
        fail(errc::invalid_resolution, "the unit of the resolution is neither inch nor cm", element, "unit");
    }
}

// What images and thumbnails have in common: the attributes of their elements, besides bounds, and the elements of
// spec §11.1 to §11.11 that they contain.
template <typename Image> void check_common(const Image& image, const std::string& element)
{
    if (sample_format_name(image.sample_format).empty()) {
        fail(errc::unsupported_sample_format, "the sample format is none of the specification", element,
             "sampleFormat");
    }
    if (color_space_name(image.color_space).empty()) {
        fail(errc::unsupported_color_space, "the colour space is none of the specification", element, "colorSpace");
    }
    check_geometry(image.geometry, image.color_space, element);
    if (!pixel_data_size(image.geometry, image.sample_format)) {
        fail(errc::invalid_geometry, "the pixel data of the image would be larger than 2^64 bytes", element,
             "geometry");
    }
    if (image.pixel_storage != pixel_storage::planar && image.pixel_storage != pixel_storage::normal) {
        fail(errc::invalid_image, "the storage model is neither planar nor normal", element, "pixelStorage");
    }
    if (image.image_type && image_type_name(*image.image_type).empty()) {
        fail(errc::invalid_image, "the image type is none of the specification", element, "imageType");
    }
    if (!std::isfinite(image.offset) || image.offset < 0.0) {
        fail(errc::invalid_image, "the offset is not a finite value of at least zero (spec §11.5.2)", element,
             "offset");
    }
    if (orientation_name(image.orientation).empty()) {
        fail(errc::invalid_image, "the orientation is none of the specification", element, "orientation");
    }
    if (!image.id.empty() && !is_unique_element_id(image.id)) {
        fail(errc::invalid_image_id, quote(image.id) + " is not an image identifier: [_a-zA-Z][_a-zA-Z0-9]*", element,
             "id");
    }
    if (!image.uuid.empty()) {
        bool valid = false;
        try {
            valid = is_version_4_uuid(parse_uuid(image.uuid));
        } catch (const invalid_data_error&) {
            valid = false;
        }
        if (!valid) {
            fail(errc::invalid_uuid, quote(image.uuid) + " is not a version 4 UUID in canonical form (spec §11.5.2)",
                 element, "uuid");
        }
    }
    check_properties(image.properties, image.tables, element);
    check_image_values(image.properties, element);
    check_keywords(image.fits_keywords, element);
    check_icc_profile(image.icc_profile, element);
    check_working_space(image.rgb_working_space, element);
    check_display(image.display_function, element);
    check_resolution(image.resolution, element);
}

void check_thumbnail(const thumbnail& image, const std::string& parent)
{
    const std::string element = parent + "/Thumbnail";
    check_common(image, element);
    const bool restricted =
        image.geometry.dimensions.size() == 2 &&
        (image.sample_format == sample_format::uint8 || image.sample_format == sample_format::uint16) &&
        (image.color_space == color_space::gray || image.color_space == color_space::rgb) &&
        image.geometry.channels <= nominal_channels(image.color_space) + 1;
    if (!restricted) {
        fail(errc::invalid_thumbnail,
             "a thumbnail is a two-dimensional Gray or RGB image of UInt8 or UInt16 samples with at most one alpha "
             "channel (spec §11.12)",
             element);
    }
    const std::optional<std::uint64_t> size = pixel_data_size(image.geometry, image.sample_format);
    if (image.pixels.size() != size) {
        fail(errc::pixel_data_size_mismatch,
             "the thumbnail has " + std::to_string(image.pixels.size()) + " bytes of pixel data, and its geometry " +
                 std::to_string(size.value_or(0)),
             element);
    }
}

void check_image(const image_info& image, const std::string& element)
{
    check_common(image, element);
    const bool floating =
        image.sample_format == sample_format::float32 || image.sample_format == sample_format::float64;
    if (image.bounds) {
        const bounds& range = *image.bounds;
        if (!std::isfinite(range.lower) || !std::isfinite(range.upper) || range.lower >= range.upper) {
            fail(errc::invalid_image, "the bounds are not two finite values in increasing order", element, "bounds");
        }
    } else if (floating) {
        fail(errc::invalid_image, "a floating point image needs bounds (spec §11.5.1)", element, "bounds");
    }
    if (image.color_filter_array) {
        const std::string cfa = element + "/ColorFilterArray";
        if (image.geometry.dimensions.size() != 2) {
            fail(errc::invalid_color_filter_array,
                 "only a two-dimensional image has a colour filter array (spec §11.10)", cfa);
        }
        try {
            check_color_filter_array(*image.color_filter_array);
        } catch (const invalid_data_error& failure) {
            fail(errc::invalid_color_filter_array, failure.what(), cfa, "pattern");
        }
        check_text(image.color_filter_array->name, cfa, "name", "the name");
    }
    if (image.thumbnail) {
        check_thumbnail(*image.thumbnail, element);
    }
}

} // namespace

void validate_unit(const unit_contents& unit, const write_options& options)
{
    check_metadata(unit.metadata, options);
    check_properties(unit.properties, unit.tables, "/xisf");
    std::unordered_set<std::string_view> ids;
    for (std::size_t i = 0; i < unit.images.size(); ++i) {
        const image_info& image = unit.images[i];
        const std::string element = "/xisf/Image[" + std::to_string(i + 1) + "]";
        check_image(image, element);
        if (!image.id.empty() && !ids.insert(image.id).second) {
            fail(errc::duplicate_image_id, "another image has the identifier " + image.id + " (spec §11.5.2)", element,
                 "id");
        }
    }
}

} // namespace openxisf::detail
