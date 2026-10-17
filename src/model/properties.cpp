// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/properties.h"

#include <openxisf/error.h>

#include "core/quote.h"
#include "core/utc_time.h"
#include "model/format_specifier.h"
#include "model/held_size.h"
#include "model/property_catalog.h"
#include "model/property_text.h"
#include "model/property_types.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace openxisf::detail {

namespace {

// The metadata properties that every unit must have (spec §11.4.1).
constexpr std::array<std::string_view, 2> mandatory_metadata{"XISF:CreationTime", "XISF:CreatorApplication"};

// The TimePoint that text holds, or nothing when it is not a TimePoint value.
std::optional<date_time> time_point_held(std::string_view text)
{
    try {
        return parse_time_point(text);
    } catch (const invalid_data_error&) {
        return std::nullopt;
    }
}

// The properties of one object, as they are associated with it, each identifier once, and the Property element of each.
// The identifiers include those of the properties whose values are left in their data blocks.
struct property_set
{
    std::vector<property> items{};
    std::unordered_set<std::string> ids{};
    std::vector<std::size_t> elements{};
    std::vector<std::string> deferred_ids{};
    // The element that associated each Property element with the object, by the index of the Property element.
    std::unordered_map<std::size_t, std::size_t> associations{};
    // The standalone properties, whose identifiers need not be unique, since they belong to no object (spec §8.4).
    bool standalone = false;
};

// A Property element as read: its property, or nothing when it is unavailable or its value is left in its data block,
// whose identifier is then kept.
struct read_property
{
    std::optional<property> item{};
    std::string deferred_id{};
};

class property_reader
{
public:
    property_reader(const unit_outline& outline, const std::vector<data_block>& blocks, value_reader& values,
                    ancillary_budget& budget, diagnostic_log& log)
        : outline_(outline), blocks_(blocks), values_(values), budget_(budget), log_(log),
          slots_(outline.elements.size(), no_slot)
    {}

    unit_properties read()
    {
        read_elements();
        count_uses();
        solution_checked_.assign(properties_.size(), false);
        image_value_checked_.assign(properties_.size(), false);
        unit_properties result;
        result.metadata = metadata();
        element_properties standalone = collect(no_element);
        result.standalone = std::move(standalone.properties);
        result.standalone_deferred_ids = std::move(standalone.deferred_ids);
        for (std::size_t index = 0; index < outline_.elements.size(); ++index) {
            const element_kind kind = outline_.elements[index].kind;
            if (kind == element_kind::image || kind == element_kind::thumbnail) {
                result.objects.push_back(collect(index));
            }
        }
        return result;
    }

private:
    static constexpr std::size_t no_slot = std::numeric_limits<std::size_t>::max();

    // Reads every Property element once, in document order, with its block: the blocks are in document order too.
    void read_elements()
    {
        std::size_t next_block = 0;
        for (std::size_t index = 0; index < outline_.elements.size(); ++index) {
            while (next_block < blocks_.size() && blocks_[next_block].element < index) {
                ++next_block;
            }
            if (outline_.elements[index].kind != element_kind::property) {
                continue;
            }
            const data_block* block =
                next_block < blocks_.size() && blocks_[next_block].element == index ? &blocks_[next_block] : nullptr;
            slots_[index] = properties_.size();
            properties_.push_back(read_element(index, block));
        }
    }

    // The number of objects that each property is associated with: the one that contains it, and one for each
    // Reference that names it. The last association takes the property, the others copy it.
    void count_uses()
    {
        uses_.assign(properties_.size(), 1);
        for (const outline_element& element : outline_.elements) {
            if (element.kind == element_kind::reference && follows_references(element.parent) &&
                names_property(element)) {
                ++uses_[slots_[element.target]];
            }
        }
    }

    // Metadata, Image and Thumbnail elements have properties through their Reference elements.
    [[nodiscard]] bool follows_references(std::size_t owner) const noexcept
    {
        if (owner == no_element) {
            return false;
        }
        const element_kind kind = outline_.elements[owner].kind;
        return kind == element_kind::metadata || kind == element_kind::image || kind == element_kind::thumbnail;
    }

    [[nodiscard]] bool names_property(const outline_element& reference) const noexcept
    {
        return reference.target != no_element && outline_.elements[reference.target].kind == element_kind::property;
    }

