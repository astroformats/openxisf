// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// What an untrusted header costs in memory, through the public reader.

#include <openxisf/error.h>
#include <openxisf/reader.h>

#include "allocation/allocations.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::reader;
using openxisf::test::bytes;
using openxisf::test::bytes_allocated_by;
using openxisf::test::header_xml;

// Attributes of a data block made of separators, 20 MiB each: a location, a compression and a list of subblocks.
TEST(reader, attributes_of_separators_take_no_memory_in_proportion_to_their_length)
{
    if (!openxisf::test::counts_the_allocations_of_the_library()) {
        GTEST_SKIP() << "the allocations of the library are not counted in this build";
    }
    const std::string separators(std::size_t{20} << 20, ':');
    const auto image = [](std::string_view attributes) {
        return R"(<Image geometry="1:1:1" sampleFormat="UInt8" colorSpace="Gray" )" + std::string(attributes) + "/>";
    };
    const std::string header =
        header_xml(image(R"(location="attachment:)" + separators + "\"") +
                   image(R"(location="attachment:{0}" compression="zlib)" + separators + "\"") +
                   image(R"(location="attachment:{0}" compression="zlib:1" subblocks=")" + separators + "\""));
    std::vector<std::byte> unit = openxisf::test::file_with_attachments(header, {bytes("\x01")});

    std::vector<openxisf::diagnostic> diagnostics;
    const std::uint64_t allocated = bytes_allocated_by([&unit, &diagnostics] {
        const reader file = openxisf::test::open_unit(std::move(unit));
        diagnostics.assign(file.diagnostics().begin(), file.diagnostics().end());
    });
    ASSERT_EQ(diagnostics.size(), 3U) << openxisf::test::describe(diagnostics);
    EXPECT_EQ(diagnostics[0].code, errc::invalid_location);
    EXPECT_EQ(diagnostics[1].code, errc::invalid_compression);
    EXPECT_EQ(diagnostics[2].code, errc::invalid_subblocks);
    // The text of the header is read once.
    EXPECT_LT(allocated, 2 * header.size());
}

// An image of 256 MiB whose compressed data block has 9 bytes, which no codec decodes to that much: the block is
// unavailable from the open, and a read of its samples allocates none of them.
TEST(reader, a_few_stored_bytes_cannot_make_a_read_allocate_what_they_declare)
{
    if (!openxisf::test::counts_the_allocations_of_the_library()) {
        GTEST_SKIP() << "the allocations of the library are not counted in this build";
    }
    for (const std::string_view codec : {"zlib", "lz4", "lz4hc", "zstd"}) {
        SCOPED_TRACE(codec);
        const std::string header = header_xml(
            R"(<Image geometry="16384:16384:1" sampleFormat="UInt8" colorSpace="Gray" location="attachment:{0}" )"
            R"(compression=")" +
            std::string(codec) + R"(:268435456"/>)");
        const reader file =
            openxisf::test::open_unit(openxisf::test::file_with_attachments(header, {bytes("123456789")}));
        ASSERT_EQ(file.diagnostics().size(), 1U) << openxisf::test::describe(file.diagnostics());
        EXPECT_EQ(file.diagnostics().front().code, errc::invalid_subblocks);
        const std::uint64_t allocated = bytes_allocated_by([&file] {
            EXPECT_TRUE(openxisf::test::throws<openxisf::invalid_data_error>(
                errc::invalid_subblocks, [&file] { (void)file.read_pixels<std::uint8_t>(0); }));
            EXPECT_TRUE(openxisf::test::throws<openxisf::invalid_data_error>(errc::invalid_subblocks,
                                                                             [&file] { (void)file.read_pixels(0); }));
        });
        EXPECT_LT(allocated, std::uint64_t{1} << 20);
    }
}

} // namespace
