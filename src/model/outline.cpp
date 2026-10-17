// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/outline.h"

#include <openxisf/error.h>

#include "core/quote.h"
#include "xml/namespaces.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <unordered_map>

namespace openxisf::detail {

namespace {

struct kind_name
{
    element_kind kind{};
    std::string_view name{};
};

// In the order of element_kind.
constexpr std::array<kind_name, 17> element_names{{
    {.kind = element_kind::metadata, .name = "Metadata"},
    {.kind = element_kind::image, .name = "Image"},
    {.kind = element_kind::thumbnail, .name = "Thumbnail"},
    {.kind = element_kind::property, .name = "Property"},
    {.kind = element_kind::table, .name = "Table"},
    {.kind = element_kind::structure, .name = "Structure"},
    {.kind = element_kind::fits_keyword, .name = "FITSKeyword"},
    {.kind = element_kind::icc_profile, .name = "ICCProfile"},
    {.kind = element_kind::rgb_working_space, .name = "RGBWorkingSpace"},
    {.kind = element_kind::display_function, .name = "DisplayFunction"},
    {.kind = element_kind::color_filter_array, .name = "ColorFilterArray"},
    {.kind = element_kind::resolution, .name = "Resolution"},
    {.kind = element_kind::reference, .name = "Reference"},
    {.kind = element_kind::field, .name = "Field"},
    {.kind = element_kind::row, .name = "Row"},
    {.kind = element_kind::cell, .name = "Cell"},
    {.kind = element_kind::data, .name = "Data"},
}};

constexpr bool names_in_order() noexcept
{
    for (std::size_t i = 0; i < element_names.size(); ++i) {
        if (static_cast<std::size_t>(element_names[i].kind) != i) {
            return false;
        }
    }
    return true;
}
static_assert(names_in_order());

// Element names are case-sensitive (spec §11).
std::optional<element_kind> kind_of(std::string_view name) noexcept
{
    for (const kind_name& entry : element_names) {
        if (entry.name == name) {
            return entry.kind;
        }
    }
    return std::nullopt;
}

constexpr std::uint32_t bit(element_kind kind) noexcept
{
    return std::uint32_t{1} << static_cast<unsigned int>(kind);
}

constexpr std::uint32_t image_children =
    bit(element_kind::property) | bit(element_kind::table) | bit(element_kind::fits_keyword) |
    bit(element_kind::icc_profile) | bit(element_kind::rgb_working_space) | bit(element_kind::display_function) |
    bit(element_kind::color_filter_array) | bit(element_kind::resolution) | bit(element_kind::thumbnail) |
    bit(element_kind::reference) | bit(element_kind::data);

constexpr std::uint32_t root_children = (image_children & ~bit(element_kind::data)) | bit(element_kind::metadata) |
                                        bit(element_kind::image) | bit(element_kind::structure);

// The elements that an element can contain (spec §11), or the root element when parent is empty. A Data element holds
// an embedded block (spec §10.3).
std::uint32_t allowed_children(std::optional<element_kind> parent) noexcept
{
    if (!parent) {
        return root_children;
    }
    switch (*parent) {
    case element_kind::metadata:
        return bit(element_kind::property) | bit(element_kind::reference);
    case element_kind::image:
        return image_children;
    case element_kind::thumbnail:
        // An image without a color filter array or a thumbnail of its own (spec §11.12).
        return image_children & ~(bit(element_kind::color_filter_array) | bit(element_kind::thumbnail));
    case element_kind::property:
    case element_kind::icc_profile:
    case element_kind::cell:
        return bit(element_kind::data);
    case element_kind::table:
        return bit(element_kind::structure) | bit(element_kind::reference) | bit(element_kind::row);
    case element_kind::structure:
        return bit(element_kind::field);
    case element_kind::row:
        return bit(element_kind::cell);
    case element_kind::fits_keyword:
    case element_kind::rgb_working_space:
    case element_kind::display_function:
    case element_kind::color_filter_array:
    case element_kind::resolution:
    case element_kind::reference:
    case element_kind::field:
    case element_kind::data:
        return 0;
    }
    return 0;
}

pugi::xml_node first_child_element(const pugi::xml_node& node)
{
    pugi::xml_node child = node.first_child();
    while (!child.empty() && child.type() != pugi::node_element) {
        child = child.next_sibling();
    }
    return child;
}

pugi::xml_node next_sibling_element(const pugi::xml_node& node)
{
    pugi::xml_node sibling = node.next_sibling();
    while (!sibling.empty() && sibling.type() != pugi::node_element) {
        sibling = sibling.next_sibling();
    }
    return sibling;
}

// The path of the child element named name at position among the children of parent, which is no_element for the root
// element: the path of the parent and a step of the local name.
std::string child_path(const unit_outline& outline, std::size_t parent, std::string_view name, std::size_t position)
{
    std::string path = outline.path(parent);
    path += '/';
    path += local_name(name);
    path += '[';
    path += std::to_string(position);
    path += ']';
    return path;
}

struct uid_entry
{
    std::size_t index = no_element;
    bool unique = true;
};

using uid_table = std::unordered_map<std::string_view, uid_entry>;

void report_ignored(const unit_outline& outline, std::size_t parent, const pugi::xml_node& node, std::size_t position,
                    bool xisf, std::optional<element_kind> kind, diagnostic_log& log)
{
    const error_context context{.element = child_path(outline, parent, node.name(), position)};
    if (parent == no_element && !xisf) {
        log.info(errc::unknown_element, "the extension element " + quote(node.name()) + " is ignored", context);
    } else if (xisf && !kind) {
        log.warning(errc::unknown_element,
                    "the element " + quote(node.name()) + " is not defined by XISF, and is ignored", context);
    } else {
        const std::string container =
            parent == no_element ? "the root element" : std::string(element_name(outline.elements[parent].kind));
        log.warning(errc::unknown_element,
                    "the element " + quote(node.name()) + " is not allowed in " + container + ", and is ignored",
                    context);
    }
}

void register_uid(const unit_outline& outline, std::size_t index, uid_table& uids, diagnostic_log& log)
{
    const outline_element& element = outline.elements[index];
    const pugi::xml_attribute attribute = element.node.attribute("uid");
    if (!attribute) {
        return;
    }
    const std::string_view uid = attribute.value();
    // A Reference cannot have a uid, so that references cannot be chained (spec §11.13). It is still recorded, so that
    // a Reference that names it is reported as chained.
    if (element.kind == element_kind::reference) {
        log.warning(errc::invalid_uid, "a Reference element cannot have a uid",
                    {.element = outline.path(index), .attribute = "uid"});
    } else if (!is_unique_element_id(uid)) {
        log.warning(errc::invalid_uid, quote(uid) + " is not a unique element identifier",
                    {.element = outline.path(index), .attribute = "uid"});
    }
    const auto [entry, inserted] = uids.try_emplace(uid, uid_entry{.index = index});
    if (!inserted) {
        entry->second.unique = false;
        log.error(errc::duplicate_uid,
                  "the uid " + quote(uid) + " is already the uid of " + outline.path(entry->second.index),
                  {.element = outline.path(index), .attribute = "uid"});
    }
}

// Visits the elements in document order, without recursion. Elements that are not in the outline are not entered.
void walk(unit_outline& outline, uid_table& uids, diagnostic_log& log)
{
    struct frame
    {
        std::size_t index = no_element;
        pugi::xml_node next{};
    };
    std::vector<frame> stack;
    // The child elements seen so far, by name, for each frame of the stack. The tables are reused from one element to
    // the next at the same depth, and the depth of the outline is bounded by the nesting of the specification. They are
    // ordered maps, whose clear() costs the names that a table holds, where a hash table, which keeps its buckets,
    // would make every later element at the depth of one with many children pay for them.
    std::vector<std::map<std::string_view, std::size_t>> seen;
    const auto enter = [&stack, &seen](std::size_t index, const pugi::xml_node& element) {
        stack.push_back({.index = index, .next = first_child_element(element)});
        if (seen.size() < stack.size()) {
            seen.emplace_back();
        } else {
            seen[stack.size() - 1].clear();
        }
    };

    namespace_scope scope;
    scope.enter(outline.root);
    enter(no_element, outline.root);
    while (!stack.empty()) {
        frame& top = stack.back();
        if (top.next.empty()) {
            if (top.index != no_element) {
                outline.elements[top.index].end = outline.elements.size();
            }
            scope.leave();
            stack.pop_back();
            continue;
        }
        const pugi::xml_node node = top.next;
        top.next = next_sibling_element(node);
        const std::size_t parent = top.index;
        const std::size_t position = ++seen[stack.size() - 1][local_name(node.name())];
        // The paths of the children of an element are built from its own, computed when the first one is seen.
        if (parent != no_element && outline.elements[parent].path.empty()) {
            outline.elements[parent].path = outline.path(parent);
        }

        scope.enter(node);
        const bool xisf = is_xisf_namespace(scope.namespace_of(node.name()));
        const std::optional<element_kind> kind = xisf ? kind_of(local_name(node.name())) : std::nullopt;
        const std::optional<element_kind> parent_kind =
            parent == no_element ? std::nullopt : std::optional(outline.elements[parent].kind);
        if (!kind || (allowed_children(parent_kind) & bit(*kind)) == 0) {
            report_ignored(outline, parent, node, position, xisf, kind, log);
            scope.leave();
            continue;
        }

        const std::size_t index = outline.elements.size();
        outline.elements.push_back({.kind = *kind, .node = node, .parent = parent, .position = position});
        if (is_core_element(*kind)) {
            register_uid(outline, index, uids, log);
        }
        enter(index, node);
    }
}

void resolve_references(unit_outline& outline, const uid_table& uids, diagnostic_log& log)
{
    for (std::size_t index = 0; index < outline.elements.size(); ++index) {
        outline_element& element = outline.elements[index];
        if (element.kind != element_kind::reference) {
            continue;
        }
        const pugi::xml_attribute ref = element.node.attribute("ref");
        if (!ref) {
            log.error(errc::dangling_reference, "the Reference element has no ref attribute",
                      {.element = outline.path(index)});
            continue;
        }
        const auto fail = [&](errc code, const std::string& problem) {
            log.error(code, "the uid " + quote(ref.value()) + problem,
                      {.element = outline.path(index), .attribute = "ref"});
        };
        const auto found = uids.find(ref.value());
        if (found == uids.end()) {
            fail(errc::dangling_reference, " is the uid of no element");
        } else if (!found->second.unique) {
            fail(errc::duplicate_uid, " is the uid of more than one element");
        } else if (outline.elements[found->second.index].kind == element_kind::reference) {
            fail(errc::chained_reference, " is the uid of another Reference");
        } else {
            element.target = found->second.index;
        }
    }
}

} // namespace

std::string_view element_name(element_kind kind) noexcept
{
    return element_names[static_cast<std::size_t>(kind)].name;
}

bool is_core_element(element_kind kind) noexcept
{
    return kind != element_kind::field && kind != element_kind::row && kind != element_kind::cell &&
           kind != element_kind::data;
}

bool is_unique_element_id(std::string_view id) noexcept
{
    const auto is_first = [](char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    const auto is_next = [&is_first](char c) { return is_first(c) || (c >= '0' && c <= '9'); };
    return !id.empty() && is_first(id.front()) && std::all_of(id.begin() + 1, id.end(), is_next);
}

std::string unit_outline::path(std::size_t index) const
{
    if (index == no_element) {
        return root_path;
    }
    const outline_element& element = elements[index];
    if (!element.path.empty()) {
        return element.path;
    }
    return child_path(*this, element.parent, element.node.name(), element.position);
}

unit_outline build_outline(const pugi::xml_node& root, diagnostic_log& log)
{
    unit_outline outline{.root = root, .root_path = "/" + std::string(local_name(root.name()))};
    uid_table uids;
    walk(outline, uids, log);
    resolve_references(outline, uids, log);
    return outline;
}

} // namespace openxisf::detail