    // Calls visit with the index of every element that owner contains directly, in document order. no_element is the
    // root element.
    template <typename Visit> void for_each_child(std::size_t owner, const Visit& visit) const
    {
        const std::size_t first = owner == no_element ? 0 : owner + 1;
        const std::size_t end = owner == no_element ? outline_.elements.size() : outline_.elements[owner].end;
        for (std::size_t index = first; index < end; ++index) {
            if (outline_.elements[index].parent == owner) {
                visit(index);
            }
        }
    }

    // The property that the element at index contains or names, if any, and the index of the Property element.
    [[nodiscard]] std::optional<std::size_t> property_of(std::size_t index) const noexcept
    {
        const outline_element& element = outline_.elements[index];
        if (element.kind == element_kind::property) {
            return index;
        }
        if (element.kind == element_kind::reference && follows_references(element.parent) && names_property(element)) {
            return element.target;
        }
        return std::nullopt;
    }

    // Associates the property of the Property element at target with an object, through the element at by: the
    // Property element itself, or a Reference that names it.
    void add(property_set& set, std::size_t target, std::size_t by, std::string_view owner)
    {
        const std::size_t slot = slots_[target];
        std::optional<property>& found = properties_[slot].item;
        const std::string& id = found ? found->id : properties_[slot].deferred_id;
        const bool last_use = --uses_[slot] == 0;
        if (id.empty()) {
            return;
        }
        // A Property element that the object contains and names, or names twice, is one property of it: the
        // Reference that associates it again, after or before the element itself, is redundant.
        if (const auto [earlier, added] = set.associations.try_emplace(target, by); !added) {
            log_.warning(errc::invalid_reference,
                         "the Reference names " + outline_.path(target) + ", which is already associated with " +
                             std::string(owner) + ": it is the same property, associated once",
                         {.element = outline_.path(by == target ? earlier->second : by), .attribute = "ref"});
            return;
        }
        const error_context context{.element = outline_.path(by), .attribute = by == target ? "id" : "ref"};
        if (set.ids.contains(id)) {
            if (set.standalone) {
                log_.warning(errc::duplicate_property_id,
                             "another standalone property has the identifier " + quote(id) +
                                 ", so only the first is listed with them; a Reference to this one still associates it",
                             context);
            } else {
                log_.error(errc::duplicate_property_id,
                           "another property of " + std::string(owner) + " has the identifier " + quote(id), context);
            }
            return;
        }
        if (!found) {
            // A value left in its data block holds the place of its property, which a full open would read.
            set.ids.insert(id);
            set.deferred_ids.push_back(id);
            return;
        }
        if (!last_use && !charge_copy(*found, context)) {
            return;
        }
        set.ids.insert(found->id);
        set.elements.push_back(target);
        if (last_use) {
            set.items.push_back(std::move(*found));
        } else {
            set.items.push_back(*found);
        }
    }

    // A copy of a property counts against the budget, so that a large property named by many References cannot exhaust
    // the memory. False when it is beyond the budget, which is recorded.
    bool charge_copy(const property& item, const error_context& context)
    {
        return budget_.copy(held_size(item), "the property " + quote(item.id), context, log_);
    }

    // The properties of an Image or Thumbnail element, or of the root element (no_element), which has no path.
    element_properties collect(std::size_t owner)
    {
        const std::string name = owner == no_element ? "the root element" : outline_.path(owner);
        property_set set{.standalone = owner == no_element};
        for_each_child(owner, [&](std::size_t index) {
            if (const std::optional<std::size_t> target = property_of(index)) {
                add(set, *target, index, name);
            }
        });
        check_solution_types(set);
        if (owner != no_element) {
            check_image_values(set);
        }
        return {.element = owner,
                .path = owner == no_element ? std::string() : name,
                .properties = property_list(std::move(set.items)),
                .deferred_ids = std::move(set.deferred_ids)};
    }

    // The properties of the unit: those of its Metadata elements, of which there should be exactly one (spec §11.4).
    property_list metadata()
    {
        property_set set;
        std::size_t first = no_element;
        for_each_child(no_element, [&](std::size_t owner) {
            if (outline_.elements[owner].kind != element_kind::metadata) {
                return;
            }
            if (first == no_element) {
                first = owner;
            } else {
                log_.warning(errc::invalid_metadata,
                             "the unit has more than one Metadata element; their properties are all read",
                             {.element = outline_.path(owner)});
            }
            for_each_child(owner, [&](std::size_t index) { add_metadata(set, index); });
        });

        if (first == no_element) {
            log_.warning(errc::invalid_metadata, "the unit has no Metadata element",
                         {.element = outline_.path(no_element)});
        } else {
            for (const std::string_view id : mandatory_metadata) {
                if (!set.ids.contains(std::string(id))) {
                    log_.warning(errc::invalid_metadata, "the Metadata element has no " + std::string(id) + " property",
                                 {.element = outline_.path(first)});
                }
            }
        }
        check_solution_types(set);
        return property_list(std::move(set.items));
    }

