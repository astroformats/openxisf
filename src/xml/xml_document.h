// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/limits.h>

#include <pugixml.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

// XML 1.0 from untrusted input, through pugixml. pugixml does not check every well-formedness rule; what it leaves out
// and matters to XISF is checked here.

namespace openxisf::detail {

/// True for the white space of XML 1.0: space, tab, carriage return and line feed.
[[nodiscard]] constexpr bool is_xml_white_space(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/// True when text is empty or holds only XML white space.
[[nodiscard]] bool is_xml_white_space(std::string_view text) noexcept;

/// Parses text, which is untrusted, as an XML document. offset is where the text starts in its source, for the context
/// of errors.
///
/// The document may have several elements at its top level, as a signed XISF header has (spec §9.5). Character data
/// that is only white space is kept when it is the only content of its element, where it can be the value of a String
/// property (spec §11.1.6), and dropped elsewhere. The predefined entities and character references are replaced; any
/// other entity reference is left as written, since entities cannot be declared.
///
/// Throws invalid_data_error when the text is not valid UTF-8 (errc::invalid_utf8), is not well-formed XML, refers to
/// a character that XML does not allow, such as U+0000, or repeats an attribute of an element (errc::invalid_xml), or
/// has a document type declaration (errc::doctype_not_allowed).
/// Throws limit_error when its elements are nested deeper than limits.max_xml_depth (errc::xml_too_deep) or are more
/// than limits.max_xml_elements (errc::too_many_xml_elements). The limits count every element of the document, and
/// are checked on the tags of the text before the document is built, so that a header beyond them costs no memory;
/// text that is not well-formed fails with a limit when its tags are beyond one.
[[nodiscard]] std::unique_ptr<pugi::xml_document> parse_xml(std::string_view text, std::uint64_t offset,
                                                            const limits& limits);

/// A character of a text that XML 1.0 does not allow: where it is, and its code point.
struct restricted_character
{
    std::size_t offset = 0;
    std::uint32_t code_point = 0;
};

/// The first character of text that XML 1.0 does not allow (Legal Character) and that parse_xml() reads all the same:
/// a control character other than tab, line feed and carriage return, U+FFFE or U+FFFF, written as it is anywhere, or
/// as a character reference where pugixml reads one. Empty when there is none.
[[nodiscard]] std::optional<restricted_character> first_restricted_character(std::string_view text);

/// Where the first character data outside the elements of text is: text other than white space before, between or after
/// its top-level elements, comments and processing instructions, or a CDATA section there, which XML does not allow and
/// pugixml accepts. Empty when there is none. text is a document that parse_xml() accepted.
[[nodiscard]] std::optional<std::size_t> first_text_outside_elements(std::string_view text);

/// The character data of element: the text of its character data and CDATA children, in order.
[[nodiscard]] std::string character_data(const pugi::xml_node& element);

/// The character data of element as XML 1.0 defines them, where pugixml loses white space: taken from text, which its
/// document was parsed from, when the content of element holds a comment, a processing instruction or a CDATA
/// section, with references replaced as parse_xml() replaces them, CDATA sections as they are, comments and processing
/// instructions left out, line ends normalized and the content of child elements skipped; otherwise
/// character_data(element), the same text.
[[nodiscard]] std::string character_data(const pugi::xml_node& element, std::string_view text);

/// Where node starts in the text it was parsed from: the '<' of an element. Empty when pugixml cannot tell.
[[nodiscard]] std::optional<std::size_t> element_offset(const pugi::xml_node& element) noexcept;

/// Past the first terminator at or after position from of text, or the end of text when there is none.
[[nodiscard]] std::size_t past(std::string_view text, std::size_t from, std::string_view terminator) noexcept;

/// Where the tag that starts at text[start], a '<', ends: its first '>' outside quoted attribute values. Empty when it
/// does not end.
[[nodiscard]] std::optional<std::size_t> tag_end(std::string_view text, std::size_t start) noexcept;

/// The length of the element that starts at text[start], from its '<' to the end of its end tag, found by the nesting
/// of its tags; comments, CDATA sections and processing instructions are skipped. In a document that parse_xml()
/// accepted, that is the element. In other text it is where the element would end by its tags alone, whatever their
/// names, and an element that does not end extends to the end of text.
[[nodiscard]] std::size_t element_length(std::string_view text, std::size_t start);

} // namespace openxisf::detail
