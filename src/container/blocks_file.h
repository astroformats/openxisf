// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/limits.h>

#include "io/thread_safe_source.h"

#include <cstdint>
#include <string_view>
#include <unordered_set>
#include <vector>

// Data blocks files (spec §9.4): a signature, then a block index, a linked list of nodes whose elements place the data
// blocks in the file. Integers are little-endian.

namespace openxisf::detail {

/// The first bytes of a data blocks file.
inline constexpr std::string_view blocks_file_signature = "XISB0100";

/// The signature and the reserved field of a data blocks file, after which its first index node starts.
inline constexpr std::uint64_t blocks_file_header_size = 16;

/// A block index node starts with the number of its elements, a reserved field and the position of the next node.
inline constexpr std::uint64_t index_node_header_size = 16;

/// A block index element: identifier, position, length, uncompressed length and a reserved field.
inline constexpr std::uint64_t index_element_size = 40;

/// A block index element.
struct index_element
{
    std::uint64_t id = 0;
    /// The position of the block in the file, or 0 for a free element, which points to no block.
    std::uint64_t position = 0;
    std::uint64_t length = 0;
    /// The length of the block before it was compressed, or 0 when it is not compressed.
    std::uint64_t uncompressed_length = 0;

    friend bool operator==(const index_element&, const index_element&) = default;
};

/// What the block index of a data blocks file says about the identifiers that a unit looks for. Its vectors move
/// without throwing, as the hash tables of some standard libraries do not.
struct block_index
{
    /// The first element of each identifier looked for that the index has, free or not, in increasing order of their
    /// identifiers.
    std::vector<index_element> elements{};
    /// The identifiers looked for that several elements have, in increasing order.
    std::vector<std::uint64_t> duplicates{};
    /// How many reserved fields of the file, its nodes and its elements are not zero.
    std::uint64_t nonzero_reserved_fields = 0;

    /// The element of the identifier id, or null when the index has none.
    [[nodiscard]] const index_element* find(std::uint64_t id) const noexcept;

    /// True when several elements have the identifier id.
    [[nodiscard]] bool is_duplicate(std::uint64_t id) const noexcept;
};

/// Reads the block index of the data blocks file in source, keeping the elements whose identifiers are in wanted, so
/// that what it holds is bounded by the header that names them. It reads every node once: a node must be inside the
/// file after its first 16 bytes, no node may come back, and the nodes together, with their elements, cannot take
/// more bytes than the file has, so that the work is bounded by its size.
///
/// Throws invalid_data_error with errc::invalid_blocks_file when source does not start with the signature, or when a
/// node breaks those rules; limit_error with errc::too_many_index_nodes for more nodes than limits.max_index_nodes, and
/// with errc::too_many_index_elements for more elements in all than limits.max_index_elements, counted from the length
/// of each node before its elements are read; and what the source throws. The messages do not name the file, which
/// the caller knows by its location.
[[nodiscard]] block_index read_block_index(const thread_safe_source& source,
                                           const std::unordered_set<std::uint64_t>& wanted, const limits& limits);

} // namespace openxisf::detail
