// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/error.h>
#include <openxisf/limits.h>
#include <openxisf/reader.h>

#include "container/block_attributes.h"
#include "core/diagnostic_log.h"
#include "io/thread_safe_source.h"
#include "model/outline.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The data blocks of a unit (spec §10): where each one is stored and how, from the attributes of the element that
// serializes it, and the bytes it holds.

namespace openxisf::detail {

/// Where and how a block is stored.
struct block_descriptor
{
    block_location location{};
    byte_order order = byte_order::little;
    std::optional<block_checksum> checksum{};
    std::optional<block_compression> compression{};
    /// For an inline or embedded block, its bytes as stored: decoded from their text, and verified against the checksum
    /// when the unit was opened.
    std::vector<std::byte> data{};
    /// For an external block, the file that holds it, at location.position and location.size, which
    /// locate_external_blocks() sets. The blocks of a file share it.
    std::shared_ptr<const thread_safe_source> external{};
};

/// A data block, and the element that serializes it.
struct data_block
{
    /// The element in the outline of the unit.
    std::size_t element = no_element;
    /// The path of the element, for messages.
    std::string path{};
    /// How the block is stored, when it is available.
    std::optional<block_descriptor> descriptor{};
    /// Why the block is unavailable, when it is not: the error diagnostic recorded when the unit was opened, and the
    /// class of the exception of the resolver or source that failed, which a read of the block throws again.
    std::optional<diagnostic> problem{};
    error_origin origin{};
};

/// What the rules for data blocks need to know about the unit.
struct block_context
{
    unit_storage storage = unit_storage::monolithic;
    /// The first byte after the header of a monolithic file, where attached blocks can start.
    std::uint64_t header_end = 0;
    /// The size of the source, where attached blocks must end.
    std::uint64_t source_size = 0;
    /// True when the unit has an XML signature (spec §9.5).
    bool signed_unit = false;
};

/// The data blocks of a unit, in document order: one for each Image, Thumbnail, Property, ICCProfile or Cell element of
/// outline with a location attribute. Every problem is recorded in log, and a block that cannot be read is unavailable:
/// - a malformed attribute, a missing or repeated Data element of an embedded block, text that is not valid Base64 or
///   hexadecimal data, an attachment beyond the end of the file or inside the header, an attached block in a header
///   file or an external block in a monolithic file, subblocks whose sizes do not add up, and an inline or embedded
///   block that fails its checksum are errors;
/// - an unknown codec or checksum algorithm is an error too;
/// - Base64 data without padding, uppercase hexadecimal digits, an empty block other than an inline one, a byteOrder
///   attribute on an ICCProfile element, an inline block of an Image or Thumbnail element, child elements of another
///   element with an inline block, block attributes of an embedded block on the element instead of its Data element,
///   and block attributes or Data elements that have no block to describe are warnings, and so is an attached or
///   external block of a signed unit without a checksum (spec §10.5).
///
/// An external block of a header file is described without its file, its place in the file, and the check of its
/// subblocks, which locate_external_blocks() adds.
[[nodiscard]] std::vector<data_block> describe_blocks(const unit_outline& outline, const block_context& context,
                                                      diagnostic_log& log);

/// Throws the exception of throw_unit_error() for an unavailable block, as a read of it does, so that a caller can
/// refuse it before it allocates anything for its data.
void check_available(const data_block& block);

/// The bytes of a block as stored: read from source, or from its external file, and verified against the checksum, but
/// not decompressed. An attached or external block is verified before it is returned, and an inline or embedded one
/// was verified when the unit was opened.
///
/// Throws the exception of throw_unit_error() for an unavailable block; integrity_error with errc::checksum_mismatch;
/// limit_error with errc::allocation_too_large when an attached or external block is larger than
/// limits.max_allocation; and what the source throws.
[[nodiscard]] std::vector<std::byte> read_stored_block(const thread_safe_source& source, const data_block& block,
                                                       const limits& limits);

/// The size of the pieces in which an attached or external block is read when the progress of the read is
/// reported.
inline constexpr std::size_t default_piece_size = std::size_t{4} << 20;

/// Called while the data of a block are obtained, with the bytes processed so far out of work_size() of its
/// descriptor: first the bytes read, piece by piece, then the bytes decompressed, subblock by subblock. It may throw to
/// stop the read; the exception passes through.
using block_progress = std::function<void(std::uint64_t done)>;

/// The data of a block: its stored bytes, verified, then decompressed and unshuffled when the block is compressed (spec
/// §10.6). The byte order stays that of the block. With a progress function, an attached or external block is read in
/// pieces of piece_size bytes.
///
/// Throws what read_stored_block() throws, and what decompress_block() throws, with the element of the block as
/// context: integrity_error with errc::corrupt_compressed_data, limit_error with errc::allocation_too_large when the
/// uncompressed size is above limits.max_allocation or with errc::zstd_window_too_large, and unsupported_error with
/// errc::codec_failure. What progress throws passes through.
[[nodiscard]] std::vector<std::byte> read_block(const thread_safe_source& source, const data_block& block,
                                                const limits& limits, const block_progress& progress = {},
                                                std::size_t piece_size = default_piece_size);

/// Reads the data of a block, as read_block() returns them, into destination, which must have their size (data_size()
/// of its descriptor): the stored bytes of a block that is not compressed are read straight into it, and a compressed
/// block is decompressed into it. Throws what read_block() throws, and usage_error with errc::invalid_argument when
/// destination has another size. When it throws, destination may hold part of the data, but never bytes that failed
/// their checksum.
void read_block_into(const thread_safe_source& source, const data_block& block, const limits& limits,
                     std::span<std::byte> destination, const block_progress& progress = {},
                     std::size_t piece_size = default_piece_size);

/// The data of an available block, as read_block() returns them, with errors that have no context, so that the caller
/// can give them its own. What the source throws passes through.
[[nodiscard]] std::vector<std::byte> read_block_data(const thread_safe_source& source,
                                                     const block_descriptor& descriptor, const limits& limits,
                                                     const block_progress& progress = {},
                                                     std::size_t piece_size = default_piece_size);

/// The size of a block as stored: of the attachment or the external block, or of the decoded bytes of an inline or
/// embedded block.
[[nodiscard]] std::uint64_t stored_size(const block_descriptor& descriptor) noexcept;

/// The size of the data of a block, which read_block() returns: the uncompressed size of a compressed block.
[[nodiscard]] std::uint64_t data_size(const block_descriptor& descriptor) noexcept;

/// The bytes that read_block() processes, as its progress counts them: the stored size of the block, plus its
/// uncompressed size when it is compressed.
[[nodiscard]] std::uint64_t work_size(const block_descriptor& descriptor) noexcept;

} // namespace openxisf::detail
