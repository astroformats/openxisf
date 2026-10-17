// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/text_grammar.h"

#include "core/xoshiro.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace {

using openxisf::errc;
using openxisf::int128;
using openxisf::invalid_data_error;
using openxisf::uint128;
using openxisf::detail::check_float128;
using openxisf::detail::format_boolean;
using openxisf::detail::format_float;
using openxisf::detail::format_integer;
using openxisf::detail::is_float_text;
using openxisf::detail::parse_boolean;
using openxisf::detail::parse_float;
using openxisf::detail::parse_integer;
using openxisf::detail::trim_white_space;
using openxisf::test::throws;

template <typename T> testing::AssertionResult invalid_integer(std::string_view text)
{
    return throws<invalid_data_error>(errc::invalid_integer, [text] { (void)parse_integer<T>(text); });
}

template <typename T> testing::AssertionResult integer_out_of_range(std::string_view text)
{
    return throws<invalid_data_error>(errc::value_out_of_range, [text] { (void)parse_integer<T>(text); });
}

template <typename T> testing::AssertionResult invalid_float(std::string_view text)
{
    return throws<invalid_data_error>(errc::invalid_float, [text] { (void)parse_float<T>(text); });
}

template <typename T> testing::AssertionResult float_out_of_range(std::string_view text)
{
    return throws<invalid_data_error>(errc::value_out_of_range, [text] { (void)parse_float<T>(text); });
}

// A decimal integer with its magnitude increased by one: the first value beyond an extreme.
std::string one_beyond(std::string decimal)
{
    std::size_t i = decimal.size();
    while (i > 0 && decimal[i - 1] == '9') {
        decimal[--i] = '0';
    }
    if (i == 0 || decimal[i - 1] == '-') {
        decimal.insert(i, "1");
    } else {
        ++decimal[i - 1];
    }
    return decimal;
}

// ---------------------------------------------------------------------------------------------------------------------
// Spec §8.3.1: decimal integers

TEST(text_grammar, decimal_integers_of_the_specification)
{
    EXPECT_EQ(parse_integer<std::int32_t>("987"), 987);
    EXPECT_EQ(parse_integer<std::int32_t>(" -123"), -123);
    EXPECT_EQ(parse_integer<std::int32_t>(" +45678"), 45678);
    EXPECT_EQ(parse_integer<std::int32_t>(" 0"), 0);
}

TEST(text_grammar, decimal_zero_takes_either_sign_in_every_type)
{
    for (const std::string_view zero : {"0", "+0", "-0"}) {
        EXPECT_EQ(parse_integer<std::int8_t>(zero), 0) << zero;
        EXPECT_EQ(parse_integer<std::uint8_t>(zero), 0U) << zero;
        EXPECT_EQ(parse_integer<std::uint64_t>(zero), 0U) << zero;
        EXPECT_EQ(parse_integer<uint128>(zero), uint128{}) << zero;
        EXPECT_EQ(parse_integer<int128>(zero), int128{}) << zero;
    }
}

TEST(text_grammar, decimal_integers_have_no_leading_zeros)
{
    for (const std::string_view text : {"00", "007", "-01", "+00", "0123"}) {
        EXPECT_TRUE(invalid_integer<std::int32_t>(text)) << text;
    }
}

TEST(text_grammar, malformed_integers_are_invalid)
{
    for (const std::string_view text :
         {"",     " ",    "+",     "-",   "--1", "+-1", "1.0",      "1e3",          "12a",
          "a12",  "1 2",  "1,000", "0x",  "0b",  "0o",  "x1",       "0h1",          "-0x1",
          "+0x1", "0x 1", "0x1 2", "0b2", "0o8", "0xG", "\xD9\xA1", "\xEF\xBC\x91", "1\xC2\xA0"}) {
        EXPECT_TRUE(invalid_integer<std::int64_t>(text)) << testing::PrintToString(text);
    }
}

// Each standard integer type at its extremes, and one beyond them.
template <typename T> class integer_type : public testing::Test
{};

using standard_integers = testing::Types<std::int8_t, std::uint8_t, std::int16_t, std::uint16_t, std::int32_t,
                                         std::uint32_t, std::int64_t, std::uint64_t>;
