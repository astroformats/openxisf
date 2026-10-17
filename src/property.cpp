// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/error.h>
#include <openxisf/property.h>

#include "core/text_grammar.h"
#include "core/utc_time.h"
#include "model/property_text.h"
#include "model/property_types.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace openxisf {

namespace {

// The bits of an IEEE 754 binary128 value (spec §8.1).
constexpr int float128_fraction_bits = 112;
constexpr std::uint64_t float128_exponent_mask = 0x7FFF;
constexpr int float128_exponent_bias = 16383;

// The bits of a double.
constexpr int double_fraction_bits = 52;
constexpr int double_exponent_bias = 1023;
constexpr int double_lowest_exponent = -1074; // of the least significant bit of the smallest subnormal
constexpr std::uint64_t double_exponent_limit = 2047;

// An unsigned integer of up to 128 bits.
struct wide
{
    std::uint64_t high = 0;
    std::uint64_t low = 0;

    [[nodiscard]] int bit_length() const noexcept
    {
        return high != 0 ? 128 - std::countl_zero(high) : 64 - std::countl_zero(low);
    }

    [[nodiscard]] bool bit(int index) const noexcept
    {
        return index >= 64 ? ((high >> (index - 64)) & 1U) != 0 : ((low >> index) & 1U) != 0;
    }

    // True when one of the bits below index is set.
    [[nodiscard]] bool any_below(int index) const noexcept
    {
        if (index >= 64) {
            return low != 0 || (index > 64 && (high & ((std::uint64_t{1} << (index - 64)) - 1)) != 0);
        }
        return index > 0 && (low & ((std::uint64_t{1} << index) - 1)) != 0;
    }

    // The bits from index on, which must fit in 64 bits.
    [[nodiscard]] std::uint64_t from(int index) const noexcept
    {
        if (index >= 64) {
            return high >> (index - 64);
        }
        return index == 0 ? low : (low >> index) | (high << (64 - index));
    }
};

} // namespace

double to_double(float128 value) noexcept
{
    const std::uint64_t sign = value.high & (std::uint64_t{1} << 63U);
    const std::uint64_t exponent = (value.high >> 48U) & float128_exponent_mask;
    const wide fraction{.high = value.high & ((std::uint64_t{1} << 48U) - 1), .low = value.low};
    const auto with_sign = [sign](std::uint64_t bits) { return std::bit_cast<double>(sign | bits); };

    if (exponent == float128_exponent_mask) {
        return with_sign(fraction.high == 0 && fraction.low == 0
                             ? std::bit_cast<std::uint64_t>(std::numeric_limits<double>::infinity())
                             : std::bit_cast<std::uint64_t>(std::numeric_limits<double>::quiet_NaN()));
    }
    if (exponent == 0 && fraction.high == 0 && fraction.low == 0) {
        return with_sign(0);
    }

    // The value is significand × 2^scale, the significand an integer of up to 113 bits.
    wide significand = fraction;
    int scale = 1 - float128_exponent_bias - float128_fraction_bits;
    if (exponent != 0) {
        significand.high |= std::uint64_t{1} << 48U;
        scale = static_cast<int>(exponent) - float128_exponent_bias - float128_fraction_bits;
    }

    // The scale of the least significant bit that a double keeps: 53 bits below the leading one, but not below the
    // smallest subnormal.
    const int leading = scale + significand.bit_length() - 1;
    const int kept_scale = std::max(leading - double_fraction_bits, double_lowest_exponent);
    const int shift = kept_scale - scale;
    std::uint64_t kept = 0;
    if (shift <= 0) {
        kept = significand.low << -shift;
    } else if (shift <= 114) {
        // Rounded to nearest, ties to even.
        kept = significand.from(shift);
        if (significand.bit(shift - 1) && (significand.any_below(shift - 1) || (kept & 1U) != 0)) {
            ++kept;
        }
    }

    // kept is at most 2^53, with its scale kept_scale.
    int kept_exponent = kept_scale;
    if (kept == std::uint64_t{1} << 53U) {
        kept >>= 1U;
        ++kept_exponent;
    }
    if (kept < std::uint64_t{1} << 52U) {
        // A subnormal, or zero.
        return with_sign(kept);
    }
    const int biased_exponent = kept_exponent + double_fraction_bits + double_exponent_bias;
    const auto biased = static_cast<std::uint64_t>(biased_exponent);
    if (biased >= double_exponent_limit) {
        return with_sign(std::bit_cast<std::uint64_t>(std::numeric_limits<double>::infinity()));
    }
    return with_sign((biased << 52U) | (kept & ((std::uint64_t{1} << 52U) - 1)));
}

date_time to_date_time(std::chrono::sys_time<std::chrono::nanoseconds> time) noexcept
{
    return detail::date_time_of(time);
}

std::string_view property_type_name(property_type type) noexcept
{
    return detail::type_name(type);
}

// ---------------------------------------------------------------------------------------------------------------------
// property_value

