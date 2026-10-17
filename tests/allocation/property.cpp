// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Copies of property values and lists when their allocations fail.

#include <openxisf/property.h>

#include "allocation/allocations.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <type_traits>
#include <vector>

namespace {

using openxisf::property_list;
using openxisf::property_value;

static_assert(std::is_nothrow_move_constructible_v<property_value> &&
              std::is_nothrow_move_assignable_v<property_value>);
static_assert(std::is_nothrow_move_constructible_v<property_list> && std::is_nothrow_move_assignable_v<property_list>);

// Whether the copies of this test can be made to fail, and are made by the operator new of the test.
bool can_fail_copies()
{
    return openxisf::test::counts_the_allocations_of_the_library() && openxisf::test::can_fail_allocations();
}

// A value assigned a value of another type, of another shape or of another length keeps its type, its value and its
// dimensions when the copy cannot be allocated. The copy is made by the copy constructor, which with the copy
// constructor of std::variant crashed under libstdc++ 12 to 16 when it failed.
TEST(property_value, a_copy_assignment_that_fails_leaves_the_value_as_it_was)
{
    if (!can_fail_copies()) {
        GTEST_SKIP() << "the allocations of the library cannot be made to fail in this build";
    }
    const std::vector<property_value> values{
        property_value("a String long enough to be allocated on the heap by std::string"),
        property_value::matrix(2, 2, std::vector<std::uint8_t>{1, 2, 3, 4}),
        property_value(std::vector<std::uint8_t>(1000, 9)),
        property_value(std::vector<double>{1.0, 2.0, 3.0}),
        property_value(std::int32_t{7}),
    };
    for (std::size_t from = 0; from < values.size(); ++from) {
        for (std::size_t to = 0; to < values.size(); ++to) {
            SCOPED_TRACE(std::to_string(from) + " assigned " + std::to_string(to));
            property_value value = values[from];
            const std::uint64_t failures =
                openxisf::test::fail_each_allocation([&value, &values, to] { value = values[to]; },
                                                     [&value, &values, from] {
                                                         EXPECT_EQ(value.type(), values[from].type());
                                                         EXPECT_EQ(value, values[from]);
                                                     });
            EXPECT_EQ(value, values[to]);
            // Only the integer needs no memory of its own.
            EXPECT_EQ(failures == 0, to == values.size() - 1) << failures;
        }
    }
}

// A list assigned another one, whose properties are the same in another order with other values, and one more, keeps
// its properties when any allocation of the copy fails.
TEST(property_list, a_copy_assignment_that_fails_leaves_the_list_as_it_was)
{
    if (!can_fail_copies()) {
        GTEST_SKIP() << "the allocations of the library cannot be made to fail in this build";
    }
    const std::string long_id = "Test:An_identifier_longer_than_the_buffer_of_a_short_string";
    property_list list;
    list.set(long_id, 1);
    list.set("Test:B", 2);
    property_list other;
    other.set("Test:B", 3);
    other.set(long_id, 4);
    other.set("Test:C", "a String long enough to be allocated on the heap by std::string");

    property_list target = list;
    const std::uint64_t failures = openxisf::test::fail_each_allocation([&target, &other] { target = other; },
                                                                        [&target, &list] { EXPECT_EQ(target, list); });
    EXPECT_GE(failures, 3U);
    EXPECT_EQ(target, other);
}

} // namespace
