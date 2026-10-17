// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// What the data blocks of a unit (spec §10) cost in memory while they are described.

#include "container/data_block.h"

#include <openxisf/error.h>

#include "allocation/allocations.h"
#include "model/unit.h"
#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

using openxisf::detail::subblock;
using openxisf::detail::unit;
using openxisf::test::bytes_allocated_by;
using openxisf::test::descriptor_at;
using openxisf::test::open_internal;
using openxisf::test::value_of;

// A list of subblocks can be as long as the header, and is read once, into the block, whose checks take no copy of it.
TEST(data_block, the_subblocks_of_a_block_are_checked_where_they_are_read)
{
    constexpr std::size_t count = 1'000'000;
    std::string subblocks = "1,1";
    for (std::size_t i = 1; i < count; ++i) {
        subblocks += ":1,1";
    }
    // A gray 8-bit image of one row.
    const std::string header = openxisf::test::header_xml(
        R"(<Image geometry=")" + std::to_string(count) +
        R"(:1:1" sampleFormat="UInt8" colorSpace="Gray" location="attachment:{0}" compression="zlib:)" +
        std::to_string(count) + R"(" subblocks=")" + subblocks + "\"/>");
    std::vector<std::byte> file = openxisf::test::file_with_attachments(header, {std::vector<std::byte>(count)});

    std::vector<openxisf::diagnostic> diagnostics;
    std::size_t listed = 0;
    const std::uint64_t allocated = bytes_allocated_by([&file, &diagnostics, &listed] {
        const unit opened = open_internal(std::move(file));
        diagnostics = opened.diagnostics;
        listed = value_of(descriptor_at(opened, "/xisf/Image[1]").compression).subblocks.size();
    });
    EXPECT_TRUE(openxisf::test::no_diagnostics(diagnostics));
    EXPECT_EQ(listed, count);
    // The text of the header, read once, the list, and what the rest of the unit takes.
    EXPECT_LT(allocated, header.size() + (count * sizeof(subblock)) + (std::size_t{1} << 20));
}

} // namespace
