// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/limits.h>

#include "core/diagnostic_log.h"

#include <pugixml.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace openxisf::detail {

/// The detached XML signature of a header and the root element that it signs, both exactly as written (spec §9.5).
struct header_signature
{
    /// The Signature element.
    std::string element{};
    /// The XISF root element.
    std::string signed_element{};
};

/// An XISF header, parsed and checked down to its root element (spec §9.5).
struct parsed_header
{
    std::unique_ptr<pugi::xml_document> document{};
    /// The XISF root element.
    pugi::xml_node root{};
    /// The detached XML signature that follows the root element. Empty when the unit is not signed.
    std::optional<header_signature> signature{};
    /// The text that the document was parsed from, at the offsets that pugixml gives: the text given to parse_header(),
    /// without a byte order mark, which the caller keeps for as long as it uses this view.
    std::string_view text{};
};

/// Parses the header of a unit, which starts at offset in its source, and checks its XML declaration, its root element
/// and what follows the root element. A UTF-8 byte order mark at the start is skipped.
///
/// The signature is the first Signature element of the XML signature namespace after the root element. It is read, not
/// verified: a signature that is not well-formed XML, that is not structured as an XML signature, or that does not name
/// the root element by its id attribute, is a warning. When the header is not well-formed XML only after its root
/// element, the part that is, up to the last element before the problem, is parsed on its own, as a decoder that
/// isolates the root element would (spec §9.5): the element that holds the problem is a warning, or the signature, kept
/// as far as the nesting of its tags goes, and nothing after it is read.
///
/// Throws what parse_xml() throws for the part of the header that it parses, the root element and what precedes it
/// included; invalid_data_error when the declaration claims another encoding than UTF-8 for a header beyond ASCII
/// (errc::invalid_xml), or when there is no XISF root element or more than one
/// (errc::invalid_root_element); and unsupported_error when the root element is of another version than 1.0
/// (errc::unsupported_version). Tolerated problems are recorded in log.
[[nodiscard]] parsed_header parse_header(std::string_view text, std::uint64_t offset, const limits& limits,
                                         diagnostic_log& log);

} // namespace openxisf::detail