property_value::property_value(bool value) noexcept
    : type_(property_type::boolean), value_(std::in_place_type<bool>, value)
{}
property_value::property_value(std::int8_t value) noexcept
    : type_(property_type::int8), value_(std::in_place_type<std::int8_t>, value)
{}
property_value::property_value(std::uint8_t value) noexcept
    : type_(property_type::uint8), value_(std::in_place_type<std::uint8_t>, value)
{}
property_value::property_value(std::int16_t value) noexcept
    : type_(property_type::int16), value_(std::in_place_type<std::int16_t>, value)
{}
property_value::property_value(std::uint16_t value) noexcept
    : type_(property_type::uint16), value_(std::in_place_type<std::uint16_t>, value)
{}
property_value::property_value(std::int32_t value) noexcept
    : type_(property_type::int32), value_(std::in_place_type<std::int32_t>, value)
{}
property_value::property_value(std::uint32_t value) noexcept
    : type_(property_type::uint32), value_(std::in_place_type<std::uint32_t>, value)
{}
property_value::property_value(std::int64_t value) noexcept
    : type_(property_type::int64), value_(std::in_place_type<std::int64_t>, value)
{}
property_value::property_value(std::uint64_t value) noexcept
    : type_(property_type::uint64), value_(std::in_place_type<std::uint64_t>, value)
{}
property_value::property_value(int128 value) noexcept
    : type_(property_type::int128), value_(std::in_place_type<int128>, value)
{}
property_value::property_value(uint128 value) noexcept
    : type_(property_type::uint128), value_(std::in_place_type<uint128>, value)
{}
property_value::property_value(float value) noexcept
    : type_(property_type::float32), value_(std::in_place_type<float>, value)
{}
property_value::property_value(double value) noexcept
    : type_(property_type::float64), value_(std::in_place_type<double>, value)
{}
property_value::property_value(std::complex<float> value) noexcept
    : type_(property_type::complex32), value_(std::in_place_type<std::complex<float>>, value)
{}
property_value::property_value(std::complex<double> value) noexcept
    : type_(property_type::complex64), value_(std::in_place_type<std::complex<double>>, value)
{}
property_value::property_value(std::string value) noexcept : value_(std::in_place_type<std::string>, std::move(value))
{}
property_value::property_value(std::string_view value) : value_(std::in_place_type<std::string>, value) {}
property_value::property_value(const char* value) : value_(std::in_place_type<std::string>, value) {}
property_value::property_value(date_time value) noexcept
    : type_(property_type::time_point), value_(std::in_place_type<date_time>, value)
{}

// The copy is made from the value that other holds, with the constructor of std::variant for one alternative, and not
// with the copy constructor of std::variant. That of libstdc++ 12 to 16, when the copy of the alternative throws, has
// the storage of the variant destroy the alternative of the index variant_npos, which it takes for a valid one in a
// variant whose alternatives never leave it valueless, as here: a failed allocation became undefined behaviour, a
// crash, instead of std::bad_alloc. The constructor for one alternative throws before the storage exists.
property_value::property_value(const property_value& other)
    : type_(other.type_),
      value_(std::visit(
          [](const auto& held) { return storage(std::in_place_type<std::remove_cvref_t<decltype(held)>>, held); },
          other.value_)),
      rows_(other.rows_), columns_(other.columns_)
{}

property_value::property_value(property_type type, storage value) noexcept : type_(type), value_(std::move(value)) {}

property_value property_value::from_float128_text(std::string_view text)
{
    try {
        return {property_type::float128, std::string(detail::check_float128(text))};
    } catch (const invalid_data_error& failure) {
        throw usage_error(errc::invalid_argument, failure.what());
    }
}

property_value property_value::from_complex128_text(std::string_view text)
{
    try {
        return {property_type::complex128, detail::check_complex128(text)};
    } catch (const invalid_data_error& failure) {
        throw usage_error(errc::invalid_argument, failure.what());
    }
}

std::uint64_t property_value::length() const
{
    return std::visit(
        []<typename T>(const T& value) -> std::uint64_t {
            // The vectors that hold elements; a String is not one.
            if constexpr (!std::same_as<T, std::string> && requires { value.size(); }) {
                return value.size();
            } else {
                return 0;
            }
        },
        value_);
}

void property_value::check_matrix_size(std::uint64_t rows, std::uint64_t columns, std::size_t size)
{
    const bool overflow = rows != 0 && columns > std::numeric_limits<std::uint64_t>::max() / rows;
    if (overflow || rows * columns != size) {
        throw usage_error(errc::invalid_argument, "a matrix of " + std::to_string(rows) + " by " +
                                                      std::to_string(columns) + " cannot have " + std::to_string(size) +
                                                      " elements");
    }
}

void property_value::throw_type_mismatch(property_type type)
{
    throw usage_error(errc::invalid_argument, "the property value is of type " + std::string(property_type_name(type)) +
                                                  ", which is not held in the requested C++ type");
}

// ---------------------------------------------------------------------------------------------------------------------
// property_list

property_list::property_list(std::vector<property> items) : properties_(std::move(items))
{
    std::unordered_set<std::string_view> ids;
    for (const property& item : properties_) {
        if (!ids.insert(item.id).second) {
            throw usage_error(errc::invalid_argument, "the property " + item.id + " is in the list more than once");
        }
    }
}

property_list::const_iterator property_list::begin() const noexcept
{
    return properties_.begin();
}

property_list::const_iterator property_list::end() const noexcept
{
    return properties_.end();
}

std::size_t property_list::size() const noexcept
{
    return properties_.size();
}

bool property_list::empty() const noexcept
{
    return properties_.empty();
}

const property* property_list::find(std::string_view id) const noexcept
{
    for (const property& item : properties_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

bool property_list::contains(std::string_view id) const noexcept
{
    return find(id) != nullptr;
}

const property& property_list::at(std::string_view id) const
{
    if (const property* found = find(id)) {
        return *found;
    }
    throw usage_error(errc::invalid_argument, "there is no property " + std::string(id));
}

void property_list::set(property item)
{
    for (property& existing : properties_) {
        if (existing.id == item.id) {
            existing = std::move(item);
            return;
        }
    }
    properties_.push_back(std::move(item));
}

void property_list::set(std::string id, property_value value)
{
    set(property{.id = std::move(id), .value = std::move(value)});
}

bool property_list::erase(std::string_view id)
{
    return std::erase_if(properties_, [id](const property& item) { return item.id == id; }) != 0;
}

} // namespace openxisf
