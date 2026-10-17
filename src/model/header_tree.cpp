// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/header_tree.h"

#include <openxisf/color.h>
#include <openxisf/error.h>
#include <openxisf/version.h>

#include "core/endian.h"
#include "core/text_grammar.h"
#include "core/uuid.h"
#include "model/ancillary_attributes.h"
#include "model/format_specifier.h"
#include "model/pixel_layout.h"
#include "model/property_data.h"
#include "model/property_text.h"
#include "model/property_types.h"
#include "xml/xml_writer.h"

#include <array>
#include <bit>
#include <concepts>
#include <string_view>

namespace openxisf::detail {

namespace {

// The embedded profile flag of an ICC profile is bit 0 of its flags, which are big-endian at bytes 44 to 47, so the
// least significant bit of byte 47 (ICC.1:2022 §3.1.4, §7.2.11).
constexpr std::size_t icc_flags_last_byte = 47;

constexpr std::string_view indentation = "   ";

std::string format_double(double value)
{
    return format_float(value);
}

template <std::size_t N> std::string format_list(const std::array<double, N>& values)
{
    std::string text;
    for (const double value : values) {
        if (!text.empty()) {
            text += ':';
        }
        text += format_double(value);
    }
    return text;
}

std::string format_geometry(const geometry& size)
{
    std::string text;
    for (const std::uint64_t length : size.dimensions) {
        text += format_integer(length) + ":";
    }
    return text + format_integer(size.channels);
}

class tree_builder
{
public:
    explicit tree_builder(const write_options& options) noexcept : options_(options) {}

    std::vector<block_source> release_blocks() noexcept
    {
        return std::move(blocks_);
    }

    xml_element property_element(const property& item)
    {
        xml_element element{.name = "Property"};
        add(element, "id", item.id);
        add(element, "type", type_name(item.value.type()));
        if (item.format) {
            if (std::string format = format_specifier_text(*item.format); !format.empty()) {
                add(element, "format", std::move(format));
            }
        }
        if (!item.comment.empty()) {
            add(element, "comment", item.comment);
        }
        serialize_value(element, item.value);
        return element;
    }

    xml_element table_element(const table& item)
    {
        xml_element element{.name = "Table"};
        add(element, "id", item.id);
        if (!item.caption.empty()) {
            add(element, "caption", item.caption);
        }
        // Assigned rather than cast: std::size_t is std::uint64_t on some platforms and not on others.
        const std::uint64_t rows = item.rows.size();
        const std::uint64_t columns = item.fields.size();
        add(element, "rows", format_integer(rows));
        add(element, "columns", format_integer(columns));
        if (!item.comment.empty()) {
            add(element, "comment", item.comment);
        }

        xml_element structure{.name = "Structure"};
        for (const table_field& field : item.fields) {
            xml_element field_element{.name = "Field"};
            add(field_element, "id", field.id);
            add(field_element, "type", type_name(field.type));
            if (!field.header.empty()) {
                add(field_element, "header", field.header);
            }
            if (field.format) {
                if (std::string format = format_specifier_text(*field.format); !format.empty()) {
                    add(field_element, "format", std::move(format));
                }
            }
            structure.children.push_back(std::move(field_element));
        }
        element.children.push_back(std::move(structure));

        for (const std::vector<property_value>& row : item.rows) {
            xml_element row_element{.name = "Row"};
            for (const property_value& cell : row) {
                xml_element cell_element{.name = "Cell"};
                serialize_value(cell_element, cell);
                row_element.children.push_back(std::move(cell_element));
            }
            element.children.push_back(std::move(row_element));
        }
        return element;
    }

