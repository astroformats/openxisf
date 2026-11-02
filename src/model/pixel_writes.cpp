// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/pixel_writes.h"

#include <openxisf/error.h>

#include "core/allocation.h"
#include "core/data_encoding.h"
#include "core/scratch_buffer.h"
#include "crypto/hash.h"
#include "model/pixels.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <string>

namespace openxisf::detail {

namespace {

// The exception that refuses to write the block of the image at index in place, for the reason why.
unsupported_error not_editable(const data_block& block, std::size_t index, const std::string& why)
{
    return {errc::not_editable,
            "the data block of image " + std::to_string(index) + " cannot be written in place: " + why,
            {.element = block.path}};
}

// What a message calls the data of a write with a region or a pitch.
std::string region_of(std::size_t index, const pixel_write_options& options)
{
    return (options.region ? "the region of image " : "image ") + std::to_string(index);
}

// Reports the progress of a write to options.progress, which can cancel it, out of total bytes, at most once for every
// chunk bytes written and once at the end.
class write_progress
{
public:
    write_progress(const pixel_write_options& options, const data_block& block, std::uint64_t total,
                   std::uint64_t chunk)
        : function_(options.progress), block_(block), total_(total), chunk_(chunk)
    {
        report();
    }

    [[nodiscard]] bool reported() const noexcept
    {
        return static_cast<bool>(function_);
    }

    // Counts bytes more written.
    void advance(std::uint64_t bytes)
    {
        done_ += bytes;
        if (function_ && (done_ - last_ >= chunk_ || done_ == total_)) {
            last_ = done_;
            report();
        }
    }

private:
    void report() const
    {
        if (function_ && !function_(done_, total_)) {
            throw cancelled_error(errc::cancelled, "the write of the pixel data was cancelled",
                                  {.element = block_.path});
        }
    }

    const progress_function& function_;
    const data_block& block_;
    std::uint64_t total_;
    std::uint64_t chunk_;
    std::uint64_t done_ = 0;
    std::uint64_t last_ = 0;
};

// The digest in algorithm of the stored bytes of an attached block, read through device in pieces of piece_size bytes.
std::vector<std::byte> stored_digest(const thread_safe_device& device, const block_descriptor& descriptor,
                                     hash_algorithm algorithm, std::size_t piece_size)
{
    const std::uint64_t size = descriptor.location.size;
    hasher hash(algorithm);
    scratch_buffer piece(std::min<std::uint64_t>(std::max<std::size_t>(piece_size, 1), size));
    for (std::uint64_t offset = 0; offset < size;) {
        const std::span<std::byte> part =
            piece.bytes().first(std::min<std::uint64_t>(piece.bytes().size(), size - offset));
        device.read(descriptor.location.position + offset, part);
        hash.update(part);
        offset += part.size();
    }
    return hash.finish();
}

} // namespace

std::vector<bool> overlapping_blocks(const std::vector<data_block>& blocks)
{
    // The attached blocks that hold bytes, in the order of their starts.
    struct extent
    {
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        std::size_t block = 0;
    };
    std::vector<extent> attached;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        if (const std::optional<block_descriptor>& descriptor = blocks[i].descriptor;
            descriptor && descriptor->location.kind == location_kind::attachment && descriptor->location.size > 0) {
            // The block lies within the file, whose size fits in 64 bits.
            attached.push_back({.start = descriptor->location.position,
                                .end = descriptor->location.position + descriptor->location.size,
                                .block = i});
        }
    }
    std::ranges::sort(attached, {}, &extent::start);
    // A block shares bytes with one before it when it starts before the end of one of them, and with one after it when
    // the next one starts before its end.
    std::vector<bool> overlapping(blocks.size());
    std::uint64_t reach = 0;
    for (std::size_t k = 0; k < attached.size(); ++k) {
        if ((k > 0 && attached[k].start < reach) ||
            (k + 1 < attached.size() && attached[k + 1].start < attached[k].end)) {
            overlapping[attached[k].block] = true;
        }
        reach = std::max(reach, attached[k].end);
    }
    return overlapping;
}

void check_editable(const unit& opened, std::size_t index, const std::vector<bool>& overlapping)
{
    const std::size_t which = opened.images.blocks[index];
    const data_block& block = opened.blocks[which];
    if (!block.descriptor) {
        throw not_editable(block, index,
                           "it is unavailable" + (block.problem ? ": " + block.problem->message : std::string()));
    }
    const block_descriptor& descriptor = *block.descriptor;
    if (descriptor.location.kind != location_kind::attachment) {
        throw not_editable(block, index,
                           is_external(descriptor.location.kind) ? "it is in an external file" : "it is in the header");
    }
    if (descriptor.compression) {
        throw not_editable(block, index, "it is compressed");
    }
    if (overlapping[which]) {
        throw not_editable(block, index, "it shares bytes with another data block, which writing it would change");
    }
    if (descriptor.checksum && !descriptor.digest_offset) {
        throw not_editable(block, index,
                           "its checksum attribute is not written on its element as algorithm:digest, which a new "
                           "digest would replace");
    }
}

