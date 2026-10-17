// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "xml/xml_document.h"

#include <openxisf/error.h>

#include "core/hex.h"
#include "core/quote.h"
#include "core/utf8.h"

#include <algorithm>
#include <array>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace openxisf::detail {

namespace {

// The default options of pugixml (CDATA sections, escapes, normalized white space in attribute values and line ends),
// plus the nodes that the checks need: the XML declaration, a document type declaration, and white space as the only
// content of an element.
constexpr unsigned int parse_options =
    pugi::parse_default | pugi::parse_ws_pcdata_single | pugi::parse_declaration | pugi::parse_doctype;

error_context context_of(const pugi::xml_node& element, std::uint64_t offset)
{
    if (const std::optional<std::size_t> start = element_offset(element)) {
        return {.offset = offset + *start};
    }
    return {};
}

// For a node that is not an element, pugixml gives the offset of its content.
error_context context_of_content(const pugi::xml_node& node, std::uint64_t offset)
{
    const std::ptrdiff_t content = node.offset_debug();
    if (content < 0) {
        return {};
    }
    return {.offset = offset + static_cast<std::uint64_t>(content)};
}

// pugixml accepts an attribute twice in an element, and finds the first. Another reader would refuse the element or
// take the last, so the meaning is not clear.
void check_unique_attributes(const pugi::xml_node& element, std::vector<std::string_view>& names, std::uint64_t offset)
{
    names.clear();
    for (const pugi::xml_attribute& attribute : element.attributes()) {
        names.emplace_back(attribute.name());
    }
    std::ranges::sort(names);
    if (const auto repeated = std::ranges::adjacent_find(names); repeated != names.end()) {
        throw invalid_data_error(errc::invalid_xml,
                                 "the element " + quote(element.name()) + " has the attribute " + quote(*repeated) +
                                     " more than once",
                                 context_of(element, offset));
    }
}

// The value of the character reference at text[start], which starts with "&#", as pugixml reads it: decimal digits, or
// x and hexadecimal digits, then a semicolon. Saturated beyond the last code point. Empty when pugixml leaves the
// reference as written.
std::optional<std::uint32_t> character_reference(std::string_view text, std::size_t start) noexcept
{
    constexpr std::uint32_t beyond_unicode = 0x110000;
    std::size_t i = start + 2;
    std::uint32_t radix = 10;
    if (i < text.size() && text[i] == 'x') {
        radix = 16;
        ++i;
    }
    std::uint32_t value = 0;
    std::size_t digits = 0;
    for (; i < text.size() && text[i] != ';'; ++i, ++digits) {
        const std::uint32_t digit = hex_digit_value(text[i]);
        if (digit >= radix) {
            return std::nullopt;
        }
        value = std::min((value * radix) + digit, beyond_unicode);
    }
    if (i == text.size() || digits == 0) {
        return std::nullopt;
    }
    return value;
}

// True for the first byte of an XML name: a letter, '_', ':' or a byte of a character beyond ASCII.
bool starts_name(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == ':' ||
           static_cast<unsigned char>(c) >= 0x80;
}

// True when the processing instruction at the start of text, "<?", is an XML declaration as pugixml takes one: its
// target is xml in any case. pugixml reads the pseudo-attributes of a declaration as attribute values, references
// included.
bool is_declaration(std::string_view text) noexcept
{
    const auto lowercase = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    if (text.size() < 6 || lowercase(text[2]) != 'x' || lowercase(text[3]) != 'm' || lowercase(text[4]) != 'l') {
        return false;
    }
    // The target ends there: pugixml takes letters, digits, '_', ':', '-', '.' and bytes beyond ASCII for its name.
    const char next = text[5];
    const bool longer = starts_name(next) || (next >= '0' && next <= '9') || next == '-' || next == '.';
    return !longer;
}

// Calls visit(position, value) for each character reference that pugixml reads in text, with the value that
// character_reference() gives it: those of character data and attribute values, and of the pseudo-attributes of an
// XML declaration. References in comments, CDATA sections and other processing instructions are text. A tag, and an
// XML declaration, is passed over to its first '>' outside quoted attribute values, so that what an attribute value
// holds, such as "<!--", is part of the value. Each search stops where its region ends, so that a header of many
// comments is not searched to its end for each of them.
template <typename Visit> void for_each_character_reference(std::string_view text, Visit visit)
{
    const auto visit_between = [text, &visit](std::size_t from, std::size_t to) {
        const std::string_view region = text.substr(0, to);
        for (std::size_t i = from; (i = region.find("&#", i)) != std::string_view::npos; i += 2) {
            if (const std::optional<std::uint32_t> value = character_reference(text, i)) {
                visit(i, *value);
            }
        }
    };
    std::size_t visited = 0;
    std::size_t i = 0;
    while ((i = text.find('<', i)) != std::string_view::npos) {
        const std::string_view rest = text.substr(i);
        std::size_t skipped = 0;
        if (rest.starts_with("<!--")) {
            skipped = past(text, i + 4, "-->");
        } else if (rest.starts_with("<![CDATA[")) {
            skipped = past(text, i + 9, "]]>");
        } else if (rest.starts_with("<?") && !is_declaration(rest)) {
            skipped = past(text, i + 2, "?>");
        } else {
            i = tag_end(text, i).value_or(text.size());
            continue;
        }
        visit_between(visited, i);
        visited = skipped;
        i = skipped;
    }
    visit_between(visited, text.size());
}

// pugixml replaces a character reference with the UTF-8 form of any number, so a reference to U+0000, to a surrogate or
// beyond U+10FFFF would put a NUL that ends the text early, or invalid UTF-8, into a value. XML 1.0 forbids them (Legal
// Character).
void check_character_references(std::string_view text, std::uint64_t offset)
{
    for_each_character_reference(text, [text, offset](std::size_t position, std::uint32_t value) {
        if (value == 0 || (value >= 0xD800 && value <= 0xDFFF) || value > 0x10FFFF) {
            const std::string_view rest = text.substr(position);
            throw invalid_data_error(errc::invalid_xml,
                                     "the character reference " + quote(rest.substr(0, rest.find(';') + 1)) +
                                         " does not name a character that XML allows",
                                     {.offset = offset + position});
        }
    });
}

// The characters that XML 1.0 does not allow (Legal Character) and pugixml reads all the same: the control characters
// other than tab, line feed and carriage return, U+FFFE and U+FFFF. U+0000 and surrogates are refused before.
bool is_restricted(std::uint32_t value) noexcept
{
    return (value >= 0x1 && value <= 0x1F && value != 0x9 && value != 0xA && value != 0xD) || value == 0xFFFE ||
           value == 0xFFFF;
}

[[noreturn]] void throw_too_many_elements(const limits& limits, error_context context)
{
    throw limit_error(errc::too_many_xml_elements,
                      "the header has more than " + std::to_string(limits.max_xml_elements) + " XML elements",
                      std::move(context));
}

[[noreturn]] void throw_too_deep(const limits& limits, error_context context)
{
    throw limit_error(errc::xml_too_deep,
                      "the header nests XML elements deeper than " + std::to_string(limits.max_xml_depth) + " levels",
                      std::move(context));
}

// Counts the elements of text and their nesting from its tags, before pugixml builds the document, which costs about
// 64 bytes for each element: a header beyond the limits is refused before it is built, whatever its size. Comments,
// CDATA sections, processing instructions and declarations are skipped, and a tag ends at its first '>' outside quoted
// attribute values. In well-formed text the counts are those of check_tree(), which checks the document once more
// with the context of the element at fault; in other text they are what the tags give, and the text fails either way.
void check_tag_limits(std::string_view text, std::uint64_t offset, const limits& limits)
{
    if (limits.max_xml_elements == 0 && limits.max_xml_depth == 0) {
        return;
    }
    std::uint64_t elements = 0;
    std::uint64_t depth = 0;
    std::size_t i = 0;
    while ((i = text.find('<', i)) != std::string_view::npos) {
        const std::string_view rest = text.substr(i);
        if (rest.starts_with("<!--")) {
            i = past(text, i + 4, "-->");
        } else if (rest.starts_with("<![CDATA[")) {
            i = past(text, i + 9, "]]>");
        } else if (rest.starts_with("<?")) {
            i = past(text, i + 2, "?>");
        } else if (rest.starts_with("<!")) {
            i = past(text, i + 2, ">");
        } else if (rest.starts_with("</")) {
            if (depth > 0) {
                --depth;
            }
            i = past(text, i + 2, ">");
        } else if (rest.size() < 2 || !starts_name(rest[1])) {
            // Not a tag, which the parse reports.
            ++i;
        } else {
            ++elements;
            if (limits.max_xml_elements != 0 && elements > limits.max_xml_elements) {
                throw_too_many_elements(limits, {.offset = offset + i});
            }
            ++depth;
            if (limits.max_xml_depth != 0 && depth > limits.max_xml_depth) {
                throw_too_deep(limits, {.offset = offset + i});
            }
            const std::optional<std::size_t> end = tag_end(text, i);
            if (!end) {
                // The tag does not end, which the parse reports.
                return;
            }
            if (text[*end - 1] == '/') {
                --depth;
            }
            i = *end + 1;
        }
    }
}

// Visits every node in document order, without recursion, so that no nesting depth can exhaust the stack.
void check_tree(const pugi::xml_document& document, std::uint64_t offset, const limits& limits)
{
    std::vector<std::string_view> names;
    std::uint64_t elements = 0;
    std::uint64_t depth = 1;
    pugi::xml_node node = document.first_child();
    while (!node.empty()) {
        if (node.type() == pugi::node_doctype) {
            throw invalid_data_error(errc::doctype_not_allowed,
                                     "the header has a document type declaration, which XISF does not allow",
                                     context_of_content(node, offset));
        }
        if (node.type() == pugi::node_element) {
            ++elements;
            if (limits.max_xml_elements != 0 && elements > limits.max_xml_elements) {
                throw_too_many_elements(limits, context_of(node, offset));
            }
            if (limits.max_xml_depth != 0 && depth > limits.max_xml_depth) {
                throw_too_deep(limits, context_of(node, offset));
            }
            check_unique_attributes(node, names, offset);
        }

        // The next node: the first child, or else the next sibling of the node or of its closest ancestor that has
        // one.
        if (const pugi::xml_node child = node.first_child(); !child.empty()) {
            node = child;
            ++depth;
            continue;
        }
        while (!node.next_sibling() && node.parent().type() != pugi::node_document) {
            node = node.parent();
            --depth;
        }
        node = node.next_sibling();
    }
}

void append_utf8(std::string& text, std::uint32_t code_point)
{
    if (code_point < 0x80) {
        text += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        text += static_cast<char>(0xC0 | (code_point >> 6));
        text += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        text += static_cast<char>(0xE0 | (code_point >> 12));
        text += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        text += static_cast<char>(0xF0 | (code_point >> 18));
        text += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        text += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

// Appends what the reference at text[start], a '&', stands for to data, as pugixml replaces it: a predefined entity or
// a character reference; anything else, such as an entity that cannot be declared, is left as written. Returns where
// the text after it starts. The values of character references were checked by parse_xml().
std::size_t append_reference(std::string& data, std::string_view text, std::size_t start)
{
    static constexpr std::array<std::pair<std::string_view, char>, 5> entities{
        {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&apos;", '\''}, {"&quot;", '"'}}};
    const std::string_view rest = text.substr(start);
    for (const auto& [entity, character] : entities) {
        if (rest.starts_with(entity)) {
            data += character;
            return start + entity.size();
        }
    }
    if (rest.starts_with("&#")) {
        if (const std::optional<std::uint32_t> value = character_reference(text, start)) {
            append_utf8(data, *value);
            return text.find(';', start) + 1;
        }
    }
    data += '&';
    return start + 1;
}

} // namespace

std::size_t past(std::string_view text, std::size_t from, std::string_view terminator) noexcept
{
    const std::size_t found = text.find(terminator, from);
    return found == std::string_view::npos ? text.size() : found + terminator.size();
}

bool is_xml_white_space(std::string_view text) noexcept
{
    return std::ranges::all_of(text, [](char c) { return is_xml_white_space(c); });
}

std::unique_ptr<pugi::xml_document> parse_xml(std::string_view text, std::uint64_t offset, const limits& limits)
{
    if (!is_valid_utf8(text)) {
        throw invalid_data_error(errc::invalid_utf8, "the header is not valid UTF-8, or contains U+0000",
                                 {.offset = offset});
    }
    check_character_references(text, offset);
    check_tag_limits(text, offset, limits);

    auto document = std::make_unique<pugi::xml_document>();
    const pugi::xml_parse_result result =
        document->load_buffer(text.data(), text.size(), parse_options, pugi::encoding_utf8);
    if (result.status == pugi::status_out_of_memory) {
        throw std::bad_alloc();
    }
    if (!result) {
        throw invalid_data_error(errc::invalid_xml,
                                 std::string("the header is not well-formed XML: ") + result.description(),
                                 {.offset = offset + static_cast<std::uint64_t>(result.offset)});
    }
    check_tree(*document, offset, limits);
    return document;
}

std::optional<restricted_character> first_restricted_character(std::string_view text)
{
    std::optional<restricted_character> first;
    for (std::size_t i = 0; i < text.size() && !first; ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        const std::string_view next = text.substr(i, 3);
        if (is_restricted(byte)) {
            first = restricted_character{.offset = i, .code_point = byte};
        } else if (next == "\xEF\xBF\xBE" || next == "\xEF\xBF\xBF") {
            first = restricted_character{.offset = i, .code_point = next == "\xEF\xBF\xBE" ? 0xFFFEU : 0xFFFFU};
        }
    }
    for_each_character_reference(text, [&first](std::size_t position, std::uint32_t value) {
        if (is_restricted(value) && (!first || position < first->offset)) {
            first = restricted_character{.offset = position, .code_point = value};
        }
    });
    return first;
}

std::optional<std::size_t> first_text_outside_elements(std::string_view text)
{
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t next = std::min(text.find('<', i), text.size());
        for (std::size_t j = i; j < next; ++j) {
            if (!is_xml_white_space(text[j])) {
                return j;
            }
        }
        if (next == text.size()) {
            break;
        }
        const std::string_view rest = text.substr(next);
        if (rest.starts_with("<![CDATA[")) {
            return next;
        }
        if (rest.starts_with("<!--")) {
            i = past(text, next + 4, "-->");
        } else if (rest.starts_with("<?")) {
            i = past(text, next + 2, "?>");
        } else if (rest.starts_with("<!")) {
            i = past(text, next + 2, ">");
        } else {
            i = next + element_length(text, next);
        }
    }
    return std::nullopt;
}

std::string character_data(const pugi::xml_node& element)
{
    std::string text;
    for (pugi::xml_node child = element.first_child(); !child.empty(); child = child.next_sibling()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            text += child.value();
        }
    }
    return text;
}

std::string character_data(const pugi::xml_node& element, std::string_view text)
{
    const std::optional<std::size_t> start = element_offset(element);
    if (!start) {
        return character_data(element);
    }
    const std::optional<std::size_t> start_tag_end = tag_end(text, *start);
    if (!start_tag_end || text[*start_tag_end - 1] == '/') {
        return character_data(element);
    }
    // The end tag starts at the last "</" of the element.
    const std::size_t end = *start + element_length(text, *start);
    const std::size_t content_start = *start_tag_end + 1;
    const std::string_view content = text.substr(content_start, text.rfind("</", end - 1) - content_start);
    if (content.find("<!--") == std::string_view::npos && content.find("<?") == std::string_view::npos &&
        content.find("<![CDATA[") == std::string_view::npos) {
        return character_data(element);
    }

    std::string data;
    // Line ends are normalized before anything else (XML 1.0, End-of-Line Handling), in CDATA sections too.
    const auto append_normalized = [&data](std::string_view part) {
        for (std::size_t i = 0; i < part.size(); ++i) {
            if (part[i] != '\r') {
                data += part[i];
            } else {
                data += '\n';
                if (i + 1 < part.size() && part[i + 1] == '\n') {
                    ++i;
                }
            }
        }
    };
    std::size_t i = 0;
    while (i < content.size()) {
        const std::size_t markup = std::min(content.find_first_of("<&", i), content.size());
        append_normalized(content.substr(i, markup - i));
        i = markup;
        if (i == content.size()) {
            break;
        }
        const std::string_view rest = content.substr(i);
        if (rest.starts_with("<!--")) {
            i = past(content, i + 4, "-->");
        } else if (rest.starts_with("<![CDATA[")) {
            const std::size_t close = std::min(content.find("]]>", i + 9), content.size());
            append_normalized(content.substr(i + 9, close - (i + 9)));
            i = past(content, close, "]]>");
        } else if (rest.starts_with("<?")) {
            i = past(content, i + 2, "?>");
        } else if (rest.starts_with("<")) {
            // A child element, whose character data are its own.
            i += element_length(content, i);
        } else {
            i = append_reference(data, content, i);
        }
    }
    return data;
}

std::optional<std::size_t> element_offset(const pugi::xml_node& element) noexcept
{
    // pugixml gives the offset of the name, which follows the '<' immediately.
    const std::ptrdiff_t name = element.offset_debug();
    if (name < 1) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(name - 1);
}

std::optional<std::size_t> tag_end(std::string_view text, std::size_t start) noexcept
{
    char quote_mark = 0;
    for (std::size_t i = start + 1; i < text.size(); ++i) {
        const char c = text[i];
        if (quote_mark != 0) {
            if (c == quote_mark) {
                quote_mark = 0;
            }
        } else if (c == '"' || c == '\'') {
            quote_mark = c;
        } else if (c == '>') {
            return i;
        }
    }
    return std::nullopt;
}

std::size_t element_length(std::string_view text, std::size_t start)
{
    std::size_t depth = 0;
    std::size_t i = start;
    while ((i = text.find('<', i)) != std::string_view::npos) {
        // Markup that may hold '<' and '>' without being a tag.
        const std::string_view rest = text.substr(i);
        if (rest.starts_with("<!--")) {
            i = past(text, i + 4, "-->");
            continue;
        }
        if (rest.starts_with("<![CDATA[")) {
            i = past(text, i + 9, "]]>");
            continue;
        }
        if (rest.starts_with("<?")) {
            i = past(text, i + 2, "?>");
            continue;
        }

        const std::optional<std::size_t> end = tag_end(text, i);
        if (!end) {
            break;
        }
        i = *end + 1;

        if (rest.starts_with("</")) {
            if (depth <= 1) {
                return i - start;
            }
            --depth;
        } else if (text[*end - 1] == '/') {
            if (depth == 0) {
                return i - start;
            }
        } else if (!rest.starts_with("<!")) {
            // A start tag. No other declaration than those skipped above can be inside an element of a document that
            // parse_xml() accepted.
            ++depth;
        }
    }
    return text.size() - start;
}

} // namespace openxisf::detail
