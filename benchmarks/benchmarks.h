// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <vector>

// The benchmarks register themselves from main(), so that their names are readable (compress/zstd+sh4) and no macro
// defines statics. The numbers are what performance decisions rest on, so the data look like an astronomical image: a
// smooth background with a little noise.

namespace openxisf::bench {

/// The sample formats of the synthetic images.
enum class samples
{
    uint16,
    float32,
};

/// A synthetic sky of width × height pixels and the given channels, planar, in little-endian samples: a background
/// between about 0.05 and 0.15 with noise of about 0.001, a little brighter in each channel. The same arguments give
/// the same bytes.
[[nodiscard]] std::vector<std::byte> sky_image(std::size_t width, std::size_t height, std::size_t channels,
                                               samples format);

/// The 1024 × 1024 sky of one channel, built once: 4 MiB of Float32 samples, or 2 MiB of UInt16 samples.
[[nodiscard]] const std::vector<std::byte>& small_sky(samples format);

/// The data block transforms: each codec with and without byte shuffling, byte shuffling alone, and the checksum
/// algorithms.
void register_block_benchmarks();

/// Whole units: the header of a unit with many properties and keywords, pixel reads against raw reads of the same
/// bytes, storage conversion, checksummed reads, a 100 MiB frame read and written in subblocks, and regions of
/// interest: tiles read from memory, from files and around the cache of the system, the tile of a stacking of 32
/// frames, and an editor filling a frame tile by tile.
void register_unit_benchmarks();

} // namespace openxisf::bench
