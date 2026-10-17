// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// What the attributes of data blocks (spec §10.3 to §10.6) cost in memory while they are read.

#include "container/block_attributes.h"

#include <openxisf/error.h>

#include "allocation/allocations.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::invalid_data_error;
using openxisf::detail::parse_compression;
using openxisf::detail::parse_location;
using openxisf::detail::parse_subblocks;
using openxisf::detail::subblock;
using openxisf::test::bytes_allocated_by;
using openxisf::test::throws;

// A header can hold an attribute of millions of separators. Its parts are read only as far as its form goes, and a list
// of subblocks is checked whole before it is stored, so a malformed value takes no memory but the message that refuses
// it.
TEST(block_attributes, a_value_of_separators_is_refused_without_storing_its_parts)
{
    const std::string separators(std::size_t{16} << 20, ':');
    struct malformed
    {
        std::string text{};
        errc code = errc::invalid_location;
        std::function<void(std::string_view)> parse{};
    };
    const std::vector<malformed> values{
        {.text = "attachment:" + separators,
         .code = errc::invalid_location,
         .parse = [](std::string_view text) { (void)parse_location(text); }},
        {.text = "zlib" + separators,
         .code = errc::invalid_compression,
         .parse = [](std::string_view text) { (void)parse_compression(text); }},
        {.text = "zstd+sh" + separators,
         .code = errc::invalid_compression,
         .parse = [](std::string_view text) { (void)parse_compression(text); }},
        {.text = separators,
         .code = errc::invalid_subblocks,
         .parse = [](std::string_view text) { (void)parse_subblocks(text); }},
        {.text = "1,1" + separators,
         .code = errc::invalid_subblocks,
         .parse = [](std::string_view text) { (void)parse_subblocks(text); }},
    };
    for (const malformed& value : values) {
        bool refused = false;
        const std::uint64_t allocated = bytes_allocated_by([&value, &refused] {
            refused = throws<invalid_data_error>(value.code, [&value] { value.parse(value.text); });
        });
        EXPECT_TRUE(refused) << value.text.substr(0, 12);
        EXPECT_LT(allocated, 4096U) << value.text.substr(0, 12);
    }
}

// A list of subblocks takes the memory of its subblocks, and nothing more while it is read.
TEST(block_attributes, a_list_of_subblocks_is_read_into_memory_of_its_size)
{
    constexpr std::size_t count = 1'000'000;
    std::string text = "1,1";
    for (std::size_t i = 1; i < count; ++i) {
        text += ":1,1";
    }
    std::vector<subblock> subblocks;
    const std::uint64_t allocated = bytes_allocated_by([&text, &subblocks] { subblocks = parse_subblocks(text); });
    ASSERT_EQ(subblocks.size(), count);
    EXPECT_LE(allocated, (count * sizeof(subblock)) + 1024);
}

} // namespace