    xml_element image_element(const image_info& image, std::span<const std::byte> pixels, const std::string& uuid)
    {
        xml_element element{.name = "Image"};
        add_image_attributes(element, image, uuid);
        add_pixel_block(element, pixels, image.sample_format);
        add_described_children(element, image);
        if (image.color_filter_array) {
            const color_filter_array& filter = *image.color_filter_array;
            xml_element cfa{.name = "ColorFilterArray"};
            add(cfa, "pattern", filter.pattern);
            add(cfa, "width", format_integer(filter.width));
            add(cfa, "height", format_integer(filter.height));
            if (!filter.name.empty()) {
                add(cfa, "name", filter.name);
            }
            element.children.push_back(std::move(cfa));
        }
        if (image.thumbnail) {
            const thumbnail& small = *image.thumbnail;
            xml_element child{.name = "Thumbnail"};
            add_image_attributes(child, small,
                                 small.uuid.empty() ? std::string() : format_uuid(parse_uuid(small.uuid)));
            add_pixel_block(child, small.pixels, small.sample_format);
            add_described_children(child, small);
            element.children.push_back(std::move(child));
        }
        return element;
    }

private:
    static void add(xml_element& element, std::string_view name, std::string value)
    {
        element.attributes.emplace_back(std::string(name), std::move(value));
    }

    static void add(xml_element& element, std::string_view name, std::string_view value)
    {
        add(element, name, std::string(value));
    }

    // A block of a Property, Cell or ICCProfile element, which can be inline (spec §10.3). An empty block, the value of
    // an empty vector or matrix, must be (spec §11.1.8).
    void add_ancillary_block(xml_element& element, std::vector<std::byte> data, std::size_t item_size)
    {
        const bool small = data.size() <= options_.max_inline_block_size;
        element.block = blocks_.size();
        blocks_.push_back({.owned = std::move(data), .item_size = item_size, .inline_data = small});
    }

    // Pixel data, which are attached: an Image element cannot have an inline block (spec §11.5), and a baseline encoder
    // attaches them (spec §7.1). They are borrowed, and on a big-endian host, where samples of more than one byte are
    // swapped, marked for the copy of to_little_endian().
    void add_pixel_block(xml_element& element, std::span<const std::byte> pixels, sample_format format)
    {
        const std::size_t item_size = byte_order_item_size(format);
        element.block = blocks_.size();
        blocks_.push_back({.borrowed = pixels,
                           .item_size = item_size,
                           .native_order = std::endian::native == std::endian::big && item_size > 1});
    }

    void serialize_value(xml_element& element, const property_value& value)
    {
        switch (category_of(value.type())) {
        case type_category::scalar:
        case type_category::complex:
        case type_category::time_point:
            add(element, "value", format_value_attribute(value));
            break;
        case type_category::string:
            serialize_string(element, value.get<std::string>());
            break;
        case type_category::vector:
            add(element, "length", format_integer(value.length()));
            add_ancillary_block(element, encode_elements(value, byte_order::little),
                                byte_order_item_size(element_type(value.type())));
            break;
        case type_category::matrix:
            add(element, "rows", format_integer(value.rows()));
            add(element, "columns", format_integer(value.columns()));
            add_ancillary_block(element, encode_elements(value, byte_order::little),
                                byte_order_item_size(element_type(value.type())));
            break;
        }
    }

    // As character data when XML can hold it and it is not too long for the header, and in a data block otherwise
    // (spec §11.1.6).
    void serialize_string(xml_element& element, const std::string& text)
    {
        if (text.size() <= max_character_data && is_xml_text(text)) {
            element.text = text;
            return;
        }
        std::vector<std::byte> data(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            data[i] = static_cast<std::byte>(text[i]);
        }
        add_ancillary_block(element, std::move(data), 1);
    }

    // Spec §11.5.1: bounds equal to the default range [0, 2^k - 1] of an unsigned integer image should not be
    // specified, so that a model read from another encoder that wrote them reads back without them.
    static bool is_default_range(const image_info& image)
    {
        image_info plain;
        plain.sample_format = image.sample_format;
        return image.bounds == plain.representable_range();
    }

