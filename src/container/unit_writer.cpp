// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "container/unit_writer.h"

#include <openxisf/error.h>
#include <openxisf/version.h>

#include "codec/compressed_block.h"
#include "container/block_attributes.h"
#include "container/blocks_file.h"
#include "container/file_layout.h"
#include "core/checked_math.h"
#include "core/data_encoding.h"
#include "core/endian.h"
#include "crypto/hash.h"
#include "model/header_tree.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace openxisf::detail {

namespace {

// The data of an attached block go to the sink, and through the hash, in pieces of this size.
constexpr std::size_t piece_size = std::size_t{4} << 20;

// The header length of a monolithic file is a 32-bit integer (spec §9.2).
constexpr std::uint64_t max_header_length = 0xFFFF'FFFF;

// A number of unknown value, written at its full width in the longest header that the blocks can give.
constexpr std::uint64_t unknown = std::numeric_limits<std::uint64_t>::max();

// The characters of a subblock in the subblocks attribute: at least "1,1:", and at most two numbers of 20 digits.
constexpr std::uint64_t shortest_subblock = 4;
constexpr std::uint64_t longest_subblock = 42;

constexpr std::array<std::byte, 4096> zeros{};

compression_codec codec_of(codec value) noexcept
{
    switch (value) {
    case codec::zlib:
        return compression_codec::zlib;
    case codec::lz4:
        return compression_codec::lz4;
    case codec::lz4hc:
        return compression_codec::lz4hc;
    case codec::zstd:
        break;
    }
    return compression_codec::zstd;
}

hash_algorithm algorithm_of(checksum_algorithm value) noexcept
{
    switch (value) {
    case checksum_algorithm::sha1:
        return hash_algorithm::sha1;
    case checksum_algorithm::sha256:
        return hash_algorithm::sha256;
    case checksum_algorithm::sha512:
        return hash_algorithm::sha512;
    case checksum_algorithm::sha3_256:
        return hash_algorithm::sha3_256;
    case checksum_algorithm::sha3_512:
        break;
    }
    return hash_algorithm::sha3_512;
}

[[noreturn]] void throw_header_too_large(std::uint64_t length)
{
    throw validation_error(errc::header_too_large,
                           "the header would have " + std::to_string(length) +
                               " bytes, more than 2^32 - 1, the most that the header length of a monolithic file can "
                               "count (spec §9.2), and the most that OpenXISF writes in a header file too",
                           {.element = "/xisf"});
}

std::uint64_t aligned(std::uint64_t position, std::uint16_t alignment)
{
    if (alignment <= 1 || position % alignment == 0) {
        return position;
    }
    return checked_add(position, std::uint64_t{alignment} - (position % alignment));
}

// The first 16 bytes of a monolithic file (spec §9.2): the signature, the header length and the reserved field.
std::array<std::byte, monolithic_header_offset> preamble(std::uint64_t header_length)
{
    std::array<std::byte, monolithic_header_offset> bytes{};
    for (std::size_t i = 0; i < monolithic_signature.size(); ++i) {
        bytes[i] = static_cast<std::byte>(monolithic_signature[i]);
    }
    store_little_endian<std::uint32_t>(std::span(bytes).subspan<8, 4>(), static_cast<std::uint32_t>(header_length));
    return bytes;
}

void write_zeros(output_sink& sink, std::uint64_t count)
{
    while (count > 0) {
        const std::size_t size = std::min<std::uint64_t>(count, zeros.size());
        sink.write(std::span(zeros).first(size));
        count -= size;
    }
}

std::span<const std::byte> bytes_of(const std::string& text) noexcept
{
    return std::as_bytes(std::span(text.data(), text.size()));
}

// How the data blocks are encoded, from the options.
struct block_encoding_options
{
    std::optional<compression_codec> codec{};
    int level = 0;
    bool shuffle = false;
    std::uint64_t subblock_size = 0;
    std::optional<hash_algorithm> checksum{};
};

block_encoding_options encoding_of(const write_options& options) noexcept
{
    block_encoding_options encoding{
        .level = options.compression_level, .shuffle = options.byte_shuffle, .subblock_size = options.subblock_size};
    if (options.codec) {
        encoding.codec = codec_of(*options.codec);
    }
    if (options.checksum) {
        encoding.checksum = algorithm_of(*options.checksum);
    }
    return encoding;
}

compression_options compression_for(compression_codec codec, const block_encoding_options& encoding,
                                    const block_source& block) noexcept
{
    // Shuffling bytes one by one changes nothing, so blocks of bytes are compressed without it.
    return {.codec = codec,
            .item_size = encoding.shuffle && block.item_size > 1 ? block.item_size : 0,
            .level = encoding.level,
            .max_subblock_size = encoding.subblock_size};
}

// A single subblock is the whole block, which a subblocks attribute does not describe.
block_compression without_single_subblock(block_compression compression)
{
    if (compression.subblocks.size() == 1) {
        compression.subblocks.clear();
    }
    return compression;
}

block_checksum checksum_of(hash_algorithm algorithm, std::span<const std::byte> stored)
{
    return {.algorithm = algorithm, .digest = compute_digest(algorithm, stored)};
}

// An inline block, encoded whole: compressed when that makes it smaller (spec §10.6), with the checksum of its stored
// bytes (spec §10.6.1), and in Base64.
block_header encode_inline(const block_source& block, const block_encoding_options& encoding)
{
    block_header header{.inline_data = true};
    std::span<const std::byte> stored = block.data();
    compressed_block compressed;
    if (encoding.codec && !stored.empty()) {
        compressed = compress_block(stored, compression_for(*encoding.codec, encoding, block));
        if (compressed.data.size() < stored.size()) {
            header.compression = compressed.compression;
            stored = compressed.data;
        }
    }
    header.size = stored.size();
    if (encoding.checksum) {
        header.checksum = checksum_of(*encoding.checksum, stored);
    }
    header.text = encode_base64(stored);
    return header;
}

// Reports the progress of a save, which its function can cancel.
class progress_counter
{
public:
    explicit progress_counter(const progress_function& function) noexcept : function_(function) {}

