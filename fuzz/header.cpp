// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Opens the input as a unit in memory, with small limits, leniently and strictly. Opening succeeds, or fails with an
// error of untrusted input: invalid data, data that fail their checksum, an unsupported feature or a limit. Any other
// exception escapes and fails the run. The two modes agree: the strict open fails with the code of the first error that
// the lenient one recorded, also when the cap on diagnostics, which inputs reach, folds that error into the last one,
// and without such an error both find the same diagnostics and images. Every diagnostic is valid UTF-8, whatever the
// input holds. The pixels of every image are then read in both storage models: each read returns exactly the data size
// of its image, or fails with an error of untrusted input. What describes the images keeps the promises of the API:
// thumbnails hold their pixel data, tables one value of the type of each field in each row, colour filter arrays a
// pattern of their size, and display functions and working spaces values in their ranges. A header-only open, once its
// ancillary data are loaded, holds what the lenient open holds. A signed unit returns its signature and its root
// element as the input holds them, in valid UTF-8, and both modes return the same. Values compare by their bits, since
// a NaN is not equal to itself.

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>
#include <openxisf/types.h>

#include "core/utf8.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace {

using openxisf::diagnostic;

// Stops the run, so that the fuzzer reports the input that broke a property.
void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

// A reader, or the code of the error that refused the unit.
struct outcome
{
    std::optional<openxisf::reader> unit{};
    std::optional<openxisf::errc> failure{};
};

outcome open(std::span<const std::byte> data, bool strict, bool header_only = false)
{
    const openxisf::read_options options{.strict = strict,
                                         .header_only = header_only,
                                         .limits = {.max_header_size = std::uint64_t{1} << 20,
                                                    .max_xml_depth = 32,
                                                    .max_xml_elements = 10'000,
                                                    .max_allocation = std::uint64_t{1} << 20,
                                                    .max_ancillary_data = std::uint64_t{1} << 20,
                                                    .max_zstd_window = std::uint64_t{1} << 16,
                                                    .max_diagnostics = 64,
                                                    .max_model_objects = 256}};
    try {
        return {.unit = openxisf::reader(std::make_unique<openxisf::memory_source>(data), options)};
    } catch (const openxisf::invalid_data_error& failure) {
        return {.failure = failure.code()};
    } catch (const openxisf::integrity_error& failure) {
        return {.failure = failure.code()};
    } catch (const openxisf::unsupported_error& failure) {
        return {.failure = failure.code()};
    } catch (const openxisf::limit_error& failure) {
        return {.failure = failure.code()};
    }
}

bool same(const diagnostic& a, const diagnostic& b)
{
    return a.severity == b.severity && a.code == b.code && a.message == b.message && a.context == b.context;
}

// The pixels of image index of unit in a storage model, or nothing when an error of untrusted input refused them.
std::optional<std::vector<std::byte>> read_pixels(const openxisf::reader& unit, std::size_t index,
                                                  openxisf::pixel_storage storage)
{
    try {
        return unit.read_pixels(index, {.storage = storage});
    } catch (const openxisf::invalid_data_error&) {
        return std::nullopt;
    } catch (const openxisf::integrity_error&) {
        return std::nullopt;
    } catch (const openxisf::unsupported_error&) {
        return std::nullopt;
    } catch (const openxisf::limit_error&) {
        return std::nullopt;
    }
}

// Reads the pixels of every image of unit in both storage models. A read that succeeds has the size of the image, and
// both models store a single channel the same way.
void read_every_image(const openxisf::reader& unit)
{
    for (std::size_t i = 0; i < unit.images().size(); ++i) {
        const std::optional<std::vector<std::byte>> planar = read_pixels(unit, i, openxisf::pixel_storage::planar);
        const std::optional<std::vector<std::byte>> normal = read_pixels(unit, i, openxisf::pixel_storage::normal);
        require(planar.has_value() == normal.has_value());
        if (planar && normal) {
            require(planar->size() == unit.image(i).data_size() && normal->size() == planar->size());
            require(unit.image(i).geometry.channels > 1 || *normal == *planar);
        }
    }
}

bool in_unit_range(double value)
{
    return value >= 0.0 && value <= 1.0;
}