TYPED_TEST_SUITE(integer_type, standard_integers);

TYPED_TEST(integer_type, decimal_extremes_parse_and_one_beyond_is_out_of_range)
{
    using limits = std::numeric_limits<TypeParam>;
    const std::string min = std::to_string(limits::min());
    const std::string max = std::to_string(limits::max());

    EXPECT_EQ(parse_integer<TypeParam>(min), limits::min());
    EXPECT_EQ(parse_integer<TypeParam>(max), limits::max());
    EXPECT_TRUE(integer_out_of_range<TypeParam>(one_beyond(max)));
    EXPECT_TRUE(integer_out_of_range<TypeParam>(limits::is_signed ? one_beyond(min) : "-1"));
}

TYPED_TEST(integer_type, radix_values_are_the_bit_pattern_of_the_type)
{
    using limits = std::numeric_limits<TypeParam>;
    const std::string ones(2 * sizeof(TypeParam), 'F');
    const std::string sign_bit = "8" + std::string((2 * sizeof(TypeParam)) - 1, '0');

    if constexpr (limits::is_signed) {
        EXPECT_EQ(parse_integer<TypeParam>("0x" + ones), TypeParam{-1});
        EXPECT_EQ(parse_integer<TypeParam>("0x" + sign_bit), limits::min());
    } else {
        EXPECT_EQ(parse_integer<TypeParam>("0x" + ones), limits::max());
        EXPECT_EQ(parse_integer<TypeParam>("0x" + sign_bit), limits::max() / 2 + 1);
    }
    EXPECT_EQ(parse_integer<TypeParam>("0x0000" + ones), parse_integer<TypeParam>("0x" + ones));
    EXPECT_TRUE(integer_out_of_range<TypeParam>("0x1" + ones));
    EXPECT_TRUE(integer_out_of_range<TypeParam>("0b1" + std::string(8 * sizeof(TypeParam), '0')));
}

TEST(text_grammar, extremes_of_the_128_bit_types)
{
    constexpr std::uint64_t all = std::numeric_limits<std::uint64_t>::max();
    constexpr std::int64_t sign = std::numeric_limits<std::int64_t>::min();
    const std::string uint128_max = "340282366920938463463374607431768211455";
    const std::string int128_max = "170141183460469231731687303715884105727";
    const std::string int128_min = "-170141183460469231731687303715884105728";

    EXPECT_EQ(parse_integer<uint128>(uint128_max), (uint128{.high = all, .low = all}));
    EXPECT_EQ(parse_integer<int128>(int128_max),
              (int128{.high = std::numeric_limits<std::int64_t>::max(), .low = all}));
    EXPECT_EQ(parse_integer<int128>(int128_min), (int128{.high = sign, .low = 0}));
    EXPECT_EQ(parse_integer<int128>("-1"), (int128{.high = -1, .low = all}));
    EXPECT_EQ(parse_integer<int128>("0x" + std::string(32, 'f')), (int128{.high = -1, .low = all}));
    EXPECT_EQ(parse_integer<int128>("0x8" + std::string(31, '0')), (int128{.high = sign, .low = 0}));
    EXPECT_EQ(parse_integer<uint128>("18446744073709551616"), (uint128{.high = 1, .low = 0}));

    EXPECT_TRUE(integer_out_of_range<uint128>(one_beyond(uint128_max)));
    EXPECT_TRUE(integer_out_of_range<uint128>("-1"));
    EXPECT_TRUE(integer_out_of_range<int128>(one_beyond(int128_max)));
    EXPECT_TRUE(integer_out_of_range<int128>(one_beyond(int128_min)));
    EXPECT_TRUE(integer_out_of_range<uint128>("0x1" + std::string(32, '0')));
    EXPECT_TRUE(integer_out_of_range<uint128>("1" + std::string(100, '0')));
}

// ---------------------------------------------------------------------------------------------------------------------
// Spec §8.3.2: binary, octal and hexadecimal integers

