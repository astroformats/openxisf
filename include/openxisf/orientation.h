// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/export.h>
#include <openxisf/types.h>

#include <cstddef>
#include <span>
#include <vector>

/// @file
/// The orientation of images (spec §11.5.2), applied to show them.
///
/// The specification lets a decoder apply the orientation of an image to show it, and forbids applying it before
/// processing that depends on the physical disposition of the pixels, such as calibration. These functions are meant
/// for the first use only: the reader never applies an orientation, and pixel data turned by them must not be written
/// back as the image.

namespace openxisf {

/// The geometry of a two-dimensional image once turned to an orientation: its width and height are exchanged by the
/// orientations that rotate it by 90 degrees.
/// @throws usage_error when size is not two-dimensional.
[[nodiscard]] OPENXISF_API geometry oriented_geometry(const geometry& size, orientation turn);

/// The pixel data of a two-dimensional image turned to an orientation, in the same storage model and sample format:
/// the pixels of an image of oriented_geometry(). pixels holds the samples of an image of geometry size, of
/// sample_size bytes each, in the storage model storage.
/// @throws usage_error when size is not two-dimensional, sample_size is zero, or pixels does not hold the samples of
///         size.
[[nodiscard]] OPENXISF_API std::vector<std::byte> orient_pixels(std::span<const std::byte> pixels, const geometry& size,
                                                                std::size_t sample_size, pixel_storage storage,
                                                                orientation turn);

} // namespace openxisf
