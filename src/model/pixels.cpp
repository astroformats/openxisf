// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/pixels.h"

#include <openxisf/error.h>

#include "codec/compressed_block.h"
#include "core/allocation.h"
#include "core/parallel.h"
#include "core/scratch_buffer.h"
#include "model/pixel_layout.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>

namespace openxisf::detail {

namespace {

// Runs of a region separated by less than this are read in one read, through the staging buffer: a read of a few KiB
// from a solid-state disk costs about what a sequential read of 64 KiB costs.
constexpr std::uint64_t coalescing_gap = std::uint64_t{64} << 10;

// A group whose runs hold at least this share of the bytes between its first and its last byte, a quarter, is read
// through whatever its gaps, which a rotating disk reads faster in one stretch.
constexpr std::uint64_t dense_share = 4;

// The subblocks of a region decoded at once hold at most this many bytes, unless one alone holds more.
constexpr std::uint64_t decoding_budget = std::uint64_t{1} << 30;

// The progress of the read of an available block, which reports to options.progress and throws cancelled_error when it
// returns false, out of total units of work; empty without options.progress. It is first called before anything is
// read.
block_progress start_progress(const pixel_read_options& options, const data_block& block, std::uint64_t total)
{
    if (!options.progress) {
        return {};
    }
    block_progress progress = [&options, &block, total](std::uint64_t done) {
        if (!options.progress(done, total)) {
            throw cancelled_error(errc::cancelled, "the read of the pixel data was cancelled", {.element = block.path});
        }
    };
    progress(0);
    return progress;
}

// The data of the block of the image at index, in native byte order. options.progress follows the read and can cancel
// it.
std::vector<std::byte> read_native(const unit& opened, std::size_t index, const image_info& info,
                                   const pixel_read_options& options, std::size_t piece_size)
{
    const data_block& block = opened.pixel_block(index);
    if (!block.descriptor) {
        // Throws the error of the unavailable block, before anything is read.
        return read_block(opened.source, block, opened.limits);
    }
    const block_descriptor& descriptor = *block.descriptor;
    std::vector<std::byte> data =
        read_block(opened.source, block, opened.limits, start_progress(options, block, work_size(descriptor)),
                   piece_size, options.verify_checksum);
    to_native_byte_order(data, info.sample_format, descriptor.order);
    return data;
}

// Reads the data of the block of the image at index into destination, which has their size, in native byte order:
// straight from the source when the block is not compressed, so that no buffer of the size of the image is needed.
void read_native_into(const unit& opened, std::size_t index, const image_info& info, std::span<std::byte> destination,
                      const pixel_read_options& options, std::size_t piece_size)
{
    const data_block& block = opened.pixel_block(index);
    if (!block.descriptor) {
        // Throws the error of the unavailable block, before anything is read.
        read_block_into(opened.source, block, opened.limits, destination);
        return;
    }
    const block_descriptor& descriptor = *block.descriptor;
    read_block_into(opened.source, block, opened.limits, destination,
                    start_progress(options, block, work_size(descriptor)), piece_size, options.verify_checksum);
    to_native_byte_order(destination, info.sample_format, descriptor.order);
}

// True when the samples must change their storage model to the one asked for. A single channel is stored the same way
// in both models.
bool converts_storage(const image_info& info, const pixel_read_options& options) noexcept
{
    return options.storage.value_or(info.pixel_storage) != info.pixel_storage && info.geometry.channels > 1;
}

void convert(const image_info& info, std::span<const std::byte> data, std::span<std::byte> destination)
{
    convert_storage(data, destination, info.pixel_storage, info.geometry.pixel_count(), info.geometry.channels,
                    sample_size(info.sample_format));
}

// True for a read of the whole image with its rows one after the other, as reads were before regions.
bool reads_whole_image(const pixel_read_options& options) noexcept
{
    return !options.region && options.pitch == 0;
}

// The layout of a read of the image with options, whose region and pitch it checks: usage_error when they do not fit
// the image.
region_layout read_layout(const image_info& info, const pixel_read_options& options)
{
    return layout_of(info, options.region, options.storage, options.pitch);
}

// What a message calls the destination of a read with a region or a pitch.
std::string region_of(std::size_t index, const pixel_read_options& options)
{
    return (options.region ? "the region of image " : "image ") + std::to_string(index);
}

// Copies the bytes of the region that window holds, the bytes of the block from start on, into place, from the run of
// cursor on, those before it having been copied from earlier windows. The cursor stays at the first run that goes on
// after the window.
void gather_window(std::span<const std::byte> window, std::uint64_t start, region_runs::cursor& run,
                   const region_runs& runs, const region_sink& sink) noexcept
{
    const std::uint64_t end = start + window.size();
    const std::uint64_t length = runs.length();
    for (; !run.done() && run.offset() < end; run.next()) {
        const std::uint64_t from = std::max(run.offset(), start);
        const std::uint64_t to = std::min(run.offset() + length, end);
        if (from < to) {
            sink.put(run.position() + (from - run.offset()), window.subspan(from - start, to - from));
        }
        if (run.offset() + length > end) {
            return;
        }
    }
}

// The stored bytes of a block stored apart, in the file that holds them, which a read of a region reads in parts.
struct stored_bytes
{
    const thread_safe_source& source;
    const block_descriptor& descriptor;