TEST(text_grammar, radix_integers_of_the_specification)
{
    EXPECT_EQ(parse_integer<std::uint32_t>("0b10100111100101"), 10725U);
    EXPECT_EQ(parse_integer<std::uint32_t>(" 0o570261"), 192689U);
    EXPECT_EQ(parse_integer<std::int32_t>(" 0x80E950AB"), -2132193109);
    EXPECT_EQ(parse_integer<std::uint32_t>(" 0x80E950AB"), 2162774187U);
}

TEST(text_grammar, radix_prefixes_and_hexadecimal_digits_take_either_case)
{
    EXPECT_EQ(parse_integer<std::uint16_t>("0B101"), 5);
    EXPECT_EQ(parse_integer<std::uint16_t>("0O17"), 15);
    EXPECT_EQ(parse_integer<std::uint16_t>("0XfF"), 255);
    EXPECT_EQ(parse_integer<std::uint16_t>("0xff39"), 0xFF39);
}

TEST(text_grammar, radix_integers_may_start_with_zeros)
{
    EXPECT_EQ(parse_integer<std::uint8_t>("0x00FF"), 255);
    EXPECT_EQ(parse_integer<std::int8_t>("0x00FF"), -1);
    EXPECT_EQ(parse_integer<std::uint8_t>("0b0"), 0);
    EXPECT_EQ(parse_integer<std::uint64_t>("0x" + std::string(1000, '0') + "1"), 1U);
}

// ---------------------------------------------------------------------------------------------------------------------
// Spec §8.3.3: floating point values

TEST(text_grammar, floating_point_values_of_the_specification)
{
    EXPECT_EQ(parse_float<double>("123"), 123.0);
    EXPECT_EQ(parse_float<double>(" 123.456"), 123.456);
    EXPECT_EQ(parse_float<double>(" -123.456"), -123.456);
    EXPECT_EQ(parse_float<double>(" .123"), 0.123);
    EXPECT_EQ(parse_float<double>(" +.123"), 0.123);
    EXPECT_EQ(parse_float<double>(" 1e0"), 1.0);
    EXPECT_EQ(parse_float<double>(" -0.123e+02"), -12.3);
    EXPECT_TRUE(std::isnan(parse_float<double>(" NaN")));
    EXPECT_EQ(parse_float<double>(" -Inf"), -std::numeric_limits<double>::infinity());
}

TEST(text_grammar, every_non_finite_spelling_is_accepted)
{
    EXPECT_TRUE(std::isnan(parse_float<double>("NaN")));
    EXPECT_TRUE(std::isnan(parse_float<double>("nan")));
    EXPECT_TRUE(std::isnan(parse_float<double>("-nan")));
    EXPECT_TRUE(std::signbit(parse_float<double>("-nan")));
    EXPECT_EQ(parse_float<double>("+Inf"), std::numeric_limits<double>::infinity());
    EXPECT_EQ(parse_float<double>("inf"), std::numeric_limits<double>::infinity());
    EXPECT_EQ(parse_float<double>("-Inf"), -std::numeric_limits<double>::infinity());
    EXPECT_EQ(parse_float<double>("-inf"), -std::numeric_limits<double>::infinity());
    EXPECT_TRUE(std::isnan(parse_float<float>("NaN")));
    EXPECT_EQ(parse_float<float>("-inf"), -std::numeric_limits<float>::infinity());
}

TEST(text_grammar, other_non_finite_spellings_are_invalid)
{
    for (const std::string_view text : {"Inf", "+inf", "+nan", "-NaN", "+NaN", "NAN", "INF", "Infinity", "infinity",
                                        "-infinity", "nan(1)", "+ Inf", "- inf", "Nan", "iNf"}) {
        EXPECT_TRUE(invalid_float<double>(text)) << text;
    }
}

TEST(text_grammar, malformed_floating_point_values_are_invalid)
{
    for (const std::string_view text :
         {"",     " ",   ".",   "+",     "-",     "1.",   "-1.", "1.e5",  "e5",    ".e5", "1e",       "1e+",      "1e-",
          "1ee5", "--1", "+-1", "1.2.3", "0x1p3", "0x10", "1,5", "1 000", "1e5.0", "1d5", "\xD9\xA1", "1\xC2\xA0"}) {
        EXPECT_TRUE(invalid_float<double>(text)) << testing::PrintToString(text);
    }
}

