// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Properties as text (spec §8.4, §11.1). The input holds, one per line: a type name, a value, a format specifier, an
// identifier, and attributes written as they are into the start tag of a Property element; whatever follows is the
// character data of that element.
//
// The value is parsed as the value attribute of the type, and as a TimePoint; the format specifier is parsed too. Each
// parser accepts its text, and what it accepts formats back to text that parses to the same value, or it throws the
// error of its grammar. Then a unit with a Property element made of the lines is opened, leniently and strictly, with
// small limits: the two agree as in fuzz_header, and every property that the unit has is of the type that its element
// names, or a TimePoint where the element names a String and the identifier is that of a reserved TimePoint property,
// which the reader takes for the TimePoint that the String holds, and is rendered as text with its format specifier
// (spec §8.4.3): valid UTF-8, padded to its width within the bound of the renderer, its first element the start of the
// whole. Any other exception escapes and fails the run.

#include "model/property_text.h"

#include <openxisf/error.h>
#include <openxisf/format.h>
#include <openxisf/io.h>
#include <openxisf/property.h>

#include "core/utc_time.h"
#include "core/utf8.h"
#include "model/format_specifier.h"
#include "model/property_catalog.h"
#include "model/property_types.h"
#include "model/unit.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace detail = openxisf::detail;
using openxisf::errc;
using openxisf::property_type;
using openxisf::property_value;

// Stops the run, so that the fuzzer reports the input that broke a property.
void require(bool condition)
{
    if (!condition) {
        std::abort();
    }
}

constexpr std::size_t field_count = 5;

struct fields
{
    std::array<std::string_view, field_count> lines{};
    std::string_view content{};
};

fields split(std::string_view input)
{
    fields result;
    for (std::string_view& line : result.lines) {
        const std::size_t end = input.find('\n');
        line = input.substr(0, end);
        input = end == std::string_view::npos ? std::string_view() : input.substr(end + 1);
    }
    result.content = input;
    return result;
}

bool has_code(const openxisf::error& failure, std::initializer_list<errc> codes)
{
    return std::ranges::find(codes, failure.code()) != codes.end();
}

// The value attribute of every type that has one. A NaN is not equal to itself, so values are compared through their
// text, which is stable from the first formatting on.
void check_value(property_type type, std::string_view text)
{
    std::optional<property_value> value;
    try {
        value = detail::parse_value_attribute(type, text);
    } catch (const openxisf::invalid_data_error& failure) {
        require(has_code(failure, {errc::invalid_integer, errc::invalid_float, errc::invalid_boolean,
                                   errc::value_out_of_range, errc::invalid_complex, errc::invalid_time_point}));
        return;
    }
    require(value->type() == type);
    if (type == property_type::time_point && !detail::is_valid_time_point(value->get<openxisf::date_time>())) {
        return;
    }
    const std::string formatted = detail::format_value_attribute(*value);
    const property_value again = detail::parse_value_attribute(type, formatted);
    require(detail::format_value_attribute(again) == formatted);
    require(again == *value || formatted.find("NaN") != std::string::npos);
}

void check_format(std::string_view text)
{
    std::optional<openxisf::property_format> format;
    try {
        format = detail::parse_format_specifier(text);
    } catch (const openxisf::invalid_data_error& failure) {
        require(failure.code() == errc::invalid_format_specifier);
        return;
    }
    const std::string formatted = detail::format_specifier_text(*format);
    if (formatted.empty()) {
        require(*format == openxisf::property_format{});
    } else {
        require(detail::parse_format_specifier(formatted) == *format);
    }
}

void parse_each(const fields& input, std::optional<property_type> type)
{
    if (type) {
        require(openxisf::property_type_name(*type) == input.lines[0] ||
                detail::alternate_type_name(*type) == input.lines[0]);
        const detail::type_category category = detail::category_of(*type);
        if (category == detail::type_category::scalar || category == detail::type_category::complex ||
            category == detail::type_category::time_point) {
            check_value(*type, input.lines[1]);
        }
    }
    check_value(property_type::time_point, input.lines[1]);
    check_format(input.lines[2]);
    (void)detail::is_property_id(input.lines[3]);
}

std::string escaped(std::string_view text)
{
    std::string result;
    for (const char c : text) {
        switch (c) {
        case '&':
            result += "&amp;";
            break;
        case '<':
            result += "&lt;";
            break;
        case '>':
            result += "&gt;";
            break;
        case '"':
            result += "&quot;";
            break;
        default:
            result += c;
        }
    }
    return result;
}

std::string attribute(std::string_view name, std::string_view value)
{
    return value.empty() ? std::string() : " " + std::string(name) + "=\"" + escaped(value) + "\"";
}

// Attributes as written, which cannot close the start tag: a misplaced quote or ampersand only makes the XML invalid.
std::string in_start_tag(std::string_view text)
{
    std::string result;
    for (const char c : text) {
        if (c == '<') {
            result += "&lt;";
        } else if (c == '>') {
            result += "&gt;";
        } else {
            result += c;
        }
    }
    return result;
}

// An attribute value as XML reads it: tabs and carriage returns become spaces (XML 1.0, 3.3.3).
std::string normalized(std::string_view value)
{
    std::string result(value);
    std::ranges::replace(result, '\t', ' ');
    std::ranges::replace(result, '\r', ' ');
    return result;
}

