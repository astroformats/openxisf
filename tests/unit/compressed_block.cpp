// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Compressed blocks as a whole (spec §10.6): every codec with and without byte shuffling, subblocks, compression levels
// (spec §11.4.2) and the limits of the decoders.

#include "codec/compressed_block.h"

#include <openxisf/error.h>
#include <openxisf/limits.h>

#include "codec/codecs.h"
#include "codec/shuffle.h"
#include "support/bytes.h"
#include "support/faulty_io.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::detail::block_compression;
using openxisf::detail::codec_level;
using openxisf::detail::compress_block;
using openxisf::detail::compressed_block;
using openxisf::detail::compression_codec;
using openxisf::detail::decompress_block;
using openxisf::detail::subblock;
using openxisf::detail::subblock_size;
using openxisf::detail::subblocks_of;
using openxisf::test::throws;

constexpr std::uint64_t no_limit = std::numeric_limits<std::uint64_t>::max();

constexpr std::array<compression_codec, 4> all_codecs{compression_codec::zlib, compression_codec::lz4,
                                                      compression_codec::lz4hc, compression_codec::zstd};

// Little-endian 16-bit samples of a slow ramp with a little noise, which compress as images do: better with shuffling.
std::vector<std::byte> samples(std::size_t length)
{
    std::vector<std::byte> data(length);
    std::uint32_t state = 12345;
    for (std::size_t i = 0; i < length; ++i) {
        state = (state * 1'103'515'245U) + 12'345U;
        const auto value = static_cast<std::uint32_t>(((i / 2) * 7) + ((state >> 16U) & 0x1FU));
        data[i] = static_cast<std::byte>(i % 2 == 0 ? value & 0xFFU : (value >> 8U) & 0xFFU);
    }
    return data;
}

std::string name_of(compression_codec codec)
{
    return std::string(openxisf::detail::codec_name(codec));
}

std::vector<std::byte> decompressed(const compressed_block& block, const openxisf::limits& limits = {})
{
    return decompress_block(block.data, block.compression, limits);
}

// ---------------------------------------------------------------------------------------------------------------------
// Round trips

struct round_trip_case
{
    compression_codec codec = compression_codec::zlib;
    std::uint64_t item_size = 0;
    std::size_t length = 0;
};

std::string case_name(const round_trip_case& value)
{
    return name_of(value.codec) + (value.item_size == 0 ? "" : "_sh" + std::to_string(value.item_size)) + "_length" +
           std::to_string(value.length);
}

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const round_trip_case& value, std::ostream* output)
{
    *output << case_name(value);
}

