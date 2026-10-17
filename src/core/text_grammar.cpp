// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/text_grammar.h"

#include <openxisf/error.h>

#include "core/hex.h"
#include "core/quote.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>
#include <system_error>
#include <type_traits>

#if (defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200000) || (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE < 12)
#include <cerrno>
#include <cstdlib>
#include <new>
#if defined(__APPLE__)
#include <xlocale.h>
#else
#include <locale.h>
#endif
#endif

namespace openxisf::detail {

namespace {

// The names of spec §8.4.4, for messages.
template <typename T> constexpr std::string_view type_name()
{
    if constexpr (std::same_as<T, std::int8_t>) {
        return "Int8";
    } else if constexpr (std::same_as<T, std::uint8_t>) {
        return "UInt8";
    } else if constexpr (std::same_as<T, std::int16_t>) {
        return "Int16";
    } else if constexpr (std::same_as<T, std::uint16_t>) {
        return "UInt16";
    } else if constexpr (std::same_as<T, std::int32_t>) {
        return "Int32";
    } else if constexpr (std::same_as<T, std::uint32_t>) {
        return "UInt32";
    } else if constexpr (std::same_as<T, std::int64_t>) {
        return "Int64";
    } else if constexpr (std::same_as<T, std::uint64_t>) {
        return "UInt64";
    } else if constexpr (std::same_as<T, int128>) {
        return "Int128";
    } else if constexpr (std::same_as<T, uint128>) {
        return "UInt128";
    } else if constexpr (std::same_as<T, float>) {
        return "Float32";
    } else {
        return "Float64";
    }
}

[[noreturn]] void throw_out_of_range(std::string_view text, std::string_view type)
{
    throw invalid_data_error(errc::value_out_of_range, quote(text) + " is out of the range of " + std::string(type));
}

bool is_digit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

// ---------------------------------------------------------------------------------------------------------------------
// Integers

// An integer that follows the grammar. Binary, octal and hexadecimal values have no sign.
struct integer_text
{
    bool negative = false;
    unsigned radix = 10;
    std::string_view digits{};
};

std::optional<integer_text> scan_integer(std::string_view text) noexcept
{
    const std::string_view value = trim_white_space(text);

    // Spec §8.3.2: 0b, 0o or 0x and at least one digit of the radix, which may start with zeros.
    if (value.size() > 2 && value[0] == '0') {
        unsigned radix = 0;
        switch (value[1]) {
        case 'b':
        case 'B':
            radix = 2;
            break;
        case 'o':
        case 'O':
            radix = 8;
            break;
        case 'x':
        case 'X':
            radix = 16;
            break;
        default:
            break;
        }
        if (radix != 0) {
            const std::string_view digits = value.substr(2);
            if (!std::ranges::all_of(digits, [radix](char c) { return hex_digit_value(c) < radix; })) {
                return std::nullopt;
            }
            return integer_text{.negative = false, .radix = radix, .digits = digits};
        }
    }

    // Spec §8.3.1: an optional sign, then 0 or digits without leading zeros.
    integer_text result;
    std::string_view digits = value;
    if (!digits.empty() && (digits[0] == '+' || digits[0] == '-')) {
        result.negative = digits[0] == '-';
        digits.remove_prefix(1);
    }
    if (digits.empty() || !std::ranges::all_of(digits, is_digit) || (digits.size() > 1 && digits[0] == '0')) {
        return std::nullopt;
    }
    result.digits = digits;
    return result;
}

// The value of the digits, or nothing when it needs more than 128 bits. The digits are accumulated in 32-bit limbs,
// least significant first, so that no 128-bit type is needed.
std::optional<uint128> magnitude(const integer_text& value) noexcept
{
    std::array<std::uint32_t, 4> limbs{};
    for (const char c : value.digits) {
        std::uint64_t carry = hex_digit_value(c);
        for (std::uint32_t& limb : limbs) {
            const std::uint64_t product = (std::uint64_t{limb} * value.radix) + carry;
            limb = static_cast<std::uint32_t>(product);
            carry = product >> 32U;
        }
        if (carry != 0) {
            return std::nullopt;
        }
    }
    return uint128{.high = (std::uint64_t{limbs[3]} << 32U) | limbs[2],
                   .low = (std::uint64_t{limbs[1]} << 32U) | limbs[0]};
}

// value < 2^bits, for bits from 1 to 128.
bool below_power_of_two(const uint128& value, unsigned bits) noexcept
{
    if (bits >= 128) {
        return true;
    }
    if (bits >= 64) {
        return (value.high >> (bits - 64)) == 0;
    }
    return value.high == 0 && (value.low >> bits) == 0;
}

// value == 2^bits, for bits from 0 to 127.
bool is_power_of_two(const uint128& value, unsigned bits) noexcept
{
    if (bits >= 64) {
        return value.low == 0 && value.high == std::uint64_t{1} << (bits - 64);
    }
    return value.high == 0 && value.low == std::uint64_t{1} << bits;
}

// -value modulo 2^128: the two's complement bit pattern of a negative value.
uint128 negate(const uint128& value) noexcept
{
    return {.high = value.low == 0 ? ~value.high + 1 : ~value.high, .low = ~value.low + 1};
}

template <xisf_integer T> constexpr unsigned bit_count() noexcept
{
    if constexpr (std::same_as<T, int128> || std::same_as<T, uint128>) {
        return 128;
    } else {
        return sizeof(T) * 8;
    }
}

template <xisf_integer T> constexpr bool is_signed_integer() noexcept
{
    if constexpr (std::same_as<T, int128>) {
        return true;
    } else if constexpr (std::same_as<T, uint128>) {
        return false;
    } else {
        return std::is_signed_v<T>;
    }
}

// The T whose two's complement bit pattern is the low bit_count<T>() bits of bits.
template <xisf_integer T> T from_bit_pattern(const uint128& bits) noexcept
{
    if constexpr (std::same_as<T, uint128>) {
        return bits;
    } else if constexpr (std::same_as<T, int128>) {
        return {.high = static_cast<std::int64_t>(bits.high), .low = bits.low};
    } else if constexpr (std::same_as<T, std::uint64_t>) {
        return bits.low;
    } else {
        // Conversions to signed types are modular since C++20.
        return static_cast<T>(bits.low);
    }
}

// The decimal digits of value, in groups of nine taken from the 32-bit limbs.
std::string format_decimal(const uint128& value)
{
    // Up to 39 digits, written in five groups of nine.
    std::array<char, 45> buffer{};
    char* const end = buffer.data() + buffer.size();
    if (value.high == 0) {
        return {buffer.data(), std::to_chars(buffer.data(), end, value.low).ptr};
    }

    constexpr std::uint32_t group_base = 1'000'000'000;
    // The most significant first, the order of the long division.
    std::array<std::uint32_t, 4> limbs = {
        static_cast<std::uint32_t>(value.high >> 32U), static_cast<std::uint32_t>(value.high),
        static_cast<std::uint32_t>(value.low >> 32U), static_cast<std::uint32_t>(value.low)};
    char* start = end;
    while (std::ranges::any_of(limbs, [](std::uint32_t limb) { return limb != 0; })) {
        std::uint64_t remainder = 0;
        for (std::uint32_t& limb : limbs) {
            const std::uint64_t current = (remainder << 32U) | limb;
            limb = static_cast<std::uint32_t>(current / group_base);
            remainder = current % group_base;
        }
        for (int digit = 0; digit < 9; ++digit) {
            *--start = static_cast<char>('0' + (remainder % 10));
            remainder /= 10;
        }
    }
    while (*start == '0') {
        ++start;
    }
    return {start, end};
}

// ---------------------------------------------------------------------------------------------------------------------
// Floating point values

enum class float_kind
{
    number,
    nan,
    positive_infinity,
    negative_infinity,
};

// A floating point value that follows the grammar, with white space removed.
struct float_text
{
    float_kind kind = float_kind::number;
    bool negative = false;
    std::string_view number{};          // the number for std::from_chars, without a leading '+'
    std::string_view integer_digits{};  // the digits before the decimal point
    std::string_view fraction_digits{}; // the digits after it
    std::int64_t exponent = 0;          // saturated far beyond the range of any type
};

std::optional<float_text> scan_float(std::string_view text) noexcept
{
    const std::string_view value = trim_white_space(text);

    // The non-numeric forms are case-sensitive; Inf needs a sign, and inf and nan accept only a minus sign.
    if (value == "NaN" || value == "nan") {
        return float_text{.kind = float_kind::nan};
    }
    if (value == "-nan") {
        return float_text{.kind = float_kind::nan, .negative = true};
    }
    if (value == "+Inf" || value == "inf") {
        return float_text{.kind = float_kind::positive_infinity};
    }
    if (value == "-Inf" || value == "-inf") {
        return float_text{.kind = float_kind::negative_infinity};
    }

    // [-+]?([0-9]*\.)?[0-9]+([eE][-+]?[0-9]+)? : a decimal point must be followed by a digit.
    float_text result;
    std::size_t i = 0;
    if (i < value.size() && (value[i] == '+' || value[i] == '-')) {
        result.negative = value[i] == '-';
        ++i;
    }
    result.number = value.substr(result.negative ? 0 : i);

    const auto digits_from = [&value, &i](std::size_t start) {
        while (i < value.size() && is_digit(value[i])) {
            ++i;
        }
        return value.substr(start, i - start);
    };
    result.integer_digits = digits_from(i);
    if (i < value.size() && value[i] == '.') {
        ++i;
        result.fraction_digits = digits_from(i);
        if (result.fraction_digits.empty()) {
            return std::nullopt;
        }
    } else if (result.integer_digits.empty()) {
        return std::nullopt;
    }

    if (i < value.size() && (value[i] == 'e' || value[i] == 'E')) {
        ++i;
        bool negative_exponent = false;
        if (i < value.size() && (value[i] == '+' || value[i] == '-')) {
            negative_exponent = value[i] == '-';
            ++i;
        }
        const std::string_view exponent_digits = digits_from(i);
        if (exponent_digits.empty()) {
            return std::nullopt;
        }
        constexpr std::int64_t saturation = 1'000'000'000'000'000;
        std::int64_t exponent = 0;
        for (const char c : exponent_digits) {
            exponent = std::min((exponent * 10) + (c - '0'), saturation);
        }
        result.exponent = negative_exponent ? -exponent : exponent;
    }

    if (i != value.size()) {
        return std::nullopt;
    }
    return result;
}

// The decimal exponent of the first nonzero digit of a number, or nothing when the number is zero.
std::optional<std::int64_t> leading_exponent(const float_text& value) noexcept
{
    std::int64_t leading = 0;
    if (const std::size_t first = value.integer_digits.find_first_not_of('0'); first != std::string_view::npos) {
        leading = static_cast<std::int64_t>(value.integer_digits.size() - first) - 1;
    } else if (const std::size_t fraction_first = value.fraction_digits.find_first_not_of('0');
               fraction_first != std::string_view::npos) {
        leading = -static_cast<std::int64_t>(fraction_first) - 1;
    } else {
        return std::nullopt;
    }
    return leading + value.exponent;
}

// True when the magnitude of a number is at least one. When a conversion is out of range, this tells an overflow from
// an underflow.
bool at_least_one(const float_text& value) noexcept
{
    const std::optional<std::int64_t> exponent = leading_exponent(value);
    return exponent && *exponent >= 0;
}

// The smallest magnitude that rounds beyond the largest finite Float128: (2 - 2^-113) × 2^16383, whose first digit has
// the decimal exponent 4932. Its first 64 significant digits; the others are not all zero.
constexpr std::string_view float128_overflow_digits =
    "1189731495357231765085759326628007073479956869869102141501186852";
constexpr std::int64_t float128_overflow_exponent = 4932;

// True when a number rounds beyond the largest finite Float128. The threshold has more digits than those kept here, so
// a number that agrees with them is below it; one that has more digits, and agrees over the first 64, is taken as
// below.
bool beyond_float128(const float_text& value)
{
    const std::optional<std::int64_t> exponent = leading_exponent(value);
    if (!exponent || *exponent != float128_overflow_exponent) {
        return exponent && *exponent > float128_overflow_exponent;
    }
    std::string digits = std::string(value.integer_digits) + std::string(value.fraction_digits);
    digits.erase(0, digits.find_first_not_of('0'));
    digits.erase(digits.find_last_not_of('0') + 1);
    const std::size_t common = std::min(digits.size(), float128_overflow_digits.size());
    return std::string_view(digits).substr(0, common).compare(float128_overflow_digits.substr(0, common)) > 0;
}

enum class conversion
{
    converted,
    out_of_range,
    failed,
};

#if (defined(_LIBCPP_VERSION) && _LIBCPP_VERSION < 200000) || (defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE < 12)

// libc++ implements floating-point std::from_chars only from LLVM 20, and that of libstdc++ before GCC 12 reports a
// subnormal result as out of range without storing it. strtod_l with the "C" locale reads the same syntax without
// depending on the global locale. Only an overflow is reported as out of range: an underflow keeps the rounded value,
// a zero or a subnormal.
template <xisf_float T> conversion convert(std::string_view number, T& value)
{
    const std::string terminated(number);
    const locale_t c_locale = newlocale(LC_ALL_MASK, "C", locale_t{});
    if (c_locale == locale_t{}) {
        throw std::bad_alloc();
    }
    errno = 0;
    char* end = nullptr;
    if constexpr (std::same_as<T, float>) {
        value = strtof_l(terminated.c_str(), &end, c_locale);
    } else {
        value = strtod_l(terminated.c_str(), &end, c_locale);
    }
    const bool overflow = errno == ERANGE && std::isinf(value);
    freelocale(c_locale);

    if (end != terminated.c_str() + terminated.size()) {
        return conversion::failed;
    }
    return overflow ? conversion::out_of_range : conversion::converted;
}

#else

template <xisf_float T> conversion convert(std::string_view number, T& value) noexcept
{
    const char* const begin = number.data();
    const char* const end = begin + number.size();
    const auto result = std::from_chars(begin, end, value, std::chars_format::general);
    if (result.ec == std::errc::result_out_of_range) {
        return conversion::out_of_range;
    }
    // std::from_chars accepts a superset of the grammar, so it always reads the whole number.
    if (result.ec != std::errc{} || result.ptr != end) {
        return conversion::failed;
    }
    return conversion::converted;
}

#endif

} // namespace

// White space of spec §8.3.5. The specification uses the \s class of ECMAScript regular expressions, which also holds
// Unicode spaces such as U+00A0; only its ASCII members are accepted.
bool is_white_space(char c) noexcept
{
    return c == ' ' || (c >= '\t' && c <= '\r');
}

std::string_view trim_white_space(std::string_view text) noexcept
{
    while (!text.empty() && is_white_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_white_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

template <xisf_integer T> T parse_integer(std::string_view text)
{
    const std::optional<integer_text> scanned = scan_integer(text);
    if (!scanned) {
        throw invalid_data_error(errc::invalid_integer, quote(text) + " is not an integer");
    }

    const std::optional<uint128> value = magnitude(*scanned);
    constexpr unsigned bits = bit_count<T>();
    bool in_range = false;
    if (value) {
        if (scanned->radix != 10) {
            in_range = below_power_of_two(*value, bits);
        } else if constexpr (is_signed_integer<T>()) {
            in_range = below_power_of_two(*value, bits - 1) || (scanned->negative && is_power_of_two(*value, bits - 1));
        } else {
            in_range = !scanned->negative ? below_power_of_two(*value, bits) : *value == uint128{};
        }
    }
    if (!in_range) {
        throw_out_of_range(text, type_name<T>());
    }
    return from_bit_pattern<T>(scanned->negative ? negate(*value) : *value);
}

template <xisf_float T> T parse_float(std::string_view text)
{
    const std::optional<float_text> scanned = scan_float(text);
    if (!scanned) {
        throw invalid_data_error(errc::invalid_float, quote(text) + " is not a floating point value");
    }

    constexpr T infinity = std::numeric_limits<T>::infinity();
    constexpr T not_a_number = std::numeric_limits<T>::quiet_NaN();
    switch (scanned->kind) {
    case float_kind::nan:
        return scanned->negative ? -not_a_number : not_a_number;
    case float_kind::positive_infinity:
        return infinity;
    case float_kind::negative_infinity:
        return -infinity;
    case float_kind::number:
        break;
    }

    T value = 0;
    switch (convert(scanned->number, value)) {
    case conversion::converted:
        return value;
    case conversion::out_of_range:
        if (at_least_one(*scanned)) {
            throw_out_of_range(text, type_name<T>());
        }
        return scanned->negative ? -T{0} : T{0};
    case conversion::failed:
        break;
    }
    throw invalid_data_error(errc::invalid_float, quote(text) + " is not a floating point value");
}

bool is_float_text(std::string_view text) noexcept
{
    return scan_float(text).has_value();
}

std::string_view check_float128(std::string_view text)
{
    const std::optional<float_text> scanned = scan_float(text);
    if (!scanned) {
        throw invalid_data_error(errc::invalid_float, quote(text) + " is not a floating point value");
    }
    // Spec §8.3.3: encoders should write a non-finite value as NaN, +Inf or -Inf; the sign of a NaN means nothing.
    switch (scanned->kind) {
    case float_kind::nan:
        return "NaN";
    case float_kind::positive_infinity:
        return "+Inf";
    case float_kind::negative_infinity:
        return "-Inf";
    case float_kind::number:
        break;
    }
    if (beyond_float128(*scanned)) {
        throw_out_of_range(text, "Float128");
    }
    return trim_white_space(text);
}

bool parse_boolean(std::string_view text)
{
    const std::string_view value = trim_white_space(text);
    if (value == "true" || value == "1") {
        return true;
    }
    if (value == "false" || value == "0") {
        return false;
    }
    throw invalid_data_error(errc::invalid_boolean, quote(text) + " is not a Boolean value");
}

template <xisf_integer T> std::string format_integer(T value)
{
    if constexpr (std::same_as<T, uint128>) {
        return format_decimal(value);
    } else if constexpr (std::same_as<T, int128>) {
        const uint128 bits{.high = static_cast<std::uint64_t>(value.high), .low = value.low};
        return value.high < 0 ? '-' + format_decimal(negate(bits)) : format_decimal(bits);
    } else {
        std::array<char, 24> buffer{};
        return {buffer.data(), std::to_chars(buffer.data(), buffer.data() + buffer.size(), value).ptr};
    }
}

template <xisf_float T> std::string format_float(T value)
{
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value > 0 ? "+Inf" : "-Inf";
    }
    std::array<char, 32> buffer{};
    return {buffer.data(), std::to_chars(buffer.data(), buffer.data() + buffer.size(), value).ptr};
}

std::string_view format_boolean(bool value) noexcept
{
    return value ? "true" : "false";
}

template std::int8_t parse_integer<std::int8_t>(std::string_view);
template std::uint8_t parse_integer<std::uint8_t>(std::string_view);
template std::int16_t parse_integer<std::int16_t>(std::string_view);
template std::uint16_t parse_integer<std::uint16_t>(std::string_view);
template std::int32_t parse_integer<std::int32_t>(std::string_view);
template std::uint32_t parse_integer<std::uint32_t>(std::string_view);
template std::int64_t parse_integer<std::int64_t>(std::string_view);
template std::uint64_t parse_integer<std::uint64_t>(std::string_view);
template int128 parse_integer<int128>(std::string_view);
template uint128 parse_integer<uint128>(std::string_view);

template float parse_float<float>(std::string_view);
template double parse_float<double>(std::string_view);

template std::string format_integer<std::int8_t>(std::int8_t);
template std::string format_integer<std::uint8_t>(std::uint8_t);
template std::string format_integer<std::int16_t>(std::int16_t);
template std::string format_integer<std::uint16_t>(std::uint16_t);
template std::string format_integer<std::int32_t>(std::int32_t);
template std::string format_integer<std::uint32_t>(std::uint32_t);
template std::string format_integer<std::int64_t>(std::int64_t);
template std::string format_integer<std::uint64_t>(std::uint64_t);
template std::string format_integer<int128>(int128);
template std::string format_integer<uint128>(uint128);

template std::string format_float<float>(float);
template std::string format_float<double>(double);

} // namespace openxisf::detail
