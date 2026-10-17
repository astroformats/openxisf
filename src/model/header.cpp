// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/header.h"

#include <openxisf/error.h>

#include "core/quote.h"
#include "xml/namespaces.h"
#include "xml/xml_document.h"

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace openxisf::detail {

namespace {

constexpr std::string_view utf8_byte_order_mark = "\xEF\xBB\xBF";

// The XML declaration that every header starts with (spec §9.5).
constexpr std::string_view xml_declaration = R"(<?xml version="1.0" encoding="UTF-8"?>)";

char to_lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equal_ignoring_case(std::string_view a, std::string_view b) noexcept
{
    return std::ranges::equal(a, b, [](char x, char y) { return to_lower(x) == to_lower(y); });
}

bool is_ascii(std::string_view text) noexcept
{
    return std::ranges::all_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x80U; });
}

// The name of a code point of at most four hexadecimal digits, such as U+001F.
std::string code_point_name(std::uint32_t value)
{
    constexpr std::string_view digits = "0123456789ABCDEF";
    std::string name = "U+";
    for (int shift = 12; shift >= 0; shift -= 4) {
        name += digits[(value >> static_cast<unsigned>(shift)) & 0xFU];
    }
    return name;
}

// Another declaration, or none, means the same as long as the encoding is not in question: the header was found to be
// valid UTF-8, and pugixml reads it as such whatever the declaration says.
void check_declaration(const pugi::xml_document& document, std::string_view text, std::uint64_t offset,
                       diagnostic_log& log)
{
    if (text.starts_with(xml_declaration)) {
        return;
    }
    const pugi::xml_node declaration = document.first_child();
    if (declaration.type() != pugi::node_declaration) {
        log.warning(errc::invalid_xml_declaration, "the header does not start with an XML declaration",
                    {.offset = offset});
        return;
    }
    const std::string_view encoding = declaration.attribute("encoding").value();
    if (!encoding.empty() && !equal_ignoring_case(encoding, "UTF-8") && !is_ascii(text)) {
        throw invalid_data_error(errc::invalid_xml,
                                 "the XML declaration names the encoding " + quote(encoding) +
                                     ", but the header has characters beyond ASCII, which XISF encodes in UTF-8",
                                 {.offset = offset});
    }
    log.warning(errc::invalid_xml_declaration, "the XML declaration is not " + std::string(xml_declaration),
                {.offset = offset});
}

// The root element is named xisf, case-sensitively, in the XISF namespace or in none.
bool is_root_name(std::string_view name, const namespace_scope& scope) noexcept
{
    return local_name(name) == "xisf" && is_xisf_namespace(scope.namespace_of(name));
}

pugi::xml_node check_root(const pugi::xml_document& document, diagnostic_log& log)
{
    const pugi::xml_node root = document.document_element();
    const std::string path = "/" + std::string(local_name(root.name()));
    namespace_scope scope;
    scope.enter(root);
    if (!is_root_name(root.name(), scope)) {
        throw invalid_data_error(errc::invalid_root_element,
                                 "the root element is " + quote(root.name()) + ", not an XISF root element",
                                 {.element = path});
    }

    const pugi::xml_attribute version = root.attribute("version");
    if (!version) {
        throw invalid_data_error(errc::invalid_root_element, "the root element has no version attribute",
                                 {.element = path, .attribute = "version"});
    }
    if (std::string_view(version.value()) != "1.0") {
        throw unsupported_error(errc::unsupported_version,
                                "the unit is of XISF version " + quote(version.value()) +
                                    ", and only version 1.0 is supported",
                                {.element = path, .attribute = "version"});
    }

    // White space between the child elements is not significant; other character data is not allowed.
    const auto is_text = [](const pugi::xml_node& child) {
        return (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) &&
               !is_xml_white_space(child.value());
    };
    if (!root.find_child(is_text).empty()) {
        log.warning(errc::invalid_root_element, "the root element has character data, which is ignored",
                    {.element = path});
    }
    return root;
}

// Where the comment or processing instruction at text[start] ends: past its terminator, or at the end of text when it
// has none. Empty when neither starts there.
std::optional<std::size_t> past_comment_or_instruction(std::string_view text, std::size_t start) noexcept
{
    const std::string_view rest = text.substr(start);
    const bool comment = rest.starts_with("<!--");
    if (!comment && !rest.starts_with("<?")) {
        return std::nullopt;
    }
    return comment ? past(text, start + 4, "-->") : past(text, start + 2, "?>");
}