    void start(std::uint64_t total)
    {
        total_ = total;
        report();
    }

    void advance(std::uint64_t units)
    {
        done_ += units;
        report();
    }

private:
    void report() const
    {
        if (function_ && !function_(done_, total_)) {
            throw cancelled_error(errc::cancelled, "the save was cancelled by its progress function");
        }
    }

    const progress_function& function_;
    std::uint64_t done_ = 0;
    std::uint64_t total_ = 0;
};

// The position of the first block index element of a data blocks file, after the file header and the node header.
constexpr std::uint64_t first_index_element = blocks_file_header_size + index_node_header_size;

// Writes a unit: the header and the attached blocks to one sink for a monolithic file, or the header to one sink and
// the blocks that are not inline to a data blocks file in another, for a distributed unit.
class unit_writer
{
public:
    unit_writer(const unit_contents& unit, const write_options& options, const save_context& context, output_sink& sink)
        : unit_(unit), options_(options), context_(context), sink_(sink), blocks_(sink),
          encoding_(encoding_of(options)), progress_(options.progress)
    {}

    unit_writer(const unit_contents& unit, const write_options& options, const save_context& context,
                output_sink& header, output_sink& blocks, std::string_view blocks_name, xoshiro256starstar ids)
        : unit_(unit), options_(options), context_(context), sink_(header), blocks_(blocks), blocks_name_(blocks_name),
          ids_(ids), encoding_(encoding_of(options)), progress_(options.progress)
    {}

    void write()
    {
        const std::uint64_t total = prepare();
        if (blocks_name_.empty()) {
            write_monolithic(total);
        } else {
            write_distributed(total);
        }
    }

private:
    // Builds the header and encodes the inline blocks. Returns the bytes of the attached blocks before compression. The
    // subblocks are counted before anything is copied, compressed or written: the pixel data that a big-endian host
    // swaps are copied only once the header can list them.
    std::uint64_t prepare()
    {
        tree_ = build_header_tree(unit_, options_, context_.uuids);
        headers_.resize(tree_.blocks.size());
        buffers_.resize(tree_.blocks.size());
        std::uint64_t total = 0;
        std::uint64_t subblocks = 0;
        for (std::size_t i = 0; i < tree_.blocks.size(); ++i) {
            if (tree_.blocks[i].inline_data) {
                headers_[i] = encode_inline(tree_.blocks[i], encoding_);
            } else {
                attached_.push_back(i);
                subblocks = checked_add(subblocks, subblock_count(tree_.blocks[i]));
                headers_[i].size = tree_.blocks[i].data().size();
                total = checked_add(total, headers_[i].size);
            }
        }
        check_subblock_count(subblocks);
        if (in_one_pass()) {
            check_longest_subblocks();
        }
        for (block_source& block : tree_.blocks) {
            to_little_endian(block);
        }
        return total;
    }