    template <typename Image>
    static void add_image_attributes(xml_element& element, const Image& image, const std::string& uuid)
    {
        add(element, "geometry", format_geometry(image.geometry));
        add(element, "sampleFormat", sample_format_name(image.sample_format));
        if constexpr (std::same_as<Image, image_info>) {
            if (image.bounds && !is_default_range(image)) {
                add(element, "bounds", format_double(image.bounds->lower) + ":" + format_double(image.bounds->upper));
            }
        }
        add(element, "colorSpace", color_space_name(image.color_space));
        if (image.pixel_storage != pixel_storage::planar) {
            add(element, "pixelStorage", pixel_storage_name(image.pixel_storage));
        }
        if (image.image_type) {
            add(element, "imageType", image_type_name(*image.image_type));
        }
        if (image.offset != 0.0) {
            add(element, "offset", format_double(image.offset));
        }
        if (image.orientation != orientation::none) {
            add(element, "orientation", orientation_name(image.orientation));
        }
        if (!image.id.empty()) {
            add(element, "id", image.id);
        }
        if (!uuid.empty()) {
            add(element, "uuid", uuid);
        }
    }

    // The children that images and thumbnails share: properties, tables and the elements of spec §11.6 to §11.9 and
    // §11.11.
    template <typename Image> void add_described_children(xml_element& element, const Image& image)
    {
        for (const property& item : image.properties) {
            element.children.push_back(property_element(item));
        }
        for (const table& item : image.tables) {
            element.children.push_back(table_element(item));
        }
        for (const fits_keyword& keyword : image.fits_keywords) {
            xml_element child{.name = "FITSKeyword"};
            add(child, "name", keyword.name);
            add(child, "value", keyword.value);
            add(child, "comment", keyword.comment);
            element.children.push_back(std::move(child));
        }
        if (!image.icc_profile.empty()) {
            // Unaltered, but for the embedded profile flag (spec §11.7).
            std::vector<std::byte> profile = image.icc_profile;
            profile[icc_flags_last_byte] |= std::byte{0x01};
            xml_element child{.name = "ICCProfile"};
            add_ancillary_block(child, std::move(profile), 1);
            element.children.push_back(std::move(child));
        }
        if (image.rgb_working_space) {
            const rgb_working_space& space = *image.rgb_working_space;
            xml_element child{.name = "RGBWorkingSpace"};
            add(child, "x", format_list(space.x));
            add(child, "y", format_list(space.y));
            // The coefficients that the chromaticities give, which an encoder computes (spec §11.8.1).
            const std::optional<std::array<double, 3>> luminance = luminance_coefficients(space.x, space.y);
            add(child, "Y", format_list(luminance.value_or(space.luminance)));
            add(child, "gamma", space.gamma ? format_double(*space.gamma) : std::string("sRGB"));
            if (!space.name.empty()) {
                add(child, "name", space.name);
            }
            element.children.push_back(std::move(child));
        }
        if (image.display_function) {
            const display_function& function = *image.display_function;
            xml_element child{.name = "DisplayFunction"};
            add(child, "m", format_list(function.midtones));
            add(child, "s", format_list(function.shadows));
            add(child, "h", format_list(function.highlights));
            add(child, "l", format_list(function.shadows_expansion));
            add(child, "r", format_list(function.highlights_expansion));
            if (!function.name.empty()) {
                add(child, "name", function.name);
            }
            element.children.push_back(std::move(child));
        }
        if (image.resolution) {
            xml_element child{.name = "Resolution"};
            add(child, "horizontal", format_double(image.resolution->horizontal));
            add(child, "vertical", format_double(image.resolution->vertical));
            add(child, "unit", resolution_unit_name(image.resolution->unit));
            element.children.push_back(std::move(child));
        }
    }

