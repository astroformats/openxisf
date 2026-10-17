// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The public types of property values: how C++ values map to property types (spec §8.4.4), the checked accessors, and
// the lists of properties of an object (spec §8.4.1).

#include <openxisf/property.h>

#include "core/xoshiro.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <complex>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::complex128;
using openxisf::date_time;
using openxisf::errc;
using openxisf::float128;
using openxisf::int128;
using openxisf::property;
using openxisf::property_list;
using openxisf::property_type;
using openxisf::property_value;
using openxisf::to_double;
using openxisf::uint128;
using openxisf::usage_error;
using openxisf::test::throws;

TEST(property_value, a_default_value_is_an_empty_string)
{
    const property_value value;
    EXPECT_EQ(value.type(), property_type::string);
    EXPECT_EQ(value.get<std::string>(), "");
    EXPECT_EQ(value.length(), 0U);
}

TEST(property_value, each_scalar_type_of_cpp_makes_its_property_type)
{
    EXPECT_EQ(property_value(true).type(), property_type::boolean);
    EXPECT_EQ(property_value(std::int8_t{-1}).type(), property_type::int8);
    EXPECT_EQ(property_value(std::uint8_t{1}).type(), property_type::uint8);
    EXPECT_EQ(property_value(std::int16_t{-1}).type(), property_type::int16);
    EXPECT_EQ(property_value(std::uint16_t{1}).type(), property_type::uint16);
    EXPECT_EQ(property_value(std::int32_t{-1}).type(), property_type::int32);
    EXPECT_EQ(property_value(std::uint32_t{1}).type(), property_type::uint32);
    EXPECT_EQ(property_value(std::int64_t{-1}).type(), property_type::int64);
    EXPECT_EQ(property_value(std::uint64_t{1}).type(), property_type::uint64);
    EXPECT_EQ(property_value(int128{}).type(), property_type::int128);
    EXPECT_EQ(property_value(uint128{}).type(), property_type::uint128);
    EXPECT_EQ(property_value(1.5F).type(), property_type::float32);
    EXPECT_EQ(property_value(1.5).type(), property_type::float64);
    EXPECT_EQ(property_value(std::complex<float>(1, 2)).type(), property_type::complex32);
    EXPECT_EQ(property_value(std::complex<double>(1, 2)).type(), property_type::complex64);
    EXPECT_EQ(property_value(date_time{}).type(), property_type::time_point);
}

TEST(property_value, other_integer_types_map_by_width_and_signedness)
{
    // long and long long are the 64-bit integers on one platform or the other, and int is 32 bits everywhere.
    EXPECT_EQ(property_value(5).type(), property_type::int32);
    EXPECT_EQ(property_value(5U).type(), property_type::uint32);
    EXPECT_EQ(property_value(-5LL).type(), property_type::int64);
    EXPECT_EQ(property_value(5ULL).type(), property_type::uint64);
    EXPECT_EQ(property_value(-5LL).get<std::int64_t>(), -5);
    const auto expected_long = sizeof(long) == 8 ? property_type::int64 : property_type::int32;
    EXPECT_EQ(property_value(-5L).type(), expected_long);
    EXPECT_EQ(property_value(static_cast<signed char>(-3)).get<std::int8_t>(), -3);
    EXPECT_EQ(property_value(static_cast<unsigned short>(3)).get<std::uint16_t>(), 3U);
}

TEST(property_value, character_types_are_promoted_to_32_bit_integers)
{
    // The behaviour of 1.0, pinned: the constructor of other integer types leaves the character types out, and the
    // promotions of C++ give them the constructors of the 32-bit integers, so that a character becomes its code.
    EXPECT_EQ(property_value('R'), property_value(std::int32_t{82}));
    EXPECT_EQ(property_value(u8'x').type(), property_type::int32);
    EXPECT_EQ(property_value(u'x').type(), property_type::int32);
    EXPECT_EQ(property_value(L'x').type(), property_type::int32);
    EXPECT_EQ(property_value(U'x'), property_value(std::uint32_t{0x78}));
}

TEST(property_value, text_makes_a_string_value)
{
    const std::string text = "  Ñandú  ";
    EXPECT_EQ(property_value(text).get<std::string>(), text);
    EXPECT_EQ(property_value(std::string_view(text)).get<std::string>(), text);
    EXPECT_EQ(property_value("text").type(), property_type::string);
    EXPECT_EQ(property_value("text").get<std::string>(), "text");
}

