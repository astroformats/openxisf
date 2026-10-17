// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include "core/scratch_buffer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

// The adapters of the compression libraries (spec §10.6.3 to §10.6.10), one subblock at a time. A decoder fills its
// output exactly and consumes all of its input, or throws integrity_error with errc::corrupt_compressed_data; it never
// returns part of a subblock. An encoder appends to its output. Out of memory is std::bad_alloc; a library that fails
// for any other reason than the data, such as a version mismatch, throws unsupported_error with errc::codec_failure.
// Messages have no context; the caller adds it.
//
// The encoder classes keep the state of their library from one input to the next, reset for each, so that a thread
// that compresses many subblocks does not allocate and initialize a state for each; an input gives the bytes that a new
// state gives it. The compress functions make a new state for their input.

namespace openxisf::detail {

/// The default levels of the libraries, which the abstract level 0 selects.
inline constexpr int zlib_default_level = 6;
inline constexpr int lz4hc_default_level = 9;
inline constexpr int zstd_default_level = 3;

/// The largest input that LZ4 compresses, LZ4_MAX_INPUT_SIZE.
inline constexpr std::uint64_t lz4_max_input_size = 0x7E00'0000;

/// The largest compressed or decompressed subblock that the LZ4 decoder takes: the largest int.
inline constexpr std::uint64_t lz4_max_decoded_size = 0x7FFF'FFFF;

/// The most that zlib takes or gives in one call, which its 32-bit counters can hold.
inline constexpr std::size_t zlib_max_piece = 0xFFFF'FFFF;

/// Decompresses a zlib stream (RFC 1950), without a preset dictionary. Any size works: zlib gets the input and the
/// output in pieces of at most max_piece bytes, which only tests set lower.
void zlib_decompress(std::span<const std::byte> input, std::span<std::byte> output,
                     std::size_t max_piece = zlib_max_piece);

/// Compresses input into a zlib stream at a zlib level from 0 to 9, in pieces like zlib_decompress().
void zlib_compress(std::span<const std::byte> input, int level, std::vector<std::byte>& output,
                   std::size_t max_piece = zlib_max_piece);

/// Compresses inputs one after another into zlib streams, as zlib_compress() does, with one deflate state, which
/// deflateReset() makes that of a new stream for each.
class zlib_encoder
{
public:
    explicit zlib_encoder(int level, std::size_t max_piece = zlib_max_piece);
    zlib_encoder(const zlib_encoder&) = delete;
    zlib_encoder& operator=(const zlib_encoder&) = delete;
    ~zlib_encoder();

    void compress(std::span<const std::byte> input, std::vector<std::byte>& output);

private:
    // The z_stream, at an address that does not change, which zlib requires.
    struct state;
    std::unique_ptr<state> state_;
    std::size_t max_piece_;
};

/// Decompresses an LZ4 block (the block format, without a frame), as written by LZ4 and LZ4HC. Both sizes are at most
/// lz4_max_decoded_size.
void lz4_decompress(std::span<const std::byte> input, std::span<std::byte> output);

/// Compresses input, at most lz4_max_input_size bytes, into an LZ4 block.
void lz4_compress(std::span<const std::byte> input, std::vector<std::byte>& output);

/// Compresses input, at most lz4_max_input_size bytes, into an LZ4 block with LZ4HC at a level from 1 to 12.
void lz4hc_compress(std::span<const std::byte> input, int level, std::vector<std::byte>& output);

/// Compresses inputs one after another into LZ4 blocks, as lz4hc_compress() does, with one LZ4HC stream of about
/// 256 KiB, allocated here, so that a failure is std::bad_alloc, and prepared with LZ4_resetStreamHC_fast() for each
/// input, which unlike a new state is not cleared.
class lz4hc_encoder
{
public:
    explicit lz4hc_encoder(int level);

    void compress(std::span<const std::byte> input, std::vector<std::byte>& output);

private:
    scratch_buffer stream_;
    int level_;
};

/// Decompresses a single Zstandard frame (RFC 8878). A frame that does not declare its content size is decoded through
/// a window buffer, whose size is limited to the largest power of two not above max_window (at least 1 KiB; 0 means
/// the largest that Zstandard supports); a larger window is a limit_error with errc::zstd_window_too_large. A frame
/// that declares its content size is decoded straight into output and needs no window buffer.
void zstd_decompress(std::span<const std::byte> input, std::span<std::byte> output, std::uint64_t max_window);

/// Checks what zstd_decompress() checks before it decodes: that input is exactly one Zstandard frame, which declares a
/// content size of expected bytes or none, so that a block can be checked before the memory of its data is allocated.
/// Throws integrity_error with errc::corrupt_compressed_data.
void zstd_check_frame(std::span<const std::byte> input, std::uint64_t expected);

/// True when input starts with the header of a Zstandard frame that does not declare its content size, which
/// zstd_decompress() decodes through a window buffer. False for any other input, which it decodes in place or refuses.
[[nodiscard]] bool zstd_needs_window(std::span<const std::byte> input) noexcept;

/// Compresses input into a single Zstandard frame, which declares its content size, at a level from 1 to 22.
void zstd_compress(std::span<const std::byte> input, int level, std::vector<std::byte>& output);

/// Compresses inputs one after another into Zstandard frames, as zstd_compress() does, with one compression context.
class zstd_encoder
{
public:
    explicit zstd_encoder(int level);
    zstd_encoder(const zstd_encoder&) = delete;
    zstd_encoder& operator=(const zstd_encoder&) = delete;
    ~zstd_encoder();

    void compress(std::span<const std::byte> input, std::vector<std::byte>& output);

private:
    // The ZSTD_CCtx, which zstd.h declares.
    struct state;
    std::unique_ptr<state> state_;
};

} // namespace openxisf::detail