// What a thumbnail, a table, a colour filter array, a display function and a working space promise.
void check_description(const openxisf::image_info& info)
{
    if (info.thumbnail) {
        const openxisf::thumbnail& small = *info.thumbnail;
        require(small.geometry.dimensions.size() == 2);
        require(small.sample_format == openxisf::sample_format::uint8 ||
                small.sample_format == openxisf::sample_format::uint16);
        require(small.pixels.size() == small.geometry.sample_count() * openxisf::sample_size(small.sample_format));
    }
    for (const openxisf::table& item : info.tables) {
        require(!item.fields.empty());
        for (const std::vector<openxisf::property_value>& row : item.rows) {
            require(row.size() == item.fields.size());
            for (std::size_t column = 0; column < row.size(); ++column) {
                require(row[column].type() == item.fields[column].type);
            }
        }
    }
    if (info.color_filter_array) {
        const openxisf::color_filter_array& filter = *info.color_filter_array;
        require(filter.width * filter.height == filter.pattern.size() && !filter.pattern.empty());
    }
    if (info.display_function) {
        const openxisf::display_function& function = *info.display_function;
        for (std::size_t i = 0; i < 4; ++i) {
            require(in_unit_range(function.midtones[i]) && in_unit_range(function.shadows[i]) &&
                    in_unit_range(function.highlights[i]) && function.shadows[i] <= function.highlights[i] &&
                    function.shadows_expansion[i] <= 0.0 && function.highlights_expansion[i] >= 1.0);
        }
    }
    if (info.rgb_working_space) {
        const openxisf::rgb_working_space& space = *info.rgb_working_space;
        require(!space.gamma || *space.gamma > 0.0);
        for (std::size_t i = 0; i < 3; ++i) {
            require(in_unit_range(space.x[i]) && in_unit_range(space.y[i]) && in_unit_range(space.luminance[i]));
        }
    }
    if (info.resolution) {
        require(info.resolution->horizontal > 0.0 && info.resolution->vertical > 0.0);
    }
}

// The same number, where two NaNs are the same, since a NaN read twice is not equal to itself.
template <typename T> bool same_number(const T& a, const T& b)
{
    if constexpr (std::is_floating_point_v<T>) {
        return a == b || (std::isnan(a) && std::isnan(b));
    } else if constexpr (std::is_same_v<T, std::complex<float>> || std::is_same_v<T, std::complex<double>>) {
        return same_number(a.real(), b.real()) && same_number(a.imag(), b.imag());
    } else {
        return a == b;
    }
}

// The same value, its numbers compared with same_number().
bool same_value(const openxisf::property_value& a, const openxisf::property_value& b)
{
    if (a.type() != b.type() || a.rows() != b.rows() || a.columns() != b.columns() ||
        a.data().index() != b.data().index()) {
        return false;
    }
    return std::visit(
        [&b]<typename T>(const T& held) {
            const T& other = std::get<T>(b.data());
            if constexpr (std::is_same_v<T, std::string>) {
                return held == other;
            } else if constexpr (requires { held.size(); }) {
                return std::ranges::equal(held, other, [](const auto& x, const auto& y) { return same_number(x, y); });
            } else {
                return same_number(held, other);
            }
        },
        a.data());
}

bool same_properties(const openxisf::property_list& a, const openxisf::property_list& b)
{
    return std::ranges::equal(a, b, [](const openxisf::property& x, const openxisf::property& y) {
        return x.id == y.id && x.format == y.format && x.comment == y.comment && same_value(x.value, y.value);
    });
}

bool same_tables(std::span<const openxisf::table> a, std::span<const openxisf::table> b)
{
    return std::ranges::equal(a, b, [](const openxisf::table& x, const openxisf::table& y) {
        return x.id == y.id && x.fields == y.fields && x.caption == y.caption && x.comment == y.comment &&
               std::ranges::equal(x.rows, y.rows,
                                  [](const auto& r, const auto& s) { return std::ranges::equal(r, s, same_value); });
    });
}

