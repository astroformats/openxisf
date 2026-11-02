// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/image.h>
#include <openxisf/reader.h>

#include "container/data_block.h"
#include "model/unit.h"

#include <cstddef>
#include <span>
#include <vector>

// Reading the pixel data of the images of a unit: the data of their block, in native byte order (spec §10.4), in the
// storage model asked for (spec §8.5.3).

namespace openxisf::detail {

/// The image at index of an opened unit. Throws usage_error when there is none.
[[nodiscard]] const image_info& image_at(const unit& opened, std::size_t index);

/// How the pixel data of the image at index are stored, as reader::block() describes them, with the exceptions it
/// documents: the block is located first, as a read locates it, and its error is thrown when it is unavailable.
[[nodiscard]] block_info block_of(const unit& opened, std::size_t index);

/// The pixel data of the image at index, as reader::read_pixels() reads them, with the exceptions it documents. An
/// attachment is read in pieces of piece_size bytes when there is a progress function, and the runs of a region that
/// lie close together through a staging buffer of piece_size bytes.
[[nodiscard]] std::vector<std::byte> read_pixels(const unit& opened, std::size_t index,
                                                 const pixel_read_options& options,
                                                 std::size_t piece_size = default_piece_size);

/// Reads the pixel data of the image at index into destination, which must have exactly the size of the pixel data,
/// or of those of the region of options, or with a pitch at least the bytes that the rows of the region span.
void read_pixels(const unit& opened, std::size_t index, std::span<std::byte> destination,
                 const pixel_read_options& options, std::size_t piece_size = default_piece_size);

/// The image at index, for a typed read. Throws usage_error when there is none, or when it has another sample format.
[[nodiscard]] const image_info& typed_image_at(const unit& opened, std::size_t index, sample_format format);

/// The number of samples of the image at index, for a typed read that allocates them. Throws what typed_image_at()
/// throws; the error of its data block when that is unavailable, as a read throws it, so that nothing is allocated for
/// a block that cannot be read; and limit_error with errc::allocation_too_large when its pixel data are larger than
/// limits.max_allocation.
[[nodiscard]] std::size_t typed_sample_count(const unit& opened, std::size_t index, sample_format format);

/// The number of samples that a typed read of the image at index with options allocates: those of the region of
/// options, or with a pitch those that its rows span. Throws what typed_sample_count() throws, and usage_error when the
/// region or the pitch do not fit the image.
[[nodiscard]] std::size_t typed_sample_count(const unit& opened, std::size_t index, sample_format format,
                                             const pixel_read_options& options);

} // namespace openxisf::detail
