// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "container/blocks_file.h"

#include <openxisf/error.h>

#include "core/endian.h"

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace openxisf::detail {

namespace {

// The elements of a node are read this many at a time, so that a node of many elements needs no large buffer.
constexpr std::uint64_t elements_per_read = 1024;

[[noreturn]] void throw_invalid(const std::string& message)
{
    throw invalid_data_error(errc::invalid_blocks_file, message);
}

// The little-endian integer at offset Offset of bytes, which hold it.
template <std::unsigned_integral T, std::size_t Offset> T field(std::span<const std::byte> bytes) noexcept
{
    return load_little_endian<T>(bytes.subspan<Offset, sizeof(T)>());
}

class index_reader
{
public:
    index_reader(const thread_safe_source& source, const std::unordered_set<std::uint64_t>& wanted,
                 const limits& limits)
        : source_(source), wanted_(wanted), limits_(limits), size_(source.size())
    {}

    block_index read()
    {
        read_signature();
        std::uint64_t node = blocks_file_header_size;
        std::uint64_t count = 0;
        while (node != 0) {
            if (limits_.max_index_nodes != 0 && count == limits_.max_index_nodes) {
                throw limit_error(errc::too_many_index_nodes, "the block index has more than " +
                                                                  std::to_string(limits_.max_index_nodes) +
                                                                  " nodes, the limit");
            }
            ++count;
            node = read_node(node);
        }
        // Built from the tables of the walk, and returned without being moved.
        block_index index{.duplicates = {duplicates_.begin(), duplicates_.end()}, .nonzero_reserved_fields = reserved_};
        index.elements.reserve(elements_.size());
        for (const auto& entry : elements_) {
            index.elements.push_back(entry.second);
        }
        std::ranges::sort(index.elements, {}, &index_element::id);
        std::ranges::sort(index.duplicates);
        return index;
    }

private:
    void read_signature()
    {
        std::array<std::byte, blocks_file_header_size> header{};
        if (size_ < header.size()) {
            throw_invalid("the file has " + std::to_string(size_) + " bytes, fewer than the " +
                          std::to_string(header.size()) + " of the signature and reserved field of a data blocks file");
        }
        source_.read(0, header);
        if (!std::ranges::equal(std::span(header).first(blocks_file_signature.size()), blocks_file_signature,
                                [](std::byte b, char c) { return std::to_integer<char>(b) == c; })) {
            throw_invalid("the file is not a data blocks file: it does not start with the signature XISB0100");
        }
        count_reserved(field<std::uint64_t, 8>(header));
        used_ = header.size();
    }

    // Reads the node at position, and returns the position of the next one, 0 after the last.
    std::uint64_t read_node(std::uint64_t position)
    {
        if (!visited_.insert(position).second) {
            throw_invalid("the block index comes back to its node at byte " + std::to_string(position));
        }
        if (position < blocks_file_header_size || position > size_ || size_ - position < index_node_header_size) {
            throw_invalid("the block index node at byte " + std::to_string(position) +
                          " is not inside the file after its first " + std::to_string(blocks_file_header_size) +
                          " bytes; the file has " + std::to_string(size_));
        }
        std::array<std::byte, index_node_header_size> header{};
        source_.read(position, header);
        const auto length = field<std::uint32_t, 0>(header);
        count_reserved(field<std::uint32_t, 4>(header));
        const auto next = field<std::uint64_t, 8>(header);

        // At most 2^32 - 1 elements of 40 bytes: no overflow in 64 bits.
        const std::uint64_t elements_size = std::uint64_t{length} * index_element_size;
        const std::uint64_t start = position + index_node_header_size;
        if (elements_size > size_ - start) {
            throw_invalid("the " + std::to_string(length) + " elements of the block index node at byte " +
                          std::to_string(position) + " go past the end of the file, which has " +
                          std::to_string(size_) + " bytes");
        }
        // Nodes may overlap, but not read the same bytes again and again.
        const std::uint64_t node_size = index_node_header_size + elements_size;
        if (node_size > size_ - used_) {
            throw_invalid("the nodes of the block index, up to the node at byte " + std::to_string(position) +
                          ", take more bytes than the file has, " + std::to_string(size_));
        }
        // The elements are counted before they are read: the 2^32 - 1 elements that a node of a sparse file can declare
        // take minutes to read.
        if (limits_.max_index_elements != 0 && length > limits_.max_index_elements - counted_) {
            throw limit_error(errc::too_many_index_elements, "the block index has more than " +
                                                                 std::to_string(limits_.max_index_elements) +
                                                                 " elements, the limit");
        }
        counted_ += length;
        used_ += node_size;
        read_elements(start, length);
        return next;
    }

    void read_elements(std::uint64_t start, std::uint64_t count)
    {
        for (std::uint64_t done = 0; done < count;) {
            const std::uint64_t batch = std::min(count - done, elements_per_read);
            const std::size_t bytes = batch * index_element_size;
            buffer_.resize(bytes);
            source_.read(start + done * index_element_size, buffer_);
            for (std::size_t offset = 0; offset < buffer_.size(); offset += index_element_size) {
                add(std::span<const std::byte>(buffer_).subspan(offset, index_element_size));
            }
            done += batch;
        }
    }

    void add(std::span<const std::byte> bytes)
    {
        const index_element element{.id = field<std::uint64_t, 0>(bytes),
                                    .position = field<std::uint64_t, 8>(bytes),
                                    .length = field<std::uint64_t, 16>(bytes),
                                    .uncompressed_length = field<std::uint64_t, 24>(bytes)};
        count_reserved(field<std::uint64_t, 32>(bytes));
        if (wanted_.contains(element.id) && !elements_.emplace(element.id, element).second) {
            duplicates_.insert(element.id);
        }
    }

    void count_reserved(std::uint64_t value) noexcept
    {
        if (value != 0) {
            ++reserved_;
        }
    }

    const thread_safe_source& source_;
    const std::unordered_set<std::uint64_t>& wanted_;
    const limits& limits_;
    std::uint64_t size_;
    // The bytes of the file header and of the nodes read so far, and the number of their elements.
    std::uint64_t used_ = 0;
    std::uint64_t counted_ = 0;
    std::unordered_set<std::uint64_t> visited_{};
    std::vector<std::byte> buffer_{};
    // The first element of each identifier looked for, the identifiers of several elements, and the reserved fields
    // that are not zero.
    std::unordered_map<std::uint64_t, index_element> elements_{};
    std::unordered_set<std::uint64_t> duplicates_{};
    std::uint64_t reserved_ = 0;
};

} // namespace

const index_element* block_index::find(std::uint64_t id) const noexcept
{
    const auto found = std::ranges::lower_bound(elements, id, {}, &index_element::id);
    return found != elements.end() && found->id == id ? &*found : nullptr;
}

bool block_index::is_duplicate(std::uint64_t id) const noexcept
{
    return std::ranges::binary_search(duplicates, id);
}

block_index read_block_index(const thread_safe_source& source, const std::unordered_set<std::uint64_t>& wanted,
                             const limits& limits)
{
    return index_reader(source, wanted, limits).read();
}

} // namespace openxisf::detail