region_layout write_layout(const unit& opened, std::size_t index, std::span<const std::byte> data,
                           const pixel_write_options& options)
{
    const image_info& info = image_at(opened, index);
    const region_layout layout = layout_of(info, options.region, options.storage, options.pitch);
    if (options.pitch == 0 ? data.size() != layout.size : data.size() < layout.size) {
        throw usage_error(errc::invalid_argument,
                          "the data have " + std::to_string(data.size()) + " bytes, and " +
                              (options.pitch == 0 ? "the pixel data of " + region_of(index, options) + " have "
                                                  : "the rows of " + region_of(index, options) + " span ") +
                              std::to_string(layout.size));
    }
    return layout;
}

void write_pixels(const unit& opened, const thread_safe_device& device, std::size_t index,
                  std::span<const std::byte> data, const region_layout& layout, const pixel_write_options& options,
                  std::size_t piece_size)
{
    const image_info& info = opened.images.infos[index];
    const data_block& block = opened.blocks[opened.images.blocks[index]];
    if (!block.descriptor) {
        check_available(block);
        return;
    }
    const block_descriptor& descriptor = *block.descriptor;
    const std::size_t size = sample_size(info.sample_format);
    const std::uint64_t bytes = layout.box.samples * size;

    // The samples come from data, in its rows, or from a buffer of the region in the storage model of the image, its
    // rows one after the other, when data hold them in the other model.
    region_source source(data, layout.rows.row, layout.rows.pitch);
    std::optional<scratch_buffer> converted;
    if (layout.converts) {
        with_context_of(block, [&] { check_allocation(bytes, opened.limits, "the region of the image has"); });
        converted.emplace(bytes);
        const pixel_storage given =
            info.pixel_storage == pixel_storage::planar ? pixel_storage::normal : pixel_storage::planar;
        const std::uint64_t stored_row = rows_of(layout.box, size, info.pixel_storage, 0).row;
        convert_storage(data, converted->bytes(), given, layout.box.samples / layout.box.channel_count,
                        layout.box.channel_count, size, layout.box.size.front(), layout.rows.pitch, stored_row);
        source = region_source(converted->bytes(), bytes, bytes);
    }

    // Samples that change their byte order go through a staging buffer, in parts of whole samples, as the bytes do
    // whose progress is reported.
    const bool swaps = (descriptor.order == byte_order::little) != (std::endian::native == std::endian::little);
    const std::uint64_t chunk = std::max<std::uint64_t>(size, (std::max<std::size_t>(piece_size, 1) / size) * size);
    write_progress progress(options, block, bytes, chunk);
    std::optional<scratch_buffer> staging;
    const auto write_piece = [&](std::uint64_t offset, std::span<const std::byte> piece) {
        if (!swaps && !progress.reported()) {
            device.write(offset, piece);
            return;
        }
        for (std::uint64_t from = 0; from < piece.size(); from += chunk) {
            const std::span<const std::byte> part =
                piece.subspan(from, std::min<std::uint64_t>(chunk, piece.size() - from));
            if (swaps) {
                if (!staging) {
                    staging.emplace(std::min(chunk, bytes));
                }
                const std::span<std::byte> swapped = staging->bytes().first(part.size());
                std::ranges::copy(part, swapped.begin());
                to_native_byte_order(swapped, info.sample_format, descriptor.order);
                device.write(offset + from, swapped);
            } else {
                device.write(offset + from, part);
            }
            progress.advance(part.size());
        }
    };

    const region_runs runs(info.geometry, size, info.pixel_storage, layout.box);
    const std::uint64_t base = descriptor.location.position;
    for (region_runs::cursor run = runs.at(0); !run.done(); run.next()) {
        // The pieces of a run come in order, one within each row of the data.
        std::uint64_t at = base + run.offset();
        source.for_each_piece(run.position(), runs.length(), [&](std::span<const std::byte> piece) {
            write_piece(at, piece);
            at += piece.size();
        });
    }
}

void refresh_digest(const thread_safe_device& device, block_descriptor& descriptor, std::size_t piece_size)
{
    if (!descriptor.checksum || !descriptor.digest_offset) {
        return;
    }
    block_checksum& checksum = *descriptor.checksum;
    std::vector<std::byte> digest = stored_digest(device, descriptor, checksum.algorithm, piece_size);
    const std::string text = encode_hex(digest);
    device.write(*descriptor.digest_offset, std::as_bytes(std::span(text)));
    checksum.digest = std::move(digest);
}

void verify_digest(const thread_safe_device& device, const data_block& block, std::size_t piece_size)
{
    if (!block.descriptor || !block.descriptor->checksum) {
        return;
    }
    const block_descriptor& descriptor = *block.descriptor;
    const block_checksum& checksum = *descriptor.checksum;
    if (stored_digest(device, descriptor, checksum.algorithm, piece_size) != checksum.digest) {
        throw integrity_error(errc::checksum_mismatch,
                              "the " + std::string(hash_name(checksum.algorithm)) +
                                  " digest of the data block differs from its checksum, so it is not written",
                              {.element = block.path, .offset = descriptor.location.position});
    }
}

} // namespace openxisf::detail
