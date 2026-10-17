// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/property.h>

#include <concepts>
#include <cstdint>
#include <string>
#include <string_view>

// The plain-text serialization of scalars (spec §8.3): integers, floating point values and Booleans, as they appear
// in XML attribute values. Parsers throw invalid_data_error, whose message has no context; the caller adds it.

namespace openxisf::detail {

/// The integer scalar types of spec §8.1.
template <typename T>
concept xisf_integer = std::same_as<T, std::int8_t> || std::same_as<T, std::uint8_t> || std::same_as<T, std::int16_t> ||
                       std::same_as<T, std::uint16_t> || std::same_as<T, std::int32_t> ||
                       std::same_as<T, std::uint32_t> || std::same_as<T, std::int64_t> ||
                       std::same_as<T, std::uint64_t> || std::same_as<T, int128> || std::same_as<T, uint128>;

/// The floating point scalar types of spec §8.1 that have a C++ type. Float128 values are kept as text.
template <typename T>
concept xisf_float = std::same_as<T, float> || std::same_as<T, double>;

/// True for the white space of spec §8.3.5.
[[nodiscard]] bool is_white_space(char c) noexcept;

/// text without leading and trailing white space.
[[nodiscard]] std::string_view trim_white_space(std::string_view text) noexcept;

/// Parses an integer of type T (spec §8.3.1, §8.3.2), ignoring leading and trailing white space. A binary, octal or
/// hexadecimal value is the bit pattern of T, so 0xFF is -1 for std::int8_t.
template <xisf_integer T> [[nodiscard]] T parse_integer(std::string_view text);

/// Parses a floating point value (spec §8.3.3), ignoring leading and trailing white space. The result is the nearest
/// value of T. A value too large for T is out of range; a value too small for T becomes a zero of the same sign.
template <xisf_float T> [[nodiscard]] T parse_float(std::string_view text);

/// True when text is a floating point value of spec §8.3.3, white space included.
[[nodiscard]] bool is_float_text(std::string_view text) noexcept;

/// Checks that text is a floating point value of spec §8.3.3 within the range of Float128, and returns it without
/// leading and trailing white space, or a non-finite value as NaN, +Inf or -Inf, the forms that encoders use. Float128
/// values are kept as text. A value is out of range when it rounds beyond the largest finite Float128, as far as its
/// first 64 significant digits tell.
[[nodiscard]] std::string_view check_float128(std::string_view text);

/// Parses a Boolean value (spec §8.3.4): true, false, 1 or 0, ignoring leading and trailing white space.
[[nodiscard]] bool parse_boolean(std::string_view text);

/// The decimal representation of value.
template <xisf_integer T> [[nodiscard]] std::string format_integer(T value);

/// The shortest representation that parses back to value, or NaN, +Inf or -Inf (spec §8.3.3).
template <xisf_float T> [[nodiscard]] std::string format_float(T value);

/// "true" or "false".
[[nodiscard]] std::string_view format_boolean(bool value) noexcept;

} // namespace openxisf::detail
