// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/value_reader.h"

#include <openxisf/error.h>

#include "core/text_grammar.h"
#include "model/property_data.h"
#include "model/property_text.h"
#include "model/property_types.h"
#include "xml/xml_document.h"

#include <limits>

namespace openxisf::detail {

namespace {

std::string name_of_type(const value_element& element)
{
    return std::string(property_type_name(element.type));
}

} // namespace

std::optional<property_value> value_reader::read(const value_element& element)
{
    const type_category category = category_of(element.type);
    if (category == type_category::scalar || category == type_category::complex ||
        category == type_category::time_point) {
        return read_value_attribute(element);
    }
    if (element.block != nullptr && !load_blocks_) {
        return std::nullopt;
    }
    switch (category) {
    case type_category::string:
        return read_string(element);
    case type_category::vector:
        return read_vector(element);
    case type_category::matrix:
        return read_matrix(element);
    case type_category::scalar:
    case type_category::complex:
    case type_category::time_point:
        break;
    }
    return std::nullopt;
}

bool value_reader::defers(const value_element& element) const noexcept
{
    const type_category category = category_of(element.type);
    return !load_blocks_ && element.block != nullptr && element.block->descriptor.has_value() &&
           (category == type_category::string || category == type_category::vector ||
            category == type_category::matrix);
}

// Scalars, complex numbers and TimePoints are written in the value attribute (spec §11.1.4, §11.1.5, §11.1.7).
std::optional<property_value> value_reader::read_value_attribute(const value_element& element)
{
    if (!element.node.attribute("location").empty()) {
        log_.warning(errc::invalid_property,
                     "a value of type " + name_of_type(element) +
                         " is written in its value attribute, so its data block is ignored",
                     {.element = element.path, .attribute = "location"});
    }
    const pugi::xml_attribute value = element.node.attribute("value");
    if (value.empty()) {
        log_.error(errc::invalid_property, "a value of type " + name_of_type(element) + " needs a value attribute",
                   {.element = element.path, .attribute = "value"});
        return std::nullopt;
    }
    try {
        return parse_value_attribute(element.type, value.value());
    } catch (const invalid_data_error& failure) {
        log_.error(failure.code(), failure.what(), {.element = element.path, .attribute = "value"});
    }
    return std::nullopt;
}

// Strings, vectors and matrices have no value attribute (spec §11.1.6, §11.1.8, §11.1.9).
void value_reader::ignore_value_attribute(const value_element& element)
{
    if (!element.node.attribute("value").empty()) {
        log_.warning(errc::invalid_property,
                     "a value of type " + name_of_type(element) + " has no value attribute, so it is ignored",
                     {.element = element.path, .attribute = "value"});
    }
}

// In the character data, white space included, or in a data block (spec §11.1.6).
std::optional<property_value> value_reader::read_string(const value_element& element)
{
    ignore_value_attribute(element);
    if (element.block == nullptr) {
        // All of its white space, beside a comment, a processing instruction or a CDATA section too.
        return property_value(character_data(element.node, header_text_));
    }
    if (!element.block->descriptor) {
        return std::nullopt;
    }
    const block_descriptor& descriptor = *element.block->descriptor;
    // The character data of an inline block are the block.
    if (descriptor.location.kind != location_kind::inline_data && !is_xml_white_space(character_data(element.node))) {
        log_.warning(errc::invalid_property,
                     "the String value is in a data block, so the character data of the element are ignored",
                     {.element = element.path});
    }
    const std::optional<std::vector<std::byte>> data = load(element, descriptor);
    if (!data) {
        return std::nullopt;
    }
    try {
        return property_value(decode_string(*data));
    } catch (const invalid_data_error& failure) {
        log_.error(failure.code(), failure.what(), {.element = element.path});
    }
    // The data are not kept, so they leave the limit to the blocks after them, as a block that fails to load does.
    budget_.release(block_cost(descriptor));
    return std::nullopt;
}

// In a data block, with a length attribute (spec §11.1.8).
std::optional<property_value> value_reader::read_vector(const value_element& element)
{
    ignore_value_attribute(element);
    std::optional<std::uint64_t> length;
    if (!read_count(element, "length", length)) {
        return std::nullopt;
    }
    if (element.block == nullptr) {
        if (length.has_value() && *length != 0) {
            log_.error(errc::invalid_property,
                       "a vector of " + std::to_string(*length) + " elements needs a data block",
                       {.element = element.path, .attribute = "length"});
            return std::nullopt;
        }
        log_.warning(errc::invalid_property, "an empty vector is written as an empty inline data block",
                     {.element = element.path});
        return decode_vector(element.type, {}, byte_order::little);
    }
    if (!element.block->descriptor) {
        return std::nullopt;
    }
    const block_descriptor& descriptor = *element.block->descriptor;
    if (length) {
        if (!check_size(element, descriptor, *length, "length")) {
            return std::nullopt;
        }
    } else {
        log_.warning(errc::invalid_property, "the vector has no length attribute",
                     {.element = element.path, .attribute = "length"});
        const std::size_t size = value_size(element_type(element.type));
        if (data_size(descriptor) % size != 0) {
            log_.error(errc::invalid_property_length,
                       "the data block of " + std::to_string(data_size(descriptor)) +
                           " bytes does not hold a whole number of " +
                           std::string(property_type_name(element_type(element.type))) + " elements",
                       {.element = element.path});
            return std::nullopt;
        }
    }
    const std::optional<std::vector<std::byte>> data = load(element, descriptor);
    if (!data) {
        return std::nullopt;
    }
    return decode_vector(element.type, *data, descriptor.order);
}

// In a data block, with rows and columns attributes (spec §11.1.9).
std::optional<property_value> value_reader::read_matrix(const value_element& element)
{
    ignore_value_attribute(element);
    std::optional<std::uint64_t> rows;
    std::optional<std::uint64_t> columns;
    if (!read_count(element, "rows", rows) || !read_count(element, "columns", columns)) {
        return std::nullopt;
    }
    if (!rows || !columns) {
        log_.error(errc::invalid_property, "a matrix value needs rows and columns attributes",
                   {.element = element.path, .attribute = rows.has_value() ? "columns" : "rows"});
        return std::nullopt;
    }
    const bool fits = *rows == 0 || *columns <= std::numeric_limits<std::uint64_t>::max() / *rows;
    if (element.block == nullptr) {
        if (*rows != 0 && *columns != 0) {
            log_.error(errc::invalid_property,
                       "a matrix of " + std::to_string(*rows) + " by " + std::to_string(*columns) +
                           " needs a data block",
                       {.element = element.path});
            return std::nullopt;
        }
        log_.warning(errc::invalid_property, "an empty matrix is written as an empty inline data block",
                     {.element = element.path});
        return decode_matrix(element.type, *rows, *columns, {}, byte_order::little);
    }
    if (!element.block->descriptor) {
        return std::nullopt;
    }
    const block_descriptor& descriptor = *element.block->descriptor;
    if (!fits) {
        log_.error(errc::invalid_property_length,
                   "a matrix of " + std::to_string(*rows) + " by " + std::to_string(*columns) +
                       " has more elements than a data block can hold",
                   {.element = element.path, .attribute = "rows"});
        return std::nullopt;
    }
    if (!check_size(element, descriptor, *rows * *columns, "rows")) {
        return std::nullopt;
    }
    const std::optional<std::vector<std::byte>> data = load(element, descriptor);
    if (!data) {
        return std::nullopt;
    }
    return decode_matrix(element.type, *rows, *columns, *data, descriptor.order);
}

// The unsigned integer of the attribute name in count, left empty when there is no such attribute. False when the
// attribute is malformed, which is recorded.
bool value_reader::read_count(const value_element& element, const char* name, std::optional<std::uint64_t>& count)
{
    const pugi::xml_attribute attribute = element.node.attribute(name);
    if (attribute.empty()) {
        return true;
    }
    try {
        count = parse_integer<std::uint64_t>(attribute.value());
        return true;
    } catch (const invalid_data_error& failure) {
        log_.error(failure.code(), failure.what(), {.element = element.path, .attribute = name});
    }
    return false;
}

// True when count elements fill the data block of the element exactly; recorded when they do not.
bool value_reader::check_size(const value_element& element, const block_descriptor& descriptor, std::uint64_t count,
                              const char* attribute)
{
    const std::uint64_t size = data_size(descriptor);
    const std::optional<std::uint64_t> needed = elements_size(element.type, count);
    if (needed == size) {
        return true;
    }
    const std::string elements =
        std::to_string(count) + " " + std::string(property_type_name(element_type(element.type))) + " elements";
    log_.error(errc::invalid_property_length,
               (needed ? elements + " take " + std::to_string(*needed) + " bytes"
                       : elements + " take more bytes than 64 bits can count") +
                   ", but the data block has " + std::to_string(size),
               {.element = element.path, .attribute = attribute});
    return false;
}

// The data of the block of the element, decompressed, within the budget; nothing when they cannot be read, which is
// recorded. What the source throws passes through.
std::optional<std::vector<std::byte>> value_reader::load(const value_element& element,
                                                         const block_descriptor& descriptor)
{
    return load_block(source_, descriptor, limits_, budget_, element.path, log_);
}

} // namespace openxisf::detail