// Where the root element ends, found from the text alone: the end of the first element after the XML declaration,
// comments and processing instructions. Empty when something else comes first.
std::optional<std::size_t> find_root_end(std::string_view text)
{
    std::size_t i = 0;
    while ((i = text.find('<', i)) != std::string_view::npos) {
        if (const std::optional<std::size_t> skipped = past_comment_or_instruction(text, i)) {
            i = *skipped;
            continue;
        }
        const std::string_view rest = text.substr(i);
        if (rest.starts_with("<!") || rest.starts_with("</")) {
            return std::nullopt;
        }
        return i + element_length(text, i);
    }
    return std::nullopt;
}

// The end of the last element, comment or processing instruction after the root element that ends before position, or
// the end of the root element when none does. position is before the end of text, so one that does not end holds it.
std::size_t end_before(std::string_view text, std::size_t root_end, std::uint64_t position)
{
    std::size_t end = root_end;
    std::size_t i = root_end;
    while ((i = text.find('<', i)) != std::string_view::npos) {
        const std::optional<std::size_t> skipped = past_comment_or_instruction(text, i);
        const std::size_t item_end = skipped ? *skipped : i + element_length(text, i);
        if (item_end > position) {
            break;
        }
        end = item_end;
        i = item_end;
    }
    return end;
}

// The part of a header that is well-formed XML: all of it, or, when it is not well-formed only after its root element,
// up to the last element, comment or processing instruction before the problem.
struct well_formed_part
{
    std::unique_ptr<pugi::xml_document> document{};
    /// Where the part ends in the text.
    std::size_t end = 0;
    /// Where the problem after it is in the source, when there is one.
    std::uint64_t problem = 0;
};

// What follows the root element does not change it, and a decoder that cannot parse more than one top-level element
// isolates it (spec §9.5). Each parse finds the first problem of one kind only, either in the text, the syntax or the
// attributes, so a part may take a few: each one ends before the last.
well_formed_part parse_well_formed_part(std::string_view text, std::uint64_t offset, const limits& limits)
{
    well_formed_part part{.end = text.size()};
    for (;;) {
        try {
            part.document = parse_xml(text.substr(0, part.end), offset, limits);
            return part;
        } catch (const invalid_data_error& failure) {
            const std::optional<std::uint64_t> where = failure.context().offset;
            const std::optional<std::size_t> root_end = find_root_end(text.substr(0, part.end));
            if (failure.code() != errc::invalid_xml || !where || !root_end || *where - offset < *root_end) {
                throw;
            }
            const std::size_t end = end_before(text.substr(0, part.end), *root_end, *where - offset);
            if (end >= part.end) {
                throw;
            }
            part.end = end;
            part.problem = *where;
        }
    }
}

// An element exactly as written, which pugixml cannot reproduce: it would print the element in its own way. text is
// what its document was parsed from.
std::string element_text(const pugi::xml_node& element, std::string_view text)
{
    const std::optional<std::size_t> start = element_offset(element);
    if (!start) {
        // pugixml knows the offset of every element of a document parsed from one buffer.
        throw invalid_data_error(errc::invalid_xml, "an element cannot be located in the header");
    }
    return std::string(text.substr(*start, element_length(text, *start)));
}

// The name in the start tag that begins text, as far as it goes: up to white space, '/' or '>'. Empty for other markup.
std::string_view tag_name(std::string_view text) noexcept
{
    if (text.size() < 2 || text[1] == '/' || text[1] == '!' || text[1] == '?') {
        return {};
    }
    const std::size_t end = text.find_first_of(" \t\r\n/>", 1);
    return text.substr(1, end == std::string_view::npos ? std::string_view::npos : end - 1);
}

// The start tag that begins text, read on its own as an empty element. Null when it cannot be: when text holds no
// start tag, or one that is not well-formed.
std::unique_ptr<pugi::xml_document> start_tag_alone(std::string_view text, const limits& limits)
{
    const std::optional<std::size_t> end = tag_end(text, 0);
    if (tag_name(text).empty() || !end) {
        return nullptr;
    }
    std::string tag(text.substr(0, *end));
    if (!tag.ends_with('/')) {
        tag += '/';
    }
    tag += '>';
    try {
        return parse_xml(tag, 0, limits);
    } catch (const error&) {
        return nullptr;
    }
}

