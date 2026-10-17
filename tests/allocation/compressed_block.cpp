// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// What the compression of a block in subblocks (spec §10.6) asks for in memory.

#include "codec/compressed_block.h"

#include <openxisf/error.h>

#include "allocation/allocations.h"
#include "core/parallel.h"
#include "support/bytes.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using openxisf::detail::compression_codec;
using openxisf::detail::subblock;
using openxisf::test::bytes_allocated_by;

// LZ4HC has a state of about 256 KiB, which its encoder asks operator new for. It is made once for each subblock that
// a batch compresses at once, and kept for the subblocks of the following batches, rather than made for each subblock.
TEST(compressed_block, the_state_of_a_codec_is_made_once_for_each_subblock_of_a_batch)
{
    constexpr std::size_t count = 4096;
    const std::vector<std::byte> data = openxisf::test::pattern(count);
    const auto compress = [](std::span<const std::byte> block) {
        (void)openxisf::detail::compress_subblocks(
            block, {.codec = compression_codec::lz4hc, .max_subblock_size = 1},
            [](std::span<const std::byte> /*stored*/, const subblock& /*sizes*/) {});
    };
    // A state, and what the rest of the compression of one subblock takes.
    const std::uint64_t one = bytes_allocated_by([&data, &compress] { compress(std::span(data).first(1)); });
    const std::uint64_t all = bytes_allocated_by([&data, &compress] { compress(data); });
    const std::uint64_t states = std::min<std::uint64_t>(openxisf::detail::concurrency(), count);
    // A state for each subblock of a batch, and the list of the subblocks.
    EXPECT_LT(all, (states * one) + (count * 64)) << one << " bytes for one subblock";
}

// A Zstandard frame declares the size of its content, which is checked before the memory of the block is allocated:
// here a frame of 1000 zeros, in a block that declares as much as its codec can decode from its bytes.
TEST(compressed_block, a_zstd_frame_that_declares_another_size_is_refused_before_the_block_is_allocated)
{
    const std::vector<std::byte> zeros(1000);
    for (const std::uint64_t item_size : {0U, 2U}) {
        SCOPED_TRACE(item_size);
        openxisf::detail::compressed_block block =
            openxisf::detail::compress_block(zeros, {.codec = compression_codec::zstd, .item_size = item_size});
        ASSERT_LT(block.data.size(), zeros.size());
        block.compression.uncompressed_size = 32768 * block.data.size();
        const std::uint64_t allocated = bytes_allocated_by([&block] {
            EXPECT_TRUE(openxisf::test::throws<openxisf::integrity_error>(openxisf::errc::corrupt_compressed_data, [&] {
                (void)openxisf::detail::decompress_block(block.data, block.compression, {});
            }));
        });
        EXPECT_LT(allocated, 4096U);
        // Into memory of the caller, a shuffled block needs a buffer of its size.
        std::vector<std::byte> destination(block.compression.uncompressed_size);
        const std::uint64_t buffered = bytes_allocated_by([&block, &destination] {
            EXPECT_TRUE(openxisf::test::throws<openxisf::integrity_error>(openxisf::errc::corrupt_compressed_data, [&] {
                openxisf::detail::decompress_block_into(block.data, block.compression, {}, destination);
            }));
        });
        EXPECT_LT(buffered, 4096U);
    }
}

} // namespace
