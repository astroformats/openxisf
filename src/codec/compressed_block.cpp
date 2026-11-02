// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "codec/compressed_block.h"

#include <openxisf/error.h>

#include "codec/codecs.h"
#include "codec/shuffle.h"
#include "core/allocation.h"
#include "core/checked_math.h"
#include "core/parallel.h"
#include "core/scratch_buffer.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace openxisf::detail {

namespace {

constexpr int highest_level = 100;

// The subblocks that compress_subblocks() compresses at once take at most about this many bytes of data, unless one
// alone is larger; their shuffled copies and the output of the codec take about as much again.
constexpr std::uint64_t parallel_compression_budget = std::uint64_t{1} << 30;

// The sum of the sizes, or nothing when it does not fit in 64 bits.
std::optional<std::uint64_t> total(const std::vector<subblock>& subblocks, std::uint64_t subblock::* size) noexcept
{
    std::uint64_t sum = 0;
    for (const subblock& part : subblocks) {
        if (part.*size > std::numeric_limits<std::uint64_t>::max() - sum) {
            return std::nullopt;
        }
        sum += part.*size;
    }
    return sum;
}

std::string describe_total(const std::optional<std::uint64_t>& sum)
{
    return sum ? "add up to " + std::to_string(*sum) : "add up to more than 2^64";
}

void check_totals(const block_compression& compression, std::uint64_t stored_size)
{
    const std::optional<std::uint64_t> uncompressed = total(compression.subblocks, &subblock::uncompressed_size);
    if (uncompressed != compression.uncompressed_size) {
        throw invalid_data_error(errc::invalid_subblocks, "the uncompressed sizes of the subblocks " +
                                                              describe_total(uncompressed) +
                                                              ", not to the uncompressed size of the block, " +
                                                              std::to_string(compression.uncompressed_size));
    }
    const std::optional<std::uint64_t> compressed = total(compression.subblocks, &subblock::compressed_size);
    if (compressed != stored_size) {
        throw invalid_data_error(errc::invalid_subblocks,
                                 "the compressed sizes of the subblocks " + describe_total(compressed) +
                                     ", not to the size of the stored block, " + std::to_string(stored_size));
    }
}

// The largest subblock, compressed or not, that the decoder of codec takes.
std::uint64_t max_decoded_size(compression_codec codec) noexcept
{
    switch (codec) {
    case compression_codec::lz4:
    case compression_codec::lz4hc:
        return lz4_max_decoded_size;
    case compression_codec::zlib:
    case compression_codec::zstd:
        break;
    }
    return std::numeric_limits<std::uint64_t>::max();
}

// The most that the encoder of codec takes at once.
std::uint64_t max_encoded_size(compression_codec codec) noexcept
{
    switch (codec) {
    case compression_codec::zlib:
        return (std::uint64_t{1} << 32) - (std::uint64_t{1} << 24);
    case compression_codec::lz4:
    case compression_codec::lz4hc:
        return lz4_max_input_size;
    case compression_codec::zstd:
        break;
    }
    return std::numeric_limits<std::uint64_t>::max();
}

// The most bytes that the decoder of codec gives for each byte of its input. A zlib stream (RFC 1950) gives at most
// 1032: deflate codes a match of 258 bytes in two bits at best (RFC 1951). An LZ4 block gives less than 255: each byte
// that extends the length of a match adds at most 255 bytes. A Zstandard frame gives at most 32768 (RFC 8878): a block
// decodes to at most 128 KiB, and takes at least 4 bytes, those of a run of one byte; its header comes on top.
std::uint64_t max_expansion(compression_codec codec) noexcept
{
    switch (codec) {
    case compression_codec::zlib:
        return 1032;
    case compression_codec::lz4:
    case compression_codec::lz4hc:
        return 255;
    case compression_codec::zstd:
        break;
    }
    return 32768;
}

// A subblock that did not get smaller is stored as it is, which its two equal sizes tell (spec §10.6.1, Revision 2).
// The specification lets a decoder try to decompress such a subblock first, since encoders of earlier revisions could
// compress every subblock; this decoder reads it as stored, as PixInsight does, so codec output of exactly the size of
// its input is read as data.
bool stored_as_is(const subblock& part) noexcept
{
    return part.compressed_size == part.uncompressed_size;
}

// The name of a subblock in a message: the block itself when it has no subblocks.
std::string name_of(const block_compression& compression, std::size_t index)
{
    return compression.subblocks.empty() ? std::string("the block") : "subblock " + std::to_string(index + 1);
}

// Spec §11.4.2: the abstract range 1 to 100 maps linearly to lowest to highest, rounded to the nearest level.
int scale(int level, int lowest, int highest) noexcept
{
    return lowest + (((level - 1) * (highest - lowest)) + ((highest_level - 1) / 2)) / (highest_level - 1);
}

void decompress_subblock(compression_codec codec, std::span<const std::byte> input, std::span<std::byte> output,
                         const limits& limits)
{
    switch (codec) {
    case compression_codec::zlib:
        zlib_decompress(input, output);
        return;
    case compression_codec::lz4:
    case compression_codec::lz4hc:
        lz4_decompress(input, output);
        return;
    case compression_codec::zstd:
        zstd_decompress(input, output, limits.max_zstd_window);
        return;
    }
}

// The encoder of the codec of a block, made for the first subblock that it compresses and kept for the others, so that
// the state of the codec is not made again for each subblock. LZ4 has no state to keep.
struct subblock_encoder
{
    std::optional<zlib_encoder> zlib{};
    std::optional<lz4hc_encoder> lz4hc{};
    std::optional<zstd_encoder> zstd{};
};

template <typename Encoder> Encoder& made(std::optional<Encoder>& encoder, int level)
{
    if (!encoder) {
        return encoder.emplace(level);
    }
    return *encoder;
}

void compress_subblock(compression_codec codec, std::span<const std::byte> input, int level, subblock_encoder& encoder,
                       std::vector<std::byte>& output)
{
    switch (codec) {
    case compression_codec::zlib:
        made(encoder.zlib, level).compress(input, output);
        return;
    case compression_codec::lz4:
        lz4_compress(input, output);
        return;
    case compression_codec::lz4hc:
        made(encoder.lz4hc, level).compress(input, output);
        return;
    case compression_codec::zstd:
        made(encoder.zstd, level).compress(input, output);
        return;
    }
}

} // namespace