std::vector<round_trip_case> round_trip_cases()
{
    std::vector<round_trip_case> cases;
    for (const compression_codec codec : all_codecs) {
        for (const std::uint64_t item_size : {0U, 1U, 2U, 4U, 8U, 16U}) {
            for (const std::size_t length : {0U, 1U, 15U, 16U, 17U, 100'003U}) {
                cases.push_back({.codec = codec, .item_size = item_size, .length = length});
            }
        }
    }
    return cases;
}

class compressed_block_round_trip : public testing::TestWithParam<round_trip_case>
{};

TEST_P(compressed_block_round_trip, restores_the_data)
{
    const auto [codec, item_size, length] = GetParam();
    const std::vector<std::byte> data = samples(length);

    const compressed_block block = compress_block(data, {.codec = codec, .item_size = item_size});
    EXPECT_EQ(block.compression,
              (block_compression{.codec = codec, .uncompressed_size = length, .item_size = item_size}));
    EXPECT_EQ(decompressed(block), data);
}

INSTANTIATE_TEST_SUITE_P(codecs, compressed_block_round_trip, testing::ValuesIn(round_trip_cases()),
                         [](const testing::TestParamInfo<round_trip_case>& parameter) {
                             return case_name(parameter.param);
                         });

TEST(compressed_block, byte_shuffling_helps_the_codecs_with_multibyte_samples)
{
    const std::vector<std::byte> data = samples(100'000);
    for (const compression_codec codec : all_codecs) {
        EXPECT_LT(compress_block(data, {.codec = codec, .item_size = 2}).data.size(),
                  compress_block(data, {.codec = codec}).data.size())
            << name_of(codec);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Subblocks

TEST(compressed_block, a_block_above_the_subblock_size_is_divided_and_shuffled_before_the_division)
{
    // 10,007 bytes in subblocks of at most 1,000: ten of 1,000 and one of 7.
    const std::vector<std::byte> data = samples(10'007);
    std::vector<std::byte> shuffled(data.size());
    openxisf::detail::shuffle_bytes(data, shuffled, 4);

    for (const compression_codec codec : all_codecs) {
        const compressed_block block =
            compress_block(data, {.codec = codec, .item_size = 4, .max_subblock_size = 1000});
        const std::vector<subblock>& subblocks = block.compression.subblocks;
        ASSERT_EQ(subblocks.size(), 11U) << name_of(codec);
        EXPECT_EQ(subblocks.back().uncompressed_size, 7U);
        EXPECT_EQ(decompressed(block), data) << name_of(codec);

        // Each subblock is a stream of its own, and together they hold the shuffled block.
        std::vector<std::byte> joined;
        std::size_t offset = 0;
        for (const subblock& part : subblocks) {
            EXPECT_EQ(part.uncompressed_size, &part == &subblocks.back() ? 7U : 1000U);
            const std::vector<std::byte> piece =
                decompress_block(std::span(block.data).subspan(offset, part.compressed_size),
                                 {.codec = codec, .uncompressed_size = part.uncompressed_size}, {});
            joined.insert(joined.end(), piece.begin(), piece.end());
            offset += part.compressed_size;
        }
        EXPECT_EQ(offset, block.data.size());
        EXPECT_EQ(joined, shuffled) << name_of(codec);
    }
}

// PixInsight writes a subblock that does not compress as it is, with equal sizes (see the large sample L1), and reads
// codec output that is not smaller than its data as data stored as they are, so the encoder stores such subblocks so.
TEST(compressed_block, a_subblock_that_does_not_compress_is_stored_as_it_is)
{
    // 4,000 bytes that every codec compresses, followed by 4,000 bytes of noise, which none does.
    std::vector<std::byte> data = openxisf::test::pattern(4000);
    std::uint32_t state = 1;
    for (std::size_t i = 0; i < 4000; ++i) {
        state = (state * 1'664'525U) + 1'013'904'223U;
        data.push_back(static_cast<std::byte>(state >> 24U));
    }
    for (const compression_codec codec : all_codecs) {
        const compressed_block block = compress_block(data, {.codec = codec, .max_subblock_size = 4000});
        ASSERT_EQ(block.compression.subblocks.size(), 2U);
        EXPECT_LT(block.compression.subblocks[0].compressed_size, 4000U) << name_of(codec);
        EXPECT_EQ(block.compression.subblocks[1].compressed_size, 4000U) << name_of(codec);
        EXPECT_TRUE(std::ranges::equal(std::span(block.data).last(4000), std::span(data).last(4000)));
        EXPECT_EQ(decompressed(block), data) << name_of(codec);
        // A block that does not compress at all comes back as it is.
        const compressed_block noise = compress_block(std::span(data).last(4000), {.codec = codec});
        EXPECT_TRUE(noise.compression.subblocks.empty());
        EXPECT_TRUE(std::ranges::equal(noise.data, std::span(data).last(4000)));
    }
}

TEST(compressed_block, codec_output_of_the_size_of_its_data_is_not_stored)
{
    // Equal sizes mean data stored as they are, so codec output exactly as long as its data would be read back as data.
    // Noise, a run of zeros and noise again: LZ4 spends a few bytes on the noise and saves on the run, and for some
    // lengths of the two they are equal. Those are searched for, since versions of LZ4 differ.
    constexpr std::size_t head = 1000;
    constexpr std::size_t tail = 100;
    std::array<std::byte, head + tail> noise{};
    std::uint32_t state = 7;
    for (std::byte& value : noise) {
        state = (state * 1'664'525U) + 1'013'904'223U;
        value = static_cast<std::byte>(state >> 24U);
    }
    for (std::size_t length = 900; length < head; ++length) {
        for (std::size_t run = 0; run < 64; ++run) {
            // Built at its size and filled, rather than resized: GCC 14 takes the growth of a resize for a size that
            // may wrap around (-Wstringop-overflow).
            std::vector<std::byte> data(length + run + tail);
            std::copy_n(noise.begin(), length, data.begin());
            std::copy_n(noise.begin() + head, tail, data.begin() + static_cast<std::ptrdiff_t>(length + run));
            std::vector<std::byte> output;
            openxisf::detail::lz4_compress(data, output);
            if (output.size() == data.size()) {
                const compressed_block block = compress_block(data, {.codec = compression_codec::lz4});
                EXPECT_EQ(block.data, data);
                EXPECT_EQ(decompressed(block), data);
                return;
            }
        }
    }
    ADD_FAILURE() << "no noise and run of zeros make the output of LZ4 as long as its data";
}

TEST(compressed_block, subblocks_are_handed_over_in_order_with_their_sizes)
{
    // 2,500 bytes in subblocks of 1,000: each one reaches the store, in order, with its sizes, on the calling thread.
    // Whether a subblock is stored before the next is compressed depends on the build: with oneTBB a batch of them is
    // compressed first.
    const std::vector<std::byte> data = samples(2500);
    for (const compression_codec codec : all_codecs) {
        std::vector<std::byte> stored;
        std::vector<subblock> handed;
        const block_compression compression =
            openxisf::detail::compress_subblocks(data, {.codec = codec, .item_size = 2, .max_subblock_size = 1000},
                                                 [&](std::span<const std::byte> part, const subblock& sizes) {
                                                     EXPECT_EQ(part.size(), sizes.compressed_size);
                                                     stored.insert(stored.end(), part.begin(), part.end());
                                                     handed.push_back(sizes);
                                                 });
        EXPECT_EQ(compression.subblocks, handed) << name_of(codec);
        ASSERT_EQ(handed.size(), 3U);
        EXPECT_EQ(handed.back().uncompressed_size, 500U);
        EXPECT_EQ(decompress_block(stored, compression, {}), data) << name_of(codec);

        // A block within the subblock size is one subblock, which is listed.
        EXPECT_EQ(openxisf::detail::compress_subblocks(data, {.codec = codec}, [](auto, auto) {}).subblocks.size(), 1U);
    }
    // What the store throws stops the compression.
    std::size_t calls = 0;
    EXPECT_THROW((void)openxisf::detail::compress_subblocks(data, {.max_subblock_size = 1000},
                                                            [&calls](auto, auto) {
                                                                ++calls;
                                                                throw openxisf::test::injected_fault();
                                                            }),
                 openxisf::test::injected_fault);
    EXPECT_EQ(calls, 1U);
}

TEST(compressed_block, a_subblock_with_equal_sizes_is_copied_whatever_the_codec_could_take)
{
    // Stored as they are, the bytes need no decoder, so LZ4's limit does not apply either.
    const std::vector<std::byte> data = samples(100);
    const block_compression whole{.codec = compression_codec::lz4, .uncompressed_size = 100};
    EXPECT_EQ(decompress_block(data, whole, {}), data);
    EXPECT_EQ(subblocks_of({.codec = compression_codec::lz4, .uncompressed_size = std::uint64_t{1} << 40},
                           std::uint64_t{1} << 40)
                  .size(),
              1U);
}

TEST(compressed_block, a_block_within_the_subblock_size_has_no_subblocks)
{
    const std::vector<std::byte> data = samples(1000);
    for (const std::uint64_t size : {std::uint64_t{1000}, std::uint64_t{0}, no_limit}) {
        EXPECT_TRUE(compress_block(data, {.max_subblock_size = size}).compression.subblocks.empty()) << size;
    }
    EXPECT_EQ(compress_block(data, {.max_subblock_size = 999}).compression.subblocks.size(), 2U);
}

TEST(compressed_block, the_subblocks_of_the_encoders_fit_their_codecs)
{
    // zlib counts in 32 bits, and the room below 2^32 holds the growth of data that do not compress.
    struct limit
    {
        compression_codec codec = compression_codec::zlib;
        std::uint64_t size = 0;
    };
    for (const limit& entry : {limit{.codec = compression_codec::zlib, .size = 0xFF00'0000U},
                               limit{.codec = compression_codec::lz4, .size = 2'113'929'216U},
                               limit{.codec = compression_codec::lz4hc, .size = 2'113'929'216U},
                               limit{.codec = compression_codec::zstd, .size = no_limit}}) {
        EXPECT_EQ(subblock_size({.codec = entry.codec}), entry.size) << name_of(entry.codec);
        EXPECT_EQ(subblock_size({.codec = entry.codec, .max_subblock_size = no_limit}), entry.size);
        EXPECT_EQ(subblock_size({.codec = entry.codec, .max_subblock_size = 1000}), 1000U);
    }
}

TEST(compressed_block, the_subblocks_account_for_every_byte_of_their_block)
{
    // A block of 3 stored bytes, declared as 10 bytes uncompressed. zlib takes subblocks of any size, so only the sums
    // count.
    const auto subblocks = [](std::vector<subblock> parts) {
        return subblocks_of({.codec = compression_codec::zlib, .uncompressed_size = 10, .subblocks = std::move(parts)},
                            3);
    };
    const std::vector<subblock> valid{{.compressed_size = 1, .uncompressed_size = 4},
                                      {.compressed_size = 2, .uncompressed_size = 6}};
    EXPECT_EQ(subblocks(valid), valid);
    EXPECT_EQ(subblocks({}), (std::vector<subblock>{{.compressed_size = 3, .uncompressed_size = 10}}));

    const std::vector<std::vector<subblock>> invalid{
        {{.compressed_size = 3, .uncompressed_size = 9}},
        {{.compressed_size = 2, .uncompressed_size = 10}},
        {{.compressed_size = 4, .uncompressed_size = 10}},
        {{.compressed_size = 1, .uncompressed_size = 4}, {.compressed_size = 2, .uncompressed_size = 5}},
        {{.compressed_size = 3, .uncompressed_size = no_limit}, {.compressed_size = 0, .uncompressed_size = 11}},
        {{.compressed_size = no_limit, .uncompressed_size = 10}, {.compressed_size = 4, .uncompressed_size = 0}}};
    for (const std::vector<subblock>& parts : invalid) {
        EXPECT_TRUE(throws<openxisf::invalid_data_error>(errc::invalid_subblocks, [&] { (void)subblocks(parts); }))
            << parts.front().compressed_size << "," << parts.front().uncompressed_size;
    }
}

TEST(compressed_block, a_subblock_must_fit_the_decoder_of_its_codec)
{
    // LZ4 decodes at most 2^31 - 1 bytes at once, compressed or not. zlib and Zstandard have no limit. 2^24 stored
    // bytes can decode to 2^31 bytes with any codec.
    constexpr std::uint64_t largest = 0x7FFF'FFFF;
    constexpr std::uint64_t stored = std::uint64_t{1} << 24;
    for (const compression_codec codec : all_codecs) {
        const bool lz4 = codec == compression_codec::lz4 || codec == compression_codec::lz4hc;
        const block_compression whole{.codec = codec, .uncompressed_size = largest + 1};
        const block_compression divided{.codec = codec,
                                        .uncompressed_size = largest + 1,
                                        .subblocks = {{.compressed_size = stored, .uncompressed_size = largest},
                                                      {.compressed_size = 1, .uncompressed_size = 1}}};
        EXPECT_EQ(subblocks_of(divided, stored + 1).size(), 2U);
        EXPECT_EQ(subblocks_of({.codec = codec, .uncompressed_size = 5}, largest).size(), 1U);
        if (lz4) {
            EXPECT_TRUE(throws<openxisf::invalid_data_error>(errc::invalid_subblocks,
                                                             [&] { (void)subblocks_of(whole, stored); }));
            EXPECT_TRUE(throws<openxisf::invalid_data_error>(errc::invalid_subblocks, [&] {
                (void)subblocks_of({.codec = codec, .uncompressed_size = 5}, largest + 1);
            }));
        } else {
            EXPECT_EQ(subblocks_of(whole, stored).size(), 1U) << name_of(codec);
        }
    }
}

TEST(compressed_block, a_subblock_cannot_declare_more_than_its_codec_decodes_from_its_bytes)
{
    // A zlib stream decodes to at most 1032 bytes for each of its bytes, an LZ4 block to 255 and a Zstandard frame to
    // 32768, so 9 stored bytes give at most 9288, 2295 and 294912 bytes. A subblock stored as it is has equal sizes.
    struct bound
    {
        compression_codec codec = compression_codec::zlib;
        std::uint64_t ratio = 0;
        std::uint64_t most = 0;
    };
    for (const bound& entry : {bound{.codec = compression_codec::zlib, .ratio = 1032, .most = 9288},
                               bound{.codec = compression_codec::lz4, .ratio = 255, .most = 2295},
                               bound{.codec = compression_codec::lz4hc, .ratio = 255, .most = 2295},
                               bound{.codec = compression_codec::zstd, .ratio = 32768, .most = 294'912}}) {
        SCOPED_TRACE(name_of(entry.codec));
        EXPECT_EQ(subblocks_of({.codec = entry.codec, .uncompressed_size = entry.most}, 9).size(), 1U);
        EXPECT_EQ(subblocks_of({.codec = entry.codec, .uncompressed_size = 0}, 9).size(), 1U);
        try {
            (void)subblocks_of({.codec = entry.codec, .uncompressed_size = entry.most + 1}, 9);
            ADD_FAILURE() << "no error";
        } catch (const openxisf::invalid_data_error& failure) {
            EXPECT_EQ(failure.code(), errc::invalid_subblocks);
            EXPECT_EQ(std::string_view(failure.what()),
                      "the block declares " + std::to_string(entry.most + 1) + " bytes, but " + name_of(entry.codec) +
                          " decodes at most " + std::to_string(entry.ratio) + " times its stored size, " +
                          std::to_string(entry.most) + " bytes");
        }
        // The subblock that declares too much is named.
        const block_compression divided{.codec = entry.codec,
                                        .uncompressed_size = entry.most + 2,
                                        .subblocks = {{.compressed_size = 1, .uncompressed_size = 1},
                                                      {.compressed_size = 9, .uncompressed_size = entry.most + 1}}};
        try {
            (void)subblocks_of(divided, 10);
            ADD_FAILURE() << "no error";
        } catch (const openxisf::invalid_data_error& failure) {
            EXPECT_EQ(failure.code(), errc::invalid_subblocks);
            EXPECT_TRUE(std::string_view(failure.what()).starts_with("subblock 2 declares ")) << failure.what();
        }
        EXPECT_EQ(subblocks_of({.codec = entry.codec, .uncompressed_size = 1'000'000}, 1'000'000).size(), 1U);
    }
}

TEST(compressed_block, the_densest_output_of_the_encoders_decodes_within_the_bounds)
{
    // Zeros at the highest level of each codec come close to its bound, and decode. Zstandard writes runs of 128 KiB in
    // 4 bytes each, but the first of a frame and the header of the frame.
    const std::vector<std::byte> zeros(std::size_t{4} << 20);
    struct bound
    {
        compression_codec codec = compression_codec::zlib;
        std::uint64_t ratio = 0;
    };
    for (const bound& entry :
         {bound{.codec = compression_codec::zlib, .ratio = 1032}, bound{.codec = compression_codec::lz4, .ratio = 255},
          bound{.codec = compression_codec::lz4hc, .ratio = 255},
          bound{.codec = compression_codec::zstd, .ratio = 32768}}) {
        SCOPED_TRACE(name_of(entry.codec));
        const compressed_block block = compress_block(zeros, {.codec = entry.codec, .level = 100});
        ASSERT_TRUE(block.compression.subblocks.empty());
        EXPECT_LT(zeros.size(), entry.ratio * block.data.size());
        EXPECT_GT(zeros.size(), entry.ratio * block.data.size() * 4 / 5) << block.data.size() << " bytes";
        EXPECT_EQ(decompressed(block), zeros);
    }
}

TEST(compressed_block, a_corrupt_subblock_is_named_in_the_error)
{
    compressed_block block =
        compress_block(samples(3000), {.codec = compression_codec::zstd, .max_subblock_size = 1000});
    ASSERT_EQ(block.compression.subblocks.size(), 3U);
    // The last byte of the second subblock.
    block.data[block.compression.subblocks[0].compressed_size + block.compression.subblocks[1].compressed_size - 1] ^=
        std::byte{0xFF};
    try {
        (void)decompressed(block);
        ADD_FAILURE() << "no error";
    } catch (const openxisf::integrity_error& failure) {
        EXPECT_EQ(failure.code(), errc::corrupt_compressed_data);
        EXPECT_TRUE(std::string_view(failure.what()).starts_with("subblock 2 of 3: ")) << failure.what();
    }
}

TEST(compressed_block, progress_follows_the_subblocks_and_can_stop_the_decompression)
{
    // Three subblocks of 1000, 1000 and 500 bytes: the progress counts their uncompressed bytes.
    const compressed_block block =
        compress_block(samples(2500), {.codec = compression_codec::lz4, .max_subblock_size = 1000});
    std::vector<std::uint64_t> calls;
    const std::vector<std::byte> data =
        decompress_block(block.data, block.compression, {}, [&calls](std::uint64_t done) { calls.push_back(done); });
    EXPECT_EQ(data, samples(2500));
    EXPECT_EQ(calls, (std::vector<std::uint64_t>{1000, 2000, 2500}));

    // What the progress function throws passes through.
    EXPECT_THROW((void)decompress_block(block.data, block.compression, {},
                                        [](std::uint64_t done) {
                                            if (done == 2000) {
                                                throw openxisf::test::injected_fault();
                                            }
                                        }),
                 openxisf::test::injected_fault);
}

TEST(compressed_block, a_block_decompresses_into_memory_of_the_caller)
{
    // With and without byte shuffling, in several subblocks.
    for (const std::uint64_t item_size : {0U, 2U}) {
        const compressed_block block = compress_block(
            samples(2500), {.codec = compression_codec::lz4, .item_size = item_size, .max_subblock_size = 1000});
        std::vector<std::byte> destination(2500);
        openxisf::detail::decompress_block_into(block.data, block.compression, {}, destination);
        EXPECT_EQ(destination, samples(2500)) << item_size;
        for (const std::size_t size : {2499U, 2501U}) {
            std::vector<std::byte> other(size);
            EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument, [&] {
                openxisf::detail::decompress_block_into(block.data, block.compression, {}, other);
            })) << size;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Limits

TEST(compressed_block, a_block_that_decompresses_beyond_the_allocation_limit_is_not_decompressed)
{
    const compressed_block block = compress_block(samples(1000), {});
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::allocation_too_large,
                                              [&block] { (void)decompressed(block, {.max_allocation = 999}); }));
    EXPECT_EQ(decompressed(block, {.max_allocation = 1000}).size(), 1000U);
    EXPECT_EQ(decompressed(block, {.max_allocation = 0}).size(), 1000U);
    // Nor can a block declare a size that no allocation can hold: its codec cannot decode that much from its bytes.
    block_compression huge = block.compression;
    huge.uncompressed_size = std::uint64_t{1} << 63;
    EXPECT_TRUE(throws<openxisf::invalid_data_error>(
        errc::invalid_subblocks, [&] { (void)decompress_block(block.data, huge, {.max_allocation = 0}); }));

    // Into memory of the caller, only a shuffled block needs a buffer of its size.
    std::vector<std::byte> destination(1000);
    openxisf::detail::decompress_block_into(block.data, block.compression, {.max_allocation = 999}, destination);
    EXPECT_EQ(destination, samples(1000));
    const compressed_block shuffled = compress_block(samples(1000), {.item_size = 2});
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::allocation_too_large, [&] {
        openxisf::detail::decompress_block_into(shuffled.data, shuffled.compression, {.max_allocation = 999},
                                                destination);
    }));
}

// ---------------------------------------------------------------------------------------------------------------------
// Compression levels (spec §11.4.2)

TEST(compressed_block, abstract_levels_map_linearly_to_the_levels_of_each_codec)
{
    struct mapping
    {
        compression_codec codec = compression_codec::zlib;
        int level = 0;
        int expected = 0;
    };
    // 0 is the default of the codec; 1 to 100 cover the range of the codec, rounded to the nearest level.
    const std::vector<mapping> mappings{{.codec = compression_codec::zlib, .level = 0, .expected = 6},
                                        {.codec = compression_codec::zlib, .level = 1, .expected = 0},
                                        {.codec = compression_codec::zlib, .level = 6, .expected = 0},
                                        {.codec = compression_codec::zlib, .level = 7, .expected = 1},
                                        {.codec = compression_codec::zlib, .level = 50, .expected = 4},
                                        {.codec = compression_codec::zlib, .level = 100, .expected = 9},
                                        {.codec = compression_codec::lz4hc, .level = 0, .expected = 9},
                                        {.codec = compression_codec::lz4hc, .level = 1, .expected = 1},
                                        {.codec = compression_codec::lz4hc, .level = 50, .expected = 6},
                                        {.codec = compression_codec::lz4hc, .level = 100, .expected = 12},
                                        {.codec = compression_codec::zstd, .level = 0, .expected = 3},
                                        {.codec = compression_codec::zstd, .level = 1, .expected = 1},
                                        {.codec = compression_codec::zstd, .level = 50, .expected = 11},
                                        {.codec = compression_codec::zstd, .level = 90, .expected = 20},
                                        {.codec = compression_codec::zstd, .level = 100, .expected = 22},
                                        {.codec = compression_codec::lz4, .level = 0, .expected = 0},
                                        {.codec = compression_codec::lz4, .level = 100, .expected = 0}};
    for (const mapping& entry : mappings) {
        EXPECT_EQ(codec_level(entry.codec, entry.level), entry.expected)
            << name_of(entry.codec) << " at " << entry.level;
    }
    for (const int level : {-1, 101}) {
        EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument,
                                                  [level] { (void)codec_level(compression_codec::zstd, level); }));
    }
}

TEST(compressed_block, the_default_level_of_each_codec_has_an_abstract_level)
{
    // XISF:CompressionLevel reports it, and it gives the default level back.
    using openxisf::detail::default_abstract_level;
    EXPECT_EQ(default_abstract_level(compression_codec::zlib), 67);
    EXPECT_EQ(default_abstract_level(compression_codec::lz4hc), 73);
    EXPECT_EQ(default_abstract_level(compression_codec::zstd), 10);
    EXPECT_EQ(default_abstract_level(compression_codec::lz4), 0);
    for (const compression_codec codec : {compression_codec::zlib, compression_codec::lz4hc, compression_codec::zstd}) {
        EXPECT_EQ(codec_level(codec, default_abstract_level(codec)), codec_level(codec, 0)) << name_of(codec);
    }
}

TEST(compressed_block, the_level_reaches_the_codec)
{
    // A higher level makes these data no larger, once they are shuffled, and the highest smaller than the lowest. Two
    // levels next to each other may compress alike in another version of a codec.
    const std::vector<std::byte> data = samples(100'000);
    for (const compression_codec codec : {compression_codec::zlib, compression_codec::lz4hc, compression_codec::zstd}) {
        std::vector<std::size_t> sizes;
        for (const int level : {1, 50, 100}) {
            const compressed_block block = compress_block(data, {.codec = codec, .item_size = 2, .level = level});
            EXPECT_EQ(decompressed(block), data);
            sizes.push_back(block.data.size());
        }
        EXPECT_LE(sizes[1], sizes[0]) << name_of(codec);
        EXPECT_LE(sizes[2], sizes[1]) << name_of(codec);
        EXPECT_LT(sizes[2], sizes[0]) << name_of(codec);
    }
    // The abstract level 1 is zlib's level 0, which stores the data in deflate's own blocks, larger than the data; so
    // the subblock is stored as it is.
    EXPECT_EQ(compress_block(data, {.codec = compression_codec::zlib, .level = 1}).data, data);
}

} // namespace