    // True when the attached blocks are compressed or hashed, so that their sizes or digests are known once written.
    [[nodiscard]] bool encoded() const noexcept
    {
        return (encoding_.codec || encoding_.checksum) && !attached_.empty();
    }

    // True when the unit is written in one pass, its header once its blocks are, which needs a sink of the blocks that
    // can rewrite: the sink of a monolithic file, or the data blocks file of a distributed unit.
    [[nodiscard]] bool in_one_pass() const
    {
        return encoded() && blocks_.can_rewrite();
    }

    void write_monolithic(std::uint64_t total)
    {
        if (in_one_pass()) {
            const std::uint64_t room = longest_header_size();
            progress_.start(total);
            write_streamed(room);
        } else if (encoded()) {
            progress_.start(checked_multiply(total, std::uint64_t{2}));
            encode_attached();
            write_in_order();
        } else {
            progress_.start(total);
            write_in_order();
        }
        sink_.finish();
    }

    // A distributed unit (spec §9.1.2): the data blocks file first, whose index has a known size, so that the places of
    // the blocks are known before they are written, then the header file, which lists them. Both are written
    // completely before either is finished, and the data blocks file is finished first, so that a header file never
    // names blocks that are not there yet. A header too large for its length is found before either file is written:
    // in its longest form for blocks written in one pass, and once their sizes are known for the others.
    void write_distributed(std::uint64_t total)
    {
        if (attached_.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw validation_error(errc::header_too_large,
                                   "a data blocks file of one index node holds at most 2^32 - 1 blocks, and the unit "
                                   "has " +
                                       std::to_string(attached_.size()),
                                   {.element = "/xisf"});
        }
        const std::uint64_t index_end = first_index_element + (attached_.size() * index_element_size);
        std::string text;
        if (in_one_pass()) {
            (void)longest_header_size();
            progress_.start(total);
            write_zeros(blocks_, index_end);
            for (const std::size_t i : attached_) {
                write_zeros(blocks_, aligned(blocks_.position(), options_.block_alignment) - blocks_.position());
                stream_block(i);
            }
            assign_index_ids();
            blocks_.rewrite(0, blocks_file_start());
            text = format();
        } else {
            if (encoded()) {
                progress_.start(checked_multiply(total, std::uint64_t{2}));
                encode_attached();
            } else {
                progress_.start(total);
            }
            place_attached(index_end);
            assign_index_ids();
            text = format();
            blocks_.write(blocks_file_start());
            write_attached();
        }
        sink_.write(bytes_of(text));
        blocks_.finish();
        sink_.finish();
    }

    // Each block in the data blocks file gets an identifier of its own (spec §9.4).
    void assign_index_ids()
    {
        if (!ids_) {
            return;
        }
        xoshiro256starstar& ids = *ids_;
        std::unordered_set<std::uint64_t> used;
        for (const std::size_t i : attached_) {
            std::uint64_t id = ids();
            while (!used.insert(id).second) {
                id = ids();
            }
            headers_[i].index_id = id;
        }
    }

    // The signature and reserved field of the data blocks file, then its block index: one node, with an element for
    // each attached block (spec §9.4).
    std::vector<std::byte> blocks_file_start() const
    {
        std::vector<std::byte> bytes(first_index_element + (attached_.size() * index_element_size));
        for (std::size_t i = 0; i < blocks_file_signature.size(); ++i) {
            bytes[i] = static_cast<std::byte>(blocks_file_signature[i]);
        }
        const std::span<std::byte> data(bytes);
        store_little_endian(data.subspan<blocks_file_header_size, 4>(), static_cast<std::uint32_t>(attached_.size()));
        std::size_t offset = first_index_element;
        for (const std::size_t i : attached_) {
            const block_header& header = headers_[i];
            const std::uint64_t uncompressed = header.compression ? header.compression->uncompressed_size : 0;
            for (const std::uint64_t value : {header.index_id, header.position, header.size, uncompressed}) {
                store_little_endian(data.subspan(offset).first<sizeof(std::uint64_t)>(), value);
                offset += sizeof(std::uint64_t);
            }
            // The reserved field stays zero.
            offset += sizeof(std::uint64_t);
        }
        return bytes;
    }

    // The subblocks of the attached blocks are listed in the header, so a very small subblock size could make it far
    // too long: they are counted before any is compressed.
    static void check_subblock_count(std::uint64_t subblocks)
    {
        if (subblocks > max_header_length / shortest_subblock) {
            throw_header_too_large(checked_multiply(subblocks, shortest_subblock));
        }
    }

