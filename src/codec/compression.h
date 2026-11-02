// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

// How a data block is compressed (spec §10.6): the codec, the item size of byte shuffling, and the subblocks.

namespace openxisf::detail {

/// The compression codecs of spec §10.6.4 to §10.6.11.
enum class compression_codec : std::uint8_t
{
    zlib,
    lz4,
    lz4hc,
    zstd,
};

/// The name of a codec in messages.
[[nodiscard]] std::string_view codec_name(compression_codec codec) noexcept;

/// A compression subblock (spec §10.6).
struct subblock
{
    std::uint64_t compressed_size = 0;
    std::uint64_t uncompressed_size = 0;

    friend bool operator==(const subblock&, const subblock&) = default;
};

/// How a block is compressed: the value of a compression attribute, with the subblocks of its block.
struct block_compression
{
    compression_codec codec = compression_codec::zlib;
    std::uint64_t uncompressed_size = 0;
    /// The item size of byte shuffling (spec §10.6.3) for the codecs with byte shuffling, and 0 for the others.
    std::uint64_t item_size = 0;
    /// The subblocks in storage order, or none when the block is compressed as a whole.
    std::vector<subblock> subblocks{};

    friend bool operator==(const block_compression&, const block_compression&) = default;
};

} // namespace openxisf::detail