TEST(property_value, get_returns_the_value_in_its_own_type_only)
{
    const property_value value(1.5F);
    EXPECT_EQ(value.get<float>(), 1.5F);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&value] { (void)value.get<double>(); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&value] { (void)value.get<std::string>(); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&value] { (void)value.elements<float>(); }));
    EXPECT_EQ(property_value(date_time{.year = 2026, .month = 3}).get<date_time>().month, 3U);
    EXPECT_EQ(property_value(int128{.high = -1, .low = 7}).get<int128>(), (int128{.high = -1, .low = 7}));
}

template <typename T> void check_vector(property_type vector, property_type matrix)
{
    const std::vector<T> elements(6);
    const property_value as_vector(elements);
    EXPECT_EQ(as_vector.type(), vector);
    EXPECT_EQ(as_vector.length(), 6U);
    EXPECT_EQ(as_vector.elements<T>().size(), 6U);
    EXPECT_EQ(as_vector.rows(), 0U);
    EXPECT_EQ(as_vector.columns(), 0U);

    const property_value as_matrix = property_value::matrix(2, 3, elements);
    EXPECT_EQ(as_matrix.type(), matrix);
    EXPECT_EQ(as_matrix.length(), 6U);
    EXPECT_EQ(as_matrix.rows(), 2U);
    EXPECT_EQ(as_matrix.columns(), 3U);
    EXPECT_EQ(as_matrix.elements<T>().size(), 6U);
}

TEST(property_value, each_element_type_makes_its_vector_and_matrix_types)
{
    check_vector<std::int8_t>(property_type::i8_vector, property_type::i8_matrix);
    check_vector<std::uint8_t>(property_type::ui8_vector, property_type::ui8_matrix);
    check_vector<std::int16_t>(property_type::i16_vector, property_type::i16_matrix);
    check_vector<std::uint16_t>(property_type::ui16_vector, property_type::ui16_matrix);
    check_vector<std::int32_t>(property_type::i32_vector, property_type::i32_matrix);
    check_vector<std::uint32_t>(property_type::ui32_vector, property_type::ui32_matrix);
    check_vector<std::int64_t>(property_type::i64_vector, property_type::i64_matrix);
    check_vector<std::uint64_t>(property_type::ui64_vector, property_type::ui64_matrix);
    check_vector<int128>(property_type::i128_vector, property_type::i128_matrix);
    check_vector<uint128>(property_type::ui128_vector, property_type::ui128_matrix);
    check_vector<float>(property_type::f32_vector, property_type::f32_matrix);
    check_vector<double>(property_type::f64_vector, property_type::f64_matrix);
    check_vector<float128>(property_type::f128_vector, property_type::f128_matrix);
    check_vector<std::complex<float>>(property_type::c32_vector, property_type::c32_matrix);
    check_vector<std::complex<double>>(property_type::c64_vector, property_type::c64_matrix);
    check_vector<complex128>(property_type::c128_vector, property_type::c128_matrix);
}

TEST(property_value, a_matrix_needs_rows_times_columns_elements)
{
    EXPECT_EQ(property_value::matrix(0, 5, std::vector<double>{}).length(), 0U);
    EXPECT_EQ(property_value::matrix(4, 0, std::vector<double>{}).rows(), 4U);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [] { (void)property_value::matrix(2, 3, std::vector<double>(5)); }));
    // A product that wraps around to the size, 2^64 to 0, is refused too.
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] {
        (void)property_value::matrix(std::uint64_t{1} << 32U, std::uint64_t{1} << 32U, std::vector<double>{});
    }));
}

TEST(property_value, float128_and_complex128_scalars_are_kept_as_text)
{
    const property_value quad = property_value::from_float128_text("1.000000000000000000000000000000001");
    EXPECT_EQ(quad.type(), property_type::float128);
    EXPECT_EQ(quad.get<std::string>(), "1.000000000000000000000000000000001");
    const property_value complex = property_value::from_complex128_text("(1e4000,-2)");
    EXPECT_EQ(complex.type(), property_type::complex128);
    EXPECT_EQ(complex.get<std::string>(), "(1e4000,-2)");
    // Without the white space around the value and its parts, which encoders do not write (spec §8.3.5).
    EXPECT_EQ(property_value::from_float128_text(" 1.5\t").get<std::string>(), "1.5");
    EXPECT_EQ(property_value::from_complex128_text(" ( 1.5 , -2 )\n").get<std::string>(), "(1.5,-2)");

    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { (void)property_value::from_float128_text("x"); }));
    EXPECT_TRUE(
        throws<usage_error>(errc::invalid_argument, [] { (void)property_value::from_float128_text("1e4933"); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { (void)property_value::from_complex128_text("1"); }));
}