    // The number of subblocks of a block when it is compressed, none without a codec.
    std::uint64_t subblock_count(const block_source& block) const
    {
        if (!encoding_.codec) {
            return 0;
        }
        const std::uint64_t size = block.data().size();
        const std::uint64_t limit = subblock_size(compression_for(*encoding_.codec, encoding_, block));
        return (size / limit) + (size % limit == 0 ? 0 : 1);
    }

    std::string format() const
    {
        std::string text = format_header(tree_, property_elements(generated_metadata()), headers_, blocks_name_);
        if (text.size() > max_header_length) {
            throw_header_too_large(text.size());
        }
        return text;
    }

    // The metadata properties that the writer writes itself (spec §11.4).
    std::vector<property> generated_metadata() const
    {
        std::vector<property> metadata{
            {.id = "XISF:CreationTime", .value = context_.creation_time},
            {.id = "XISF:CreatorApplication", .value = options_.creator_application},
            {.id = "XISF:CreatorModule", .value = std::string("OpenXISF ") + OPENXISF_VERSION_STRING},
        };
        if (const std::string_view os = creator_os(); !os.empty()) {
            metadata.push_back({.id = "XISF:CreatorOS", .value = os});
        }
        // Spec §11.4.2: the alignment of attached blocks, which only a monolithic file has.
        if (blocks_name_.empty()) {
            metadata.push_back({.id = "XISF:BlockAlignmentSize", .value = options_.block_alignment});
        }
        metadata.push_back({.id = "XISF:MaxInlineBlockSize", .value = options_.max_inline_block_size});

        std::string codecs;
        bool checksums = false;
        for (const block_header& header : headers_) {
            checksums = checksums || header.checksum.has_value();
            if (!header.compression) {
                continue;
            }
            const std::string_view name = compression_name(*header.compression);
            if (("," + codecs + ",").find("," + std::string(name) + ",") == std::string::npos) {
                codecs += codecs.empty() ? std::string(name) : "," + std::string(name);
            }
        }
        if (checksums && encoding_.checksum) {
            metadata.push_back({.id = "XISF:ChecksumAlgorithms", .value = checksum_name(*encoding_.checksum)});
        }
        if (!codecs.empty() && encoding_.codec) {
            metadata.push_back({.id = "XISF:CompressionCodecs", .value = codecs});
            if (const std::optional<int> level = reported_compression_level(*encoding_.codec, encoding_.level)) {
                metadata.push_back({.id = "XISF:CompressionLevel", .value = *level});
            }
        }
        return metadata;
    }

    // Sets the positions of the attached blocks, which follow the byte at start.
    void place_attached(std::uint64_t start)
    {
        std::uint64_t position = aligned(start, options_.block_alignment);
        for (const std::size_t i : attached_) {
            position = aligned(position, options_.block_alignment);
            headers_[i].position = position;
            position = checked_add(position, headers_[i].size);
        }
    }

    // The attached blocks, compressed into memory or hashed before the header is written, for a sink that cannot
    // rewrite.
    void encode_attached()
    {
        for (const std::size_t i : attached_) {
            const block_source& block = tree_.blocks[i];
            const std::span<const std::byte> data = block.data();
            if (encoding_.codec) {
                std::vector<std::byte> stored;
                std::optional<hasher> hash;
                if (encoding_.checksum) {
                    hash.emplace(*encoding_.checksum);
                }
                const block_compression compression =
                    compress_subblocks(data, compression_for(*encoding_.codec, encoding_, block),
                                       [&](std::span<const std::byte> part, const subblock& sizes) {
                                           stored.insert(stored.end(), part.begin(), part.end());
                                           if (hash) {
                                               hash->update(part);
                                           }
                                           progress_.advance(sizes.uncompressed_size);
                                       });
                if (settle_compression(headers_[i], data, stored.size(), compression, hash)) {
                    buffers_[i] = std::move(stored);
                }
                continue;
            }
            if (!encoding_.checksum) {
                continue;
            }
            hasher hash(*encoding_.checksum);
            for (std::size_t offset = 0; offset < data.size(); offset += piece_size) {
                const std::span<const std::byte> piece =
                    data.subspan(offset, std::min(piece_size, data.size() - offset));
                hash.update(piece);
                progress_.advance(piece.size());
            }
            headers_[i].checksum = block_checksum{.algorithm = hash.algorithm(), .digest = hash.finish()};
        }
    }