// The same image, its properties and tables compared with same_value(), and everything else with ==.
bool same_image(const openxisf::image_info& a, const openxisf::image_info& b)
{
    if (!same_properties(a.properties, b.properties) || !same_tables(a.tables, b.tables) ||
        a.thumbnail.has_value() != b.thumbnail.has_value()) {
        return false;
    }
    openxisf::image_info x = a;
    openxisf::image_info y = b;
    x.properties = y.properties = {};
    x.tables.clear();
    y.tables.clear();
    if (x.thumbnail && y.thumbnail) {
        if (!same_properties(x.thumbnail->properties, y.thumbnail->properties) ||
            !same_tables(x.thumbnail->tables, y.thumbnail->tables)) {
            return false;
        }
        x.thumbnail->properties = y.thumbnail->properties = {};
        x.thumbnail->tables.clear();
        y.thumbnail->tables.clear();
    }
    return x == y;
}

// The signature and the root element that it signs, which a signed unit returns as the input holds them.
void check_signature(std::string_view input, const openxisf::reader& unit)
{
    const std::string_view signature = unit.signature_xml();
    const std::string_view signed_element = unit.signed_xml();
    const auto appears = [input](std::string_view part) {
        return std::search(input.begin(), input.end(), std::boyer_moore_horspool_searcher(part.begin(), part.end())) !=
               input.end();
    };
    require((unit.signature() == openxisf::signature_status::none) == signature.empty());
    require(signature.empty() == signed_element.empty());
    if (!signature.empty()) {
        require(openxisf::detail::is_valid_utf8(signature) && openxisf::detail::is_valid_utf8(signed_element));
        require(signature.starts_with('<') && signed_element.starts_with('<') && signed_element.ends_with('>'));
        require(appears(signature) && appears(signed_element));
    }
}

// A header-only open of the unit, once its ancillary data are loaded, holds what the lenient open holds.
void check_header_only(std::span<const std::byte> data, const openxisf::reader& lenient)
{
    outcome header_only = open(data, false, true);
    if (!header_only.unit) {
        std::abort();
    }
    openxisf::reader& unit = *header_only.unit;
    // Without loaded data, more copies fit in the limit of the ancillary data, so the images can differ until then.
    require(!unit.ancillary_data_loaded());
    unit.load_ancillary_data();
    require(unit.ancillary_data_loaded());
    require(std::ranges::equal(unit.diagnostics(), lenient.diagnostics(), same));
    require(std::ranges::equal(unit.images(), lenient.images(), same_image));
    require(same_tables(unit.tables(), lenient.tables()));
    require(same_properties(unit.properties(), lenient.properties()) &&
            same_properties(unit.metadata(), lenient.metadata()));
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::span<const std::byte> unit(reinterpret_cast<const std::byte*>(data), size);
    const outcome lenient = open(unit, false);
    const outcome strict = open(unit, true);
    if (!lenient.unit) {
        require(!strict.unit);
        return 0;
    }

    const std::span<const diagnostic> found = lenient.unit->diagnostics();
    for (const diagnostic& entry : found) {
        require(!entry.message.empty() && openxisf::detail::is_valid_utf8(entry.message));
        require(openxisf::detail::is_valid_utf8(entry.context.element));
    }
    read_every_image(*lenient.unit);
    check_signature(std::string_view(reinterpret_cast<const char*>(data), size), *lenient.unit);
    for (const openxisf::image_info& info : lenient.unit->images()) {
        check_description(info);
    }
    check_header_only(unit, *lenient.unit);
    const auto first_error = std::ranges::find(found, openxisf::severity::error, &diagnostic::severity);
    if (first_error != found.end()) {
        require(!strict.unit && strict.failure == first_error->code);
        return 0;
    }
    // Without an error, strict reading must succeed too.
    if (!strict.unit) {
        std::abort();
    }
    require(std::ranges::equal(found, strict.unit->diagnostics(), same));
    require(strict.unit->storage() == lenient.unit->storage());
    require(strict.unit->signature() == lenient.unit->signature());
    require(strict.unit->signature_xml() == lenient.unit->signature_xml() &&
            strict.unit->signed_xml() == lenient.unit->signed_xml());
    require(std::ranges::equal(strict.unit->images(), lenient.unit->images(), same_image));
    require(same_tables(strict.unit->tables(), lenient.unit->tables()));
    return 0;
}
