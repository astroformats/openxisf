// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/images.h"

#include <openxisf/error.h>

#include "core/quote.h"
#include "core/uuid.h"
#include "model/held_size.h"
#include "model/image_attributes.h"
#include "model/pixel_layout.h"
#include "model/shared_objects.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace openxisf::detail {

namespace {

constexpr std::size_t no_index = std::numeric_limits<std::size_t>::max();

bool is_floating_point(sample_format format) noexcept
{
    return format == sample_format::float32 || format == sample_format::float64;
}

// The elements that can be associated with an Image or Thumbnail element, besides properties, which the properties of
// the unit associate: a thumbnail has no colour filter array or thumbnail of its own (spec §11.12).
bool associable(element_kind owner, element_kind kind) noexcept
{
    switch (kind) {
    case element_kind::table:
    case element_kind::fits_keyword:
    case element_kind::icc_profile:
    case element_kind::rgb_working_space:
    case element_kind::display_function:
    case element_kind::resolution:
        return true;
    case element_kind::color_filter_array:
    case element_kind::thumbnail:
        return owner == element_kind::image;
    default:
        return false;
    }
}

std::string_view described_as(element_kind kind) noexcept
{
    switch (kind) {
    case element_kind::icc_profile:
        return "an ICC profile";
    case element_kind::rgb_working_space:
        return "an RGB working space";
    case element_kind::display_function:
        return "a display function";
    case element_kind::color_filter_array:
        return "a colour filter array";
    case element_kind::resolution:
        return "a resolution";
    case element_kind::thumbnail:
        return "a thumbnail";
    default:
        return "an element";
    }
}

thumbnail make_thumbnail(image_info info, std::vector<std::byte> pixels)
{
    return {.geometry = std::move(info.geometry),
            .sample_format = info.sample_format,
            .color_space = info.color_space,
            .pixel_storage = info.pixel_storage,
            .image_type = info.image_type,
            .offset = info.offset,
            .orientation = info.orientation,
            .id = std::move(info.id),
            .uuid = std::move(info.uuid),
            .properties = std::move(info.properties),
            .tables = std::move(info.tables),
            .fits_keywords = std::move(info.fits_keywords),
            .icc_profile = std::move(info.icc_profile),
            .rgb_working_space = std::move(info.rgb_working_space),
            .display_function = std::move(info.display_function),
            .resolution = info.resolution,
            .pixels = std::move(pixels)};
}

// An Image or Thumbnail element that could be read.
struct read_image
{
    image_info info{};
    /// The element in the outline, and the index of its data block.
    std::size_t element = no_index;
    std::size_t block = no_index;
    /// The pixel data of a thumbnail.
    std::vector<std::byte> pixels{};
    /// The identifiers of the properties of the object whose values are left in their data blocks.
    std::vector<std::string> deferred_ids{};
};

// The identifiers of the properties and tables of an object, which has each once (spec §8.4.1), gathered when its first
// table comes: those of its properties, also of those whose values are left in their data blocks, and of its tables.
class object_ids
{
public:
    object_ids(const property_list& properties, const std::vector<std::string>& deferred)
        : properties_(properties), deferred_(deferred)
    {}

    [[nodiscard]] bool contains(const std::string& id)
    {
        gather();
        return ids_.contains(id);
    }

    void insert(const std::string& id)
    {
        gather();
        ids_.insert(id);
    }

    /// Records that the element at by associates the Table element at target with the object, and returns the element
    /// that associated it before, when one did: a table contained and named, or named twice, is one table of it.
    std::optional<std::size_t> associate_table(std::size_t target, std::size_t by)
    {
        const auto [earlier, added] = tables_.try_emplace(target, by);
        if (added) {
            return std::nullopt;
        }
        return earlier->second;
    }

private:
    void gather()
    {
        if (gathered_) {
            return;
        }
        for (const property& item : properties_) {
            ids_.insert(item.id);
        }
        ids_.insert(deferred_.begin(), deferred_.end());
        gathered_ = true;
    }

