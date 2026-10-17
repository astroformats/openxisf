// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/utc_time.h"

#include "support/throws.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string_view>

namespace {

using openxisf::date_time;
using openxisf::errc;
using openxisf::invalid_data_error;
using openxisf::detail::civil_date;
using openxisf::detail::civil_from_days;
using openxisf::detail::days_from_civil;
using openxisf::detail::format_time_point;
using openxisf::detail::format_utc_time;
using openxisf::detail::is_valid_time_point;
using openxisf::detail::parse_time_point;
using openxisf::test::throws;
using std::chrono::nanoseconds;
using std::chrono::sys_time;

// The days were computed with Python's datetime module.
TEST(utc_time, known_dates)
{
    const auto check = [](civil_date date, std::int64_t days) {
        EXPECT_EQ(days_from_civil(date), days) << date.year << '-' << date.month << '-' << date.day;
        EXPECT_EQ(civil_from_days(days), date) << days;
    };
    check({.year = 1970, .month = 1, .day = 1}, 0);
    check({.year = 1969, .month = 12, .day = 31}, -1);
    check({.year = 2000, .month = 2, .day = 29}, 11016);
    check({.year = 2000, .month = 3, .day = 1}, 11017);
    check({.year = 1900, .month = 2, .day = 28}, -25509);
    check({.year = 1900, .month = 3, .day = 1}, -25508);
    check({.year = 2100, .month = 3, .day = 1}, 47541);
    check({.year = 1600, .month = 2, .day = 29}, -135081);
    check({.year = 1, .month = 1, .day = 1}, -719162);
    check({.year = 0, .month = 3, .day = 1}, -719468);
    check({.year = 2026, .month = 10, .day = 2}, 20728);
    check({.year = 9999, .month = 12, .day = 31}, 2932896);
}

// The calendar of the C++ standard library is the independent reference.
TEST(utc_time, agrees_with_the_standard_calendar)
{
    // The first day that differs stops the test.
    const auto check = [](std::int64_t days) -> testing::AssertionResult {
        const std::chrono::year_month_day expected{std::chrono::sys_days{std::chrono::days{days}}};
        const civil_date date = civil_from_days(days);
        if (date.year != static_cast<int>(expected.year()) || date.month != static_cast<unsigned>(expected.month()) ||
            date.day != static_cast<unsigned>(expected.day())) {
            return testing::AssertionFailure()
                   << "day " << days << " is " << date.year << "-" << date.month << "-" << date.day << ", not "
                   << static_cast<int>(expected.year()) << "-" << static_cast<unsigned>(expected.month()) << "-"
                   << static_cast<unsigned>(expected.day());
        }
        if (days_from_civil(date) != days) {
            return testing::AssertionFailure() << "day " << days << " comes back as " << days_from_civil(date);
        }
        return testing::AssertionSuccess();
    };
    // Every day from 1600 to 2400, then a sample of the range of the standard calendar.
    for (std::int64_t days = -135'140; days <= 157'000; ++days) {
        ASSERT_TRUE(check(days));
    }
    for (std::int64_t days = -11'000'000; days <= 11'000'000; days += 997) {
        ASSERT_TRUE(check(days));
    }
}

sys_time<nanoseconds> at(std::chrono::sys_days day, std::int64_t seconds, std::int64_t fraction)
{
    return sys_time<nanoseconds>{day} + std::chrono::seconds{seconds} + nanoseconds{fraction};
}

TEST(utc_time, formats_iso_8601_in_utc)
{
    using std::chrono::January;
    using std::chrono::October;
    using std::chrono::year;

    EXPECT_EQ(format_utc_time(sys_time<nanoseconds>{}), "1970-01-01T00:00:00Z");
    EXPECT_EQ(format_utc_time(at(year{2026} / October / 2, (18 * 3600) + (52 * 60) + 31, 0)), "2026-10-02T18:52:31Z");
    // The examples of spec §11.1.7.
    EXPECT_EQ(format_utc_time(at(year{2014} / std::chrono::December / 1, (18 * 3600) + (7 * 60) + 54, 0)),
              "2014-12-01T18:07:54Z");
    EXPECT_EQ(format_utc_time(at(year{2015} / January / 23, (19 * 3600) + (52 * 60) + 31, 460'000'000)),
              "2015-01-23T19:52:31.46Z");
}

TEST(utc_time, fraction_of_a_second_has_no_trailing_zeros)
{
    EXPECT_EQ(format_utc_time(sys_time<nanoseconds>{nanoseconds{1}}), "1970-01-01T00:00:00.000000001Z");
    EXPECT_EQ(format_utc_time(sys_time<nanoseconds>{nanoseconds{100'000'000}}), "1970-01-01T00:00:00.1Z");
    EXPECT_EQ(format_utc_time(sys_time<nanoseconds>{nanoseconds{123'456'789}}), "1970-01-01T00:00:00.123456789Z");
}

TEST(utc_time, times_before_1970_round_down)
{
    EXPECT_EQ(format_utc_time(sys_time<nanoseconds>{nanoseconds{-500'000'000}}), "1969-12-31T23:59:59.5Z");
    EXPECT_EQ(format_utc_time(sys_time<nanoseconds>{nanoseconds{-1}}), "1969-12-31T23:59:59.999999999Z");
}

TEST(utc_time, a_time_point_of_the_system_clock_is_its_civil_time)
{
    using std::chrono::January;
    using std::chrono::year;

    EXPECT_EQ(
        openxisf::to_date_time(at(year{2015} / January / 23, (19 * 3600) + (52 * 60) + 31, 460'000'000)),
        (date_time{
            .year = 2015, .month = 1, .day = 23, .hour = 19, .minute = 52, .second = 31, .nanosecond = 460'000'000}));
    EXPECT_EQ(
        openxisf::to_date_time(sys_time<nanoseconds>{nanoseconds{-1}}),
        (date_time{
            .year = 1969, .month = 12, .day = 31, .hour = 23, .minute = 59, .second = 59, .nanosecond = 999'999'999}));
    EXPECT_EQ(openxisf::to_date_time(std::chrono::sys_days{year{2024} / std::chrono::February / 29}),
              (date_time{.year = 2024, .month = 2, .day = 29}));
}

TEST(utc_time, extremes_of_a_64_bit_nanosecond_count)
{
    EXPECT_EQ(format_utc_time(sys_time<nanoseconds>{nanoseconds{std::numeric_limits<std::int64_t>::min()}}),
              "1677-09-21T00:12:43.145224192Z");
    EXPECT_EQ(format_utc_time(sys_time<nanoseconds>{nanoseconds{std::numeric_limits<std::int64_t>::max()}}),
              "2262-04-11T23:47:16.854775807Z");
}

// ---------------------------------------------------------------------------------------------------------------------
// TimePoint values (spec §8.4.4.4)

date_time utc(int year, unsigned month, unsigned day, unsigned hour = 0, unsigned minute = 0, unsigned second = 0,
              std::uint32_t nanosecond = 0)
{
    return {.year = year,
            .month = month,
            .day = day,
            .hour = hour,
            .minute = minute,
            .second = second,
            .nanosecond = nanosecond};
}

TEST(time_point, the_forms_of_iso_8601_extended)
{
    EXPECT_EQ(parse_time_point("2014-12-09T12:38:15Z"), utc(2014, 12, 9, 12, 38, 15));
    EXPECT_EQ(parse_time_point("2015-01-23T19:52:31.46Z"), utc(2015, 1, 23, 19, 52, 31, 460'000'000));
    EXPECT_EQ(parse_time_point("2026-03-14T01:59:26.535Z"), utc(2026, 3, 14, 1, 59, 26, 535'000'000));
    // A date, and times without seconds.
    EXPECT_EQ(parse_time_point("2026-03-14"), utc(2026, 3, 14));
    EXPECT_EQ(parse_time_point("2026-03-14T01:59Z"), utc(2026, 3, 14, 1, 59));
    EXPECT_EQ(parse_time_point("2026-03-14T01:59"), utc(2026, 3, 14, 1, 59));
    // Without a zone, the time is in UTC.
    EXPECT_EQ(parse_time_point("2026-03-14T01:59:26"), utc(2026, 3, 14, 1, 59, 26));
    // Any number of digits in the fraction, cut after nanoseconds.
    EXPECT_EQ(parse_time_point("2026-03-14T01:59:26.1Z").nanosecond, 100'000'000U);
    EXPECT_EQ(parse_time_point("2026-03-14T01:59:26.123456789Z").nanosecond, 123'456'789U);
    EXPECT_EQ(parse_time_point("2026-03-14T01:59:26.1234567899999Z").nanosecond, 123'456'789U);
    // Leading and trailing white space is ignored (spec §11.1.7).
    EXPECT_EQ(parse_time_point(" \t2014-12-09T12:38:15Z\n"), utc(2014, 12, 9, 12, 38, 15));
}

TEST(time_point, a_time_with_an_offset_is_the_same_instant_in_utc)
{
    EXPECT_EQ(parse_time_point("2026-03-14T01:59:26+01:00"), utc(2026, 3, 14, 0, 59, 26));
    EXPECT_EQ(parse_time_point("2026-03-14T01:59:26-05:30"), utc(2026, 3, 14, 7, 29, 26));
    EXPECT_EQ(parse_time_point("2026-03-14T01:59:26+02"), utc(2026, 3, 13, 23, 59, 26));
    EXPECT_EQ(parse_time_point("2026-03-14T01:59:26.5-00:00"), utc(2026, 3, 14, 1, 59, 26, 500'000'000));
    // Across a month, a leap day and a year.
    EXPECT_EQ(parse_time_point("2024-03-01T00:30:00+01:00"), utc(2024, 2, 29, 23, 30));
    EXPECT_EQ(parse_time_point("2025-12-31T23:30:00-01:00"), utc(2026, 1, 1, 0, 30));
    // And beyond the years that a TimePoint can be written in.
    EXPECT_EQ(parse_time_point("0000-01-01T00:00:00+01:00"), utc(-1, 12, 31, 23));
    EXPECT_EQ(parse_time_point("9999-12-31T23:00:00-01:00"), utc(10000, 1, 1));
}

TEST(time_point, leap_seconds_and_the_end_of_a_day)
{
    EXPECT_EQ(parse_time_point("2016-12-31T23:59:60Z"), utc(2016, 12, 31, 23, 59, 60));
    EXPECT_EQ(parse_time_point("2017-01-01T00:59:60+01:00"), utc(2016, 12, 31, 23, 59, 60));
    EXPECT_EQ(parse_time_point("2026-02-28T24:00:00Z"), utc(2026, 3, 1));
    EXPECT_EQ(parse_time_point("2026-02-28T24:00"), utc(2026, 3, 1));
}

TEST(time_point, other_forms_are_refused)
{
    for (const std::string_view text : {"",
                                        "2026",
                                        "2026-03",
                                        "26-03-14",
                                        "20260314",
                                        "2026-3-14",
                                        "2026-03-14T",
                                        "2026-03-14T01",
                                        "2026-03-14T1:59",
                                        "2026-03-14 01:59:26Z",
                                        "2026-03-14t01:59:26Z",
                                        "2026-03-14T01:59:26z",
                                        "2026-03-14T01:59:26,5Z",
                                        "2026-03-14T01:59:26.Z",
                                        "2026-03-14T01:59:26+0100",
                                        "2026-03-14T01:59:26+1",
                                        "2026-03-14T01:59:26+01:0",
                                        "2026-03-14T01:59:26Z+01:00",
                                        "2026-03-14T01:59:26ZZ",
                                        "2026-03-14Z",
                                        "+2026-03-14",
                                        "-2026-03-14",
                                        "2026-13-01",
                                        "2026-00-01",
                                        "2026-02-29",
                                        "2026-04-31",
                                        "2026-01-00",
                                        "2026-01-32",
                                        "2026-03-14T25:00:00",
                                        "2026-03-14T24:00:01",
                                        "2026-03-14T24:00:00.1",
                                        "2026-03-14T24:01",
                                        "2026-03-14T23:60:00",
                                        "2026-03-14T23:59:61",
                                        "2026-03-14T01:59:26+24:00",
                                        "2026-03-14T01:59:26+01:60",
                                        "2026-03-14T01:59:26 Z",
                                        "2026 -03-14",
                                        "1900-02-29"}) {
        EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_time_point, [text] { (void)parse_time_point(text); }))
            << text;
    }
    // Leap years of the Gregorian calendar.
    EXPECT_EQ(parse_time_point("2000-02-29").day, 29U);
    EXPECT_EQ(parse_time_point("2024-02-29").day, 29U);
}