std::uint64_t subblock_size(const compression_options& options) noexcept
{
    const std::uint64_t limit = max_encoded_size(options.codec);
    return options.max_subblock_size == 0 ? limit : std::min(options.max_subblock_size, limit);
}

int codec_level(compression_codec codec, int level)
{
    if (level < 0 || level > highest_level) {
        throw usage_error(errc::invalid_argument,
                          "the compression level " + std::to_string(level) + " is not between 0 and 100");
    }
    switch (codec) {
    case compression_codec::zlib:
        return level == 0 ? zlib_default_level : scale(level, 0, 9);
    case compression_codec::lz4hc:
        return level == 0 ? lz4hc_default_level : scale(level, 1, 12);
    case compression_codec::zstd:
        return level == 0 ? zstd_default_level : scale(level, 1, 22);
    case compression_codec::lz4:
        break;
    }
    return 0;
}

int default_abstract_level(compression_codec codec) noexcept
{
    // The inverse of scale(), rounded to the nearest abstract level.
    const auto inverse = [](int level, int lowest, int highest) {
        return 1 + (((level - lowest) * (highest_level - 1)) + ((highest - lowest) / 2)) / (highest - lowest);
    };
    switch (codec) {
    case compression_codec::zlib:
        return inverse(zlib_default_level, 0, 9);
    case compression_codec::lz4hc:
        return inverse(lz4hc_default_level, 1, 12);
    case compression_codec::zstd:
        return inverse(zstd_default_level, 1, 22);
    case compression_codec::lz4:
        break;
    }
    return 0;
}

void check_subblocks(const block_compression& compression, std::uint64_t stored_size)
{
    if (!compression.subblocks.empty()) {
        check_totals(compression, stored_size);
    }
    const subblock whole{.compressed_size = stored_size, .uncompressed_size = compression.uncompressed_size};
    const std::span<const subblock> subblocks =
        compression.subblocks.empty() ? std::span<const subblock>(&whole, 1) : std::span(compression.subblocks);
    const std::uint64_t limit = max_decoded_size(compression.codec);
    const std::uint64_t expansion = max_expansion(compression.codec);
    for (std::size_t i = 0; i < subblocks.size(); ++i) {
        const subblock& part = subblocks[i];
        if (stored_as_is(part)) {
            continue;
        }
        if (part.compressed_size > limit || part.uncompressed_size > limit) {
            throw invalid_data_error(errc::invalid_subblocks, name_of(compression, i) + " is larger than " +
                                                                  std::string(codec_name(compression.codec)) +
                                                                  " can decode at once, " + std::to_string(limit) +
                                                                  " bytes, and must be divided into subblocks");
        }
        // More than expansion × compressed_size, which cannot overflow then.
        if (part.uncompressed_size != 0 && (part.uncompressed_size - 1) / expansion >= part.compressed_size) {
            throw invalid_data_error(errc::invalid_subblocks,
                                     name_of(compression, i) + " declares " + std::to_string(part.uncompressed_size) +
                                         " bytes, but " + std::string(codec_name(compression.codec)) +
                                         " decodes at most " + std::to_string(expansion) + " times its stored size, " +
                                         std::to_string(expansion * part.compressed_size) + " bytes");
        }
    }
}