    // The unit in order, when the place of every block is known: the length of the header is found by fixed-point
    // iteration, since the decimal positions of the blocks change it. It grows at each step, as the positions do, and
    // stops once the header fits in front of the blocks.
    void write_in_order()
    {
        std::uint64_t length = 0;
        std::string text;
        while (true) {
            place_attached(monolithic_header_offset + length);
            text = format();
            if (text.size() <= length) {
                break;
            }
            length = text.size();
        }

        sink_.write(preamble(text.size()));
        sink_.write(bytes_of(text));
        write_attached();
    }

    // The attached blocks at their places, after what the sink of the blocks holds, with zeros in between.
    void write_attached()
    {
        for (const std::size_t i : attached_) {
            write_zeros(blocks_, headers_[i].position - blocks_.position());
            const std::span<const std::byte> stored =
                buffers_[i].empty() ? tree_.blocks[i].data() : std::span<const std::byte>(buffers_[i]);
            for (std::size_t offset = 0; offset < stored.size(); offset += piece_size) {
                blocks_.write(stored.subspan(offset, std::min(piece_size, stored.size() - offset)));
                if (buffers_[i].empty()) {
                    progress_.advance(std::min(piece_size, stored.size() - offset));
                }
            }
            if (!buffers_[i].empty()) {
                progress_.advance(tree_.blocks[i].data().size());
            }
        }
    }

    // A unit written in one pass, to a sink that can rewrite, has its header written once its blocks are, so the header
    // must be able to hold them at their longest, every subblock with numbers of 20 digits: a unit whose subblocks
    // cannot be listed so is refused before they are (prepare()).
    void check_longest_subblocks() const
    {
        std::uint64_t subblocks = 0;
        for (const std::size_t i : attached_) {
            subblocks = checked_add(subblocks, subblock_count(tree_.blocks[i]));
        }
        if (subblocks > max_header_length / longest_subblock) {
            throw_header_too_large(checked_multiply(subblocks, longest_subblock));
        }
    }

    // The size of the longest header that the attached blocks can give, for a unit written in one pass, whose subblocks
    // prepare() checked: the header finally written is no longer. A unit whose longest header is too large is refused
    // before any byte is written, and before the progress function is told of the save.
    std::uint64_t longest_header_size()
    {
        for (const std::size_t i : attached_) {
            headers_[i] = longest_header(tree_.blocks[i]);
        }
        return format().size();
    }

    // The unit in one pass, for a sink that can rewrite: room for the longest header that the blocks can give, then
    // each block as it is compressed and hashed, then the header in that room, with zeros after it.
    void write_streamed(std::uint64_t room)
    {
        // Each block is aligned before it is written, the first one included.
        write_zeros(sink_, monolithic_header_offset + room);
        for (const std::size_t i : attached_) {
            write_zeros(sink_, aligned(sink_.position(), options_.block_alignment) - sink_.position());
            stream_block(i);
        }

        const std::string text = format();
        // The room is that of the longest header, so the header fits it; one that did not would overwrite the blocks.
        if (text.size() > room) {
            throw validation_error(errc::header_too_large,
                                   "the header has " + std::to_string(text.size()) + " bytes, more than the " +
                                       std::to_string(room) + " reserved for it before the data blocks",
                                   {.element = "/xisf"});
        }
        sink_.rewrite(0, preamble(text.size()));
        sink_.rewrite(monolithic_header_offset, bytes_of(text));
    }

    // What the header can say about a block before it is written, at its longest: the numbers that are not known yet
    // at their full width, and a compression with the subblocks that it will have.
    block_header longest_header(const block_source& block) const
    {
        const std::uint64_t size = block.data().size();
        block_header header{.position = unknown, .size = unknown};
        if (encoding_.codec) {
            const compression_options options = compression_for(*encoding_.codec, encoding_, block);
            block_compression compression{
                .codec = options.codec, .uncompressed_size = size, .item_size = options.item_size};
            const std::uint64_t limit = subblock_size(options);
            if (size > limit) {
                // Assigned rather than cast: std::size_t is std::uint64_t on some platforms and not on others.
                const std::size_t count = (size / limit) + (size % limit == 0 ? 0 : 1);
                compression.subblocks.resize(count, {.compressed_size = unknown, .uncompressed_size = unknown});
            }
            header.compression = std::move(compression);
        }
        if (encoding_.checksum) {
            header.checksum = block_checksum{.algorithm = *encoding_.checksum,
                                             .digest = std::vector<std::byte>(digest_size(*encoding_.checksum))};
        }
        return header;
    }

