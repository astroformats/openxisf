// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Byte shuffling (spec §10.6.2), against the order that the specification defines: byte j of item i of n items moves
// to j * n + i, and the bytes after the last complete item stay where they are.

#include "codec/shuffle.h"

#include <openxisf/error.h>

#include "support/bytes.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <ostream>
#include <string>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::detail::shuffle_bytes;
using openxisf::detail::shuffle_kernel;
using openxisf::detail::unshuffle_bytes;
using openxisf::test::bytes;
using openxisf::test::pattern;
using openxisf::test::throws;

std::vector<std::byte> shuffled(const std::vector<std::byte>& input, std::size_t item_size,
                                shuffle_kernel kernel = shuffle_kernel::simd)
{
    std::vector<std::byte> output(input.size());
    shuffle_bytes(input, output, item_size, kernel);
    return output;
}

std::vector<std::byte> unshuffled(const std::vector<std::byte>& input, std::size_t item_size)
{
    std::vector<std::byte> output(input.size());
    unshuffle_bytes(input, output, item_size);
    return output;
}

TEST(shuffle, gathers_the_bytes_of_equal_significance)
{
    EXPECT_EQ(shuffled(bytes("a1b2c3"), 2), bytes("abc123"));
    EXPECT_EQ(shuffled(bytes("abcdABCD"), 4), bytes("aAbBcCdD"));
    EXPECT_EQ(unshuffled(bytes("abc123"), 2), bytes("a1b2c3"));
}

TEST(shuffle, keeps_the_bytes_after_the_last_complete_item_in_place)
{
    EXPECT_EQ(shuffled(bytes("abcABCxy"), 3), bytes("aAbBcCxy"));
    EXPECT_EQ(shuffled(bytes("a1b2c3x"), 2), bytes("abc123x"));
    EXPECT_EQ(unshuffled(bytes("abc123x"), 2), bytes("a1b2c3x"));
}

TEST(shuffle, copies_a_block_without_a_complete_item_or_with_items_of_one_byte)
{
    for (const std::size_t item_size : {0U, 1U, 4U, 100U}) {
        EXPECT_EQ(shuffled(bytes("abc"), item_size), bytes("abc")) << item_size;
        EXPECT_EQ(unshuffled(bytes("abc"), item_size), bytes("abc")) << item_size;
    }
    EXPECT_EQ(shuffled(bytes("abcdef"), 1), bytes("abcdef"));
}

TEST(shuffle, needs_an_output_of_the_size_of_the_input)
{
    const std::vector<std::byte> input = bytes("abcd");
    std::vector<std::byte> output(3);
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument, [&] { shuffle_bytes(input, output, 2); }));
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument, [&] { unshuffle_bytes(input, output, 2); }));
}

// Every item size of the sample formats, and the 16 bytes of a Complex64 sample, at lengths around a multiple of 16.
struct shuffle_case
{
    std::size_t item_size = 0;
    std::size_t length = 0;
};

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const shuffle_case& value, std::ostream* output)
{
    *output << "item" << value.item_size << "_length" << value.length;
}