TEST(text_grammar, floating_point_values_may_start_with_zeros)
{
    EXPECT_EQ(parse_float<double>("007.5"), 7.5);
    EXPECT_EQ(parse_float<double>("00"), 0.0);
    EXPECT_EQ(parse_float<double>("1e007"), 1e7);
}

TEST(text_grammar, zeros_keep_their_sign)
{
    EXPECT_FALSE(std::signbit(parse_float<double>("0")));
    EXPECT_FALSE(std::signbit(parse_float<double>("+0.0")));
    EXPECT_TRUE(std::signbit(parse_float<double>("-0")));
    EXPECT_TRUE(std::signbit(parse_float<float>("-.0e10")));
}

TEST(text_grammar, values_round_to_the_nearest_value_of_the_type)
{
    EXPECT_EQ(parse_float<float>("0.1"), 0.1F);
    EXPECT_EQ(parse_float<double>("0.1"), 0.1);
    EXPECT_EQ(parse_float<float>("1.25"), 1.25F);
    EXPECT_EQ(parse_float<double>("1e-300"), 1e-300);
}

TEST(text_grammar, extremes_and_subnormals_are_exact)
{
    EXPECT_EQ(parse_float<double>("1.7976931348623157e308"), std::numeric_limits<double>::max());
    EXPECT_EQ(parse_float<double>("-1.7976931348623157e308"), std::numeric_limits<double>::lowest());
    EXPECT_EQ(parse_float<double>("2.2250738585072014e-308"), std::numeric_limits<double>::min());
    EXPECT_EQ(parse_float<double>("4.9406564584124654e-324"), std::numeric_limits<double>::denorm_min());
    EXPECT_EQ(parse_float<double>("2.2250738585072009e-308"),
              std::numeric_limits<double>::min() - std::numeric_limits<double>::denorm_min());
    EXPECT_EQ(parse_float<float>("3.4028235e38"), std::numeric_limits<float>::max());
    EXPECT_EQ(parse_float<float>("1.1754944e-38"), std::numeric_limits<float>::min());
    EXPECT_EQ(parse_float<float>("1.4e-45"), std::numeric_limits<float>::denorm_min());
}

TEST(text_grammar, values_beyond_the_largest_of_the_type_are_out_of_range)
{
    for (const std::string_view text :
         {"1.8e308", "-1.8e308", "1e309", "1e999999999999999999999999", "123456789e999999999999", "0.0001e313"}) {
        EXPECT_TRUE(float_out_of_range<double>(text)) << text;
    }
    EXPECT_TRUE(float_out_of_range<float>("3.5e38"));
    EXPECT_TRUE(float_out_of_range<float>("-1e39"));
    EXPECT_TRUE(float_out_of_range<double>("1" + std::string(400, '0')));
}

TEST(text_grammar, values_below_the_smallest_of_the_type_become_zero_of_their_sign)
{
    EXPECT_EQ(parse_float<double>("1e-400"), 0.0);
    EXPECT_FALSE(std::signbit(parse_float<double>("1e-400")));
    EXPECT_TRUE(std::signbit(parse_float<double>("-1e-400")));
    EXPECT_EQ(parse_float<double>("0.000001e-999999999999999999999"), 0.0);
    EXPECT_EQ(parse_float<double>("1000e-330"), 0.0);
    EXPECT_EQ(parse_float<float>("1e-50"), 0.0F);
    EXPECT_EQ(parse_float<double>("0." + std::string(400, '0') + "1"), 0.0);
}

TEST(text_grammar, float_syntax_check_accepts_exactly_the_grammar)
{
    for (const std::string_view text : {"123", " -0.123e+02 ", ".5", "NaN", "-nan", "+Inf", "-inf",
                                        "1e99999999999999999999", "1.18973149535723176508575932662800702e4932"}) {
        EXPECT_TRUE(is_float_text(text)) << text;
    }
    for (const std::string_view text : {"", "1.", "Inf", "+nan", "0x1p3", "1e", "1 2"}) {
        EXPECT_FALSE(is_float_text(text)) << text;
    }
}