    // Completes the header of a block of data whose subblocks were compressed into stored bytes, hashed by hash: the
    // block is stored compressed when that makes it smaller, and as it is otherwise (spec §10.6), with the checksum of
    // what is stored. Returns whether it is stored compressed.
    bool settle_compression(block_header& header, std::span<const std::byte> data, std::uint64_t stored,
                            const block_compression& compression, std::optional<hasher>& hash) const
    {
        if (stored < data.size()) {
            header.size = stored;
            header.compression = without_single_subblock(compression);
            if (hash) {
                header.checksum = block_checksum{.algorithm = hash->algorithm(), .digest = hash->finish()};
            }
            return true;
        }
        if (encoding_.checksum) {
            header.checksum = checksum_of(*encoding_.checksum, data);
        }
        return false;
    }

    // Writes a block to the sink of the blocks, compressed and hashed subblock by subblock.
    void stream_block(std::size_t index)
    {
        block_header& header = headers_[index];
        const block_source& block = tree_.blocks[index];
        const std::span<const std::byte> data = block.data();
        header = {.position = blocks_.position(), .size = data.size()};
        std::optional<hasher> hash;
        if (encoding_.checksum) {
            hash.emplace(*encoding_.checksum);
        }

        if (encoding_.codec) {
            std::uint64_t stored = 0;
            const block_compression compression =
                compress_subblocks(data, compression_for(*encoding_.codec, encoding_, block),
                                   [&](std::span<const std::byte> part, const subblock& sizes) {
                                       blocks_.write(part);
                                       stored += part.size();
                                       if (hash) {
                                           hash->update(part);
                                       }
                                       progress_.advance(sizes.uncompressed_size);
                                   });
            if (!settle_compression(header, data, stored, compression, hash)) {
                // Each subblock holds its data as they are, shuffled; the data replace them.
                for (std::size_t offset = 0; offset < data.size(); offset += piece_size) {
                    blocks_.rewrite(header.position + offset,
                                    data.subspan(offset, std::min(piece_size, data.size() - offset)));
                }
            }
            return;
        }

        for (std::size_t offset = 0; offset < data.size(); offset += piece_size) {
            const std::span<const std::byte> piece = data.subspan(offset, std::min(piece_size, data.size() - offset));
            blocks_.write(piece);
            if (hash) {
                hash->update(piece);
            }
            progress_.advance(piece.size());
        }
        if (hash) {
            header.checksum = block_checksum{.algorithm = hash->algorithm(), .digest = hash->finish()};
        }
    }

    const unit_contents& unit_;
    const write_options& options_;
    const save_context& context_;
    // The sink of the header, and of the attached blocks of a monolithic file.
    output_sink& sink_;
    // The sink of the attached blocks: sink_, or the data blocks file of a distributed unit.
    output_sink& blocks_;
    // For a distributed unit, the path of the data blocks file from the directory of the header file, and the
    // generator of the identifiers of its block index elements.
    std::string_view blocks_name_{};
    std::optional<xoshiro256starstar> ids_{};
    block_encoding_options encoding_;
    progress_counter progress_;
    header_tree tree_{};
    std::vector<block_header> headers_{};
    // The compressed data of each attached block, held until it is written, for a sink that cannot rewrite.
    std::vector<std::vector<std::byte>> buffers_{};
    // The indices of the attached blocks, in document order.
    std::vector<std::size_t> attached_{};
};

} // namespace

std::string_view creator_os() noexcept
{
#if defined(_WIN32)
    return "Windows";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#else
    return {};
#endif
}

std::optional<int> reported_compression_level(compression_codec codec, int level) noexcept
{
    if (codec == compression_codec::lz4) {
        return std::nullopt;
    }
    return level != 0 ? level : default_abstract_level(codec);
}

void write_unit(const unit_contents& unit, const write_options& options, const save_context& context, output_sink& sink)
{
    unit_writer(unit, options, context, sink).write();
}

void write_distributed_unit(const unit_contents& unit, const write_options& options, const save_context& context,
                            output_sink& header, output_sink& blocks, std::string_view blocks_name,
                            xoshiro256starstar ids)
{
    unit_writer(unit, options, context, header, blocks, blocks_name, ids).write();
}

} // namespace openxisf::detail