std::vector<shuffle_case> shuffle_cases()
{
    std::vector<shuffle_case> cases;
    for (const std::size_t item_size : {1U, 2U, 3U, 4U, 8U, 16U}) {
        for (const std::size_t length : {0U, 1U, 15U, 16U, 17U, 100'003U}) {
            cases.push_back({.item_size = item_size, .length = length});
        }
    }
    return cases;
}

class shuffle_layout : public testing::TestWithParam<shuffle_case>
{};

TEST_P(shuffle_layout, follows_the_definition_and_reverses)
{
    const auto [item_size, length] = GetParam();
    const std::vector<std::byte> input = pattern(length);
    const std::size_t items = length / item_size;

    std::vector<std::byte> expected(length);
    for (std::size_t i = 0; i < items; ++i) {
        for (std::size_t j = 0; j < item_size; ++j) {
            expected[(j * items) + i] = input[(i * item_size) + j];
        }
    }
    for (std::size_t k = items * item_size; k < length; ++k) {
        expected[k] = input[k];
    }

    const std::vector<std::byte> output = shuffled(input, item_size);
    EXPECT_EQ(output, expected);
    EXPECT_EQ(unshuffled(output, item_size), input);
}

INSTANTIATE_TEST_SUITE_P(sizes, shuffle_layout, testing::ValuesIn(shuffle_cases()),
                         [](const testing::TestParamInfo<shuffle_case>& parameter) {
                             return "item" + std::to_string(parameter.param.item_size) + "_length" +
                                    std::to_string(parameter.param.length);
                         });

TEST(shuffle, a_part_of_the_shuffled_block_is_that_part_of_the_whole)
{
    // Every window of a block of 37 bytes, so that parts start and end inside runs, across runs and in the tail, with
    // the scalar code; the SIMD kernels are compared with it below.
    const std::vector<std::byte> input = pattern(37);
    for (const std::size_t item_size : {0U, 1U, 2U, 3U, 4U, 8U, 16U, 37U, 38U}) {
        const std::vector<std::byte> whole = shuffled(input, item_size, shuffle_kernel::scalar);
        for (std::size_t offset = 0; offset <= input.size(); ++offset) {
            for (std::size_t length = 0; offset + length <= input.size(); ++length) {
                std::vector<std::byte> part(length);
                openxisf::detail::shuffle_part(input, item_size, offset, part, shuffle_kernel::scalar);
                ASSERT_TRUE(std::equal(part.begin(), part.end(), whole.begin() + static_cast<std::ptrdiff_t>(offset)))
                    << "items of " << item_size << ", " << length << " bytes at " << offset;
            }
        }
    }
}

TEST(shuffle, a_part_must_lie_within_the_block)
{
    const std::vector<std::byte> input = bytes("abcd");
    std::vector<std::byte> output(3);
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument,
                                              [&] { openxisf::detail::shuffle_part(input, 2, 2, output); }));
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument,
                                              [&] { openxisf::detail::shuffle_part(input, 2, 5, {}); }));
}

// ---------------------------------------------------------------------------------------------------------------------
// The SIMD kernels, which move 16 items at a time, against the scalar code. Item sizes without a kernel are checked
// too, since they must fall back to the scalar code.

TEST(shuffle, the_simd_kernels_give_the_bytes_of_the_scalar_code)
{
    for (const std::size_t item_size : {2U, 3U, 4U, 8U, 16U}) {
        // Numbers of items on both sides of the multiples of 16, and every tail.
        for (std::size_t items = 0; items <= 70; ++items) {
            for (std::size_t tail = 0; tail < item_size; ++tail) {
                const std::vector<std::byte> input = pattern((items * item_size) + tail);
                std::vector<std::byte> scalar(input.size());
                std::vector<std::byte> simd(input.size());
                shuffle_bytes(input, scalar, item_size, shuffle_kernel::scalar);
                shuffle_bytes(input, simd, item_size, shuffle_kernel::simd);
                ASSERT_EQ(simd, scalar) << "shuffled items of " << item_size << ", " << items << " items, tail "
                                        << tail;
                unshuffle_bytes(input, scalar, item_size, shuffle_kernel::scalar);
                unshuffle_bytes(input, simd, item_size, shuffle_kernel::simd);
                ASSERT_EQ(simd, scalar) << "unshuffled items of " << item_size << ", " << items << " items, tail "
                                        << tail;
            }
        }
    }
}

TEST(shuffle, the_simd_kernels_give_every_part_of_the_scalar_code)
{
    // Every window of a block of 40 items and a tail, so that parts start and end inside the 16 items of a kernel.
    for (const std::size_t item_size : {2U, 4U, 8U}) {
        const std::vector<std::byte> input = pattern((40 * item_size) + item_size - 1);
        const std::vector<std::byte> whole = shuffled(input, item_size, shuffle_kernel::scalar);
        for (std::size_t offset = 0; offset <= input.size(); ++offset) {
            for (std::size_t length = 0; offset + length <= input.size(); ++length) {
                std::vector<std::byte> part(length);
                openxisf::detail::shuffle_part(input, item_size, offset, part, shuffle_kernel::simd);
                ASSERT_TRUE(std::equal(part.begin(), part.end(), whole.begin() + static_cast<std::ptrdiff_t>(offset)))
                    << "items of " << item_size << ", " << length << " bytes at " << offset;
            }
        }
    }
}

TEST(shuffle, the_kernels_of_the_build_are_known)
{
    // SSE2 is part of every x86-64 processor; other processors use the scalar code.
#if defined(OPENXISF_ENABLE_SIMD) && (defined(__x86_64__) || defined(_M_X64)) && !defined(_M_ARM64EC)
    EXPECT_TRUE(openxisf::detail::has_simd_shuffle());
#else
    EXPECT_FALSE(openxisf::detail::has_simd_shuffle());
#endif
}

} // namespace