void check_zstd_expansion(const block_compression& compression, std::uint64_t stored_size, std::uint64_t max_expansion)
{
    if (compression.codec != compression_codec::zstd || max_expansion == 0) {
        return;
    }
    const subblock whole{.compressed_size = stored_size, .uncompressed_size = compression.uncompressed_size};
    const std::span<const subblock> subblocks =
        compression.subblocks.empty() ? std::span<const subblock>(&whole, 1) : std::span(compression.subblocks);
    for (std::size_t i = 0; i < subblocks.size(); ++i) {
        const subblock& part = subblocks[i];
        // More than max_expansion × compressed_size, which cannot overflow then. A subblock stored as it is, of equal
        // sizes, is within every limit.
        if (part.uncompressed_size != 0 && (part.uncompressed_size - 1) / max_expansion >= part.compressed_size) {
            throw limit_error(errc::zstd_expansion_too_large,
                              name_of(compression, i) + " declares " + std::to_string(part.uncompressed_size) +
                                  " bytes, more than the limit of " + std::to_string(max_expansion) +
                                  " times its stored size for Zstandard, " +
                                  std::to_string(max_expansion * part.compressed_size) + " bytes");
        }
    }
}

std::vector<subblock> subblocks_of(const block_compression& compression, std::uint64_t stored_size)
{
    check_subblocks(compression, stored_size);
    if (compression.subblocks.empty()) {
        return {{.compressed_size = stored_size, .uncompressed_size = compression.uncompressed_size}};
    }
    return compression.subblocks;
}

namespace {

// The decompressed block is allocated by the library: its uncompressed size must be within the limit.
void check_decompressed_allocation(const block_compression& compression, const limits& limits)
{
    check_allocation(compression.uncompressed_size, limits, "the data block decompresses to");
}

// Calls function for the subblock at index of count subblocks, and gives its integrity_error the number of the subblock
// when there are several.
template <typename Function> void for_subblock(std::size_t index, std::size_t count, const Function& function)
{
    try {
        function();
    } catch (const integrity_error& failure) {
        if (count == 1) {
            throw;
        }
        throw integrity_error(failure.code(), "subblock " + std::to_string(index + 1) + " of " + std::to_string(count) +
                                                  ": " + failure.what());
    }
}

// Subblocks of a block, from first on, of total subblocks in the block: their messages name them by their place in it.
struct numbered_subblocks
{
    std::span<const subblock> subblocks{};
    std::size_t first = 0;
    std::size_t total = 0;
};

// A Zstandard frame declares the size of its content, which must be that of its subblock: a block whose frames declare
// other sizes is refused before the memory of its data is allocated.
void check_frames(std::span<const std::byte> stored, compression_codec codec, const numbered_subblocks& numbered)
{
    if (codec != compression_codec::zstd) {
        return;
    }
    const std::span<const subblock> subblocks = numbered.subblocks;
    std::size_t start = 0;
    for (std::size_t i = 0; i < subblocks.size(); ++i) {
        const auto size = static_cast<std::size_t>(subblocks[i].compressed_size);
        if (!stored_as_is(subblocks[i])) {
            for_subblock(numbered.first + i, numbered.total,
                         [&] { zstd_check_frame(stored.subspan(start, size), subblocks[i].uncompressed_size); });
        }
        start += size;
    }
}

// Decompresses subblocks of a block, which subblocks_of() checked, each into its place in data, which has their
// uncompressed size. The subblocks are independent, so each batch of as many as the threads that can run at once is
// decompressed in parallel; progress follows on the calling thread, subblock by subblock, after each batch. A
// Zstandard frame that does not declare its size is a batch of its own: it needs a window buffer of up to
// limits.max_zstd_window, which then bounds the memory of the whole read, not that of each thread.
void decompress_subblocks(std::span<const std::byte> stored, compression_codec codec,
                          const numbered_subblocks& numbered, const limits& limits, std::span<std::byte> data,
                          const subblock_progress& progress)
{
    const std::span<const subblock> subblocks = numbered.subblocks;
    // Where each subblock starts in stored and in data, and where the last one ends.
    std::vector<std::size_t> in(subblocks.size() + 1);
    std::vector<std::size_t> out(subblocks.size() + 1);
    for (std::size_t i = 0; i < subblocks.size(); ++i) {
        in[i + 1] = in[i] + static_cast<std::size_t>(subblocks[i].compressed_size);
        out[i + 1] = out[i] + static_cast<std::size_t>(subblocks[i].uncompressed_size);
    }
    const auto decompress = [&](std::size_t i) {
        const std::span<const std::byte> input = stored.subspan(in[i], in[i + 1] - in[i]);
        const std::span<std::byte> output = data.subspan(out[i], out[i + 1] - out[i]);
        for_subblock(numbered.first + i, numbered.total, [&] {
            if (stored_as_is(subblocks[i])) {
                std::ranges::copy(input, output.begin());
            } else {
                decompress_subblock(codec, input, output, limits);
            }
        });
    };
    const auto needs_window = [&](std::size_t i) {
        return codec == compression_codec::zstd && !stored_as_is(subblocks[i]) &&
               zstd_needs_window(stored.subspan(in[i], in[i + 1] - in[i]));
    };
    const std::size_t batch = concurrency();
    for (std::size_t first = 0; first < subblocks.size();) {
        std::size_t count = 1;
        if (!needs_window(first)) {
            while (count < batch && first + count < subblocks.size() && !needs_window(first + count)) {
                ++count;
            }
        }
        parallel_for(count, [&](std::size_t k) { decompress(first + k); });
        if (progress) {
            for (std::size_t i = first; i < first + count; ++i) {
                progress(out[i + 1]);
            }
        }
        first += count;
    }
}

// Decompresses a block into destination, which has its uncompressed size, through a buffer when it is shuffled.
void decompress_into(std::span<const std::byte> stored, const block_compression& compression,
                     const std::vector<subblock>& subblocks, const limits& limits, std::span<std::byte> destination,
                     const subblock_progress& progress)
{
    const numbered_subblocks all{.subblocks = subblocks, .first = 0, .total = subblocks.size()};
    if (compression.item_size <= 1) {
        decompress_subblocks(stored, compression.codec, all, limits, destination, progress);
        return;
    }
    check_decompressed_allocation(compression, limits);
    // The subblocks fill it or throw.
    scratch_buffer shuffled(destination.size());
    decompress_subblocks(stored, compression.codec, all, limits, shuffled.bytes(), progress);
    unshuffle_bytes(shuffled.bytes(), destination, static_cast<std::size_t>(compression.item_size));
}

} // namespace

