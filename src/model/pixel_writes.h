// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/editor.h>

#include "container/data_block.h"
#include "io/thread_safe_device.h"
#include "model/pixel_layout.h"
#include "model/unit.h"

#include <cstddef>
#include <span>
#include <vector>

// Writing the pixel data of the images of a unit in place, through the device that holds the unit: a region of an
// image, in the byte order of its block (spec §10.4) and in the storage model of the image (spec §8.5.3), and the
// digests of the blocks written (spec §10.5).

namespace openxisf::detail {

/// For each of blocks, whether its stored bytes share a byte with those of another attached block, which writing it
/// would change too. Elements that a Reference makes share a block share one entry of blocks, which is not another
/// block.
[[nodiscard]] std::vector<bool> overlapping_blocks(const std::vector<data_block>& blocks);

/// Throws unsupported_error with errc::not_editable, and the context of the block, unless the block of the image at
/// index of opened, which is valid, can be written in place: available, attached, not compressed, sharing no byte with
/// another block (overlapping, as overlapping_blocks() gives it), and, when it has a checksum, with a digest that can
/// be rewritten in place (block_descriptor::digest_offset).
void check_editable(const unit& opened, std::size_t index, const std::vector<bool>& overlapping);

/// The layout of a write of data into the image at index of opened with options, after the checks of the region and the
/// pitch of options, and of the size of data: exactly the bytes of the region, or with a pitch at least those that its
/// rows span. Throws usage_error otherwise, and when there is no image at index.
[[nodiscard]] region_layout write_layout(const unit& opened, std::size_t index, std::span<const std::byte> data,
                                         const pixel_write_options& options);

/// Writes data, the samples of layout (write_layout()), into the block of the image at index of opened, which
/// check_editable() accepted, through device: run by run, in the storage model of the image, each piece of a run that
/// a row of data holds in one write. The pieces go straight from data when the samples keep their byte order and
/// storage model, and otherwise through a staging buffer of piece_size bytes, or a buffer of the region in the storage
/// model of the image, within limits::max_allocation, from which the runs are written as they are. options.progress
/// counts the bytes of the region, and can cancel the write between runs. What device throws passes through.
void write_pixels(const unit& opened, const thread_safe_device& device, std::size_t index,
                  std::span<const std::byte> data, const region_layout& layout, const pixel_write_options& options,
                  std::size_t piece_size = default_piece_size);

/// Computes the digest of an attached block whose checksum has a located digest (block_descriptor::digest_offset) from
/// its stored bytes, read through device in pieces of piece_size bytes, writes it in place of the old one through
/// device, in lowercase hexadecimal digits, and gives it to descriptor. What device throws passes through.
void refresh_digest(const thread_safe_device& device, block_descriptor& descriptor,
                    std::size_t piece_size = default_piece_size);

/// Throws integrity_error with errc::checksum_mismatch, and the context of block, when the stored bytes of block, an
/// attached block that check_editable() accepted, read through device in pieces of piece_size bytes, do not have the
/// digest of its checksum. Does nothing for a block without a checksum. What device throws passes through.
void verify_digest(const thread_safe_device& device, const data_block& block,
                   std::size_t piece_size = default_piece_size);

} // namespace openxisf::detail