TEST(property_value, equality_compares_type_value_and_dimensions)
{
    EXPECT_EQ(property_value(std::int32_t{1}), property_value(1));
    EXPECT_NE(property_value(std::int32_t{1}), property_value(std::uint32_t{1}));
    EXPECT_NE(property_value::matrix(1, 2, std::vector<double>{1, 2}),
              property_value::matrix(2, 1, std::vector<double>{1, 2}));
    EXPECT_NE(property_value(std::vector<double>{1, 2}), property_value::matrix(1, 2, std::vector<double>{1, 2}));
    // A Float128 kept as text is not a String with the same text.
    EXPECT_NE(property_value::from_float128_text("1"), property_value("1"));
}

// -------------------------------------------------------------------------------------------------------------------
// to_double

// The binary128 bits of a double, which binary128 holds exactly.
float128 quad_of(double value)
{
    const auto bits = std::bit_cast<std::uint64_t>(value);
    const std::uint64_t sign = bits & (std::uint64_t{1} << 63U);
    const auto exponent = static_cast<int>((bits >> 52U) & 0x7FFU);
    std::uint64_t fraction = bits & ((std::uint64_t{1} << 52U) - 1);
    if (exponent == 0x7FF) {
        return {.high = sign | (std::uint64_t{0x7FFF} << 48U) | (fraction >> 4U), .low = fraction << 60U};
    }
    if (exponent == 0 && fraction == 0) {
        return {.high = sign, .low = 0};
    }
    int unbiased = exponent - 1023;
    if (exponent == 0) {
        // A subnormal: normalize it, binary128 has the range.
        unbiased = -1022;
        while ((fraction & (std::uint64_t{1} << 52U)) == 0) {
            fraction <<= 1U;
            --unbiased;
        }
        fraction &= (std::uint64_t{1} << 52U) - 1;
    }
    const int biased_exponent = unbiased + 16383;
    const auto biased = static_cast<std::uint64_t>(biased_exponent);
    return {.high = sign | (biased << 48U) | (fraction >> 4U), .low = fraction << 60U};
}

