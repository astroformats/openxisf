// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The codec adapters, one subblock at a time (spec §10.6.3 to §10.6.10): each decoder fills its output exactly from all
// of its input, or fails with corrupt_compressed_data. Round trips of every codec are in compressed_block.cpp; here the
// streams are malformed, or built by hand from RFC 1950, the LZ4 block format and RFC 8878, and the encoders that keep
// their state are compared with the libraries' own calls.

#include "codec/codecs.h"

#include <openxisf/error.h>

#include "core/xoshiro.h"
#include "support/bytes.h"
#include "support/throws.h"

#include <gtest/gtest.h>
#include <lz4.h>
#include <lz4hc.h>
#include <zlib.h>
#include <zstd.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <type_traits>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::detail::lz4_compress;
using openxisf::detail::lz4_decompress;
using openxisf::detail::zlib_compress;
using openxisf::detail::zlib_decompress;
using openxisf::detail::zstd_compress;
using openxisf::detail::zstd_decompress;
using openxisf::test::bytes;
using openxisf::test::pattern;
using openxisf::test::throws;

using decoder = std::function<void(std::span<const std::byte>, std::span<std::byte>)>;

std::vector<std::byte> byte_values(std::initializer_list<unsigned int> values)
{
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const unsigned int value : values) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

std::vector<std::byte> joined(std::vector<std::byte> first, const std::vector<std::byte>& second)
{
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

// Decodes input into a buffer of size bytes.
std::vector<std::byte> decoded(const decoder& decode, std::span<const std::byte> input, std::size_t size)
{
    std::vector<std::byte> output(size);
    decode(input, output);
    return output;
}

testing::AssertionResult corrupt(const decoder& decode, std::span<const std::byte> input, std::size_t size)
{
    return throws<openxisf::integrity_error>(errc::corrupt_compressed_data,
                                             [&] { (void)decoded(decode, input, size); });
}

// The failures that every decoder reports, for a valid stream of data: too little or too much output, missing or extra
// input, and no input.
void expect_exact_sizes(const decoder& decode, const std::vector<std::byte>& stream, const std::vector<std::byte>& data)
{
    EXPECT_EQ(decoded(decode, stream, data.size()), data);
    EXPECT_TRUE(corrupt(decode, stream, data.size() - 1));
    EXPECT_TRUE(corrupt(decode, stream, data.size() + 1));
    EXPECT_TRUE(corrupt(decode, std::span(stream).first(stream.size() - 1), data.size()));
    EXPECT_TRUE(corrupt(decode, std::span(stream).first(stream.size() / 2), data.size()));
    EXPECT_TRUE(corrupt(decode, joined(stream, bytes("x")), data.size()));
    EXPECT_TRUE(corrupt(decode, {}, data.size()));
}

// ---------------------------------------------------------------------------------------------------------------------
// zlib (RFC 1950)

void zlib_decoder(std::span<const std::byte> input, std::span<std::byte> output)
{
    zlib_decompress(input, output);
}

std::vector<std::byte> zlib_stream(std::span<const std::byte> data, std::size_t max_piece = 0xFFFF'FFFF)
{
    std::vector<std::byte> stream;
    zlib_compress(data, 6, stream, max_piece);
    return stream;
}

TEST(zlib_codec, decodes_exactly_the_declared_size_from_the_whole_stream)
{
    const std::vector<std::byte> data = pattern(1000);
    expect_exact_sizes(zlib_decoder, zlib_stream(data), data);
    EXPECT_TRUE(decoded(zlib_decoder, zlib_stream({}), 0).empty());
}

TEST(zlib_codec, the_adler32_check_value_covers_the_data)
{
    std::vector<std::byte> stream = zlib_stream(pattern(1000));
    stream.back() ^= std::byte{0x01};
    EXPECT_TRUE(corrupt(zlib_decoder, stream, 1000));
}

TEST(zlib_codec, refuses_streams_without_the_zlib_wrapper_and_preset_dictionaries)
{
    // Raw deflate data: the stream without its 2-byte header and 4-byte check value.
    const std::vector<std::byte> stream = zlib_stream(pattern(1000));
    EXPECT_TRUE(corrupt(zlib_decoder, std::span(stream).subspan(2, stream.size() - 6), 1000));
    // A header with the FDICT flag, followed by the identifier of a dictionary.
    EXPECT_TRUE(corrupt(zlib_decoder, joined(byte_values({0x78, 0x20, 0, 0, 0, 1}), stream), 1000));
}

TEST(zlib_codec, works_in_pieces_both_ways)
{
    // The pieces stand for the 32-bit counters of zlib, so that blocks above 4 GiB need no 4 GiB buffers.
    const std::vector<std::byte> data = pattern(10'007);
    const std::vector<std::byte> stream = zlib_stream(data, 7);
    EXPECT_EQ(stream, zlib_stream(data));
    for (const std::size_t piece : {1U, 7U, 4096U}) {
        const decoder in_pieces = [piece](std::span<const std::byte> input, std::span<std::byte> output) {
            zlib_decompress(input, output, piece);
        };
        expect_exact_sizes(in_pieces, stream, data);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// LZ4 (the block format)

void lz4_decoder(std::span<const std::byte> input, std::span<std::byte> output)
{
    lz4_decompress(input, output);
}

TEST(lz4_codec, decodes_exactly_the_declared_size_from_the_whole_block)
{
    const std::vector<std::byte> data = pattern(1000);
    std::vector<std::byte> block;
    lz4_compress(data, block);
    expect_exact_sizes(lz4_decoder, block, data);
}

TEST(lz4_codec, decodes_blocks_built_by_hand)
{
    // A sequence of 3 literals and nothing else; the empty block is a token without literals.
    EXPECT_EQ(decoded(lz4_decoder, byte_values({0x30, 'a', 'b', 'c'}), 3), bytes("abc"));
    EXPECT_TRUE(decoded(lz4_decoder, byte_values({0x00}), 0).empty());
    // A match whose offset reaches before the start of the output.
    EXPECT_TRUE(corrupt(lz4_decoder, byte_values({0x10, 'a', 0x05, 0x00, 0x50, 'a', 'b', 'c', 'd', 'e'}), 10));
}

// ---------------------------------------------------------------------------------------------------------------------
// Zstandard (RFC 8878)

constexpr std::uint64_t no_window_limit = 0;

decoder zstd_decoder(std::uint64_t max_window)
{
    return [max_window](std::span<const std::byte> input, std::span<std::byte> output) {
        zstd_decompress(input, output, max_window);
    };
}

std::vector<std::byte> magic_number()
{
    return byte_values({0x28, 0xB5, 0x2F, 0xFD});
}

// The header of the last block of a frame, a Raw_Block of data (RFC 8878 §3.1.1.2).
std::vector<std::byte> raw_block(const std::vector<std::byte>& data)
{
    const auto header = static_cast<std::uint32_t>(1U | (data.size() << 3U));
    return joined(byte_values({header & 0xFFU, (header >> 8U) & 0xFFU, (header >> 16U) & 0xFFU}), data);
}

// A frame that does not declare its content size, with a window of 2^window_log bytes.
std::vector<std::byte> unsized_frame(const std::vector<std::byte>& data, unsigned int window_log)
{
    return joined(joined(magic_number(), byte_values({0x00, (window_log - 10U) << 3U})), raw_block(data));
}

// A single-segment frame that declares a content size below 256, which has no window descriptor.
std::vector<std::byte> sized_frame(const std::vector<std::byte>& data, unsigned int declared)
{
    return joined(joined(magic_number(), byte_values({0x20, declared})), raw_block(data));
}

// A block that repeats value size times, an RLE_Block (RFC 8878 §3.1.1.2), which is not the last of its frame.
std::vector<std::byte> rle_block(std::byte value, std::size_t size)
{
    const auto header = static_cast<std::uint32_t>((1U << 1U) | (size << 3U));
    return joined(byte_values({header & 0xFFU, (header >> 8U) & 0xFFU, (header >> 16U) & 0xFFU}), {value});
}

// The last block of a frame, a Compressed_Block (RFC 8878 §3.1.1.3) that holds literals alone: fewer than 32 bytes in
// a Raw_Literals_Block, and no sequences.
std::vector<std::byte> literals_block(const std::vector<std::byte>& literals)
{
    const std::vector<std::byte> content =
        joined(joined(byte_values({static_cast<unsigned int>(literals.size() << 3U)}), literals), byte_values({0}));
    const auto header = static_cast<std::uint32_t>(1U | (2U << 1U) | (content.size() << 3U));
    return joined(byte_values({header & 0xFFU, (header >> 8U) & 0xFFU, (header >> 16U) & 0xFFU}), content);
}

TEST(zstd_codec, decodes_exactly_the_declared_size_from_the_whole_frame)
{
    const std::vector<std::byte> data = pattern(1000);
    std::vector<std::byte> frame;
    zstd_compress(data, 3, frame);
    expect_exact_sizes(zstd_decoder(no_window_limit), frame, data);
    expect_exact_sizes(zstd_decoder(no_window_limit), unsized_frame(pattern(100), 10), pattern(100));
    EXPECT_TRUE(decoded(zstd_decoder(no_window_limit), unsized_frame({}, 10), 0).empty());
}

TEST(zstd_codec, the_declared_content_size_must_be_the_uncompressed_size)
{
    const std::vector<std::byte> data = pattern(100);
    EXPECT_EQ(decoded(zstd_decoder(no_window_limit), sized_frame(data, 100), 100), data);
    // The raw block holds 100 bytes either way; the declaration is wrong.
    EXPECT_TRUE(corrupt(zstd_decoder(no_window_limit), sized_frame(data, 101), 101));
    EXPECT_TRUE(corrupt(zstd_decoder(no_window_limit), sized_frame(data, 99), 99));
}

TEST(zstd_codec, a_frame_that_declares_another_size_is_corrupt_whatever_its_window)
{
    // A single-segment frame declaring 1 GiB in 4 bytes, whose window is therefore 1 GiB too, for an output of 100
    // bytes: the size is wrong before the window matters.
    const std::vector<std::byte> data = pattern(100);
    const std::vector<std::byte> frame =
        joined(joined(magic_number(), byte_values({0xA0, 0x00, 0x00, 0x00, 0x40})), raw_block(data));
    EXPECT_TRUE(corrupt(zstd_decoder(std::uint64_t{1} << 20), frame, 100));
}

TEST(zstd_codec, a_subblock_is_exactly_one_frame_of_compressed_data)
{
    const std::vector<std::byte> data = pattern(100);
    const std::vector<std::byte> frame = unsized_frame(data, 10);
    EXPECT_TRUE(corrupt(zstd_decoder(no_window_limit), joined(frame, frame), 200));
    // A skippable frame (RFC 8878 §3.1.2) holds no data, before a frame or alone.
    const std::vector<std::byte> skippable = byte_values({0x50, 0x2A, 0x4D, 0x18, 1, 0, 0, 0, 0});
    EXPECT_TRUE(corrupt(zstd_decoder(no_window_limit), joined(skippable, frame), 100));
    EXPECT_TRUE(corrupt(zstd_decoder(no_window_limit), skippable, 0));
    EXPECT_TRUE(corrupt(zstd_decoder(no_window_limit), bytes("not a frame"), 100));
}

TEST(zstd_codec, a_frame_whose_window_exceeds_the_limit_is_refused)
{
    // The limit is rounded down to a power of two.
    const std::vector<std::byte> data = pattern(100);
    const std::vector<std::byte> frame = unsized_frame(data, 20);
    EXPECT_EQ(decoded(zstd_decoder(std::uint64_t{1} << 20), frame, 100), data);
    EXPECT_EQ(decoded(zstd_decoder(no_window_limit), frame, 100), data);
    EXPECT_TRUE(throws<openxisf::limit_error>(
        errc::zstd_window_too_large, [&] { (void)decoded(zstd_decoder((std::uint64_t{1} << 20) - 1), frame, 100); }));
    // Below the smallest window of 1 KiB, the limit is 1 KiB.
    EXPECT_EQ(decoded(zstd_decoder(1), unsized_frame(data, 10), 100), data);
}

TEST(zstd_codec, a_frame_that_declares_its_size_needs_no_window)
{
    // A frame that is not single-segment, with a window of 1 GiB and a content size of 100 in 4 bytes, decodes into the
    // output directly.
    const std::vector<std::byte> data = pattern(100);
    const std::vector<std::byte> frame =
        joined(joined(magic_number(), byte_values({0x80, (30U - 10U) << 3U, 100, 0, 0, 0})), raw_block(data));
    EXPECT_EQ(decoded(zstd_decoder(1024), frame, 100), data);
}

TEST(zstd_codec, decodes_unsized_frames_of_rle_and_compressed_blocks)
{
    // 1000 bytes of 'a' in an RLE block, then "xyz" as the literals of a compressed block, in a frame that declares no
    // content size and a window of 1 KiB.
    const std::vector<std::byte> frame =
        joined(joined(joined(magic_number(), byte_values({0x00, 0x00})), rle_block(std::byte{'a'}, 1000)),
               literals_block(bytes("xyz")));
    const std::vector<std::byte> data = joined(std::vector<std::byte>(1000, std::byte{'a'}), bytes("xyz"));
    expect_exact_sizes(zstd_decoder(no_window_limit), frame, data);
    EXPECT_EQ(decoded(zstd_decoder(1024), frame, data.size()), data);
}

TEST(zstd_codec, the_frames_of_the_library_need_no_window)
{
    // zstd_compress() writes frames that declare their content size, so the window limit, which applies only to
    // frames that do not, never refuses them. Here the data repeat after 64 KiB, far beyond the smallest window.
    constexpr std::size_t period = std::size_t{64} << 10;
    openxisf::detail::xoshiro256starstar random({7, 8, 9, 10});
    std::vector<std::byte> data(4 * period);
    for (std::size_t i = 0; i < period; ++i) {
        data[i] = static_cast<std::byte>(random() & 0xFFU);
    }
    for (std::size_t i = period; i < data.size(); ++i) {
        data[i] = data[i - period];
    }
    std::vector<std::byte> frame;
    zstd_compress(data, 19, frame);
    ASSERT_LT(frame.size(), data.size() / 2);
    EXPECT_EQ(decoded(zstd_decoder(1024), frame, data.size()), data);
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoders that keep their state from one subblock to the next

// Inputs of mixed sizes and contents that an encoder compresses one after another: empty, a byte, repeated and random
// data, more than the 64 KiB window of LZ4, and an input twice in a row, which an encoder that kept anything of the
// previous input would compress better the second time.
std::vector<std::vector<std::byte>> mixed_inputs()
{
    openxisf::detail::xoshiro256starstar random({11, 12, 13, 14});
    const auto random_bytes = [&random](std::size_t size) {
        std::vector<std::byte> data(size);
        for (std::byte& value : data) {
            value = static_cast<std::byte>(random() & 0xFFU);
        }
        return data;
    };
    const std::vector<std::byte> repeated = joined(random_bytes(3000), random_bytes(3000));
    std::vector<std::vector<std::byte>> inputs{
        random_bytes(100'000),
        pattern(1),
        {},
        pattern(70'000),
        random_bytes(17),
        repeated,
        repeated,
        pattern(300),
        random_bytes(65'536),
        pattern(4096),
        random_bytes(1),
        joined(pattern(5000), random_bytes(5000)),
        pattern(200),
    };
    // Repeated data, then the same data again in an input of its own.
    inputs.push_back(joined(inputs[3], inputs[3]));
    inputs.push_back(inputs[3]);
    return inputs;
}

using encoding = std::function<void(std::span<const std::byte> input, std::vector<std::byte>& output)>;

// An encoder, kept for every input, gives each the bytes that a new state of its library gives it at its level.
void expect_bytes_of_a_new_state(const encoding& encoder, const encoding& new_state, int level)
{
    std::size_t index = 0;
    for (const std::vector<std::byte>& input : mixed_inputs()) {
        std::vector<std::byte> output = bytes("kept");
        encoder(input, output);
        std::vector<std::byte> expected = bytes("kept");
        new_state(input, expected);
        EXPECT_TRUE(output == expected) << "level " << level << ", input " << index;
        ++index;
    }
}

// The library gets a pointer to this byte for an empty input, never a null pointer.
const char* chars_of(std::span<const std::byte> input, const std::byte& placeholder)
{
    return reinterpret_cast<const char*>(input.empty() ? &placeholder : input.data());
}

// The size of an input as a length of zlib, uLong: unsigned long, the type of std::size_t on Linux and macOS, where
// GCC calls a cast to it useless, and narrower on Windows, where the conversion needs one.
template <typename Length> Length zlib_length(std::size_t size)
{
    if constexpr (std::is_same_v<Length, std::size_t>) {
        return size;
    } else {
        return static_cast<Length>(size);
    }
}

// What 1.0.0 wrote for each subblock: zlib's compress2(), which makes a new stream with deflateInit().
TEST(zlib_codec, an_encoder_gives_each_input_the_bytes_of_a_new_stream)
{
    for (int level = 0; level <= 9; ++level) {
        openxisf::detail::zlib_encoder encoder(level);
        expect_bytes_of_a_new_state(
            [&encoder](std::span<const std::byte> input, std::vector<std::byte>& output) {
                encoder.compress(input, output);
            },
            [level](std::span<const std::byte> input, std::vector<std::byte>& output) {
                uLongf size = compressBound(zlib_length<uLong>(input.size()));
                std::vector<std::byte> stream(size);
                const std::byte placeholder{};
                ASSERT_EQ(compress2(reinterpret_cast<Bytef*>(stream.data()), &size,
                                    reinterpret_cast<const Bytef*>(chars_of(input, placeholder)),
                                    zlib_length<uLong>(input.size()), level),
                          Z_OK);
                output.insert(output.end(), stream.begin(), stream.begin() + static_cast<std::ptrdiff_t>(size));
            },
            level);
    }
}

// What 1.0.0 wrote for each subblock: LZ4_compress_HC(), which clears a new state.
TEST(lz4_codec, an_encoder_gives_each_input_the_bytes_of_a_new_state)
{
    for (int level = 1; level <= 12; ++level) {
        openxisf::detail::lz4hc_encoder encoder(level);
        expect_bytes_of_a_new_state([&encoder](std::span<const std::byte> input,
                                               std::vector<std::byte>& output) { encoder.compress(input, output); },
                                    [level](std::span<const std::byte> input, std::vector<std::byte>& output) {
                                        const int size = static_cast<int>(input.size());
                                        std::vector<std::byte> block(static_cast<std::size_t>(LZ4_compressBound(size)));
                                        const std::byte placeholder{};
                                        const int written = LZ4_compress_HC(chars_of(input, placeholder),
                                                                            reinterpret_cast<char*>(block.data()), size,
                                                                            static_cast<int>(block.size()), level);
                                        ASSERT_GT(written, 0);
                                        output.insert(output.end(), block.begin(), block.begin() + written);
                                    },
                                    level);
    }
}

// What 1.0.0 wrote for each subblock: ZSTD_compress2() with a new context at the level.
TEST(zstd_codec, an_encoder_gives_each_input_the_bytes_of_a_new_context)
{
    for (int level = 1; level <= 22; ++level) {
        openxisf::detail::zstd_encoder encoder(level);
        expect_bytes_of_a_new_state(
            [&encoder](std::span<const std::byte> input, std::vector<std::byte>& output) {
                encoder.compress(input, output);
            },
            [level](std::span<const std::byte> input, std::vector<std::byte>& output) {
                ZSTD_CCtx* const context = ZSTD_createCCtx();
                ASSERT_NE(context, nullptr);
                std::vector<std::byte> frame(ZSTD_compressBound(input.size()));
                const std::size_t set = ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel, level);
                const std::size_t written =
                    ZSTD_compress2(context, frame.data(), frame.size(), input.data(), input.size());
                (void)ZSTD_freeCCtx(context);
                ASSERT_EQ(ZSTD_isError(set), 0U);
                ASSERT_EQ(ZSTD_isError(written), 0U);
                output.insert(output.end(), frame.begin(), frame.begin() + static_cast<std::ptrdiff_t>(written));
            },
            level);
    }
}

} // namespace
