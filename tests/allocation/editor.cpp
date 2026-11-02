// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The memory that the writes of an editor ask for: a region written as it is stored goes straight from the data of the
// application to the device, and one whose samples change their byte order through one staging buffer.

#include <openxisf/editor.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/writer.h>

#include "allocation/allocations.h"
#include "support/fixture_builder.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

// A unit of a reserved gray UInt16 frame of 1024 x 1024 pixels, 2 MiB, in memory.
std::vector<std::byte> reserved_frame()
{
    openxisf::image_info info;
    info.geometry = {.dimensions = {1024, 1024}, .channels = 1};
    openxisf::writer output(
        {.creator_application = "OpenXISF tests 1.0", .creation_time = openxisf::date_time{.year = 2026}});
    (void)output.add_reserved_image(info);
    openxisf::memory_sink sink;
    output.save(sink);
    return sink.release();
}

TEST(editor, a_region_written_as_it_is_stored_allocates_nothing_of_its_size)
{
    if (!openxisf::test::counts_the_allocations_of_the_library()) {
        GTEST_SKIP() << "the allocations of the library are not counted in this build";
    }
    std::vector<std::byte> file = reserved_frame();
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
    // A big-endian block of 512 x 512 UInt16 pixels, 512 KiB, written whole: through a buffer of the region, which is
    // below the piece size of 4 MiB, at most.
    const std::vector<std::byte> stored(std::size_t{512} * 512 * 2);
    std::vector<std::byte> file = openxisf::test::file_with_attachments(
        openxisf::test::header_xml(
            R"(<Image geometry="512:512:1" sampleFormat="UInt16" byteOrder="big" location="attachment:{0}"/>)"),
        {stored});
    openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
    const std::vector<std::byte> data(stored.size(), std::byte{3});
    const std::uint64_t allocated = openxisf::test::bytes_allocated_by([&] { edit.write_pixels(0, data); });
    EXPECT_GE(allocated, data.size());
    EXPECT_LT(allocated, data.size() + 1024);
}

} // namespace
