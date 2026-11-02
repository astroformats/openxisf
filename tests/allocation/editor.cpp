// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The memory that the writes of an editor ask for: a region written as it is stored goes straight from the data of the
// application to the device, and one whose samples change their byte order through one staging buffer.

#include <openxisf/editor.h>
#include <openxisf/io.h>
#include <openxisf/types.h>

#include "allocation/allocations.h"
#include "support/fixture_builder.h"

#include <gtest/gtest.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

// A unit of a gray UInt16 frame of size x size pixels whose attached block holds zeros, in the byte order of the host,
// or in the other one when swapped: the editor writes samples straight into a block in the order of the host, and
// swaps them in a buffer of its own for a block in the other order.
std::vector<std::byte> zero_frame(std::uint64_t size, bool swapped)
{
    const bool big_endian = (std::endian::native == std::endian::big) != swapped;
    const std::string geometry = std::to_string(size) + ":" + std::to_string(size) + ":1";
    std::vector<std::byte> zeros(size * size * 2);
    return openxisf::test::file_with_attachments(
        openxisf::test::header_xml(R"(<Image geometry=")" + geometry + R"(" sampleFormat="UInt16")" +
                                   (big_endian ? R"( byteOrder="big")" : "") + R"( location="attachment:{0}"/>)"),
        {zeros});
}

TEST(editor, a_region_written_as_it_is_stored_allocates_nothing_of_its_size)
{
    if (!openxisf::test::counts_the_allocations_of_the_library()) {
        GTEST_SKIP() << "the allocations of the library are not counted in this build";
    }
    // A frame of 1024 x 1024 pixels, 2 MiB, in the byte order of the host.
    std::vector<std::byte> file = zero_frame(1024, false);
    openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
    // A tile of 512 x 512 pixels, 512 KiB, in 512 runs, and the whole frame in one.
    const std::vector<std::byte> tile(std::size_t{512} * 512 * 2, std::byte{1});
    const openxisf::pixel_region region{.origin = {256, 256}, .size = {512, 512}};
    EXPECT_LT(openxisf::test::bytes_allocated_by([&] { edit.write_pixels(0, tile, {.region = region}); }), 1024U);
    const std::vector<std::byte> whole(std::size_t{1024} * 1024 * 2, std::byte{2});
    EXPECT_LT(openxisf::test::bytes_allocated_by([&] { edit.write_pixels(0, whole); }), 1024U);
}

TEST(editor, samples_that_change_their_byte_order_take_one_staging_buffer)
{
    if (!openxisf::test::counts_the_allocations_of_the_library()) {
        GTEST_SKIP() << "the allocations of the library are not counted in this build";
    }
    // A block of 512 x 512 UInt16 pixels, 512 KiB, in the other byte order than the host's, written whole: through a
    // buffer of the region, which is below the piece size of 4 MiB, at most.
    std::vector<std::byte> file = zero_frame(512, true);
    openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
    const std::vector<std::byte> data(std::size_t{512} * 512 * 2, std::byte{3});
    const std::uint64_t allocated = openxisf::test::bytes_allocated_by([&] { edit.write_pixels(0, data); });
    EXPECT_GE(allocated, data.size());
    EXPECT_LT(allocated, data.size() + 1024);
}

} // namespace