std::vector<std::byte> decompress_block(std::span<const std::byte> stored, const block_compression& compression,
                                        const limits& limits, const subblock_progress& progress)
{
    const std::vector<subblock> subblocks = subblocks_of(compression, stored.size());
    check_frames(stored, compression.codec, {.subblocks = subblocks, .first = 0, .total = subblocks.size()});
    check_decompressed_allocation(compression, limits);
    std::vector<std::byte> data(checked_cast<std::size_t>(compression.uncompressed_size));
    decompress_into(stored, compression, subblocks, limits, data, progress);
    return data;
}

void decompress_block_into(std::span<const std::byte> stored, const block_compression& compression,
                           const limits& limits, std::span<std::byte> destination, const subblock_progress& progress)
{
    const std::vector<subblock> subblocks = subblocks_of(compression, stored.size());
    if (destination.size() != compression.uncompressed_size) {
        throw usage_error(errc::invalid_argument, "the destination has " + std::to_string(destination.size()) +
                                                      " bytes, and the data block decompresses to " +
                                                      std::to_string(compression.uncompressed_size));
    }
    check_frames(stored, compression.codec, {.subblocks = subblocks, .first = 0, .total = subblocks.size()});
    decompress_into(stored, compression, subblocks, limits, destination, progress);
}

