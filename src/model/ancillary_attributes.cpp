// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/ancillary_attributes.h"

#include <openxisf/color.h>

#include "core/quote.h"
#include "core/text_grammar.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <string>

namespace openxisf::detail {

namespace {

// The elements of a colour filter array (spec §11.10.1, Table 18).
constexpr std::string_view cfa_elements = "0RGBWCMY";

// The bytes of the header of an ICC profile (ICC.1:2022 §7.2), which holds the signature 'acsp' at byte 36.
constexpr std::size_t icc_header_size = 128;
constexpr std::size_t icc_signature_offset = 36;
constexpr std::string_view icc_signature = "acsp";

bool equals_ignoring_ascii_case(std::string_view text, std::string_view lowercase) noexcept
{
    return std::ranges::equal(text, lowercase, [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
    });
}

// The finite floating point values of text, separated by colons, one for each item of values.
void parse_float_list(std::string_view text, std::span<double> values, errc code)
{
    std::size_t start = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
        const std::size_t colon = text.find(':', start);
        const bool last = i + 1 == values.size();
        if (last != (colon == std::string_view::npos)) {
            throw invalid_data_error(code, quote(text) + " is not a list of " + std::to_string(values.size()) +
                                               " values separated by colons");
        }
        const std::string_view item = text.substr(start, last ? std::string_view::npos : colon - start);
        try {
            values[i] = parse_float<double>(item);
        } catch (const invalid_data_error&) {
            throw invalid_data_error(code, quote(item) + " is not a floating point value");
        }
        if (!std::isfinite(values[i])) {
            throw invalid_data_error(code, quote(item) + " is not a finite value");
        }
        start = colon + 1;
    }
}

bool in_unit_range(double value) noexcept
{
    return value >= 0.0 && value <= 1.0;
}

void check_unit_range(const std::array<double, 3>& values, std::string_view what)
{
    if (!std::ranges::all_of(values, in_unit_range)) {
        throw invalid_data_error(errc::invalid_rgb_working_space,
                                 "the " + std::string(what) + " of the primaries are not in [0, 1]");
    }
}

} // namespace

bool is_fits_keyword_name(std::string_view name) noexcept
{
    const auto allowed = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    };
    return name.size() <= 8 && std::ranges::all_of(name, allowed);
}

bool is_commentary_keyword(std::string_view name) noexcept
{
    return name.empty() || name == "COMMENT" || name == "HISTORY";
}

bool is_fits_keyword_text(std::string_view text) noexcept
{
    return std::ranges::all_of(text, [](char c) { return c >= ' ' && c <= '~'; });
}

bool has_icc_profile_header(std::span<const std::byte> profile) noexcept
{
    if (profile.size() < icc_header_size) {
        return false;
    }
    const std::span<const std::byte> signature = profile.subspan(icc_signature_offset, icc_signature.size());
    return std::ranges::equal(signature, icc_signature, [](std::byte stored, char expected) {
        return stored == static_cast<std::byte>(expected);
    });
}

std::optional<double> parse_gamma(std::string_view text)
{
    if (equals_ignoring_ascii_case(trim_white_space(text), "srgb")) {
        return std::nullopt;
    }
    double gamma = 0.0;
    try {
        gamma = parse_float<double>(text);
    } catch (const invalid_data_error&) {
        throw invalid_data_error(errc::invalid_rgb_working_space,
                                 "the gamma " + quote(text) + " is neither a floating point value nor sRGB");
    }
    if (!std::isfinite(gamma) || gamma <= 0.0) {
        throw invalid_data_error(errc::invalid_rgb_working_space,
                                 "the gamma " + quote(text) + " is not a finite value above zero");
    }
    return gamma;
}

std::array<double, 3> parse_triplet(std::string_view text, errc code)
{
    std::array<double, 3> values{};
    parse_float_list(text, values, code);
    return values;
}