// A monolithic file whose root element has the Property element of the input after a Metadata element.
std::vector<std::byte> unit_of(const fields& input)
{
    const std::string header =
        R"(<?xml version="1.0" encoding="UTF-8"?><xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">)"
        R"(<Metadata><Property id="XISF:CreationTime" type="TimePoint" value="2026-10-03T00:00:00Z"/>)"
        R"(<Property id="XISF:CreatorApplication" type="String">fuzz_property_text</Property></Metadata>)"
        "<Property" +
        attribute("id", input.lines[3]) + attribute("type", input.lines[0]) + attribute("value", input.lines[1]) +
        attribute("format", input.lines[2]) + " " + in_start_tag(input.lines[4]) + ">" + escaped(input.content) +
        "</Property></xisf>";
    std::vector<std::byte> file;
    for (const char c : std::string_view("XISF0100")) {
        file.push_back(static_cast<std::byte>(c));
    }
    for (std::size_t i = 0; i < 4; ++i) {
        file.push_back(static_cast<std::byte>((header.size() >> (8 * i)) & 0xFFU));
    }
    file.resize(16);
    for (const char c : header) {
        file.push_back(static_cast<std::byte>(c));
    }
    return file;
}

// A unit, or the code of the error that refused it.
struct outcome
{
    std::unique_ptr<detail::unit> unit{};
    std::optional<errc> failure{};
};

outcome open(std::span<const std::byte> data, bool strict)
{
    const openxisf::read_options options{.strict = strict,
                                         .limits = {.max_header_size = std::uint64_t{1} << 20,
                                                    .max_allocation = std::uint64_t{1} << 20,
                                                    .max_ancillary_data = std::uint64_t{1} << 20,
                                                    .max_zstd_window = std::uint64_t{1} << 16}};
    try {
        return {.unit = std::make_unique<detail::unit>(std::make_unique<openxisf::memory_source>(data), options)};
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

bool same(const openxisf::diagnostic& a, const openxisf::diagnostic& b)
{
    return a.severity == b.severity && a.code == b.code && a.message == b.message && a.context == b.context;
}

// The number of characters of UTF-8 text.
std::size_t character_count(std::string_view text)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(text, [](char c) { return (static_cast<unsigned char>(c) & 0xC0U) != 0x80U; }));
}

// A property as text, with its format: every value that a unit holds renders.
void check_rendering(const openxisf::property& item)
{
    const openxisf::property_format format = item.format.value_or(openxisf::property_format{});
    const std::string text = openxisf::format_value(item.value, format);
    require(detail::is_valid_utf8(text));
    const detail::type_category category = detail::category_of(item.value.type());
    if (category == detail::type_category::vector || category == detail::type_category::matrix) {
        if (item.value.length() != 0) {
            require(text.starts_with(openxisf::format_element(item.value, 0, format)));
        }
    } else if (category != detail::type_category::time_point && category != detail::type_category::complex) {
        // A width beyond 1024 counts as 1024; a number is at most a sign, 309 digits, a point and 1024 digits.
        const std::size_t width = std::min<std::size_t>(format.width, 1024);
        require(character_count(text) >= width);
        require(category == detail::type_category::string || text.size() <= std::max<std::size_t>(width, 1335));
    }
}

// A reserved TimePoint property written as a String that holds a TimePoint, which the reader reads as that TimePoint.
bool is_time_point_held_as_string(const openxisf::property& item, std::optional<property_type> type)
{
    return type == property_type::string && item.value.type() == property_type::time_point &&
           detail::reserved_property_type(item.id) == property_type::time_point;
}

// Every property that the unit has keeps the invariants of its type.
void check_properties(const detail::unit& opened, const fields& input, std::optional<property_type> type)
{
    for (const openxisf::property& item : opened.properties.standalone) {
        // The attributes of the last line can give the element its identifier or type when the other lines do not.
        require(input.lines[3].empty() || item.id == normalized(input.lines[3]));
        require(input.lines[0].empty() || type == item.value.type() || is_time_point_held_as_string(item, type));
        const property_value& value = item.value;
        switch (detail::category_of(value.type())) {
        case detail::type_category::string:
            require(detail::is_valid_utf8(value.get<std::string>()));
            break;
        case detail::type_category::matrix:
            require(value.rows() * value.columns() == value.length());
            break;
        case detail::type_category::vector:
            require(value.rows() == 0 && value.columns() == 0);
            break;
        default:
            require(value.length() == 0);
        }
        check_rendering(item);
    }
    require(opened.properties.metadata.size() == 2);
}

void open_and_compare(std::span<const std::byte> unit, const fields& input, std::optional<property_type> type)
{
    const outcome lenient = open(unit, false);
    const outcome strict = open(unit, true);
    if (!lenient.unit) {
        require(!strict.unit);
        return;
    }
    const std::vector<openxisf::diagnostic>& found = lenient.unit->diagnostics;
    for (const openxisf::diagnostic& entry : found) {
        require(!entry.message.empty() && detail::is_valid_utf8(entry.message));
    }
    check_properties(*lenient.unit, input, type);

    const auto first_error = std::ranges::find(found, openxisf::severity::error, &openxisf::diagnostic::severity);
    if (first_error != found.end()) {
        require(!strict.unit && strict.failure == first_error->code);
        return;
    }
    if (!strict.unit) {
        std::abort();
    }
    require(std::ranges::equal(found, strict.unit->diagnostics, same));
    require(strict.unit->properties.standalone.size() == lenient.unit->properties.standalone.size());
}

} // namespace

// The entry point that libFuzzer, and the replay driver, call for every input. Its name is fixed by libFuzzer.
// NOLINTNEXTLINE(readability-identifier-naming)
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const fields input = split(std::string_view(reinterpret_cast<const char*>(data), size));
    const std::optional<property_type> type = detail::property_type_named(input.lines[0]);
    parse_each(input, type);
    open_and_compare(unit_of(input), input, type);
    return 0;
}