    const property_list& properties_;
    const std::vector<std::string>& deferred_;
    std::unordered_set<std::string> ids_{};
    std::unordered_map<std::size_t, std::size_t> tables_{};
    bool gathered_ = false;
};

class image_reader
{
public:
    image_reader(const unit_outline& outline, const std::vector<data_block>& blocks, unit_objects& objects,
                 const thread_safe_source& source, const limits& limits, ancillary_budget& budget, bool load_blocks,
                 diagnostic_log& log)
        : outline_(outline), blocks_(blocks), properties_(objects.properties),
          tables_(std::move(objects.tables.tables)), deferred_tables_(std::move(objects.tables.deferred_ids)),
          keywords_(std::move(objects.ancillary.keywords)), icc_profiles_(std::move(objects.ancillary.icc_profiles)),
          working_spaces_(std::move(objects.ancillary.working_spaces)),
          display_functions_(std::move(objects.ancillary.display_functions)),
          filters_(std::move(objects.ancillary.filters)), resolutions_(std::move(objects.ancillary.resolutions)),
          source_(source), limits_(limits), budget_(budget), load_blocks_(load_blocks), log_(log),
          block_of_(outline.elements.size(), no_index), slot_of_(outline.elements.size(), no_index)
    {}

    unit_images read()
    {
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            block_of_[blocks_[i].element] = i;
        }
        read_elements();
        count_uses();
        // Thumbnails are complete before the images take them.
        std::unordered_map<std::size_t, thumbnail> thumbnails;
        for (read_image& image : images_) {
            if (kind_of(image.element) == element_kind::thumbnail) {
                associate(image);
                thumbnails.emplace(image.element, make_thumbnail(std::move(image.info), std::move(image.pixels)));
            }
        }
        thumbnails_ = shared_objects<thumbnail>(std::move(thumbnails));
        count_thumbnail_uses();
        for (read_image& image : images_) {
            if (kind_of(image.element) == element_kind::image) {
                associate(image);
            }
        }
        check_root_references();
        unit_images result = list();
        result.tables = standalone_tables();
        return result;
    }

private:
    [[nodiscard]] element_kind kind_of(std::size_t index) const
    {
        return outline_.elements[index].kind;
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

    // Every Image and Thumbnail element once, in document order, with the properties associated with it.
    void read_elements()
    {
        std::unordered_map<std::size_t, std::size_t> object_of;
        for (std::size_t i = 0; i < properties_.objects.size(); ++i) {
            object_of.emplace(properties_.objects[i].element, i);
        }
        for (std::size_t index = 0; index < outline_.elements.size(); ++index) {
            const element_kind kind = kind_of(index);
            if (kind != element_kind::image && kind != element_kind::thumbnail) {
                continue;
            }
            std::optional<read_image> image = read_element(index);
            if (!image) {
                continue;
            }
            if (const auto found = object_of.find(index); found != object_of.end()) {
                image->info.properties = std::move(properties_.objects[found->second].properties);
                image->deferred_ids = std::move(properties_.objects[found->second].deferred_ids);
            }
            if (kind == element_kind::image) {
                check_id(*image);
            }
            slot_of_[index] = images_.size();
            images_.push_back(std::move(*image));
        }
        properties_.objects.clear();
    }

    // Spec §11.5.2: no two Image elements of a unit have the same id. The image keeps its id, which only names it.
    void check_id(const read_image& image)
    {
        if (image.info.id.empty()) {
            return;
        }
        const auto [found, inserted] = first_with_id_.try_emplace(image.info.id, image.element);
        if (!inserted) {
            log_.warning(errc::duplicate_image_id,
                         "the id " + quote(image.info.id) + " is already the id of " + outline_.path(found->second),
                         {.element = outline_.path(image.element), .attribute = "id"});
        }
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Associations

    // The object that an element of owner associates with it: the element itself, or the element that it names when it
    // is a Reference. Nothing for elements that associate nothing, which the associations report.
    [[nodiscard]] std::optional<std::size_t> associated_by(element_kind owner, std::size_t index) const
    {
        std::size_t target = index;
        if (kind_of(index) == element_kind::reference) {
            target = outline_.elements[index].target;
            if (target == no_element) {
                return std::nullopt;
            }
        }
        if (!associable(owner, kind_of(target))) {
            return std::nullopt;
        }
        return target;
    }

    // Each use of an object by an image or thumbnail, and of a table by the root element, so that the last one takes
    // it.
    void count_uses()
    {
        for (const read_image& image : images_) {
            const element_kind owner = kind_of(image.element);
            for_each_child(image.element, [&](std::size_t index) {
                if (const std::optional<std::size_t> target = associated_by(owner, index)) {
                    add_use(*target);
                }
            });
        }
        for_each_child(no_element, [&](std::size_t index) {
            if (kind_of(index) == element_kind::table) {
                tables_.add_use(index);
            }
        });
    }

    void count_thumbnail_uses()
    {
        for (const read_image& image : images_) {
            if (kind_of(image.element) != element_kind::image) {
                continue;
            }
            for_each_child(image.element, [&](std::size_t index) {
                const std::optional<std::size_t> target = associated_by(element_kind::image, index);
                if (target && kind_of(*target) == element_kind::thumbnail) {
                    thumbnails_.add_use(*target);
                }
            });
        }
    }

    void add_use(std::size_t target)
    {
        switch (kind_of(target)) {
        case element_kind::table:
            tables_.add_use(target);
            break;
        case element_kind::fits_keyword:
            keywords_.add_use(target);
            break;
        case element_kind::icc_profile:
            icc_profiles_.add_use(target);
            break;
        case element_kind::rgb_working_space:
            working_spaces_.add_use(target);
            break;
        case element_kind::display_function:
            display_functions_.add_use(target);
            break;
        case element_kind::color_filter_array:
            filters_.add_use(target);
            break;
        case element_kind::resolution:
            resolutions_.add_use(target);
            break;
        default:
            // Thumbnails are counted once they are made.
            break;
        }
    }

    // Gives the image or thumbnail what its elements contain and name, in document order.
    void associate(read_image& image)
    {
        const element_kind owner = kind_of(image.element);
        // The kinds of which the object has taken one already, and the identifiers of its properties and tables.
        std::unordered_set<element_kind> taken;
        object_ids ids(image.info.properties, image.deferred_ids);
        for_each_child(image.element, [&](std::size_t index) {
            if (kind_of(index) == element_kind::reference) {
                check_reference(owner, index);
            }
            if (const std::optional<std::size_t> target = associated_by(owner, index)) {
                attach(image.info, owner, *target, index, taken, ids);
            }
        });
    }

    // A Reference of an Image or Thumbnail element names an object of it, or a property.
    void check_reference(element_kind owner, std::size_t index)
    {
        const std::size_t target = outline_.elements[index].target;
        if (target == no_element || kind_of(target) == element_kind::property || associable(owner, kind_of(target))) {
            return;
        }
        log_.warning(errc::invalid_reference,
                     std::string(owner == element_kind::image ? "an image" : "a thumbnail") +
                         " cannot be associated with the " + std::string(element_name(kind_of(target))) + " element " +
                         outline_.path(target) + "; the Reference is ignored",
                     {.element = outline_.path(index), .attribute = "ref"});
    }

    void attach(image_info& info, element_kind owner, std::size_t target, std::size_t by,
                std::unordered_set<element_kind>& taken, object_ids& ids)
    {
        const error_context context{.element = outline_.path(by), .attribute = by == target ? "" : "ref"};
        const std::string what = "the " + std::string(element_name(kind_of(target))) + " " + outline_.path(target);
        const element_kind kind = kind_of(target);
        if (kind == element_kind::table) {
            if (const std::optional<std::size_t> earlier = ids.associate_table(target, by)) {
                log_.warning(errc::invalid_reference,
                             "the Reference names " + what +
                                 ", which is already associated with the object: it is the same table, associated "
                                 "once",
                             {.element = outline_.path(by == target ? *earlier : by), .attribute = "ref"});
                tables_.release(target);
                return;
            }
            if (std::optional<table> item = take_table(target, ids, "the object", what, context)) {
                info.tables.push_back(std::move(*item));
            }
            return;
        }
        if (kind == element_kind::fits_keyword) {
            if (std::optional<fits_keyword> keyword = keywords_.take(target, what, context, budget_, log_)) {
                info.fits_keywords.push_back(std::move(*keyword));
            }
            return;
        }
        // The other kinds are once per object: a second one is ignored.
        if (taken.contains(kind)) {
            if (is_read(kind, target)) {
                log_.warning(errc::duplicate_element,
                             std::string(owner == element_kind::image ? "the image" : "the thumbnail") +
                                 " already has " + std::string(described_as(kind)) + ", so " + what + " is ignored",
                             context);
            }
            release(kind, target);
            return;
        }
        if (kind == element_kind::color_filter_array && info.geometry.dimensions.size() != 2) {
            log_.warning(errc::invalid_color_filter_array,
                         "a colour filter array is associated with a two-dimensional image, so " + what + " is ignored",
                         context);
            release(kind, target);
            return;
        }
        if (is_read(kind, target)) {
            taken.insert(kind);
        }
        take_single(info, kind, target, what, context);
    }

    // The table of the Table element at target for an object with the identifiers ids, which gets its identifier, owner
    // in messages, or for the standalone properties when owner is empty. Nothing when the table cannot be read, has the
    // identifier of another property of the object, which is an error and costs no copy, or of another standalone
    // property, which is a warning, since standalone properties belong to no object (spec §8.4), or is a copy beyond
    // the budget; and nothing for a table that a cell leaves in its data block, whose identifier holds its place.
    std::optional<table> take_table(std::size_t target, object_ids& ids, std::string_view owner,
                                    const std::string& what, const error_context& context)
    {
        const table* found = tables_.find(target);
        const auto deferred = deferred_tables_.find(target);
        if (found == nullptr && deferred == deferred_tables_.end()) {
            return std::nullopt;
        }
        const std::string& id = found != nullptr ? found->id : deferred->second;
        if (ids.contains(id)) {
            const error_context at{.element = context.element, .attribute = context.attribute.empty() ? "id" : "ref"};
            if (owner.empty()) {
                log_.warning(errc::duplicate_property_id,
                             "another standalone property has the identifier " + quote(id) +
                                 ", so only the first is listed with them; a Reference to " + what +
                                 " still associates it",
                             at);
            } else {
                log_.error(errc::duplicate_property_id,
                           "another property of " + std::string(owner) + " has the identifier " + quote(id) + ", so " +
                               what + " is not associated with it",
                           at);
            }
            tables_.release(target);
            return std::nullopt;
        }
        if (found == nullptr) {
            ids.insert(id);
            return std::nullopt;
        }
        std::optional<table> item = tables_.take(target, what, context, budget_, log_);
        if (item) {
            ids.insert(item->id);
        }
        return item;
    }

    [[nodiscard]] bool is_read(element_kind kind, std::size_t target) const
    {
        switch (kind) {
        case element_kind::icc_profile:
            return icc_profiles_.contains(target);
        case element_kind::rgb_working_space:
            return working_spaces_.contains(target);
        case element_kind::display_function:
            return display_functions_.contains(target);
        case element_kind::color_filter_array:
            return filters_.contains(target);
        case element_kind::resolution:
            return resolutions_.contains(target);
        case element_kind::thumbnail:
            return thumbnails_.contains(target);
        default:
            return false;
        }
    }

    void release(element_kind kind, std::size_t target)
    {
        switch (kind) {
        case element_kind::icc_profile:
            icc_profiles_.release(target);
            break;
        case element_kind::rgb_working_space:
            working_spaces_.release(target);
            break;
        case element_kind::display_function:
            display_functions_.release(target);
            break;
        case element_kind::color_filter_array:
            filters_.release(target);
            break;
        case element_kind::resolution:
            resolutions_.release(target);
            break;
        case element_kind::thumbnail:
            thumbnails_.release(target);
            break;
        default:
            break;
        }
    }

    void take_single(image_info& info, element_kind kind, std::size_t target, const std::string& what,
                     const error_context& context)
    {
        switch (kind) {
        case element_kind::icc_profile:
            if (std::optional<std::vector<std::byte>> profile =
                    icc_profiles_.take(target, what, context, budget_, log_)) {
                info.icc_profile = std::move(*profile);
            }
            break;
        case element_kind::rgb_working_space:
            info.rgb_working_space = working_spaces_.take(target, what, context, budget_, log_);
            break;
        case element_kind::display_function:
            info.display_function = display_functions_.take(target, what, context, budget_, log_);
            break;
        case element_kind::color_filter_array:
            info.color_filter_array = filters_.take(target, what, context, budget_, log_);
            break;
        case element_kind::resolution:
            info.resolution = resolutions_.take(target, what, context, budget_, log_);
            break;
        case element_kind::thumbnail:
            info.thumbnail = thumbnails_.take(target, what, context, budget_, log_);
            break;
        default:
            break;
        }
    }

    // Spec §11.13: a Reference of the root element lists an image again.
    void check_root_references()
    {
        for_each_child(no_element, [&](std::size_t index) {
            const outline_element& element = outline_.elements[index];
            if (element.kind == element_kind::reference && element.target != no_element &&
                kind_of(element.target) != element_kind::image) {
                log_.warning(errc::invalid_reference,
                             "a Reference of the root element lists an image again, but " +
                                 outline_.path(element.target) + " is not an Image element; it is ignored",
                             {.element = outline_.path(index), .attribute = "ref"});
            }
        });
    }

    // -----------------------------------------------------------------------------------------------------------------
    // The list of images

    // An entry of the list of images: the image read from an Image element, and the element that lists it, the Image
    // element itself or a Reference.
    struct listed_image
    {
        std::size_t slot = no_index;
        std::size_t element = no_index;
    };

    // The images in document order: the Image elements of the root element, and the images that its Reference elements
    // name. Each image listed again is counted against the budget before anything is allocated for it, with the size
    // of each image computed once, and copied while every image is intact; then each Image element moves its own image
    // into its place.
    unit_images list()
    {
        std::vector<listed_image> order;
        std::vector<std::optional<std::uint64_t>> sizes(images_.size());
        for_each_child(no_element, [&](std::size_t index) {
            const outline_element& element = outline_.elements[index];
            const std::size_t named = element.kind == element_kind::reference ? element.target : index;
            if (named == no_element || slot_of_[named] == no_index || kind_of(named) != element_kind::image ||
                (element.kind != element_kind::image && element.kind != element_kind::reference)) {
                return;
            }
            const read_image& image = images_[slot_of_[named]];
            if (index != image.element) {
                std::optional<std::uint64_t>& size = sizes[slot_of_[named]];
                if (!size) {
                    size = held_size(image.info);
                }
                if (!budget_.copy(*size, "the image", {.element = outline_.path(index), .attribute = "ref"}, log_)) {
                    return;
                }
            }
            order.push_back({.slot = slot_of_[named], .element = index});
        });

        unit_images result;
        result.infos.reserve(order.size());
        result.blocks.reserve(order.size());
        for (const listed_image& entry : order) {
            const read_image& image = images_[entry.slot];
            if (entry.element == image.element) {
                result.infos.emplace_back();
            } else {
                result.infos.push_back(image.info);
            }
            result.blocks.push_back(image.block);
        }
        for (std::size_t i = 0; i < order.size(); ++i) {
            read_image& image = images_[order[i].slot];
            if (order[i].element == image.element) {
                result.infos[i] = std::move(image.info);
            }
        }
        return result;
    }

    // The Table elements of the root element are standalone properties (spec §11.3), each identifier listed once among
    // them.
    std::vector<table> standalone_tables()
    {
        std::vector<table> result;
        object_ids ids(properties_.standalone, properties_.standalone_deferred_ids);
        for_each_child(no_element, [&](std::size_t index) {
            if (kind_of(index) != element_kind::table) {
                return;
            }
            const std::string what = "the table " + outline_.path(index);
            if (std::optional<table> item = take_table(index, ids, {}, what, {.element = outline_.path(index)})) {
                result.push_back(std::move(*item));
            }
        });
        return result;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // One Image or Thumbnail element

    std::optional<read_image> read_element(std::size_t index)
    {
        element_ = index;
        kind_ = kind_of(index);
        node_ = outline_.elements[index].node;
        path_ = outline_.path(index);
        read_image image{.element = index};
        image_info& info = image.info;

        // The attributes that give the samples their meaning: an image without them, or with values that cannot be
        // read, cannot be used.
        const std::optional<geometry> size = required("geometry", parse_geometry);
        if (!size) {
            return std::nullopt;
        }
        info.geometry = *size;
        const std::optional<sample_format> format = required("sampleFormat", parse_sample_format);
        if (!format) {
            return std::nullopt;
        }
        info.sample_format = *format;
        const std::optional<std::uint64_t> bytes = pixel_data_size(info.geometry, info.sample_format);
        if (!bytes) {
            log_.error(errc::invalid_geometry,
                       "the geometry and the sample format give more bytes of pixel data than 64 bits can count",
                       context("geometry"));
            return std::nullopt;
        }
        if (!read_optional("colorSpace", parse_color_space, info.color_space) || !check_channels(info) ||
            !read_optional("pixelStorage", parse_pixel_storage, info.pixel_storage) ||
            !read_optional("offset", parse_offset, info.offset) || !find_data(*bytes, image.block)) {
            return std::nullopt;
        }
        if (kind_ == element_kind::thumbnail && (!check_thumbnail(info) || !load_pixels(image))) {
            return std::nullopt;
        }

        // The attributes that only describe the image: a problem with one of them leaves the image usable.
        if (info.offset < 0.0) {
            log_.warning(errc::invalid_image, "the offset is negative, and an offset is at least zero",
                         context("offset"));
        }
        read_bounds(info);
        info.image_type = tolerated("imageType", parse_image_type);
        info.orientation = tolerated("orientation", parse_orientation).value_or(orientation::none);
        read_id(info);
        read_uuid(info);
        return image;
    }

    [[nodiscard]] error_context context(const char* attribute) const
    {
        return {.element = path_, .attribute = attribute};
    }

    [[nodiscard]] std::string element_text() const
    {
        return "the " + std::string(element_name(kind_)) + " element";
    }

    // What parse returns for the text of the attribute name, or nothing when it throws the error of a malformed or
    // unsupported value, which is recorded.
    template <typename Parse>
    std::optional<std::invoke_result_t<Parse, std::string_view>> attempt(const char* name, std::string_view text,
                                                                         Parse parse)
    {
        try {
            return parse(text);
        } catch (const invalid_data_error& failure) {
            log_.error(failure.code(), failure.what(), context(name));
        } catch (const unsupported_error& failure) {
            log_.error(failure.code(), failure.what(), context(name));
        }
        return std::nullopt;
    }

    // The value of a mandatory attribute, or nothing when it is missing or cannot be read, which is recorded.
    template <typename Parse>
    std::optional<std::invoke_result_t<Parse, std::string_view>> required(const char* name, Parse parse)
    {
        const pugi::xml_attribute attribute = node_.attribute(name);
        if (attribute.empty()) {
            log_.error(errc::invalid_image, element_text() + " has no " + std::string(name) + " attribute",
                       context(name));
            return std::nullopt;
        }
        return attempt(name, attribute.value(), parse);
    }

    // Reads an optional attribute into value, which keeps its default without one. False when the attribute cannot be
    // read, which is recorded.
    template <typename T, typename Parse> bool read_optional(const char* name, Parse parse, T& value)
    {
        const pugi::xml_attribute attribute = node_.attribute(name);
        if (attribute.empty()) {
            return true;
        }
        const std::optional<T> parsed = attempt(name, attribute.value(), parse);
        if (parsed) {
            value = *parsed;
        }
        return parsed.has_value();
    }

    // The value of an attribute whose problems are tolerated: nothing when there is none, or when it cannot be read,
    // which is a warning.
    template <typename Parse>
    std::optional<std::invoke_result_t<Parse, std::string_view>> tolerated(const char* name, Parse parse)
    {
        const pugi::xml_attribute attribute = node_.attribute(name);
        if (attribute.empty()) {
            return std::nullopt;
        }
        try {
            return parse(attribute.value());
        } catch (const invalid_data_error& failure) {
            log_.warning(failure.code(), std::string(failure.what()) + "; it is ignored", context(name));
        }
        return std::nullopt;
    }

    // Spec §8.5.1: the first channels are the nominal channels of the colour space.
    bool check_channels(const image_info& info)
    {
        const std::uint64_t nominal = nominal_channels(info.color_space);
        if (info.geometry.channels >= nominal) {
            return true;
        }
        log_.error(errc::invalid_image,
                   "an image in the " + std::string(color_space_name(info.color_space)) +
                       " colour space has at least " + std::to_string(nominal) + " channels, and this one has " +
                       std::to_string(info.geometry.channels),
                   context("colorSpace"));
        return false;
    }

    // Spec §11.5: the pixel data are a single data block, whose data are exactly the samples of the geometry. An image
    // whose block is unavailable is kept; reading its pixels throws the error of its block.
    bool find_data(std::uint64_t bytes, std::size_t& block)
    {
        block = block_of_[element_];
        if (block == no_index) {
            log_.error(errc::invalid_image, element_text() + " has no data block for its pixel data",
                       context("location"));
            return false;
        }
        const data_block& data = blocks_[block];
        if (!data.descriptor) {
            return true;
        }
        const std::uint64_t size = data_size(*data.descriptor);
        if (size != bytes) {
            log_.error(errc::pixel_data_size_mismatch,
                       "the geometry and the sample format give " + std::to_string(bytes) +
                           " bytes of pixel data, but the data block has " + std::to_string(size),
                       context("geometry"));
            return false;
        }
        return true;
    }

    // Spec §11.12: a thumbnail is a two-dimensional Gray or RGB image of UInt8 or UInt16 samples, with at most one
    // alpha channel.
    bool check_thumbnail(const image_info& info)
    {
        std::string problem;
        if (info.geometry.dimensions.size() != 2) {
            problem = "a thumbnail is two-dimensional, and this one has " +
                      std::to_string(info.geometry.dimensions.size()) + " dimensions";
        } else if (info.sample_format != sample_format::uint8 && info.sample_format != sample_format::uint16) {
            problem = "a thumbnail has UInt8 or UInt16 samples, and this one has " +
                      std::string(sample_format_name(info.sample_format)) + " samples";
        } else if (info.color_space != color_space::gray && info.color_space != color_space::rgb) {
            problem = "a thumbnail is Gray or RGB, and this one is " + std::string(color_space_name(info.color_space));
        } else if (info.geometry.channels > nominal_channels(info.color_space) + 1) {
            problem = "a thumbnail has at most one alpha channel, and this one has " +
                      std::to_string(info.geometry.channels - nominal_channels(info.color_space));
        }
        if (problem.empty()) {
            return true;
        }
        log_.error(errc::invalid_thumbnail, problem, context("geometry"));
        return false;
    }

    // The pixel data of a thumbnail are loaded with the unit, in native byte order. A thumbnail whose pixel data cannot
    // be loaded is unavailable.
    bool load_pixels(read_image& image)
    {
        const data_block& block = blocks_[image.block];
        if (!block.descriptor) {
            return false;
        }
        if (!load_blocks_) {
            return true;
        }
        std::optional<std::vector<std::byte>> data =
            load_block(source_, *block.descriptor, limits_, budget_, path_, log_);
        if (!data) {
            return false;
        }
        to_native_byte_order(*data, image.info.sample_format, block.descriptor->order);
        image.pixels = std::move(*data);
        return true;
    }

    // Spec §11.5.1: bounds are required for floating point images, and optional for the others. A thumbnail has none
    // (spec §11.12).
    void read_bounds(image_info& info)
    {
        if (kind_ == element_kind::thumbnail) {
            if (!node_.attribute("bounds").empty()) {
                log_.warning(errc::invalid_thumbnail,
                             "a thumbnail has the representable range of its sample format, so its bounds are ignored",
                             context("bounds"));
            }
            return;
        }
        info.bounds = tolerated("bounds", parse_bounds);
        if (node_.attribute("bounds").empty() && is_floating_point(info.sample_format)) {
            log_.warning(errc::invalid_image,
                         "a floating point image needs a bounds attribute, which gives its representable range",
                         context("bounds"));
        }
    }

    void read_id(image_info& info)
    {
        const pugi::xml_attribute id = node_.attribute("id");
        if (id.empty()) {
            return;
        }
        info.id = id.value();
        if (!is_unique_element_id(info.id)) {
            log_.warning(errc::invalid_image_id, quote(info.id) + " is not an image identifier", context("id"));
        }
    }

    // The canonical form accepts uppercase digits (RFC 9562); the uuid is kept in lowercase.
    void read_uuid(image_info& info)
    {
        const pugi::xml_attribute attribute = node_.attribute("uuid");
        if (attribute.empty()) {
            return;
        }
        try {
            const uuid id = parse_uuid(attribute.value());
            info.uuid = format_uuid(id);
            if (!is_version_4_uuid(id)) {
                log_.warning(errc::invalid_uuid, quote(attribute.value()) + " is not a version 4 UUID",
                             context("uuid"));
            }
        } catch (const invalid_data_error& failure) {
            log_.warning(failure.code(), std::string(failure.what()) + "; it is ignored", context("uuid"));
        }
    }

    const unit_outline& outline_;
    const std::vector<data_block>& blocks_;
    unit_properties& properties_;
    shared_objects<table> tables_;
    // The identifier of each table that a cell leaves in its data block, by the index of its element.
    std::unordered_map<std::size_t, std::string> deferred_tables_;
    shared_objects<fits_keyword> keywords_;
    shared_objects<std::vector<std::byte>> icc_profiles_;
    shared_objects<rgb_working_space> working_spaces_;
    shared_objects<display_function> display_functions_;
    shared_objects<color_filter_array> filters_;
    shared_objects<resolution> resolutions_;
    // The thumbnails, once they are made.
    shared_objects<thumbnail> thumbnails_{{}};
    const thread_safe_source& source_;
    const limits& limits_;
    ancillary_budget& budget_;
    bool load_blocks_;
    diagnostic_log& log_;
    // The data block of each element, by index in the outline, and the slot in images_ of each element read.
    std::vector<std::size_t> block_of_;
    std::vector<std::size_t> slot_of_;
    std::vector<read_image> images_{};
    // The first Image element with each id.
    std::unordered_map<std::string, std::size_t> first_with_id_{};
    // The element being read.
    std::size_t element_ = no_index;
    element_kind kind_ = element_kind::image;
    pugi::xml_node node_{};
    std::string path_{};
};

} // namespace

unit_images read_images(const unit_outline& outline, const std::vector<data_block>& blocks, unit_objects& objects,
                        const thread_safe_source& source, const limits& limits, ancillary_budget& budget,
                        bool load_blocks, diagnostic_log& log)
{
    return image_reader(outline, blocks, objects, source, limits, budget, load_blocks, log).read();
}

} // namespace openxisf::detail