std::vector<subblock_span> covering_subblocks(const block_compression& compression, std::uint64_t stored_size,
                                              const needed_bytes& needed, std::size_t most_count,
                                              std::uint64_t most_size)
{
    check_subblocks(compression, stored_size);
    const subblock whole{.compressed_size = stored_size, .uncompressed_size = compression.uncompressed_size};
    const std::span<const subblock> subblocks =
        compression.subblocks.empty() ? std::span<const subblock>(&whole, 1) : std::span(compression.subblocks);
    std::vector<subblock_span> spans;
    // Where the subblock starts, stored and uncompressed; the sums were checked.
    std::uint64_t stored = 0;
    std::uint64_t offset = 0;
    // Whether the last span ends with the subblock before.
    bool follows = false;
    for (std::size_t i = 0; i < subblocks.size(); ++i) {
        const subblock& part = subblocks[i];
        const bool covers = needed(offset, offset + part.uncompressed_size);
        if (covers && follows && spans.back().count < most_count &&
            part.uncompressed_size <= most_size - std::min(most_size, spans.back().size)) {
            subblock_span& last = spans.back();
            ++last.count;
            last.stored_size += part.compressed_size;
            last.size += part.uncompressed_size;
        } else if (covers) {
            spans.push_back({.first = i,
                             .count = 1,
                             .stored_offset = stored,
                             .stored_size = part.compressed_size,
                             .offset = offset,
                             .size = part.uncompressed_size});
        }
        follows = covers;
        stored += part.compressed_size;
        offset += part.uncompressed_size;
    }
    return spans;
}

void decompress_subblocks_into(std::span<const std::byte> stored, const block_compression& compression,
                               const subblock_span& span, const limits& limits, std::span<std::byte> data,
                               const subblock_progress& progress)
{
    // A block without subblocks is the only subblock of its span.
    const subblock whole{.compressed_size = span.stored_size, .uncompressed_size = span.size};
    const numbered_subblocks numbered{
        .subblocks = compression.subblocks.empty() ? std::span<const subblock>(&whole, 1)
                                                   : std::span(compression.subblocks).subspan(span.first, span.count),
        .first = span.first,
        .total = std::max<std::size_t>(compression.subblocks.size(), 1)};
    check_frames(stored, compression.codec, numbered);
    decompress_subblocks(stored, compression.codec, numbered, limits, data, progress);
}

block_compression compress_subblocks(std::span<const std::byte> data, const compression_options& options,
                                     const subblock_store& store)
{
    const int level = codec_level(options.codec, options.level);
    const std::uint64_t limit = subblock_size(options);
    const bool shuffled = options.item_size > 1;
    // An empty block is one empty subblock.
    const std::uint64_t count = data.empty() ? 1 : (data.size() / limit) + (data.size() % limit == 0 ? 0 : 1);

    // A subblock in the making: its data, shuffled when they are, and the output of the codec. A piece serves one call
    // of each batch, so it also keeps the encoder of that call for the next batch.
    struct piece
    {
        std::span<const std::byte> input{};
        std::vector<std::byte> shuffled_input{};
        std::vector<std::byte> output{};
        subblock_encoder encoder{};
    };
    const auto compress = [&](std::uint64_t index, piece& next) {
        const std::size_t offset = index * limit;
        const std::size_t size = std::min<std::uint64_t>(limit, data.size() - offset);
        next.input = data.subspan(offset, size);
        if (shuffled) {
            next.shuffled_input.resize(size);
            shuffle_part(data, static_cast<std::size_t>(options.item_size), offset, next.shuffled_input);
            next.input = next.shuffled_input;
        }
        next.output.clear();
        compress_subblock(options.codec, next.input, level, next.encoder, next.output);
    };

    // The subblocks are independent, so a batch of them is compressed in parallel, as many as the threads that can run
    // at once within a budget of memory, and they are stored in order on the calling thread.
    const std::uint64_t batch =
        std::min<std::uint64_t>(concurrency(), std::max<std::uint64_t>(1, parallel_compression_budget / limit));
    const std::size_t in_flight = std::min(batch, count);
    std::vector<piece> pieces(in_flight);
    block_compression result{.codec = options.codec, .uncompressed_size = data.size(), .item_size = options.item_size};
    for (std::uint64_t first = 0; first < count; first += batch) {
        const std::size_t size = std::min(batch, count - first);
        parallel_for(size, [&](std::size_t k) { compress(first + k, pieces[k]); });
        for (std::size_t k = 0; k < size; ++k) {
            const piece& done = pieces[k];
            const std::span<const std::byte> stored =
                done.output.size() >= done.input.size() ? done.input : std::span<const std::byte>(done.output);
            const subblock sizes{.compressed_size = stored.size(), .uncompressed_size = done.input.size()};
            store(stored, sizes);
            result.subblocks.push_back(sizes);
        }
    }
    return result;
}

compressed_block compress_block(std::span<const std::byte> data, const compression_options& options)
{
    compressed_block result;
    result.compression =
        compress_subblocks(data, options, [&result](std::span<const std::byte> stored, const subblock& /*sizes*/) {
            result.data.insert(result.data.end(), stored.begin(), stored.end());
        });
    if (result.compression.subblocks.size() == 1) {
        result.compression.subblocks.clear();
    }
    return result;
}

} // namespace openxisf::detail
