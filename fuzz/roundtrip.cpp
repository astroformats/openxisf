// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The writer and the reader together. The input is a stream of choices, from which a valid model of a unit is built:
// properties of every type, tables, images of every sample format and storage model with their pixel data, FITS
// keywords, ICC profiles, working spaces, display functions, colour filter arrays, resolutions and thumbnails; and the
// options of a writer: codec, shuffling, subblocks, checksum, alignment and the largest inline block. The unit is
// written as a monolithic file, or, by a later choice, as a distributed unit, to sinks that can rewrite and to sinks
// that cannot, and each unit must open strictly, without a diagnostic, and hold the model. The last choices are
// regions of each image, which must read as the samples of the model in them. Any exception escapes and fails the run.

#include <openxisf/color.h>
#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>
#include <openxisf/writer.h>

#include "container/block_attributes.h"
#include "core/data_encoding.h"
#include "model/ancillary_attributes.h"
#include "model/format_specifier.h"
#include "model/image_attributes.h"
#include "model/property_data.h"
#include "model/property_text.h"
#include "model/property_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace detail = openxisf::detail;
using openxisf::image_info;
using openxisf::property;
using openxisf::property_type;
using openxisf::property_value;

// Stops the run, so that the fuzzer reports the input whose unit did not read back.
void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

