// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/limits.h>

#include "codec/compression.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

// Compressed data blocks as a whole (spec §10.6): byte shuffling over the whole block, and its division into subblocks,
// each compressed on its own and stored after the previous one. The codecs do one subblock at a time (codec/codecs.h).

namespace openxisf::detail {

/// The level of codec for an abstract compression level (spec §11.4.2): the default level of the codec for 0, and for 1
/// (fastest) to 100 (smallest) a level that grows linearly over the range of the codec: 0 to 9 for zlib, 1 to 12 for
/// LZ4HC and 1 to 22 for Zstandard. LZ4 has no levels, and gets 0. Throws usage_error with errc::invalid_argument for a
/// level outside 0 to 100.
[[nodiscard]] int codec_level(compression_codec codec, int level);

/// The abstract level that gives the default level of codec, the level of codec_level(codec, 0): 67 for zlib, 73 for
/// LZ4HC and 10 for Zstandard. 0 for LZ4, which has no levels.
[[nodiscard]] int default_abstract_level(compression_codec codec) noexcept;

/// The subblocks of a compressed block of stored_size bytes: those of compression, or a single one for the whole block
/// when it has none. A subblock whose compressed and uncompressed sizes are equal holds its data as they are, not
/// compressed: the specification does not say so, but PixInsight writes such subblocks when they do not compress.
/// Throws invalid_data_error with errc::invalid_subblocks when the sizes do not add up to the stored and the
/// uncompressed size of the block, when a compressed subblock is larger than the decoder of the codec takes (LZ4
/// decodes at most 2^31 - 1 bytes, compressed or not), or when it declares more bytes than the codec can decode from
/// its compressed size: 1032 for each byte with zlib, 255 with LZ4 and LZ4HC, and 32768 with Zstandard. So a few
/// stored bytes cannot make a read allocate more than that.
[[nodiscard]] std::vector<subblock> subblocks_of(const block_compression& compression, std::uint64_t stored_size);

/// Checks the subblocks of a compressed block as subblocks_of() does, without a copy of them.
void check_subblocks(const block_compression& compression, std::uint64_t stored_size);

/// Called after each subblock is decompressed with the uncompressed bytes done so far, on the calling thread. It may
/// throw to stop the decompression; the exception passes through.
using subblock_progress = std::function<void(std::uint64_t done)>;

/// The data of a block, from its stored bytes: each subblock decompressed, or copied when it is stored as it is, into
/// its place, then the byte shuffling reversed. With oneTBB, the subblocks are decompressed in parallel, in batches of
/// as many as the threads of the task arena of the calling thread, and the progress of a batch follows it. Throws what
/// subblocks_of() throws; limit_error with errc::allocation_too_large when the uncompressed size is above
/// limits.max_allocation, or with errc::zstd_window_too_large; integrity_error with errc::corrupt_compressed_data when
/// a subblock does not decode to its uncompressed size, which for a Zstandard frame that declares another size, or is
/// not a single frame, is found before the data are allocated; and what the codec adapters throw for other failures.
[[nodiscard]] std::vector<std::byte> decompress_block(std::span<const std::byte> stored,
                                                      const block_compression& compression, const limits& limits,
                                                      const subblock_progress& progress = {});

/// Decompresses a block as decompress_block() does, into destination, which must have its uncompressed size: the
/// subblocks go straight into destination when the block is not shuffled, and into a buffer of the size of the block,
/// which is then unshuffled into destination, when it is. Throws what decompress_block() throws, and usage_error with
/// errc::invalid_argument when destination has another size. When it throws, destination may hold part of the data.
void decompress_block_into(std::span<const std::byte> stored, const block_compression& compression,
                           const limits& limits, std::span<std::byte> destination,
                           const subblock_progress& progress = {});

/// How compress_block() compresses a block.
struct compression_options
{
    compression_codec codec = compression_codec::zstd;
    /// The item size of byte shuffling, or 0 for a codec without byte shuffling.
    std::uint64_t item_size = 0;
    /// The abstract compression level of codec_level().
    int level = 0;
    /// The largest subblock, or 0 for the most that the encoder of the codec takes (see subblock_size()).
    std::uint64_t max_subblock_size = 0;
};

/// The size of the subblocks that compress_block() makes: options.max_subblock_size, at most what the encoder of the
/// codec takes at once. That is 2^32 - 2^24 bytes for zlib, so that a compressed subblock stays below 2^32 bytes too,
/// since zlib counts in 32 bits (spec §10.6); the most that LZ4 compresses at once for LZ4 and LZ4HC; and no limit for
/// Zstandard.
[[nodiscard]] std::uint64_t subblock_size(const compression_options& options) noexcept;

/// A compressed block: its stored bytes, and how they are compressed.
struct compressed_block
{
    std::vector<std::byte> data{};
    block_compression compression{};
};

/// Called by compress_subblocks() with each subblock once it is compressed: its stored bytes and its sizes. It may
/// throw to stop the compression; the exception passes through.
using subblock_store = std::function<void(std::span<const std::byte> stored, const subblock& sizes)>;

/// Compresses a block subblock by subblock: the block is shuffled as a whole and divided into subblocks of
/// subblock_size() bytes (the last one shorter), and each subblock is shuffled from data, compressed and given to
/// store, in order, on the calling thread. Only the subblocks in the making are held: one at a time without oneTBB, and
/// with it a batch of as many as the threads of the task arena, compressed in parallel, of about 1 GiB of data at most,
/// which takes up to about twice that with the shuffled copies and the output of the codec, besides a state of the
/// codec for each subblock of a batch, which at the highest Zstandard levels is many times a subblock of a few MiB.
/// Each state is made once for the block and kept from one batch to the next. A subblock that does not get smaller is
/// stored as it is, with equal sizes (see subblocks_of()), as PixInsight stores it: PixInsight takes any subblock that
/// is not smaller than its data for data stored as they are, so it would misread codec output that is. Returns how the
/// block is compressed, with every subblock listed, even a single one. Throws usage_error with errc::invalid_argument
/// for an invalid level, what the codec adapters throw, and what store throws.
block_compression compress_subblocks(std::span<const std::byte> data, const compression_options& options,
                                     const subblock_store& store);

/// Compresses a block with compress_subblocks() into memory. The subblocks are listed only when there are several. A
/// block that does not compress at all comes back with its stored size equal to its uncompressed size, shuffled.
[[nodiscard]] compressed_block compress_block(std::span<const std::byte> data, const compression_options& options);

} // namespace openxisf::detail
