// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Byte shuffling (spec §10.6.3). A block of n items of s bytes each is stored as s runs of n bytes: first byte 0 of
// every item, then byte 1 of every item, and so on. The bytes after the last complete item follow unchanged.

namespace openxisf::detail {

/// The code that shuffles and unshuffles. Both give the same bytes.
enum class shuffle_kernel : std::uint8_t
{
    /// Portable code, one byte at a time.
    scalar,
    /// The SIMD kernels of the build for items of 2, 4 and 8 bytes: SSE2 on x86-64 when OPENXISF_ENABLE_SIMD is on. The
    /// scalar code for other item sizes, and on other processors.
    simd,
};

/// True when the build has SIMD kernels.
[[nodiscard]] bool has_simd_shuffle() noexcept;

/// Writes the shuffled bytes of input to output, which has the same size and does not overlap it. An item size of 0 or
/// 1, or one larger than the input, copies the input unchanged.
void shuffle_bytes(std::span<const std::byte> input, std::span<std::byte> output, std::size_t item_size,
                   shuffle_kernel kernel = shuffle_kernel::simd);

/// Writes part of the shuffled bytes of input to output: the output.size() bytes that start at offset in the shuffled
/// block, which must lie within it. It computes them from input directly, so that a subblock of a shuffled block needs
/// no buffer of the whole block.
void shuffle_part(std::span<const std::byte> input, std::size_t item_size, std::size_t offset,
                  std::span<std::byte> output, shuffle_kernel kernel = shuffle_kernel::simd);

/// Reverses shuffle_bytes(): writes the unshuffled bytes of input to output, under the same conditions.
void unshuffle_bytes(std::span<const std::byte> input, std::span<std::byte> output, std::size_t item_size,
                     shuffle_kernel kernel = shuffle_kernel::simd);

} // namespace openxisf::detail
