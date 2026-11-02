// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/error.h>
#include <openxisf/io.h>

#include "support/bytes.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::io_error;
using openxisf::memory_sink;
using openxisf::memory_source;
using openxisf::usage_error;
using openxisf::test::bytes;
using openxisf::test::pattern;
using openxisf::test::text;
using openxisf::test::throws;

TEST(memory_source, reads_every_range_of_its_bytes)
{
    const std::vector<std::byte> data = pattern(37);
    const memory_source source{std::span<const std::byte>(data)};

    ASSERT_EQ(source.size(), data.size());
    for (std::size_t offset = 0; offset <= data.size(); ++offset) {
        for (std::size_t length = 0; offset + length <= data.size(); ++length) {
            std::vector<std::byte> destination(length);
            source.read(offset, destination);
            ASSERT_TRUE(std::ranges::equal(destination, std::span(data).subspan(offset, length)))
                << offset << ' ' << length;
        }
    }
}

TEST(memory_source, reads_past_the_end_are_end_of_data)
{
    const memory_source source(bytes("0123456789"));
    std::array<std::byte, 4> destination{};

    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(7, destination); }));
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(11, {}); }));
    // offset + size wraps around in 64 bits.
    EXPECT_TRUE(throws<io_error>(errc::end_of_data,
                                 [&] { source.read(std::numeric_limits<std::uint64_t>::max() - 1, destination); }));
    try {
        source.read(8, destination);
        ADD_FAILURE() << "no exception";
    } catch (const io_error& failure) {
        EXPECT_EQ(failure.context().offset, 8U);
        EXPECT_STREQ(failure.what(),
                     "a read of 4 bytes goes past the end of the source, which has 10 bytes (offset 8)");
    }
}

TEST(memory_source, empty)
{
    const memory_source source(std::vector<std::byte>{});
    std::array<std::byte, 1> destination{};

    EXPECT_EQ(source.size(), 0U);
    source.read(0, {});
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(0, destination); }));
}

TEST(memory_source, owns_a_vector_and_borrows_a_span)
{
    std::vector<std::byte> data = bytes("abc");
    const memory_source borrowing{std::span<const std::byte>(data)};
    const memory_source copying(data);
    std::vector<std::byte> moved = bytes("xyz");
    const memory_source owning(std::move(moved));
    moved = bytes("!!!");
    data[0] = std::byte{'A'};

    std::array<std::byte, 3> destination{};
    borrowing.read(0, destination);
    EXPECT_EQ(text(destination), "Abc");
    copying.read(0, destination);
    EXPECT_EQ(text(destination), "abc");
    owning.read(0, destination);
    EXPECT_EQ(text(destination), "xyz");
}

TEST(memory_source, supports_concurrent_reads)
{
    EXPECT_TRUE(memory_source(std::vector<std::byte>{}).supports_concurrent_reads());
}

TEST(memory_source, gives_the_bytes_it_borrows_or_owns_in_place)
{
    const std::vector<std::byte> data = bytes("abc");
    const memory_source borrowing{std::span<const std::byte>(data)};
    EXPECT_EQ(borrowing.bytes().data(), data.data());
    EXPECT_EQ(borrowing.bytes().size(), 3U);

    const memory_source owning(bytes("xyz"));
    EXPECT_EQ(text(owning.bytes()), "xyz");
}

TEST(memory_sink, appends_and_rewrites)
{
    memory_sink sink;
    EXPECT_TRUE(sink.can_rewrite());
    EXPECT_EQ(sink.position(), 0U);

    sink.write(bytes("hello "));
    sink.write({});
    sink.write(bytes("world"));
    EXPECT_EQ(sink.position(), 11U);

    sink.rewrite(6, bytes("WORLD"));
    sink.rewrite(0, bytes("H"));
    sink.rewrite(11, {});
    EXPECT_EQ(sink.position(), 11U);
    sink.finish();
    EXPECT_EQ(text(sink.data()), "Hello WORLD");
}

TEST(memory_sink, rewrites_beyond_the_written_bytes_are_usage_errors)
{
    memory_sink sink;
    sink.write(bytes("0123456789"));

    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.rewrite(8, bytes("abc")); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { sink.rewrite(11, {}); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { sink.rewrite(std::numeric_limits<std::uint64_t>::max(), bytes("a")); }));
    EXPECT_EQ(text(sink.data()), "0123456789");
}

TEST(memory_sink, release_moves_the_bytes_out)
{
    memory_sink sink;
    sink.write(bytes("abc"));

    EXPECT_EQ(text(sink.release()), "abc");
    EXPECT_EQ(sink.position(), 0U);
    EXPECT_TRUE(sink.data().empty());
}

} // namespace