TEST(time_point, formatting_writes_utc_without_trailing_zeros)
{
    EXPECT_EQ(format_time_point(utc(2014, 12, 9, 12, 38, 15)), "2014-12-09T12:38:15Z");
    EXPECT_EQ(format_time_point(utc(2015, 1, 23, 19, 52, 31, 460'000'000)), "2015-01-23T19:52:31.46Z");
    EXPECT_EQ(format_time_point(utc(0, 1, 1)), "0000-01-01T00:00:00Z");
    EXPECT_EQ(format_time_point(utc(9999, 12, 31, 23, 59, 60, 1)), "9999-12-31T23:59:60.000000001Z");
}

TEST(time_point, valid_time_points_are_those_that_can_be_written)
{
    EXPECT_TRUE(is_valid_time_point(utc(2026, 2, 28, 23, 59, 60, 999'999'999)));
    EXPECT_TRUE(is_valid_time_point(utc(0, 1, 1)));
    EXPECT_TRUE(is_valid_time_point(date_time{}));
    EXPECT_FALSE(is_valid_time_point(utc(-1, 12, 31)));
    EXPECT_FALSE(is_valid_time_point(utc(10000, 1, 1)));
    EXPECT_FALSE(is_valid_time_point(utc(2026, 2, 29)));
    EXPECT_FALSE(is_valid_time_point(utc(2026, 0, 1)));
    EXPECT_FALSE(is_valid_time_point(utc(2026, 1, 1, 24)));
    EXPECT_FALSE(is_valid_time_point(utc(2026, 1, 1, 0, 60)));
    EXPECT_FALSE(is_valid_time_point(utc(2026, 1, 1, 0, 0, 61)));
    EXPECT_FALSE(is_valid_time_point(utc(2026, 1, 1, 0, 0, 0, 1'000'000'000)));
}

} // namespace
