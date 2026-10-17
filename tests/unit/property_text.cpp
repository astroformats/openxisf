// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Properties as text: identifiers (spec §8.4.1), and the value attributes of scalars (spec §11.1.4), complex numbers
// (spec §11.1.5) and TimePoints (spec §11.1.7), with the range of each declared type.

#include "model/property_text.h"

#include "support/throws.h"

#include <gtest/gtest.h>

#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace {

using openxisf::date_time;
using openxisf::errc;
using openxisf::int128;
using openxisf::invalid_data_error;
using openxisf::property_type;
using openxisf::property_value;
using openxisf::uint128;
using openxisf::usage_error;
using openxisf::detail::check_complex128;
using openxisf::detail::format_value_attribute;
using openxisf::detail::is_property_id;
using openxisf::detail::parse_complex;
using openxisf::detail::parse_value_attribute;
using openxisf::test::throws;

TEST(property_text, identifiers)
{
    // The examples of spec §8.4.1, and the reserved identifiers.
    for (const std::string_view id : {"MyFirstProperty", "mySecondOne234", "_this_1_is_a_test", "Namespace:Property",
                                      "foo:bar:Foo2_Bar3", "XISF:CreationTime", "_", "a", "A:_:b9",
                                      "AstrometricSolution:DistortionModel:ImageToProjection:Local:X:Nodes"}) {
        EXPECT_TRUE(is_property_id(id)) << id;
    }
    for (const std::string_view id :
         {"", "1abc", ":a", "a:", "a::b", "a:1b", "a b", "a-b", "a.b", "Température", "a:b:", " a", "a ", "\xC3\xA9"}) {
        EXPECT_FALSE(is_property_id(id)) << id;
    }
}

// The value of text as a value attribute of type, which must be valid.
property_value value_of(property_type type, std::string_view text)
{
    return parse_value_attribute(type, text);
}

testing::AssertionResult refused(property_type type, std::string_view text, errc code)
{
    return throws<invalid_data_error>(code, [type, text] { (void)parse_value_attribute(type, text); });
}

TEST(property_text, the_examples_of_the_specification)
{
    // Spec §11.1.2, §11.1.4, §11.1.5 and §11.1.7.
    EXPECT_EQ(value_of(property_type::uint32, "2540").get<std::uint32_t>(), 2540U);
    EXPECT_EQ(value_of(property_type::float32, "1.25").get<float>(), 1.25F);
    EXPECT_EQ(value_of(property_type::uint16, "0xff39").get<std::uint16_t>(), 0xff39U);
    EXPECT_EQ(value_of(property_type::boolean, "true").get<bool>(), true);
    EXPECT_EQ(value_of(property_type::uint32, "0x8000FFA0").get<std::uint32_t>(), 0x8000FFA0U);
    EXPECT_EQ(value_of(property_type::float64, "1.1234e+04").get<double>(), 1.1234e+04);
    EXPECT_EQ(value_of(property_type::complex32, "(0.123,-0.735e-02)").get<std::complex<float>>(),
              std::complex<float>(0.123F, -0.735e-02F));
    EXPECT_EQ(value_of(property_type::time_point, "2014-12-01T18:07:54Z").get<date_time>(),
              (date_time{.year = 2014, .month = 12, .day = 1, .hour = 18, .minute = 7, .second = 54}));
    EXPECT_EQ(
        value_of(property_type::time_point, "2015-01-23T19:52:31.46Z").get<date_time>(),
        (date_time{
            .year = 2015, .month = 1, .day = 23, .hour = 19, .minute = 52, .second = 31, .nanosecond = 460'000'000}));
}