void refuse_second_root(std::string_view name, const namespace_scope& scope, const std::string& path)
{
    if (is_root_name(name, scope)) {
        throw invalid_data_error(errc::invalid_root_element, "the header has a second root element", {.element = path});
    }
}

bool is_signature_name(std::string_view name, const namespace_scope& scope) noexcept
{
    return local_name(name) == "Signature" && scope.namespace_of(name) == xml_signature_namespace;
}

// True when node is an element of the XML signature namespace with the local name name. scope holds the declarations
// of the ancestors of node.
bool is_signature_element(const pugi::xml_node& node, std::string_view name, namespace_scope& scope)
{
    if (node.type() != pugi::node_element) {
        return false;
    }
    scope.enter(node);
    const bool found = local_name(node.name()) == name && scope.namespace_of(node.name()) == xml_signature_namespace;
    scope.leave();
    return found;
}

// node, or else the first element among the siblings after it. Empty when there is none.
pugi::xml_node element_from(pugi::xml_node node)
{
    while (!node.empty() && node.type() != pugi::node_element) {
        node = node.next_sibling();
    }
    return node;
}

// True when the URI of a Reference element names the element with the given id: as a bare name, or as the XPointer
// that XML Signature §4.4.3.3 also allows.
bool names_element(std::string_view uri, std::string_view id)
{
    const std::string name(id);
    return uri == "#" + name || uri == "#xpointer(id('" + name + "'))" || uri == "#xpointer(id(\"" + name + "\"))";
}

// What a reader can check of a signature without verifying it: that it starts as an XML signature does, with its
// SignedInfo and SignatureValue elements (XML Signature §4.1), and that it signs the root element, which it must name
// by the id attribute of the root element (spec §9.5). scope holds the declarations of the signature element.
void check_signature(const pugi::xml_node& signature, namespace_scope& scope, const pugi::xml_node& root,
                     const std::string& path, diagnostic_log& log)
{
    const pugi::xml_node signed_info = element_from(signature.first_child());
    const pugi::xml_node value = element_from(signed_info.next_sibling());
    if (!is_signature_element(signed_info, "SignedInfo", scope) ||
        !is_signature_element(value, "SignatureValue", scope)) {
        log.warning(errc::invalid_signature,
                    "the signature does not start with the SignedInfo and SignatureValue elements of an XML signature",
                    {.element = path});
        return;
    }
    const std::string_view id = root.attribute("id").value();
    if (id.empty()) {
        log.warning(errc::invalid_signature,
                    "the root element has no id attribute, by which the signature must name it",
                    {.element = "/" + std::string(local_name(root.name())), .attribute = "id"});
        return;
    }
    scope.enter(signed_info);
    bool named = false;
    for (pugi::xml_node child = signed_info.first_child(); !child.empty() && !named; child = child.next_sibling()) {
        named = is_signature_element(child, "Reference", scope) && names_element(child.attribute("URI").value(), id);
    }
    scope.leave();
    if (!named) {
        log.warning(errc::invalid_signature,
                    "no Reference element of the signature names the root element, " + quote("#" + std::string(id)),
                    {.element = path + "/" + std::string(local_name(signed_info.name())) + "[1]"});
    }
}

constexpr std::string_view not_verified = "the unit is signed, and OpenXISF does not verify signatures";

// What follows the root element (spec §9.5). The first Signature element of the XML signature namespace is the detached
// signature, kept as written; a second root element is an error; anything else is ignored, with a warning.
class after_root
{
public:
    after_root(const pugi::xml_node& root, const limits& limits, diagnostic_log& log)
        : root_(root), limits_(limits), log_(log)
    {
        positions_.emplace(local_name(root.name()), std::size_t{1});
    }

    // An element after the root element. text is what its document was parsed from.
    void element(const pugi::xml_node& node, std::string_view text)
    {
        const std::string_view name = node.name();
        const std::string path = path_of(name);
        namespace_scope scope;
        scope.enter(node);
        refuse_second_root(name, scope, path);
        if (!signature_ && is_signature_name(name, scope)) {
            signature_ = element_text(node, text);
            log_.info(errc::signature_not_verified, std::string(not_verified), {.element = path});
            check_signature(node, scope, root_, path, log_);
            return;
        }
        log_.warning(errc::unknown_element, "the element " + quote(name) + " after the root element is ignored",
                     {.element = path});
    }

