// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The writer when its allocations fail.

#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>
#include <openxisf/writer.h>

#include "allocation/allocations.h"
#include "support/fixture_builder.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

// The unit that output saves into memory.
std::vector<std::byte> saved(const openxisf::writer& output)
{
    openxisf::memory_sink sink;
    output.save(sink);
    return sink.release();
}

// An image that cannot be added, because the writer cannot grow the list of its images or of their pixels, leaves the
// writer as it was: the images added after it are saved with their own pixels.
TEST(writer, an_image_that_cannot_be_added_leaves_the_writer_as_it_was)
{
    if (!openxisf::test::counts_the_allocations_of_the_library()) {
        GTEST_SKIP() << "the allocations of the library cannot be made to fail in this build";
    }
    if (!openxisf::test::can_fail_allocations()) {
        GTEST_SKIP() << "a failed allocation ends the program with the debug standard library of MSVC";
    }
    openxisf::image_info info;
    info.geometry = {.dimensions = {4, 1}, .channels = 1};
    info.sample_format = openxisf::sample_format::uint8;
    const std::vector<std::uint8_t> first{1, 2, 3, 4};
    const std::vector<std::uint8_t> second{5, 6, 7, 8};
    const std::vector<std::uint8_t> third{9, 10, 11, 12};

    openxisf::writer output(
        {.creator_application = "OpenXISF tests 1.0", .creation_time = openxisf::date_time{.year = 2026}});
    (void)output.add_image(info, std::span<const std::uint8_t>(first));
    const std::vector<std::byte> before = saved(output);
    const std::uint64_t failures = openxisf::test::fail_each_allocation(
        [&output, &info, &second] { (void)output.add_image(info, std::span<const std::uint8_t>(second)); },
        [&output, &before] {
            EXPECT_EQ(output.images().size(), 1U);
            EXPECT_EQ(saved(output), before);
        });
    // The list of pixels grows first, then that of images; a debug build of the standard library allocates more.
    EXPECT_GE(failures, 2U);
    (void)output.add_image(info, std::span<const std::uint8_t>(third));

    const openxisf::reader unit = openxisf::test::open_unit(saved(output), {.strict = true});
    ASSERT_EQ(unit.images().size(), 3U);
    EXPECT_EQ(unit.read_pixels<std::uint8_t>(0), first);
    EXPECT_EQ(unit.read_pixels<std::uint8_t>(1), second);
    EXPECT_EQ(unit.read_pixels<std::uint8_t>(2), third);
}

// The zeros of a reserved image are written, and hashed, from one piece of a few MiB: no buffer of the size of the
// image is allocated, with a checksum or without, to a sink that can rewrite or to one that cannot.
TEST(writer, a_reserved_image_allocates_no_buffer_of_its_size)
{
    if (!openxisf::test::counts_the_allocations_of_the_library()) {
        GTEST_SKIP() << "the allocations of the library are not counted in this build";
    }
    openxisf::image_info info;
    info.geometry = {.dimensions = {4096, 4096}, .channels = 1};
    info.sample_format = openxisf::sample_format::uint32;
    constexpr std::uint64_t size = std::uint64_t{64} << 20;
    for (const bool checksum : {false, true}) {
        for (const bool rewritable : {false, true}) {
            openxisf::write_options options{.creator_application = "OpenXISF tests 1.0",
                                            .creation_time = openxisf::date_time{.year = 2026}};
            if (checksum) {
                options.checksum = openxisf::checksum_algorithm::sha1;
            }
            openxisf::writer output(options);
            (void)output.add_reserved_image(info);
            std::uint64_t written = 0;
            openxisf::callback_sink sink(
                [&written](std::span<const std::byte> data) { written += data.size(); },
                rewritable ? openxisf::callback_sink::rewrite_function([](std::uint64_t, std::span<const std::byte>) {})
                           : openxisf::callback_sink::rewrite_function());
            const std::uint64_t allocated = openxisf::test::bytes_allocated_by([&] { output.save(sink); });
            EXPECT_GT(written, size) << checksum << rewritable;
            EXPECT_LT(allocated, std::uint64_t{8} << 20) << checksum << rewritable;
        }
    }
}

} // namespace
