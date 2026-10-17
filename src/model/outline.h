// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include "core/diagnostic_log.h"

#include <pugixml.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The outline of a unit: the elements of the specification in its header, which the reader builds the unit from.

namespace openxisf::detail {

/// The XML elements that the specification defines below the root element (spec §11).
enum class element_kind : std::uint8_t
{
    // Core elements.
    metadata,
    image,
    thumbnail,
    property,
    table,
    structure,
    fits_keyword,
    icc_profile,
    rgb_working_space,
    display_function,
    color_filter_array,
    resolution,
    reference,
    // Parts of core elements.
    field,
    row,
    cell,
    data,
};

/// The name of the elements of a kind, such as "FITSKeyword".
[[nodiscard]] std::string_view element_name(element_kind kind) noexcept;

/// True for the core elements of spec §11, which can have a uid. Field, Row, Cell and Data are parts of core elements.
[[nodiscard]] bool is_core_element(element_kind kind) noexcept;

/// True when id is a unique element identifier: [_a-zA-Z][_a-zA-Z0-9]* (spec §11).
[[nodiscard]] bool is_unique_element_id(std::string_view id) noexcept;

/// The index of no element: the parent of the children of the root element, and the target of a Reference that names
/// nothing.
inline constexpr std::size_t no_element = static_cast<std::size_t>(-1);

/// An element of the outline.
struct outline_element
{
    element_kind kind{};
    pugi::xml_node node{};
    /// The element that contains this one, or no_element for a child of the root element.
    std::size_t parent = no_element;
    /// The position among the sibling elements of the same name, from 1, as in the path.
    std::size_t position = 0;
    /// One past the last descendant, so that the descendants are the elements after this one, up to end.
    std::size_t end = 0;
    /// For a Reference, the element that it names, or no_element when it names none that it can.
    std::size_t target = no_element;
    /// The path of the element once it is known to contain other elements, whose paths are built from it; empty until
    /// then.
    std::string path{};
};

/// The elements that the specification defines in a header, in document order. An element that the specification does
/// not define where it appears is left out, with everything it contains.
struct unit_outline
{
    pugi::xml_node root{};
    /// The path of the root element: /xisf.
    std::string root_path{};
    std::vector<outline_element> elements{};

    /// The path of an element, which diagnostics name, or that of the root element for no_element:
    /// /xisf/Image[1]/Property[2]. Its steps are the local names of the elements, which a prefix of the XISF namespace,
    /// the only one entered, would only make longer, each with its position among the elements of that local name. It
    /// is built from the path of the parent element, which is computed once.
    [[nodiscard]] std::string path(std::size_t index) const;
};

/// Walks the elements below an XISF root element and resolves the Reference elements (spec §11, §11.13). Every problem
/// is recorded in log:
/// - an element in another namespace, at the top of the header, is an extension, and an info;
/// - any other element that the specification does not define where it appears is a warning;
/// - a uid that is not a unique element identifier, or is on a Reference element, is a warning;
/// - a uid of several core elements is an error, and so is a Reference without a ref attribute, or one that names no
///   element (errc::dangling_reference), a uid of several elements (errc::duplicate_uid) or another Reference
///   (errc::chained_reference).
[[nodiscard]] unit_outline build_outline(const pugi::xml_node& root, diagnostic_log& log);

} // namespace openxisf::detail