// The choices of the input, read in order. An exhausted input gives zeros, so every input builds a model.
class choices
{
public:
    explicit choices(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    std::uint8_t byte() noexcept
    {
        return position_ < data_.size() ? data_[position_++] : 0;
    }

    // A number below count.
    std::size_t below(std::size_t count) noexcept
    {
        return count == 0 ? 0 : byte() % count;
    }

    bool flag() noexcept
    {
        return (byte() & 1U) != 0;
    }

    std::vector<std::byte> bytes(std::size_t count)
    {
        std::vector<std::byte> result(count);
        for (std::byte& value : result) {
            value = static_cast<std::byte>(byte());
        }
        return result;
    }

    // Printable ASCII text, which every attribute can hold.
    std::string printable(std::size_t longest)
    {
        std::string text(below(longest + 1), ' ');
        for (char& c : text) {
            c = static_cast<char>(' ' + below(95));
        }
        return text;
    }

    // UTF-8 text of any character but U+0000, control characters included, which go into data blocks.
    std::string any_text(std::size_t longest)
    {
        std::string text;
        const std::size_t length = below(longest + 1);
        for (std::size_t i = 0; i < length; ++i) {
            const std::uint8_t value = byte();
            if (value == 0) {
                text += '0';
            } else if (value < 0x80U) {
                text += static_cast<char>(value);
            } else {
                // U+0080 to U+00FF, in two bytes.
                text += static_cast<char>(0xC0U | (value >> 6U));
                text += static_cast<char>(0x80U | (value & 0x3FU));
            }
        }
        return text;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t position_ = 0;
};

constexpr std::size_t type_count = static_cast<std::size_t>(property_type::c128_matrix) + 1;

template <typename T> struct is_vector : std::false_type
{};

template <typename T> struct is_vector<std::vector<T>> : std::true_type
{};

// A scalar or complex value from the bytes of its type, through a vector of one element of that type.
property_value scalar_value(property_type type, choices& input)
{
    if (type == property_type::boolean) {
        return {input.flag()};
    }
    const auto vector_type = static_cast<property_type>(static_cast<int>(property_type::i8_vector) +
                                                        static_cast<int>(type) - static_cast<int>(property_type::int8));
    const property_value vector =
        detail::decode_vector(vector_type, input.bytes(detail::value_size(type)), detail::byte_order::little);
    return std::visit(
        [&input](const auto& held) -> property_value {
            using held_type = std::remove_cvref_t<decltype(held)>;
            if constexpr (std::is_same_v<held_type, std::vector<openxisf::float128>>) {
                return property_value::from_float128_text(std::to_string(static_cast<int>(input.byte()) - 128) + "e-3");
            } else if constexpr (std::is_same_v<held_type, std::vector<openxisf::complex128>>) {
                return property_value::from_complex128_text("(" + std::to_string(input.byte()) + ",-" +
                                                            std::to_string(input.byte()) + ".5)");
            } else if constexpr (is_vector<held_type>::value) {
                return property_value(held.front());
            } else {
                return {};
            }
        },
        vector.data());
}

property_value make_value(property_type type, choices& input)
{
    switch (detail::category_of(type)) {
    case detail::type_category::scalar:
    case detail::type_category::complex:
        return scalar_value(type, input);
    case detail::type_category::string:
        return {input.any_text(20)};
    case detail::type_category::time_point:
        return {openxisf::date_time{.year = static_cast<int>(input.below(10000)),
                                    .month = static_cast<unsigned>(1 + input.below(12)),
                                    .day = static_cast<unsigned>(1 + input.below(28)),
                                    .hour = static_cast<unsigned>(input.below(24)),
                                    .minute = static_cast<unsigned>(input.below(60)),
                                    .second = static_cast<unsigned>(input.below(60)),
                                    .nanosecond = static_cast<std::uint32_t>(input.byte()) * 1'000'000U}};
    case detail::type_category::vector: {
        // Now and then one beyond the largest inline block.
        const std::size_t length = input.below(8) == 0 ? 300 + input.below(100) : input.below(6);
        const std::size_t size = detail::elements_size(type, length).value_or(0);
        return detail::decode_vector(type, input.bytes(size), detail::byte_order::little);
    }
    case detail::type_category::matrix: {
        const std::size_t rows = input.below(4);
        const std::size_t columns = input.below(4);
        const std::size_t size = detail::elements_size(type, rows * columns).value_or(0);
        return detail::decode_matrix(type, rows, columns, input.bytes(size), detail::byte_order::little);
    }
    }
    return {};
}

std::optional<openxisf::property_format> make_format(property_type type, choices& input)
{
    if (type == property_type::time_point || !input.flag()) {
        return std::nullopt;
    }
    return openxisf::property_format{.width = input.byte(),
                                     .fill = static_cast<char>('!' + input.below(26)),
                                     .precision = static_cast<std::uint32_t>(input.below(20)),
                                     .base = static_cast<openxisf::format_base>(input.below(4)),
                                     .unit = input.flag() ? "m/s" : ""};
}

openxisf::property_list make_properties(choices& input, std::string_view prefix, std::size_t most)
{
    openxisf::property_list list;
    const std::size_t count = input.below(most + 1);
    for (std::size_t i = 0; i < count; ++i) {
        const auto type = static_cast<property_type>(input.below(type_count));
        property item{.id = std::string(prefix) + ":P" + std::to_string(i), .value = make_value(type, input)};
        item.format = make_format(type, input);
        if (input.flag()) {
            item.comment = input.printable(10);
        }
        list.set(std::move(item));
    }
    return list;
}

std::vector<openxisf::table> make_tables(choices& input, std::string_view prefix)
{
    std::vector<openxisf::table> tables;
    if (!input.flag()) {
        return tables;
    }
    openxisf::table result{.id = std::string(prefix) + ":Table", .caption = input.printable(8)};
    const std::size_t fields = 1 + input.below(3);
    for (std::size_t i = 0; i < fields; ++i) {
        const auto type = static_cast<property_type>(input.below(type_count));
        result.fields.push_back({.id = "f" + std::to_string(i),
                                 .type = type,
                                 .header = input.printable(5),
                                 .format = make_format(type, input)});
    }
    const std::size_t rows = input.below(4);
    for (std::size_t r = 0; r < rows; ++r) {
        std::vector<property_value> row;
        row.reserve(result.fields.size());
        for (const openxisf::table_field& field : result.fields) {
            row.push_back(make_value(field.type, input));
        }
        result.rows.push_back(std::move(row));
    }
    tables.push_back(std::move(result));
    return tables;
}

std::vector<openxisf::fits_keyword> make_keywords(choices& input)
{
    constexpr std::array<std::string_view, 6> names{"EXPTIME", "OBJECT", "HISTORY", "COMMENT", "X_1", "DATE-OBS"};
    std::vector<openxisf::fits_keyword> keywords;
    const std::size_t count = input.below(4);
    for (std::size_t i = 0; i < count; ++i) {
        openxisf::fits_keyword keyword{.name = std::string(names[input.below(names.size())])};
        if (!detail::is_commentary_keyword(keyword.name)) {
            keyword.value = input.printable(12);
        }
        keyword.comment = input.printable(12);
        keywords.push_back(std::move(keyword));
    }
    return keywords;
}

std::vector<std::byte> make_icc_profile(choices& input)
{
    std::vector<std::byte> profile = input.bytes(128 + input.below(64));
    constexpr std::string_view signature = "acsp";
    for (std::size_t i = 0; i < signature.size(); ++i) {
        profile[36 + i] = static_cast<std::byte>(signature[i]);
    }
    return profile;
}

// The members that images and thumbnails share.
template <typename Image> void describe(Image& image, choices& input, std::string_view prefix)
{
    image.pixel_storage = input.flag() ? openxisf::pixel_storage::normal : openxisf::pixel_storage::planar;
    if (input.flag()) {
        image.image_type = static_cast<openxisf::image_type>(input.below(15));
    }
    image.offset = static_cast<double>(input.below(4)) * 0.5;
    image.orientation = static_cast<openxisf::orientation>(input.below(8));
    image.properties = make_properties(input, prefix, 3);
    image.tables = make_tables(input, prefix);
    image.fits_keywords = make_keywords(input);
    if (input.flag()) {
        image.icc_profile = make_icc_profile(input);
    }
    if (input.flag()) {
        image.rgb_working_space = input.flag() ? openxisf::rgb_working_space{.gamma = 2.2,
                                                                             .x = {0.648431, 0.230154, 0.155886},
                                                                             .y = {0.330856, 0.701572, 0.066044},
                                                                             .name = "Adobe RGB (1998)"}
                                               : openxisf::rgb_working_space{};
        openxisf::rgb_working_space& space = *image.rgb_working_space;
        space.luminance = openxisf::luminance_coefficients(space.x, space.y).value_or(space.luminance);
    }
    if (input.flag()) {
        openxisf::display_function function{.name = input.printable(6)};
        for (double& midtones : function.midtones) {
            midtones = static_cast<double>(input.below(9)) / 8.0;
        }
        image.display_function = function;
    }
    if (input.flag()) {
        image.resolution = openxisf::resolution{.horizontal = 1.0 + static_cast<double>(input.byte()),
                                                .vertical = 0.5 + static_cast<double>(input.byte()),
                                                .unit = input.flag() ? openxisf::resolution_unit::centimeter
                                                                     : openxisf::resolution_unit::inch};
    }
}

openxisf::thumbnail make_thumbnail(choices& input)
{
    openxisf::thumbnail small;
    small.color_space = input.flag() ? openxisf::color_space::rgb : openxisf::color_space::gray;
    small.sample_format = input.flag() ? openxisf::sample_format::uint16 : openxisf::sample_format::uint8;
    small.geometry = {.dimensions = {1 + input.below(6), 1 + input.below(6)},
                      .channels = openxisf::nominal_channels(small.color_space) + input.below(2)};
    describe(small, input, "Thumbnail");
    small.pixels = input.bytes(detail::pixel_data_size(small.geometry, small.sample_format).value_or(0));
    return small;
}

struct unit_model
{
    openxisf::property_list metadata{};
    openxisf::property_list properties{};
    std::vector<openxisf::table> tables{};
    std::vector<image_info> images{};
    std::vector<std::vector<std::byte>> pixels{};
};

unit_model make_model(choices& input)
{
    unit_model model;
    if (input.flag()) {
        model.metadata.set("XISF:Title", input.any_text(12));
    }
    model.properties = make_properties(input, "Standalone", 4);
    model.tables = make_tables(input, "Standalone");
    const std::size_t images = input.below(4);
    for (std::size_t i = 0; i < images; ++i) {
        image_info image;
        image.sample_format = static_cast<openxisf::sample_format>(input.below(8));
        image.color_space = static_cast<openxisf::color_space>(input.below(3));
        const std::size_t axes = 1 + input.below(3);
        for (std::size_t axis = 0; axis < axes; ++axis) {
            image.geometry.dimensions.push_back(1 + input.below(6));
        }
        image.geometry.channels = openxisf::nominal_channels(image.color_space) + input.below(3);
        const bool floating = image.sample_format == openxisf::sample_format::float32 ||
                              image.sample_format == openxisf::sample_format::float64;
        if (floating || input.flag()) {
            const auto lower = static_cast<double>(input.below(5)) - 2.0;
            image.bounds =
                openxisf::bounds{.lower = lower, .upper = lower + 1.0 + static_cast<double>(input.below(10))};
        }
        if (input.flag()) {
            image.id = "image" + std::to_string(i);
        }
        describe(image, input, "Image" + std::to_string(i));
        if (axes == 2 && input.flag()) {
            image.color_filter_array = openxisf::color_filter_array{.pattern = "RGGB", .width = 2, .height = 2};
        }
        if (input.flag()) {
            image.thumbnail = make_thumbnail(input);
        }
        model.pixels.push_back(input.bytes(image.data_size()));
        model.images.push_back(std::move(image));
    }
    return model;
}

openxisf::write_options make_options(choices& input)
{
    constexpr std::array<std::uint16_t, 5> alignments{4096, 0, 1, 7, 512};
    constexpr std::array<std::uint16_t, 3> inline_sizes{3072, 0, 16};
    openxisf::write_options options{.creator_application = "OpenXISF fuzzing 1.0",
                                    .creation_time = openxisf::date_time{.year = 2026, .month = 10, .day = 4}};
    if (input.flag()) {
        options.codec = static_cast<openxisf::codec>(input.below(4));
        options.compression_level = static_cast<int>(input.below(101));
        options.byte_shuffle = input.flag();
        options.subblock_size = input.flag() ? 0 : 1 + input.below(200);
    }
    if (input.flag()) {
        options.checksum = static_cast<openxisf::checksum_algorithm>(input.below(5));
    }
    options.block_alignment = alignments[input.below(alignments.size())];
    options.max_inline_block_size = inline_sizes[input.below(inline_sizes.size())];
    return options;
}

// A property value as text that is equal for equal values, NaNs included.
std::string canonical(const property_value& value)
{
    std::string text(detail::type_name(value.type()));
    text += '|';
    switch (detail::category_of(value.type())) {
    case detail::type_category::scalar:
    case detail::type_category::complex:
    case detail::type_category::time_point:
        return text + detail::format_value_attribute(value);
    case detail::type_category::string:
        return text + value.get<std::string>();
    case detail::type_category::vector:
    case detail::type_category::matrix:
        return text + std::to_string(value.rows()) + "x" + std::to_string(value.columns()) + "|" +
               detail::encode_hex(detail::encode_elements(value, detail::byte_order::little));
    }
    return text;
}

// A format that leaves every member at its default is not written, so both read as none.
std::string canonical(const std::optional<openxisf::property_format>& format)
{
    return format ? detail::format_specifier_text(*format) : std::string();
}

std::string canonical(const openxisf::property_list& properties)
{
    std::string text;
    for (const property& item : properties) {
        text += item.id + "|" + canonical(item.value) + "|" + canonical(item.format) + "|" + item.comment + "\n";
    }
    return text;
}

std::string canonical(const std::vector<openxisf::table>& tables)
{
    std::string text;
    for (const openxisf::table& item : tables) {
        text += item.id + "|" + item.caption + "|" + item.comment + "\n";
        for (const openxisf::table_field& field : item.fields) {
            text += field.id + "|" + std::string(detail::type_name(field.type)) + "|" + field.header + "|" +
                    canonical(field.format) + "\n";
        }
        for (const std::vector<property_value>& row : item.rows) {
            for (const property_value& cell : row) {
                text += canonical(cell) + "\n";
            }
        }
    }
    return text;
}

// An image as the reader returns it after a write: the embedded flag of its ICC profile set, its properties and
// tables compared as text.
template <typename Image> void require_same(Image read, Image written)
{
    require(canonical(read.properties) == canonical(written.properties));
    require(canonical(read.tables) == canonical(written.tables));
    read.properties = {};
    read.tables = {};
    written.properties = {};
    written.tables = {};
    if (!written.icc_profile.empty()) {
        written.icc_profile[47] |= std::byte{0x01};
    }
    if constexpr (std::is_same_v<Image, image_info>) {
        require(read.thumbnail.has_value() == written.thumbnail.has_value());
        if (read.thumbnail && written.thumbnail) {
            require_same(*read.thumbnail, *written.thumbnail);
        }
        read.thumbnail.reset();
        written.thumbnail.reset();
    }
    require(read == written);
}

// A sink that keeps what it receives, and can rewrite it or not.
class kept_output
{
public:
    explicit kept_output(bool rewritable) : rewritable_(rewritable) {}

    openxisf::output_sink& sink() noexcept
    {
        return rewritable_ ? static_cast<openxisf::output_sink&>(memory_) : appended_;
    }

    std::vector<std::byte> release()
    {
        return rewritable_ ? memory_.release() : std::move(bytes_);
    }

private:
    bool rewritable_;
    openxisf::memory_sink memory_{};
    std::vector<std::byte> bytes_{};
    openxisf::callback_sink appended_{
        [this](std::span<const std::byte> data) { bytes_.insert(bytes_.end(), data.begin(), data.end()); }};
};

// A read of a region of an image: the region, and the storage model, the pitch and the verification of the read.
struct region_read
{
    openxisf::pixel_region region{};
    openxisf::pixel_storage storage = openxisf::pixel_storage::planar;
    std::uint64_t pitch = 0;
    bool verify = true;
};

// A region of the image, within it, and how it is read.
region_read make_region_read(const image_info& image, choices& input)
{
    region_read read;
    for (const std::uint64_t length : image.geometry.dimensions) {
        const std::uint64_t start = input.below(length);
        read.region.origin.push_back(start);
        read.region.size.push_back(1 + input.below(length - start));
    }
    const std::uint64_t channels = image.geometry.channels;
    read.region.first_channel = input.below(channels);
    // 0 for every channel from the first.
    read.region.channel_count = input.below(channels - read.region.first_channel + 1);
    read.storage = input.flag() ? openxisf::pixel_storage::normal : openxisf::pixel_storage::planar;
    const std::uint64_t count =
        read.region.channel_count == 0 ? channels - read.region.first_channel : read.region.channel_count;
    const std::uint64_t row = read.region.size.front() * (read.storage == openxisf::pixel_storage::normal ? count : 1);
    read.pitch = input.flag() ? row + input.below(3) : 0;
    read.verify = !input.flag();
    return read;
}

// The samples of the region that read reads from pixels, the pixel data of image: in rows of the storage model of the
// read, with zeros between them, computed sample by sample.
std::vector<std::byte> expected_region(std::span<const std::byte> pixels, const image_info& image,
                                       const region_read& read)
{
    const openxisf::geometry& shape = image.geometry;
    const openxisf::pixel_region& region = read.region;
    const std::size_t size = openxisf::sample_size(image.sample_format);
    const std::uint64_t first = region.first_channel;
    const std::uint64_t channels = region.channel_count == 0 ? shape.channels - first : region.channel_count;
    std::uint64_t box = 1;
    for (const std::uint64_t length : region.size) {
        box *= length;
    }
    const bool planar = read.storage == openxisf::pixel_storage::planar || channels == 1;
    const std::uint64_t width = region.size.front();
    const std::uint64_t row = width * (planar ? 1 : channels);
    const std::uint64_t rows = (box / width) * (planar ? channels : 1);
    const std::uint64_t step = read.pitch == 0 ? row : read.pitch;
    std::vector<std::byte> result((((rows - 1) * step) + row) * size);
    const std::uint64_t pixel_count = shape.pixel_count();
    for (std::uint64_t q = 0; q < box; ++q) {
        // Pixel q of the region, the first axis fastest, and its index p in the image.
        std::uint64_t rest = q;
        std::uint64_t p = 0;
        std::uint64_t scale = 1;
        for (std::size_t a = 0; a < region.size.size(); ++a) {
            p += (region.origin[a] + (rest % region.size[a])) * scale;
            rest /= region.size[a];
            scale *= shape.dimensions[a];
        }
        const std::uint64_t x = q % width;
        for (std::uint64_t k = 0; k < channels; ++k) {
            const std::uint64_t c = first + k;
            const std::uint64_t from = image.pixel_storage == openxisf::pixel_storage::planar
                                           ? (c * pixel_count) + p
                                           : (p * shape.channels) + c;
            const std::uint64_t to =
                planar ? ((((k * (box / width)) + (q / width)) * step) + x) : ((q / width) * step) + (x * channels) + k;
            std::memcpy(result.data() + (to * size), pixels.data() + (from * size), size);
        }
    }
    return result;
}

openxisf::reader write_monolithic(const openxisf::writer& output, bool rewritable)
{
    kept_output file(rewritable);
    output.save(file.sink());
    return openxisf::reader(std::make_unique<openxisf::memory_source>(file.release()), {.strict = true});
}

// A header file and its data blocks file, which the resolver of the reader finds.
openxisf::reader write_distributed(const openxisf::writer& output, bool rewritable)
{
    kept_output header(rewritable);
    kept_output blocks(rewritable);
    output.save_distributed(header.sink(), blocks.sink(), "unit.xisb");
    auto file = std::make_shared<const std::vector<std::byte>>(blocks.release());
    const openxisf::external_resolver resolver = [file](const openxisf::external_reference& reference) {
        require(reference.form == openxisf::location_form::relative_path && reference.location == "unit.xisb");
        return std::make_unique<openxisf::memory_source>(std::span<const std::byte>(*file));
    };
    return openxisf::reader(std::make_unique<openxisf::memory_source>(header.release()),
                            {.strict = true, .resolver = resolver});
}

void check(const unit_model& model, const std::vector<std::vector<region_read>>& regions, const openxisf::reader& unit)
{
    require(unit.diagnostics().empty());
    require(canonical(unit.properties()) == canonical(model.properties));
    require(canonical(std::vector<openxisf::table>(unit.tables().begin(), unit.tables().end())) ==
            canonical(model.tables));
    for (const property& item : model.metadata) {
        const property* found = unit.metadata().find(item.id);
        require(found != nullptr && canonical(found->value) == canonical(item.value));
    }
    require(unit.images().size() == model.images.size());
    for (std::size_t i = 0; i < model.images.size(); ++i) {
        require_same(unit.image(i), model.images[i]);
        require(unit.read_pixels(i) == model.pixels[i]);
        for (const region_read& read : regions[i]) {
            require(unit.read_pixels(i, {.storage = read.storage,
                                         .region = read.region,
                                         .pitch = read.pitch,
                                         .verify_checksum = read.verify}) ==
                    expected_region(model.pixels[i], model.images[i], read));
        }
    }
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    choices input(std::span(data, size));
    const openxisf::write_options options = make_options(input);
    const unit_model model = make_model(input);
    // After the model, so that the inputs written before distributed units existed keep their meaning.
    const bool distributed = input.flag();
    // Last, for the same reason: two regions of each image.
    std::vector<std::vector<region_read>> regions;
    regions.reserve(model.images.size());
    for (const image_info& image : model.images) {
        regions.push_back({make_region_read(image, input), make_region_read(image, input)});
    }

    openxisf::writer output(options);
    output.metadata() = model.metadata;
    output.properties() = model.properties;
    output.tables() = model.tables;
    for (std::size_t i = 0; i < model.images.size(); ++i) {
        (void)output.add_image(model.images[i], model.pixels[i]);
    }
    for (const bool rewritable : {true, false}) {
        check(model, regions,
              distributed ? write_distributed(output, rewritable) : write_monolithic(output, rewritable));
    }
    return 0;
}