TEST(property_text, every_scalar_type_takes_its_range)
{
    EXPECT_EQ(value_of(property_type::boolean, "0").get<bool>(), false);
    EXPECT_EQ(value_of(property_type::int8, "-128").get<std::int8_t>(), -128);
    EXPECT_EQ(value_of(property_type::uint8, "255").get<std::uint8_t>(), 255U);
    EXPECT_EQ(value_of(property_type::int16, "-32768").get<std::int16_t>(), -32768);
    EXPECT_EQ(value_of(property_type::uint16, "65535").get<std::uint16_t>(), 65535U);
    EXPECT_EQ(value_of(property_type::int32, "-2147483648").get<std::int32_t>(),
              std::numeric_limits<std::int32_t>::min());
    EXPECT_EQ(value_of(property_type::uint32, "4294967295").get<std::uint32_t>(), 4294967295U);
    EXPECT_EQ(value_of(property_type::int64, "-9223372036854775808").get<std::int64_t>(),
              std::numeric_limits<std::int64_t>::min());
    EXPECT_EQ(value_of(property_type::uint64, "18446744073709551615").get<std::uint64_t>(),
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(value_of(property_type::int128, "-1").get<int128>(), (int128{.high = -1, .low = ~std::uint64_t{0}}));
    EXPECT_EQ(value_of(property_type::uint128, "18446744073709551616").get<uint128>(), (uint128{.high = 1, .low = 0}));
    EXPECT_EQ(value_of(property_type::float32, "3.4028235e38").get<float>(), std::numeric_limits<float>::max());
    EXPECT_EQ(value_of(property_type::float64, "1e-300").get<double>(), 1e-300);

    EXPECT_TRUE(refused(property_type::int8, "128", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::uint8, "256", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::uint8, "-1", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::int16, "32768", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::uint16, "0x10000", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::int32, "-2147483649", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::uint32, "4294967296", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::int64, "9223372036854775808", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::uint64, "18446744073709551616", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::uint128, "-1", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::float32, "3.5e38", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::float64, "1e309", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::float128, "1.19e4932", errc::value_out_of_range));
    EXPECT_TRUE(refused(property_type::complex32, "(1,1e39)", errc::value_out_of_range));
}

TEST(property_text, malformed_values_have_the_code_of_their_grammar)
{
    EXPECT_TRUE(refused(property_type::boolean, "yes", errc::invalid_boolean));
    EXPECT_TRUE(refused(property_type::int32, "1.5", errc::invalid_integer));
    EXPECT_TRUE(refused(property_type::uint16, "", errc::invalid_integer));
    EXPECT_TRUE(refused(property_type::int128, "0x", errc::invalid_integer));
    EXPECT_TRUE(refused(property_type::float32, "1.", errc::invalid_float));
    EXPECT_TRUE(refused(property_type::float128, "Inf", errc::invalid_float));
    EXPECT_TRUE(refused(property_type::complex64, "1,2", errc::invalid_complex));
    EXPECT_TRUE(refused(property_type::complex64, "(1,2", errc::invalid_complex));
    EXPECT_TRUE(refused(property_type::complex64, "1,2)", errc::invalid_complex));
    EXPECT_TRUE(refused(property_type::complex64, "[1,2)", errc::invalid_complex));
    EXPECT_TRUE(refused(property_type::complex64, "(1 2)", errc::invalid_complex));
    EXPECT_TRUE(refused(property_type::complex64, "(1,2,3)", errc::invalid_complex));
    EXPECT_TRUE(refused(property_type::complex64, "()", errc::invalid_complex));
    EXPECT_TRUE(refused(property_type::complex64, "(,2)", errc::invalid_float));
    EXPECT_TRUE(refused(property_type::complex128, "(1,x)", errc::invalid_float));
    EXPECT_TRUE(refused(property_type::time_point, "2014-12-01 18:07:54", errc::invalid_time_point));
}

TEST(property_text, white_space_around_values_and_parts_is_ignored)
{
    // Spec §8.3.5, and spec §11.1.7 for TimePoints.
    EXPECT_EQ(value_of(property_type::int16, " \t-7\n").get<std::int16_t>(), -7);
    EXPECT_EQ(value_of(property_type::complex64, " ( 1.5 , -2 ) ").get<std::complex<double>>(),
              std::complex<double>(1.5, -2));
    EXPECT_EQ(value_of(property_type::time_point, " 2026-01-02T03:04:05Z ").get<date_time>().second, 5U);
    // A Float128 keeps its text, without the white space, and a Complex128 without that of its parts too.
    EXPECT_EQ(value_of(property_type::float128, "  1.25  ").get<std::string>(), "1.25");
    EXPECT_EQ(check_complex128(" (1.5, 2) "), "(1.5,2)");
    EXPECT_EQ(value_of(property_type::complex128, " ( 1.5 ,\t-2 ) ").get<std::string>(), "(1.5,-2)");
}

TEST(property_text, complex_numbers_and_their_non_finite_parts)
{
    const std::complex<double> value = parse_complex<double>("(NaN,-Inf)");
    EXPECT_TRUE(std::isnan(value.real()));
    EXPECT_EQ(value.imag(), -std::numeric_limits<double>::infinity());
    EXPECT_EQ(parse_complex<float>("(-0,0)").real(), 0.0F);
    EXPECT_TRUE(std::signbit(parse_complex<float>("(-0,0)").real()));
    // A Complex128 keeps its parts as Float128 values do: non-finite ones in the forms that encoders use (spec §8.3.3).
    EXPECT_EQ(check_complex128("(-nan,inf)"), "(NaN,+Inf)");
    EXPECT_EQ(check_complex128("( +.5 , -inf )"), "(+.5,-Inf)");
}

TEST(property_text, every_value_attribute_parses_back_from_its_text)
{
    const date_time time{
        .year = 2026, .month = 3, .day = 14, .hour = 1, .minute = 59, .second = 26, .nanosecond = 535'000'000};
    for (const property_value& value :
         {property_value(true), property_value(std::int8_t{-128}), property_value(std::uint8_t{255}),
          property_value(std::int16_t{-1}), property_value(std::uint16_t{65535}), property_value(std::int32_t{-5}),
          property_value(std::uint32_t{7}), property_value(std::int64_t{-9007199254740991}),
          property_value(std::uint64_t{18446744073709551615ULL}), property_value(int128{.high = -2, .low = 9}),
          property_value(uint128{.high = 3, .low = 1}), property_value(0.1F), property_value(1e-300),
          property_value(std::numeric_limits<double>::infinity()), property_value(std::complex<float>(1.5F, -2.25F)),
          property_value(std::complex<double>(1e-10, 3)), property_value::from_float128_text("1e-4950"),
          property_value::from_complex128_text("(1,-1)"), property_value(time)}) {
        const std::string text = format_value_attribute(value);
        EXPECT_EQ(parse_value_attribute(value.type(), text), value) << text;
    }
    EXPECT_EQ(format_value_attribute(property_value(true)), "true");
    EXPECT_EQ(format_value_attribute(property_value(std::complex<float>(1.5F, -2.25F))), "(1.5,-2.25)");
    EXPECT_EQ(format_value_attribute(property_value(time)), "2026-03-14T01:59:26.535Z");
}

TEST(property_text, other_types_have_no_value_attribute)
{
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [] { (void)parse_value_attribute(property_type::string, "x"); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [] { (void)parse_value_attribute(property_type::f64_vector, "1"); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { (void)format_value_attribute(property_value("x")); }));
    // A TimePoint that is not a date cannot be written.
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [] { (void)format_value_attribute(property_value(date_time{.month = 13})); }));
}

} // namespace