TEST(text_grammar, float128_values_are_checked_against_its_range)
{
    // The largest finite Float128 is 1.18973149535723176508575932662800701619...e4932, and the values from
    // 1.18973149535723176508575932662800707347...e4932 on round beyond it.
    for (const std::string_view text :
         {"1.18973149535723176508575932662800701619e4932", "1.189731495357231765085759326628007073e4932",
          "-1.18973149535723176508575932662800707347e4932", "118973149535723176508575932662800707347e4894",
          "0.000118973149535723176508575932662800707347e4936", "1e4932", "9e4931", "1e-4966", "1e-99999", "0e99999",
          "NaN", "-Inf", "  1.5  "}) {
        EXPECT_NO_THROW((void)check_float128(text)) << text;
    }
    for (const std::string_view text : {"1.18973149535723176508575932662800707348e4932", "-1.19e4932", "2e4932",
                                        "1e4933", "1e99999999999", "118973149535723176508575932662800707348e4894"}) {
        EXPECT_TRUE(throws<invalid_data_error>(errc::value_out_of_range, [text] { (void)check_float128(text); }))
            << text;
    }
    EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_float, [] { (void)check_float128("1."); }));
    EXPECT_EQ(check_float128("  1.5  "), "1.5");
}

TEST(text_grammar, float128_text_is_kept_as_written_but_non_finite_values)
{
    // Spec §8.3.3: encoders should write NaN, +Inf and -Inf, and decoders accept nan, -nan, inf and -inf too. A
    // Float128 keeps its text, which the writer writes back: a non-finite value in the forms that encoders use, a sign
    // of NaN dropped, and a finite one as written.
    for (const auto& [text, kept] :
         std::initializer_list<std::pair<std::string_view, std::string_view>>{{"NaN", "NaN"},
                                                                              {"nan", "NaN"},
                                                                              {" -nan ", "NaN"},
                                                                              {"+Inf", "+Inf"},
                                                                              {"inf", "+Inf"},
                                                                              {"-Inf", "-Inf"},
                                                                              {"\t-inf", "-Inf"},
                                                                              {"+1.5", "+1.5"},
                                                                              {"+.5", "+.5"},
                                                                              {"1E4932", "1E4932"},
                                                                              {"-0", "-0"}}) {
        EXPECT_EQ(check_float128(text), kept) << text;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Spec §8.3.4: Boolean values

TEST(text_grammar, booleans_are_words_or_digits)
{
    EXPECT_TRUE(parse_boolean("true"));
    EXPECT_FALSE(parse_boolean("false"));
    EXPECT_TRUE(parse_boolean("1"));
    EXPECT_FALSE(parse_boolean("0"));
    EXPECT_TRUE(parse_boolean(" true\n"));
}

TEST(text_grammar, other_booleans_are_invalid)
{
    for (const std::string_view text :
         {"", "True", "FALSE", "yes", "no", "2", "01", "+1", "-0", "1.0", "0x1", "t", "truefalse", "true false"}) {
        EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_boolean, [text] { (void)parse_boolean(text); })) << text;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Spec §8.3.5: white space

TEST(text_grammar, leading_and_trailing_ascii_white_space_is_ignored)
{
    EXPECT_EQ(trim_white_space(" \t\n\v\f\r123 \t\n\v\f\r"), "123");
    EXPECT_EQ(trim_white_space("1 2"), "1 2");
    EXPECT_EQ(trim_white_space(" \t "), "");
    EXPECT_EQ(parse_integer<std::int32_t>("\r\n\t-42\t\r\n"), -42);
    EXPECT_EQ(parse_float<double>("\v1.5\f"), 1.5);
}

// The specification writes white space as the \s class of ECMAScript regular expressions, which also holds Unicode
// spaces. Only its ASCII members are accepted: no encoder writes other spaces around a number.
TEST(text_grammar, unicode_spaces_are_not_white_space)
{
    EXPECT_TRUE(invalid_integer<std::int32_t>(" 1"));
    EXPECT_TRUE(invalid_integer<std::int32_t>("1 "));
}

// ---------------------------------------------------------------------------------------------------------------------
// Formatting

TYPED_TEST(integer_type, extremes_format_in_decimal_and_parse_back)
{
    using limits = std::numeric_limits<TypeParam>;
    for (const TypeParam value : {limits::min(), limits::max(), TypeParam{0}, TypeParam{1}}) {
        const std::string text = format_integer(value);
        EXPECT_EQ(text, std::to_string(value));
        EXPECT_EQ(parse_integer<TypeParam>(text), value);
    }
}

TEST(text_grammar, the_128_bit_types_format_in_decimal)
{
    constexpr std::uint64_t all = std::numeric_limits<std::uint64_t>::max();
    constexpr std::int64_t sign = std::numeric_limits<std::int64_t>::min();

    EXPECT_EQ(format_integer(uint128{.high = all, .low = all}), "340282366920938463463374607431768211455");
    EXPECT_EQ(format_integer(uint128{.high = 1, .low = 0}), "18446744073709551616");
    EXPECT_EQ(format_integer(uint128{.high = 0, .low = 42}), "42");
    EXPECT_EQ(format_integer(uint128{}), "0");
    EXPECT_EQ(format_integer(int128{.high = sign, .low = 0}), "-170141183460469231731687303715884105728");
    EXPECT_EQ(format_integer(int128{.high = -1, .low = all}), "-1");
    EXPECT_EQ(format_integer(int128{.high = 0, .low = 1'000'000'000}), "1000000000");
    EXPECT_EQ(format_integer(int128{.high = 1, .low = 1'000'000'000}), "18446744074709551616");
}

TEST(text_grammar, floats_format_as_the_shortest_round_trip)
{
    EXPECT_EQ(format_float(0.1), "0.1");
    EXPECT_EQ(format_float(0.1F), "0.1");
    EXPECT_EQ(format_float(1e300), "1e+300");
    EXPECT_EQ(format_float(-12.3), "-12.3");
    EXPECT_EQ(format_float(100.0), "100");
    EXPECT_EQ(format_float(-0.0), "-0");
    EXPECT_EQ(format_float(std::numeric_limits<double>::denorm_min()), "5e-324");
}

TEST(text_grammar, non_finite_values_format_in_the_preferred_spelling)
{
    EXPECT_EQ(format_float(std::numeric_limits<double>::quiet_NaN()), "NaN");
    EXPECT_EQ(format_float(-std::numeric_limits<double>::quiet_NaN()), "NaN");
    EXPECT_EQ(format_float(std::numeric_limits<float>::infinity()), "+Inf");
    EXPECT_EQ(format_float(-std::numeric_limits<double>::infinity()), "-Inf");
}

// Random bit patterns cover every exponent, subnormals included. The seed is fixed, so the test is repeatable.
TEST(text_grammar, formatted_floats_follow_the_grammar_and_parse_back_exactly)
{
    openxisf::detail::xoshiro256starstar generator({1, 2, 3, 4});
    for (int i = 0; i < 20'000; ++i) {
        const auto bits = generator();
        const auto as_double = std::bit_cast<double>(bits);
        const auto as_float = std::bit_cast<float>(static_cast<std::uint32_t>(bits >> 32U));
        if (std::isfinite(as_double)) {
            const std::string text = format_float(as_double);
            ASSERT_TRUE(is_float_text(text)) << text;
            ASSERT_EQ(std::bit_cast<std::uint64_t>(parse_float<double>(text)), bits) << text;
        }
        if (std::isfinite(as_float)) {
            const std::string text = format_float(as_float);
            ASSERT_TRUE(is_float_text(text)) << text;
            ASSERT_EQ(parse_float<float>(text), as_float) << text;
            ASSERT_EQ(std::signbit(parse_float<float>(text)), std::signbit(as_float)) << text;
        }
    }
}

TEST(text_grammar, booleans_format_as_words)
{
    EXPECT_EQ(format_boolean(true), "true");
    EXPECT_EQ(format_boolean(false), "false");
}

} // namespace