    const write_options& options_;
    std::vector<block_source> blocks_;
};

void append_indentation(std::string& output, std::size_t depth)
{
    for (std::size_t i = 0; i < depth; ++i) {
        output += indentation;
    }
}

void append_attribute(std::string& output, std::string_view name, std::string_view value)
{
    output += ' ';
    output += name;
    output += '=';
    append_attribute_value(output, value);
}

// The location of a block that is not inline: attached, or in the data blocks file of a distributed unit.
std::string location_of(const block_header& block, std::string_view blocks_file)
{
    if (blocks_file.empty()) {
        return format_attachment(block.position, block.size);
    }
    return format_relative_location(blocks_file, block.index_id);
}

void append_element(std::string& output, const xml_element& element, std::span<const block_header> blocks,
                    std::string_view blocks_file, std::size_t depth)
{
    append_indentation(output, depth);
    output += '<';
    output += element.name;
    for (const auto& [name, value] : element.attributes) {
        append_attribute(output, name, value);
    }
    std::string_view text = element.text;
    if (element.block) {
        const block_header& block = blocks[*element.block];
        append_attribute(output, "location", block.inline_data ? "inline:base64" : location_of(block, blocks_file));
        if (block.compression) {
            append_attribute(output, "compression", format_compression(*block.compression));
            if (!block.compression->subblocks.empty()) {
                append_attribute(output, "subblocks", format_subblocks(block.compression->subblocks));
            }
        }
        if (block.checksum) {
            append_attribute(output, "checksum", format_checksum(*block.checksum));
        }
        if (block.inline_data) {
            text = block.text;
        }
    }
    if (element.children.empty() && text.empty()) {
        output += "/>\n";
        return;
    }
    output += '>';
    if (element.children.empty()) {
        append_character_data(output, text);
    } else {
        output += '\n';
        for (const xml_element& child : element.children) {
            append_element(output, child, blocks, blocks_file, depth + 1);
        }
        append_indentation(output, depth);
    }
    output += "</";
    output += element.name;
    output += ">\n";
}

} // namespace

void to_little_endian(block_source& block)
{
    if (!block.native_order) {
        return;
    }
    const std::span<const std::byte> native = block.data();
    std::vector<std::byte> copy(native.begin(), native.end());
    swap_byte_order(copy, block.item_size);
    block.owned = std::move(copy);
    block.borrowed.reset();
    block.native_order = false;
}

header_tree build_header_tree(const unit_contents& unit, const write_options& options,
                              std::span<const std::string> uuids)
{
    tree_builder builder(options);
    header_tree tree;
    for (const property& item : unit.metadata) {
        if (!is_generated_metadata(item.id)) {
            tree.metadata.push_back(builder.property_element(item));
        }
    }
    for (std::size_t i = 0; i < unit.images.size(); ++i) {
        tree.body.push_back(builder.image_element(unit.images[i], unit.pixels[i], uuids[i]));
    }
    for (const property& item : unit.properties) {
        tree.body.push_back(builder.property_element(item));
    }
    for (const table& item : unit.tables) {
        tree.body.push_back(builder.table_element(item));
    }
    tree.blocks = builder.release_blocks();
    return tree;
}

std::vector<xml_element> property_elements(std::span<const property> properties)
{
    const write_options options{};
    tree_builder builder(options);
    std::vector<xml_element> elements;
    elements.reserve(properties.size());
    for (const property& item : properties) {
        xml_element element = builder.property_element(item);
        // The index of a block of this builder would name a block of the unit, or none.
        if (element.block) {
            throw validation_error(errc::invalid_metadata,
                                   "the value of the property " + item.id +
                                       " needs a data block, which the metadata that the writer generates cannot have",
                                   {.element = "/xisf/Metadata/Property[@id='" + item.id + "']", .attribute = "value"});
        }
        elements.push_back(std::move(element));
    }
    return elements;
}

std::string format_header(const header_tree& tree, std::span<const xml_element> generated,
                          std::span<const block_header> blocks, std::string_view blocks_file)
{
    std::string text =
        R"(<?xml version="1.0" encoding="UTF-8"?>)"
        "\n<!--\nExtensible Image Serialization Format - XISF version 1.0\nCreated with "
        "OpenXISF " OPENXISF_VERSION_STRING " - https://github.com/openxisf/openxisf\n-->\n"
        R"(<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf")"
        R"( xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance")"
        R"( xsi:schemaLocation="http://www.pixinsight.com/xisf http://pixinsight.com/xisf/xisf-1.0.xsd">)"
        "\n";
    append_indentation(text, 1);
    text += "<Metadata>\n";
    for (const xml_element& element : generated) {
        append_element(text, element, blocks, blocks_file, 2);
    }
    for (const xml_element& element : tree.metadata) {
        append_element(text, element, blocks, blocks_file, 2);
    }
    append_indentation(text, 1);
    text += "</Metadata>\n";
    for (const xml_element& element : tree.body) {
        append_element(text, element, blocks, blocks_file, 1);
    }
    text += "</xisf>";
    return text;
}

} // namespace openxisf::detail