TEST(to_double, known_values)
{
    EXPECT_EQ(to_double({.high = 0x3FFF'0000'0000'0000, .low = 0}), 1.0);
    EXPECT_EQ(to_double({.high = 0xC000'4000'0000'0000, .low = 0}), -2.5);
    EXPECT_EQ(to_double({}), 0.0);
    EXPECT_TRUE(std::signbit(to_double({.high = 0x8000'0000'0000'0000, .low = 0})));
    EXPECT_EQ(to_double({.high = 0x7FFF'0000'0000'0000, .low = 0}), std::numeric_limits<double>::infinity());
    EXPECT_EQ(to_double({.high = 0xFFFF'0000'0000'0000, .low = 0}), -std::numeric_limits<double>::infinity());
    EXPECT_TRUE(std::isnan(to_double({.high = 0x7FFF'8000'0000'0000, .low = 0})));
    EXPECT_TRUE(std::isnan(to_double({.high = 0x7FFF'0000'0000'0000, .low = 1})));
    // The largest finite binary128 is beyond double, and so is 2^1024.
    EXPECT_EQ(to_double({.high = 0x7FFE'FFFF'FFFF'FFFF, .low = ~std::uint64_t{0}}),
              std::numeric_limits<double>::infinity());
    EXPECT_EQ(to_double({.high = std::uint64_t{1024 + 16383} << 48U, .low = 0}),
              std::numeric_limits<double>::infinity());
    // The smallest binary128 values are far below the smallest double.
    EXPECT_EQ(to_double({.high = 0, .low = 1}), 0.0);
    EXPECT_EQ(to_double({.high = std::uint64_t{1} << 48U, .low = 0}), 0.0);
}

TEST(to_double, rounds_to_nearest_even)
{
    // 1 + 2^-53 is halfway between 1 and the next double, and rounds to the even one, 1. A bit more rounds up, and so
    // does 1 + 3 × 2^-53, halfway with an odd neighbour below.
    // In binary128, 2^-53 is bit 59 of the low half of the fraction.
    const std::uint64_t one = 0x3FFF'0000'0000'0000;
    const std::uint64_t half_ulp = std::uint64_t{1} << 59U;
    const double next = std::nextafter(1.0, 2.0);
    EXPECT_EQ(to_double({.high = one, .low = half_ulp}), 1.0);
    EXPECT_EQ(to_double({.high = one, .low = half_ulp | 1U}), next);
    EXPECT_EQ(to_double({.high = one, .low = 3 * half_ulp}), std::nextafter(next, 2.0));
    // Just below the overflow threshold rounds to the largest double, at it to infinity.
    const std::uint64_t max_exponent = std::uint64_t{1023 + 16383} << 48U;
    const std::uint64_t all_48 = (std::uint64_t{1} << 48U) - 1; // the fraction bits of the high half
    EXPECT_EQ(to_double({.high = max_exponent | all_48, .low = 0xF000'0000'0000'0000}),
              std::numeric_limits<double>::max());
    EXPECT_EQ(to_double({.high = max_exponent | all_48, .low = 0xF800'0000'0000'0000}),
              std::numeric_limits<double>::infinity());
    // Half of the smallest subnormal rounds to zero, a bit more to the smallest subnormal.
    const std::uint64_t half_denormal_min = std::uint64_t{16383 - 1075} << 48U;
    EXPECT_EQ(to_double({.high = half_denormal_min, .low = 0}), 0.0);
    EXPECT_EQ(to_double({.high = half_denormal_min, .low = 1}), std::numeric_limits<double>::denorm_min());
}

TEST(to_double, gives_back_every_double)
{
    const auto check = [](double value) {
        ASSERT_EQ(std::bit_cast<std::uint64_t>(to_double(quad_of(value))), std::bit_cast<std::uint64_t>(value))
            << value;
    };
    for (const double value :
         {1.0, -1.0, 0.1, 1e300, -1e-300, std::numeric_limits<double>::max(), std::numeric_limits<double>::min(),
          std::numeric_limits<double>::denorm_min(), std::nextafter(std::numeric_limits<double>::min(), 0.0)}) {
        check(value);
    }
    openxisf::detail::xoshiro256starstar random({1, 2, 3, 4});
    for (int i = 0; i < 100'000; ++i) {
        const auto value = std::bit_cast<double>(random());
        if (!std::isnan(value)) {
            check(value);
        }
    }
}

// -------------------------------------------------------------------------------------------------------------------
// property_list

property item(std::string_view id, property_value value)
{
    return {.id = std::string(id), .value = std::move(value)};
}

TEST(property_list, set_appends_or_replaces_in_place)
{
    property_list list;
    EXPECT_TRUE(list.empty());
    list.set("A", 1);
    list.set("B", 2);
    list.set(item("A", "replaced"));
    ASSERT_EQ(list.size(), 2U);
    EXPECT_EQ(list.begin()->id, "A");
    EXPECT_EQ(list.begin()->value.get<std::string>(), "replaced");
    EXPECT_EQ(std::next(list.begin())->id, "B");
}

TEST(property_list, set_with_a_value_drops_the_format_and_the_comment_that_it_replaces)
{
    // The behaviour of 1.0, pinned (property.h): set(id, value) puts a property without format and comment in place of
    // the one of that identifier, whose format and comment are gone.
    property_list list;
    list.set(property{.id = "A", .value = 1, .format = openxisf::property_format{.width = 5}, .comment = "the first"});
    list.set("A", 2);
    EXPECT_EQ(list.at("A"), (property{.id = "A", .value = 2}));
}

TEST(property_list, finds_properties_by_identifier)
{
    property_list list;
    list.set(item("Instrument:ExposureTime", 300.0F));
    EXPECT_TRUE(list.contains("Instrument:ExposureTime"));
    EXPECT_FALSE(list.contains("instrument:exposuretime"));
    EXPECT_EQ(list.find("Missing"), nullptr);
    EXPECT_EQ(list.at("Instrument:ExposureTime").value.get<float>(), 300.0F);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&list] { (void)list.at("Missing"); }));
    EXPECT_TRUE(list.erase("Instrument:ExposureTime"));
    EXPECT_FALSE(list.erase("Instrument:ExposureTime"));
    EXPECT_TRUE(list.empty());
}

TEST(property_list, a_list_from_a_vector_keeps_the_order_and_refuses_duplicates)
{
    const property_list list({item("B", 1), item("A", 2)});
    ASSERT_EQ(list.size(), 2U);
    EXPECT_EQ(list.begin()->id, "B");
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { (void)property_list({item("A", 1), item("A", 2)}); }));
}

} // namespace