    void add_metadata(property_set& set, std::size_t index)
    {
        const outline_element& element = outline_.elements[index];
        if (element.kind == element_kind::reference && !names_property(element)) {
            if (element.target != no_element) {
                log_.warning(errc::invalid_metadata,
                             "the Reference names " + outline_.path(element.target) +
                                 ", which is not a property, and is ignored",
                             {.element = outline_.path(index), .attribute = "ref"});
            }
            return;
        }
        const std::optional<std::size_t> target = property_of(index);
        if (!target) {
            return;
        }
        const read_property& found = properties_[slots_[*target]];
        const std::string& id = found.item ? found.item->id : found.deferred_id;
        if (!id.empty() && !is_metadata_id(id)) {
            log_.warning(errc::invalid_metadata,
                         "the identifier " + quote(id) +
                             " is not in the XISF namespace, as those of the properties of the Metadata element are",
                         {.element = outline_.path(index), .attribute = index == *target ? "id" : "ref"});
        }
        add(set, *target, index, "the unit");
    }

    // -----------------------------------------------------------------------------------------------------------------
    // One Property element

    read_property read_element(std::size_t index, const data_block* block)
    {
        const pugi::xml_node node = outline_.elements[index].node;
        const std::string path = outline_.path(index);
        const pugi::xml_attribute id = node.attribute("id");
        if (id.empty() || *id.value() == '\0') {
            log_.error(errc::invalid_property_id,
                       id.empty() ? "the Property element has no id attribute" : "the property identifier is empty",
                       {.element = path, .attribute = "id"});
            return {};
        }
        if (!is_property_id(id.value())) {
            log_.warning(errc::invalid_property_id, quote(id.value()) + " is not a property identifier",
                         {.element = path, .attribute = "id"});
        }

        const pugi::xml_attribute type = node.attribute("type");
        if (type.empty()) {
            log_.error(errc::invalid_property, "the Property element has no type attribute",
                       {.element = path, .attribute = "type"});
            return {};
        }
        const std::optional<property_type> named = property_type_named(type.value());
        if (!named) {
            log_.error(errc::unsupported_property_type, quote(type.value()) + " is not a property type",
                       {.element = path, .attribute = "type"});
            return {};
        }

        const value_element element{.node = node, .path = path, .block = block, .type = *named};
        if (values_.defers(element)) {
            return {.deferred_id = id.value()};
        }
        std::optional<property_value> value = values_.read(element);
        if (!value) {
            return {};
        }
        property result{.id = id.value(), .value = std::move(*value), .comment = node.attribute("comment").value()};
        // Those of an astrometric solution depend on its revision, which check_solution_types() knows.
        if (!is_astrometric_solution_id(result.id) && !read_time_point_held_as_string(result, path)) {
            check_reserved_type(element.type, path, result.id);
        }
        // Against the type of the value read, which is TimePoint for one held as a String.
        result.format = read_format(node, path, result.value.type());
        return {.item = std::move(result)};
    }

    // A reserved TimePoint property written as a String that holds a TimePoint, as PixInsight writes
    // XISF:CreationTime, is read as that TimePoint, which an application written against the specification expects of
    // it, and the deviation is recorded. True when the value was replaced.
    bool read_time_point_held_as_string(property& item, const std::string& path)
    {
        if (item.value.type() != property_type::string ||
            reserved_property_type(item.id) != property_type::time_point) {
            return false;
        }
        const std::optional<date_time> time = time_point_held(item.value.get<std::string>());
        if (!time) {
            return false;
        }
        log_.info(errc::reserved_property_type,
                  "the property " + quote(item.id) +
                      " is a TimePoint property written as a String, and is read as the TimePoint that it holds",
                  {.element = path, .attribute = "type"});
        item.value = property_value(*time);
        return true;
    }