std::array<double, 4> parse_quadruplet(std::string_view text, errc code)
{
    std::array<double, 4> values{};
    parse_float_list(text, values, code);
    return values;
}

std::array<double, 3> check_rgb_working_space(const rgb_working_space& space)
{
    check_unit_range(space.x, "x chromaticity coordinates");
    check_unit_range(space.y, "y chromaticity coordinates");
    check_unit_range(space.luminance, "luminance coefficients");
    const std::optional<std::array<double, 3>> derived = luminance_coefficients(space.x, space.y);
    if (!derived) {
        throw invalid_data_error(errc::invalid_rgb_working_space,
                                 "the chromaticity coordinates of the primaries do not define an RGB working space");
    }
    return *derived;
}

bool derived_luminance_in_range(const std::array<double, 3>& derived) noexcept
{
    return std::ranges::all_of(derived, in_unit_range);
}

void check_display_function(const display_function& function)
{
    for (std::size_t i = 0; i < 4; ++i) {
        const double m = function.midtones[i];
        const double s = function.shadows[i];
        const double h = function.highlights[i];
        const double l = function.shadows_expansion[i];
        const double r = function.highlights_expansion[i];
        // Every value is finite, since the parsers accept no others.
        if (!in_unit_range(m) || !in_unit_range(s) || !in_unit_range(h) || s > h || l > 0.0 || r < 1.0) {
            throw invalid_data_error(errc::invalid_display_function,
                                     "the parameters of component " + std::to_string(i) +
                                         " break the constraints 0 <= m, s, h <= 1, s <= h, l <= 0 and r >= 1");
        }
    }
}

std::uint64_t parse_cfa_size(std::string_view text)
{
    std::uint64_t size = 0;
    try {
        size = parse_integer<std::uint64_t>(text);
    } catch (const invalid_data_error&) {
        throw invalid_data_error(errc::invalid_color_filter_array, quote(text) + " is not an unsigned integer");
    }
    if (size == 0) {
        throw invalid_data_error(errc::invalid_color_filter_array, "a colour filter array has no side of zero pixels");
    }
    return size;
}

void check_color_filter_array(const color_filter_array& filter)
{
    if (const std::size_t bad = filter.pattern.find_first_not_of(cfa_elements); bad != std::string::npos) {
        throw invalid_data_error(errc::invalid_color_filter_array,
                                 "the pattern " + quote(filter.pattern) + " has an element " +
                                     quote(filter.pattern.substr(bad, 1)) + " that is none of 0RGBWCMY");
    }
    const bool fits = filter.height != 0 && filter.width <= std::numeric_limits<std::uint64_t>::max() / filter.height;
    if (!fits || filter.width * filter.height != filter.pattern.size() || filter.pattern.empty()) {
        throw invalid_data_error(errc::invalid_color_filter_array,
                                 "the pattern has " + std::to_string(filter.pattern.size()) +
                                     " elements, and a matrix of " + std::to_string(filter.width) + " by " +
                                     std::to_string(filter.height) + " needs one for each pixel");
    }
}

double parse_resolution_value(std::string_view text)
{
    double value = 0.0;
    try {
        value = parse_float<double>(text);
    } catch (const invalid_data_error&) {
        throw invalid_data_error(errc::invalid_resolution, quote(text) + " is not a floating point value");
    }
    if (!std::isfinite(value) || value <= 0.0) {
        throw invalid_data_error(errc::invalid_resolution, quote(text) + " is not a finite value above zero");
    }
    return value;
}

resolution_unit parse_resolution_unit(std::string_view text)
{
    if (text == resolution_unit_name(resolution_unit::inch)) {
        return resolution_unit::inch;
    }
    if (text == resolution_unit_name(resolution_unit::centimeter)) {
        return resolution_unit::centimeter;
    }
    throw invalid_data_error(errc::invalid_resolution, quote(text) + " is neither inch nor cm");
}

} // namespace openxisf::detail