    // Reads the bytes from offset on into destination.
    void read(std::uint64_t offset, std::span<std::byte> destination) const
    {
        read_stored_part(source, descriptor, offset, destination);
    }
};

// Reads destination from the bytes at offset of block, in pieces of piece_size bytes with a progress function, which
// counts them after done.
void read_in_pieces(const stored_bytes& block, std::uint64_t offset, std::span<std::byte> destination,
                    const block_progress& progress, std::size_t piece_size, std::uint64_t done)
{
    if (!progress) {
        block.read(offset, destination);
        return;
    }
    for (std::size_t at = 0; at < destination.size();) {
        const std::size_t piece = std::min(std::max<std::size_t>(piece_size, 1), destination.size() - at);
        block.read(offset + at, destination.subspan(at, piece));
        at += piece;
        progress(done + at);
    }
}

// Reads the runs of a region of an uncompressed block into sink. The runs that gaps of less than coalescing_gap
// separate, and those of a group that holds at least a dense_share-th of its bytes, are read together into a staging
// buffer of piece_size bytes and copied from it, unless the source is shared with writers, which may be writing the
// bytes between the runs: each run is then read alone. A run read alone goes straight into the destination when it
// is one piece there. progress counts the bytes of the region.
void read_runs(const stored_bytes& block, const region_runs& runs, const region_sink& sink,
               const block_progress& progress, std::size_t piece_size)
{
    const std::uint64_t length = runs.length();
    const std::uint64_t per_group = runs.runs_per_group();
    const std::uint64_t staging_size = std::max<std::size_t>(piece_size, 1);
    // Made for the first read that needs it, of no more than the bytes that the region spans.
    std::optional<scratch_buffer> staging;
    const auto staged = [&]() -> std::span<std::byte> {
        if (!staging) {
            staging.emplace(std::min(staging_size, runs.group_end(runs.groups() - 1) - runs.group_start(0)));
        }
        return staging->bytes();
    };
    const bool alone = block.source.shared_with_writers();
    region_runs::cursor run = runs.at(0);
    region_runs::cursor first = run;
    std::uint64_t group = runs.groups();
    bool dense = false;
    std::uint64_t done = 0;
    while (!run.done()) {
        if (run.index() / per_group != group) {
            group = run.index() / per_group;
            dense = per_group * length >= (runs.group_end(group) - runs.group_start(group)) / dense_share;
        }
        // The runs read together: those that follow within the gap, or within a dense group, in a staging buffer.
        first = run;
        const std::uint64_t start = run.offset();
        std::uint64_t end = start + length;
        std::uint64_t count = 1;
        for (run.next(); !run.done(); run.next()) {
            const bool through =
                !alone && ((dense && run.index() / per_group == group) || run.offset() - end < coalescing_gap);
            if (!through || run.offset() + length - start > staging_size) {
                break;
            }
            end = run.offset() + length;
            ++count;
        }
        if (const std::span<std::byte> direct =
                count == 1 ? sink.contiguous(first.position(), length) : std::span<std::byte>();
            !direct.empty()) {
            // Its progress is that of its pieces.
            read_in_pieces(block, start, direct, progress, piece_size, done);
            done += length;
            continue;
        }
        if (count > 1) {
            const std::span<std::byte> piece = staged().first(end - start);
            block.read(start, piece);
            for (std::uint64_t k = 0; k < count; ++k, first.next()) {
                sink.put(first.position(), piece.subspan(first.offset() - start, length));
            }
        } else {
            // A run of rows of the destination that a pitch separates, through the staging buffer.
            const std::span<std::byte> buffer = staged();
            for (std::uint64_t at = 0; at < length;) {
                const std::span<std::byte> piece = buffer.first(std::min<std::uint64_t>(length - at, buffer.size()));
                block.read(start + at, piece);
                sink.put(first.position() + at, piece);
                at += piece.size();
            }
        }
        done += count * length;
        if (progress) {
            progress(done);
        }
    }
}

// True when a group of the region holds a byte of the block in [first, end).
bool holds(const region_runs& runs, std::uint64_t first, std::uint64_t end) noexcept
{
    if (first >= end) {
        return false;
    }
    // The groups are in order and apart: the first that ends after first is the only one that can.
    std::uint64_t low = 0;
    std::uint64_t high = runs.groups();
    while (low < high) {
        const std::uint64_t middle = low + ((high - low) / 2);
        if (runs.group_end(middle) > first) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return low < runs.groups() && runs.group_start(low) < end;
}

// A block shuffled by items of item_size bytes (spec §10.6.3): byte j of item i is at j × items + i, and the bytes
// after the last complete item, from tail() on, stay where they are.
struct shuffled_layout
{
    std::uint64_t item_size = 0;
    std::uint64_t items = 0;

    [[nodiscard]] std::uint64_t tail() const noexcept
    {
        return item_size * items;
    }
};

// True when a group of the region holds a byte of an item in [first, end).
bool holds_items(const region_runs& runs, const shuffled_layout& layout, std::uint64_t first,
                 std::uint64_t end) noexcept
{
    return holds(runs, first * layout.item_size, end * layout.item_size);
}

// True when the bytes [first, end) of a shuffled block hold one of the region: a byte of an item that a group holds,
// for each byte of an item that they hold, or one of the bytes that stay where they are.
bool holds_shuffled(const region_runs& runs, const shuffled_layout& layout, std::uint64_t first,
                    std::uint64_t end) noexcept
{
    if (first >= end) {
        return false;
    }
    const std::uint64_t tail = layout.tail();
    if (end > tail && holds(runs, std::max(first, tail), end)) {
        return true;
    }
    if (first >= tail) {
        return false;
    }
    // The bytes of items: in rows of one byte of every item, from row to last_row.
    const std::uint64_t last = std::min(end, tail) - 1;
    const std::uint64_t row = first / layout.items;
    const std::uint64_t last_row = last / layout.items;
    const std::uint64_t from = first - (row * layout.items);
    const std::uint64_t to = last - (last_row * layout.items) + 1;
    if (row == last_row) {
        return holds_items(runs, layout, from, to);
    }
    if (last_row > row + 1) {
        return holds_items(runs, layout, 0, layout.items);
    }
    return holds_items(runs, layout, from, layout.items) || holds_items(runs, layout, 0, to);
}

// Copies the bytes of the region that window holds, the bytes of a shuffled block from start on, into place, byte by
// byte: byte j of the items [x, y) that the window holds is byte j of each of them in the region.
void gather_shuffled(std::span<const std::byte> window, std::uint64_t start, const region_runs& runs,
                     const shuffled_layout& layout, region_runs::cursor& run, const region_sink& sink) noexcept
{
    const std::uint64_t end = start + window.size();
    const std::uint64_t size = layout.item_size;
    const std::uint64_t items = layout.items;
    const std::uint64_t length = runs.length();
    for (std::uint64_t j = start / items; j < size && j * items < end; ++j) {
        const std::uint64_t x = std::max(start, j * items) - (j * items);
        const std::uint64_t y = std::min(end, (j + 1) * items) - (j * items);
        // Byte j of item i is byte i × size + j of the block: the runs that hold one of those of [x, y).
        const std::uint64_t last = ((y - 1) * size) + j;
        for (run.move_to(runs.first_ending_after((x * size) + j)); !run.done() && run.offset() <= last; run.next()) {
            const std::uint64_t first_byte = run.offset();
            const std::uint64_t last_byte = first_byte + length - 1;
            if (last_byte < j) {
                continue;
            }
            // The items whose byte j the run holds.
            const std::uint64_t low = std::max(x, first_byte > j ? ((first_byte - j - 1) / size) + 1 : 0);
            const std::uint64_t high = std::min(y, ((last_byte - j) / size) + 1);
            for (std::uint64_t i = low; i < high; ++i) {
                sink.at(run.position() + (i * size) + j - first_byte) = window[(j * items) + i - start];
            }
        }
    }
    if (end > layout.tail()) {
        const std::uint64_t from = std::max(start, layout.tail());
        run.move_to(runs.first_ending_after(from));
        gather_window(window.subspan(from - start), from, run, runs, sink);
    }
}

// The sum, or the largest value when it does not fit: a progress total, which a read that large never reaches.
std::uint64_t add_saturated(std::uint64_t a, std::uint64_t b) noexcept
{
    return b > std::numeric_limits<std::uint64_t>::max() - a ? std::numeric_limits<std::uint64_t>::max() : a + b;
}

// Decodes the subblocks of a compressed block, described by descriptor and compressed as compression says, that hold
// bytes of the region, a batch of as many as the threads at a time, into one buffer that every batch reuses, and
// copies the region from them: only the stored bytes of those subblocks are read, unless the whole block is verified
// first, and none when they are in memory. In a shuffled block, the bytes of the samples of the region are spread over
// it: the subblocks that hold any byte of an item of the region are decoded. The errors have no context.
void decode_region(const unit& opened, const data_block& block, const block_descriptor& descriptor,
                   const block_compression& compression, const region_runs& runs, const region_sink& sink,
                   const pixel_read_options& options, std::size_t piece_size)
{
    const std::uint64_t stored = stored_size(descriptor);
    const shuffled_layout layout{
        .item_size = compression.item_size,
        .items = compression.item_size > 1 ? compression.uncompressed_size / compression.item_size : 0};
    // A block of no complete item is stored as it is.
    const bool shuffled = layout.items > 0;
    const std::vector<subblock_span> spans = covering_subblocks(
        compression, stored,
        [&](std::uint64_t first, std::uint64_t end) {
            return shuffled ? holds_shuffled(runs, layout, first, end) : holds(runs, first, end);
        },
        concurrency(), decoding_budget);

    // The bytes in memory: those of an inline or embedded block, verified when the unit was opened, or those of a
    // source that gives its content in memory, verified first unless the application opts out. Otherwise those of a
    // block stored apart, read whole and verified first, unless the application opts out, or read subblock by subblock.
    const bool apart = is_stored_apart(descriptor.location.kind);
    std::span<const std::byte> held = stored_in_memory(opened.source, descriptor);
    const bool from_source = apart && held.empty();
    const bool verify = apart && descriptor.checksum && options.verify_checksum;
    const bool in_parts = from_source && !verify;
    std::uint64_t total = verify && from_source ? stored : 0;
    std::uint64_t largest = 0;
    std::uint64_t largest_stored = 0;
    for (const subblock_span& span : spans) {
        total = add_saturated(total, add_saturated(span.size, in_parts ? span.stored_size : 0));
        largest = std::max(largest, span.size);
        largest_stored = std::max(largest_stored, span.stored_size);
    }
    const block_progress progress = start_progress(options, block, total);
    std::uint64_t done = 0;
    std::optional<scratch_buffer> whole;
    if (verify && from_source) {
        whole.emplace(read_stored_buffer(opened.source, descriptor, opened.limits, progress, piece_size));
        held = whole->bytes();
        done = stored;
    } else if (verify) {
        verify_stored(descriptor, held);
    }
    check_allocation(largest, opened.limits, "the subblocks that a read of the region decodes at once take");
    scratch_buffer data(largest);
    std::optional<scratch_buffer> input;
    if (in_parts) {
        check_allocation(largest_stored, opened.limits, "the subblocks that a read of the region reads at once take");
        input.emplace(largest_stored);
    }

    region_runs::cursor run = runs.at(0);
    for (const subblock_span& span : spans) {
        std::span<const std::byte> bytes;
        if (!in_parts) {
            bytes = held.subspan(span.stored_offset, span.stored_size);
        } else {
            const std::span<std::byte> read = input->bytes().first(span.stored_size);
            read_stored_part(opened.source, descriptor, span.stored_offset, read);
            bytes = read;
            done += span.stored_size;
            if (progress) {
                progress(done);
            }
        }
        const std::span<std::byte> decoded = data.bytes().first(span.size);
        decompress_subblocks_into(
            bytes, compression, span, opened.limits, decoded,
            progress ? subblock_progress([&progress, done](std::uint64_t part) { progress(done + part); })
                     : subblock_progress());
        done += span.size;
        if (shuffled) {
            gather_shuffled(decoded, span.offset, runs, layout, run, sink);
        } else {
            gather_window(decoded, span.offset, run, runs, sink);
        }
    }
}

// Copies the region of an available block, which descriptor describes, into sink, in the byte order of the block, by
// the path that the block takes: decoded subblock by subblock when it is compressed; gathered from its bytes when they
// are in memory, those of an inline or embedded block, verified when the unit was opened, or those of a source that
// gives its content in memory, verified first unless the application opts out; gathered from the bytes of a block with
// a checksum, read whole and verified first, unless the application opts out; and otherwise read run by run. The
// errors have no context.
void gather(const unit& opened, const data_block& block, const block_descriptor& descriptor, const region_runs& runs,
            const region_sink& sink, const pixel_read_options& options, std::size_t piece_size)
{
    if (descriptor.compression) {
        decode_region(opened, block, descriptor, *descriptor.compression, runs, sink, options, piece_size);
        return;
    }
    const std::uint64_t bytes = runs.count() * runs.length();
    region_runs::cursor run = runs.at(0);
    const bool apart = is_stored_apart(descriptor.location.kind);
    if (const std::span<const std::byte> held = stored_in_memory(opened.source, descriptor); !held.empty() || !apart) {
        const block_progress progress = start_progress(options, block, bytes);
        if (apart && options.verify_checksum) {
            verify_stored(descriptor, held);
        }
        gather_window(held, 0, run, runs, sink);
        if (progress) {
            progress(bytes);
        }
        return;
    }
    if (descriptor.checksum && options.verify_checksum) {
        // Spec §10.5: the checksum covers the whole block, which is verified before any of it is used.
        const block_progress progress = start_progress(options, block, stored_size(descriptor));
        const scratch_buffer stored =
            read_stored_buffer(opened.source, descriptor, opened.limits, progress, piece_size);
        gather_window(stored.bytes(), 0, run, runs, sink);
        return;
    }
    read_runs({.source = opened.source, .descriptor = descriptor}, runs, sink, start_progress(options, block, bytes),
              piece_size);
}

// Puts the samples of the region in sink, stored in the byte order order, in the byte order of the host, run by run:
// the bytes between the rows of the destination are not the library's.
void region_to_native_byte_order(const region_runs& runs, const region_sink& sink, sample_format format,
                                 byte_order order)
{
    if ((order == byte_order::little) == (std::endian::native == std::endian::little)) {
        return;
    }
    for (region_runs::cursor run = runs.at(0); !run.done(); run.next()) {
        sink.for_each_piece(run.position(), runs.length(), [format, order](std::span<std::byte> piece) {
            to_native_byte_order(piece, format, order);
        });
    }
}

// Reads the pixel data of the image at index with the layout of a region or a pitch into destination, which has the
// size of the layout. A layout of the whole image is read as a whole image.
void read_region_into(const unit& opened, std::size_t index, const image_info& info, const region_layout& layout,
                      std::span<std::byte> destination, const pixel_read_options& options, std::size_t piece_size)
{
    if (layout.whole) {
        if (layout.converts) {
            convert(info, read_native(opened, index, info, options, piece_size), destination);
        } else {
            read_native_into(opened, index, info, destination, options, piece_size);
        }
        return;
    }
    const data_block& block = opened.pixel_block(index);
    if (!block.descriptor) {
        // Throws the error of the unavailable block, before anything is read.
        check_available(block);
        return;
    }
    const block_descriptor& descriptor = *block.descriptor;
    const std::size_t size = sample_size(info.sample_format);
    const region_runs runs(info.geometry, size, info.pixel_storage, layout.box);
    if (!layout.converts) {
        const region_sink sink(destination, layout.rows.row, layout.rows.pitch);
        with_context_of(block, [&] { gather(opened, block, descriptor, runs, sink, options, piece_size); });
        region_to_native_byte_order(runs, sink, info.sample_format, descriptor.order);
        return;
    }
    // The region in the storage model of the image, in a buffer of its size, then converted into the rows of the
    // destination.
    const std::uint64_t bytes = layout.box.samples * size;
    scratch_buffer region = with_context_of(block, [&] {
        check_allocation(bytes, opened.limits, "the region of the image has");
        scratch_buffer held(bytes);
        gather(opened, block, descriptor, runs, region_sink(held.bytes(), bytes, bytes), options, piece_size);
        return held;
    });
    to_native_byte_order(region.bytes(), info.sample_format, descriptor.order);
    // The rows of the buffer follow each other, in the storage model of the image.
    const std::uint64_t stored_row = rows_of(layout.box, size, info.pixel_storage, 0).row;
    convert_storage(region.bytes(), destination, info.pixel_storage, layout.box.samples / layout.box.channel_count,
                    layout.box.channel_count, size, layout.box.size.front(), stored_row, layout.rows.pitch);
}

} // namespace

const image_info& image_at(const unit& opened, std::size_t index)
{
    if (index >= opened.images.infos.size()) {
        throw usage_error(errc::invalid_argument, "there is no image " + std::to_string(index) + " in a unit of " +
                                                      std::to_string(opened.images.infos.size()) + " images");
    }
    return opened.images.infos[index];
}

block_info block_of(const unit& opened, std::size_t index)
{
    (void)image_at(opened, index);
    const data_block& block = opened.pixel_block(index);
    if (!block.descriptor) {
        // Throws the error of the unavailable block, as a read does.
        check_available(block);
        return {};
    }
    return info_of(*block.descriptor);
}

std::vector<std::byte> read_pixels(const unit& opened, std::size_t index, const pixel_read_options& options,
                                   std::size_t piece_size)
{
    const image_info& info = image_at(opened, index);
    if (reads_whole_image(options)) {
        std::vector<std::byte> data = read_native(opened, index, info, options, piece_size);
        if (!converts_storage(info, options)) {
            return data;
        }
        std::vector<std::byte> converted(data.size());
        convert(info, data, converted);
        return converted;
    }
    const region_layout layout = read_layout(info, options);
    const data_block& block = opened.pixel_block(index);
    check_available(block);
    with_context_of(block, [&] {
        check_allocation(layout.size, opened.limits, "the pixel data of " + region_of(index, options) + " have");
    });
    std::vector<std::byte> data(layout.size);
    read_region_into(opened, index, info, layout, data, options, piece_size);
    return data;
}

void read_pixels(const unit& opened, std::size_t index, std::span<std::byte> destination,
                 const pixel_read_options& options, std::size_t piece_size)
{
    const image_info& info = image_at(opened, index);
    if (reads_whole_image(options)) {
        // The size was checked against the data block when the unit was opened, so it fits in 64 bits.
        const std::uint64_t size = info.data_size();
        if (destination.size() != size) {
            throw usage_error(errc::invalid_argument, "the destination has " + std::to_string(destination.size()) +
                                                          " bytes, and the pixel data of image " +
                                                          std::to_string(index) + " have " + std::to_string(size));
        }
        if (converts_storage(info, options)) {
            convert(info, read_native(opened, index, info, options, piece_size), destination);
        } else {
            read_native_into(opened, index, info, destination, options, piece_size);
        }
        return;
    }
    const region_layout layout = read_layout(info, options);
    if (options.pitch == 0 ? destination.size() != layout.size : destination.size() < layout.size) {
        throw usage_error(errc::invalid_argument,
                          "the destination has " + std::to_string(destination.size()) + " bytes, and " +
                              (options.pitch == 0 ? "the pixel data of " + region_of(index, options) + " have "
                                                  : "the rows of " + region_of(index, options) + " span ") +
                              std::to_string(layout.size));
    }
    read_region_into(opened, index, info, layout, destination.first(layout.size), options, piece_size);
}

const image_info& typed_image_at(const unit& opened, std::size_t index, sample_format format)
{
    const image_info& info = image_at(opened, index);
    if (info.sample_format != format) {
        throw usage_error(errc::invalid_argument, "the samples of image " + std::to_string(index) + " are " +
                                                      std::string(sample_format_name(info.sample_format)) +
                                                      " values, not " + std::string(sample_format_name(format)));
    }
    return info;
}

std::size_t typed_sample_count(const unit& opened, std::size_t index, sample_format format)
{
    const image_info& info = typed_image_at(opened, index, format);
    check_available(opened.pixel_block(index));
    const std::uint64_t size = info.data_size();
    check_allocation(size, opened.limits, "the pixel data of image " + std::to_string(index) + " have");
    return info.geometry.sample_count();
}

std::size_t typed_sample_count(const unit& opened, std::size_t index, sample_format format,
                               const pixel_read_options& options)
{
    if (reads_whole_image(options)) {
        return typed_sample_count(opened, index, format);
    }
    const image_info& info = typed_image_at(opened, index, format);
    const region_layout layout = read_layout(info, options);
    check_available(opened.pixel_block(index));
    check_allocation(layout.size, opened.limits, "the pixel data of " + region_of(index, options) + " have");
    return layout.size / sample_size(format);
}

} // namespace openxisf::detail