    // The reserved identifiers have the types of the specification (spec §11.4, §11.5.3). Another type is tolerated.
    void check_reserved_type(property_type type, const std::string& path, const std::string& id)
    {
        const std::optional<property_type> reserved = reserved_property_type(id);
        if (reserved && *reserved != type) {
            log_.warning(errc::reserved_property_type,
                         "the property " + quote(id) + " is a " + std::string(property_type_name(*reserved)) +
                             " property, but has the type " + std::string(property_type_name(type)),
                         {.element = path, .attribute = "type"});
        }
    }

    // The properties of the AstrometricSolution namespace have the types of revision 1.x of spec §11.5.3.7, so they are
    // checked for each object, unless the solution of the object is of another revision (spec §11.5.3.7.6). A Property
    // element that several objects have is checked once.
    void check_solution_types(const property_set& set)
    {
        const auto version = std::ranges::find(set.items, "AstrometricSolution:Version", &property::id);
        if (version != set.items.end() && is_foreign_astrometric_version(version->value)) {
            return;
        }
        for (std::size_t i = 0; i < set.items.size(); ++i) {
            const std::size_t slot = slots_[set.elements[i]];
            if (!is_astrometric_solution_id(set.items[i].id) || solution_checked_[slot]) {
                continue;
            }
            solution_checked_[slot] = true;
            check_reserved_type(set.items[i].value.type(), outline_.path(set.elements[i]), set.items[i].id);
        }
    }

    // The values of the Image namespace of an image or a thumbnail that spec §11.5.3.5 forbids are kept, with a warning
    // about each Property element once, as a reader of 1.0.0 kept them without one, and about the object for a
    // subgroup without a group.
    void check_image_values(const property_set& set)
    {
        for (std::size_t i = 0; i < set.items.size(); ++i) {
            const std::size_t slot = slots_[set.elements[i]];
            if (image_value_checked_[slot]) {
                continue;
            }
            image_value_checked_[slot] = true;
            if (const std::optional<std::string> problem = forbidden_image_value(set.items[i])) {
                log_.warning(errc::invalid_property, *problem + "; it is kept",
                             {.element = outline_.path(set.elements[i]), .attribute = "value"});
            }
        }
        if (set.ids.contains("Image:SubgroupId") && !set.ids.contains("Image:GroupId")) {
            // A subgroup whose value is left in its data block has no element here; a full open reports it.
            const auto subgroup = std::ranges::find(set.items, "Image:SubgroupId", &property::id);
            if (subgroup != set.items.end()) {
                const auto index = static_cast<std::size_t>(subgroup - set.items.begin());
                log_.warning(errc::invalid_property, std::string(subgroup_without_group) + "; it is kept",
                             {.element = outline_.path(set.elements[index]), .attribute = "id"});
            }
        }
    }

    // A format specifier only says how to show the value, so a malformed one is dropped (spec §8.4.3). TimePoint
    // properties have none (spec §8.4.3.1).
    std::optional<property_format> read_format(const pugi::xml_node& node, const std::string& path, property_type type)
    {
        const pugi::xml_attribute format = node.attribute("format");
        if (format.empty()) {
            return std::nullopt;
        }
        const error_context context{.element = path, .attribute = "format"};
        if (type == property_type::time_point) {
            log_.warning(errc::invalid_format_specifier,
                         "a TimePoint property has no format specifier, so it is ignored", context);
            return std::nullopt;
        }
        try {
            return parse_format_specifier(format.value());
        } catch (const invalid_data_error& failure) {
            log_.warning(failure.code(), std::string(failure.what()) + "; it is ignored", context);
        }
        return std::nullopt;
    }

    const unit_outline& outline_;
    const std::vector<data_block>& blocks_;
    value_reader& values_;
    ancillary_budget& budget_;
    diagnostic_log& log_;
    // The slot of each Property element of the outline in properties_.
    std::vector<std::size_t> slots_;
    // Each Property element as read.
    std::vector<read_property> properties_{};
    // How many associations each property has left.
    std::vector<std::size_t> uses_{};
    // Whether the type of each property of an astrometric solution has been checked.
    std::vector<bool> solution_checked_{};
    // Whether the value of each property of an image or a thumbnail has been checked against spec §11.5.3.5.
    std::vector<bool> image_value_checked_{};
};

} // namespace

unit_properties read_properties(const unit_outline& outline, const std::vector<data_block>& blocks,
                                value_reader& values, ancillary_budget& budget, diagnostic_log& log)
{
    return property_reader(outline, blocks, values, budget, log).read();
}

} // namespace openxisf::detail
