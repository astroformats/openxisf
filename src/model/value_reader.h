// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/limits.h>
#include <openxisf/property.h>

#include "container/data_block.h"
#include "core/diagnostic_log.h"
#include "io/thread_safe_source.h"
#include "model/ancillary_budget.h"

#include <pugixml.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The values of Property elements and of the Cell elements of tables, which serialize a value the same way (spec §11.1,
// §11.3.3).

namespace openxisf::detail {

/// An element that serializes a property value.
struct value_element
{
    pugi::xml_node node{};
    std::string path{};
    /// The data block of the element, when it has a location attribute.
    const data_block* block = nullptr;
    property_type type = property_type::string;
};

/// Reads values as their type serializes them (spec §11.1.4 to §11.1.9): scalars, complex numbers and TimePoints in a
/// value attribute; Strings as character data or in a data block; vectors and matrices in a data block, whose size must
/// agree with the length, rows and columns attributes. Data blocks are read from the source and decompressed, each
/// counted against the budget by the larger of its stored and its data size.
class value_reader
{
public:
    /// With load_blocks false, a value in a data block is not read: read() returns nothing for it, without a
    /// diagnostic. header_text is the text that the elements were parsed from (parsed_header::text), which gives the
    /// character data of a String around a comment, a processing instruction or a CDATA section (character_data()).
    value_reader(const thread_safe_source& source, const limits& limits, ancillary_budget& budget, bool load_blocks,
                 std::string_view header_text, diagnostic_log& log) noexcept
        : source_(source), limits_(limits), budget_(budget), load_blocks_(load_blocks), header_text_(header_text),
          log_(log)
    {}

    /// The value of element, or nothing when it cannot be read. Every problem is recorded in log:
    /// - a value that its type cannot have, a value attribute missing where the type needs one, a vector or matrix
    ///   without a data block or whose length, rows or columns disagree with the size of its block, a String block that
    ///   is not UTF-8, a block that fails its checksum or does not decompress, and a block beyond the budget are
    ///   errors;
    /// - a value attribute of a String, vector or matrix, a data block of a scalar, complex number or TimePoint, and
    ///   character data beside the data block of a String other than an inline one, each ignored, a vector without a
    ///   length attribute, and an empty vector or matrix without a data block are warnings. Other attributes that a
    ///   type does not use, such as a length attribute of a scalar, are ignored without one.
    ///
    /// What the source throws passes through.
    [[nodiscard]] std::optional<property_value> read(const value_element& element);

    /// True when read() leaves the value of element in its data block, which is available: the value is of a type that
    /// a data block can hold, and the reader does not load blocks. Its identifier is then known, but not its value.
    [[nodiscard]] bool defers(const value_element& element) const noexcept;

private:
    std::optional<property_value> read_value_attribute(const value_element& element);
    void ignore_value_attribute(const value_element& element);
    std::optional<property_value> read_string(const value_element& element);
    std::optional<property_value> read_vector(const value_element& element);
    std::optional<property_value> read_matrix(const value_element& element);
    bool read_count(const value_element& element, const char* name, std::optional<std::uint64_t>& count);
    bool check_size(const value_element& element, const block_descriptor& descriptor, std::uint64_t count,
                    const char* attribute);
    std::optional<std::vector<std::byte>> load(const value_element& element, const block_descriptor& descriptor);

    const thread_safe_source& source_;
    const limits& limits_;
    ancillary_budget& budget_;
    bool load_blocks_;
    std::string_view header_text_;
    diagnostic_log& log_;
};

} // namespace openxisf::detail