    // What follows the last element that is well-formed XML, when the header is not well-formed there: text, at offset
    // in the source, with the problem at where. The element that holds the problem may be the signature, which is kept
    // as far as the nesting of its tags goes. Nothing after it is read.
    void malformed(std::string_view text, std::uint64_t offset, std::uint64_t where)
    {
        // The problem is in the first element of the text, unless it is in the text before it.
        const std::size_t start = text.find('<');
        const bool in_element = start != std::string_view::npos && where >= offset + start;
        const std::string_view element = in_element ? text.substr(start) : std::string_view();
        const std::string_view name = tag_name(element);
        const std::string path = name.empty() ? std::string() : path_of(name);
        if (const std::unique_ptr<pugi::xml_document> tag = start_tag_alone(element, limits_)) {
            const pugi::xml_node node = tag->document_element();
            namespace_scope scope;
            scope.enter(node);
            refuse_second_root(node.name(), scope, path);
            if (!signature_ && is_signature_name(node.name(), scope)) {
                signature_ = std::string(element.substr(0, element_length(element, 0)));
                log_.info(errc::signature_not_verified, std::string(not_verified), {.element = path});
                log_.warning(errc::invalid_signature, "the signature is not well-formed XML",
                             {.element = path, .offset = where});
                return;
            }
        }
        log_.warning(errc::invalid_xml,
                     "the header is not well-formed XML after the root element, and the rest is ignored",
                     {.element = path, .offset = where});
    }

    // The signature, which the object no longer holds.
    [[nodiscard]] std::optional<std::string> release() noexcept
    {
        return std::move(signature_);
    }

private:
    // The path of the next element after the root element with this name: its local name, and its position among those
    // with that local name, as in the paths of the outline.
    std::string path_of(std::string_view name)
    {
        const std::string step(local_name(name));
        return "/" + step + "[" + std::to_string(++positions_[step]) + "]";
    }

    pugi::xml_node root_;
    const limits& limits_;
    diagnostic_log& log_;
    std::map<std::string, std::size_t> positions_{};
    std::optional<std::string> signature_{};
};

} // namespace

parsed_header parse_header(std::string_view text, std::uint64_t offset, const limits& limits, diagnostic_log& log)
{
    if (text.starts_with(utf8_byte_order_mark)) {
        text.remove_prefix(utf8_byte_order_mark.size());
        offset += utf8_byte_order_mark.size();
    }
    well_formed_part part = parse_well_formed_part(text, offset, limits);
    parsed_header header{.document = std::move(part.document), .text = text};
    check_declaration(*header.document, text, offset, log);
    // Such a character means what it says, and is kept; only the first one is reported, of the text that was parsed:
    // what follows where the header stops being well-formed is not read, only ignored or, for a signature, kept as
    // written, with a warning about it.
    if (const std::optional<restricted_character> found = first_restricted_character(text.substr(0, part.end))) {
        log.warning(errc::invalid_character,
                    "the header holds " + code_point_name(found->code_point) +
                        ", a character that XML 1.0 does not allow, which is read as it is",
                    {.offset = offset + found->offset});
    }
    header.root = check_root(*header.document, log);

    after_root after(header.root, limits, log);
    for (pugi::xml_node node = header.root.next_sibling(); !node.empty(); node = node.next_sibling()) {
        if (node.type() == pugi::node_element) {
            after.element(node, text);
        }
    }
    // Only markup and white space can surround the root element; what else is there is ignored.
    if (const std::optional<std::size_t> found = first_text_outside_elements(text.substr(0, part.end))) {
        log.warning(errc::invalid_xml, "the header has character data outside its root element, which is ignored",
                    {.offset = offset + *found});
    }
    if (part.end < text.size()) {
        after.malformed(text.substr(part.end), offset + part.end, part.problem);
    }
    if (std::optional<std::string> signature = after.release()) {
        header.signature =
            header_signature{.element = std::move(*signature), .signed_element = element_text(header.root, text)};
    }
    return header;
}

} // namespace openxisf::detail
