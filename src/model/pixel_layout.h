// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/image.h>
#include <openxisf/types.h>

#include "container/block_attributes.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// The layout of pixel data in memory: the byte order of their samples (spec §10.4), their storage model (spec
// §8.5.3), and where the samples of a region of interest are, in a block and in a destination.

namespace openxisf::detail {

/// The size of the items whose byte order a data block gives, for samples of a format: the size of a sample, or of
/// each part of a complex sample, whose parts are stored one after the other.
[[nodiscard]] std::size_t byte_order_item_size(sample_format format) noexcept;

/// Puts the samples of data, of the given format and stored in the given byte order, in the byte order of the host.
void to_native_byte_order(std::span<std::byte> data, sample_format format, byte_order order) noexcept;

/// Copies the samples of source, stored in the model from, into destination in the other model. Both hold pixel_count ×
/// channels samples of sample_size bytes, and they do not overlap. With oneTBB, pieces of the image are copied in
/// parallel.
void convert_storage(std::span<const std::byte> source, std::span<std::byte> destination, pixel_storage from,
                     std::size_t pixel_count, std::size_t channels, std::size_t sample_size);

/// Copies the samples of source, a region of pixel_count pixels of channels samples of sample_size bytes in rows of
/// row_length pixels, stored in the model from, into destination in the other model. The rows of source start
/// source_pitch bytes apart, and those of destination destination_pitch bytes apart (region_rows): a row is that of
/// the model of each. They do not overlap. With oneTBB, pieces of rows are copied in parallel.
void convert_storage(std::span<const std::byte> source, std::span<std::byte> destination, pixel_storage from,
                     std::size_t pixel_count, std::size_t channels, std::size_t sample_size, std::size_t row_length,
                     std::size_t source_pitch, std::size_t destination_pitch);

/// A region of an image, checked against its geometry, with every entry given: the origin and the length of its box
/// along each axis, its channels, and its number of samples.
struct region_box
{
    std::vector<std::uint64_t> origin{};
    std::vector<std::uint64_t> size{};
    std::uint64_t first_channel = 0;
    std::uint64_t channel_count = 0;
    std::uint64_t samples = 0;
};

/// The box of region in an image of geometry image, which pixel_region::sample_count() returns the samples of, with its
/// exceptions.
[[nodiscard]] region_box resolve_region(const pixel_region& region, const geometry& image);

/// True when box holds every sample of an image of geometry image.
[[nodiscard]] bool covers_image(const region_box& box, const geometry& image) noexcept;

/// The rows of a region in a destination, in bytes. A row holds the samples of the region along the first axis: of one
/// channel in the planar model, the rows of each channel after those of the channel before, and of every channel of the
/// region in the normal model, pixel by pixel; the rows follow the order of the pixels along the other axes.
struct region_rows
{
    /// The bytes of a row.
    std::uint64_t row = 0;
    /// The number of rows.
    std::uint64_t count = 0;
    /// The bytes from the start of one row to the start of the next.
    std::uint64_t pitch = 0;

    /// The bytes from the first byte of the first row to the last byte of the last.
    [[nodiscard]] std::uint64_t span() const noexcept
    {
        return ((count - 1) * pitch) + row;
    }
};

/// The rows of box, of samples of sample_size bytes, in a destination in the model storage whose rows start pitch
/// samples apart, or follow each other when pitch is 0. Throws usage_error with errc::invalid_argument when pitch is
/// shorter than a row, and with errc::arithmetic_overflow when the span of the rows does not fit in 64 bits.
[[nodiscard]] region_rows rows_of(const region_box& box, std::size_t sample_size, pixel_storage storage,
                                  std::uint64_t pitch);

/// How a region of an image is laid out in memory of the application, for a read into it or a write from it.
struct region_layout
{
    region_box box{};
    /// The rows of the region there.
    region_rows rows{};
    /// True when the samples there are in the other storage model than the image's.
    bool converts = false;
    /// True for the whole image with its rows one after the other, which is read and written as a whole image.
    bool whole = false;
    /// The bytes there: those of the region, or with a pitch those that its rows span.
    std::uint64_t size = 0;
};

/// The layout of region of an image of info, the whole image when it is empty, in memory in the storage model storage,
/// that of the image when it is empty, with rows pitch samples apart, or one after the other when pitch is 0. Throws
/// usage_error when the region or the pitch do not fit the image.
[[nodiscard]] region_layout layout_of(const image_info& info, const std::optional<pixel_region>& region,
                                      std::optional<pixel_storage> storage, std::uint64_t pitch);

/// The runs of a region in a block of pixel data: the stretches of the block that its samples fill, in the order of the
/// block, all of one length, with those that follow each other merged. A box of whole rows is one run per channel in
/// planar storage, and one in all in normal storage; a channel subset in normal storage is one run per pixel. The runs
/// are computed as they are walked, never listed. In planar storage the runs of each channel of the region make a
/// group, a range of bytes of its own, unless the channels merge into the same runs; otherwise the region is one group.
class region_runs
{
public:
    /// The runs of box, checked against image, in a block of its samples of sample_size bytes in the model storage. The
    /// pixel data of image fit in 64 bits.
    region_runs(const geometry& image, std::size_t sample_size, pixel_storage storage, const region_box& box);

    /// The number of runs.
    [[nodiscard]] std::uint64_t count() const noexcept
    {
        return count_;
    }

    /// The bytes of each run.
    [[nodiscard]] std::uint64_t length() const noexcept
    {
        return length_;
    }

    /// Where run starts in the block, for a run below count().
    [[nodiscard]] std::uint64_t offset(std::uint64_t run) const noexcept;

    /// The first run that ends after position of the block, or count() when none does.
    [[nodiscard]] std::uint64_t first_ending_after(std::uint64_t position) const noexcept;

    /// The number of groups, each of runs_per_group() runs that follow each other.
    [[nodiscard]] std::uint64_t groups() const noexcept
    {
        return groups_;
    }

    [[nodiscard]] std::uint64_t runs_per_group() const noexcept
    {
        return count_ / groups_;
    }

    /// Where group starts in the block, and where it ends: the first byte of its first run, and the byte after its
    /// last run.
    [[nodiscard]] std::uint64_t group_start(std::uint64_t group) const noexcept;
    [[nodiscard]] std::uint64_t group_end(std::uint64_t group) const noexcept;

    /// A run, which walks the runs in order.
    class cursor
    {
    public:
        /// The run, count() once the walk is over.
        [[nodiscard]] std::uint64_t index() const noexcept
        {
            return index_;
        }

        /// Where the run starts in the block, for a run below count().
        [[nodiscard]] std::uint64_t offset() const noexcept
        {
            return offset_;
        }

        /// Where the bytes of the run go among those of the region: index() × length().
        [[nodiscard]] std::uint64_t position() const noexcept
        {
            return index_ * runs_->length_;
        }

        [[nodiscard]] bool done() const noexcept
        {
            return index_ >= runs_->count_;
        }

        /// Moves to the next run.
        void next() noexcept;

        /// Moves to run, below or at count().
        void move_to(std::uint64_t run) noexcept;

    private:
        friend class region_runs;
        explicit cursor(const region_runs& runs);

        const region_runs* runs_;
        std::vector<std::uint64_t> coordinates_;
        std::uint64_t index_ = 0;
        std::uint64_t offset_ = 0;
    };

    /// A cursor at run.
    [[nodiscard]] cursor at(std::uint64_t run) const;

private:
    // An axis along which the runs repeat, from the fastest: the number of runs along it, and the bytes between them.
    struct axis
    {
        std::uint64_t count = 0;
        std::uint64_t stride = 0;
    };

    std::uint64_t base_ = 0;
    std::uint64_t length_ = 0;
    std::uint64_t count_ = 1;
    std::uint64_t groups_ = 1;
    std::vector<axis> axes_{};
};

/// Where the bytes of a region go, in the order of its runs: into a destination whose rows of row bytes start pitch
/// bytes apart (region_rows), pitch at least row.
class region_sink
{
public:
    region_sink(std::span<std::byte> destination, std::uint64_t row, std::uint64_t pitch) noexcept
        : destination_(destination), row_(row), pitch_(pitch)
    {}

    /// Calls function with each piece of the destination of the bytes [position, position + length) of the region, in
    /// order: one piece within each row.
    template <typename Function>
    void for_each_piece(std::uint64_t position, std::uint64_t length, const Function& function) const
    {
        if (pitch_ == row_) {
            function(destination_.subspan(position, length));
            return;
        }
        while (length > 0) {
            const std::uint64_t within = position % row_;
            const std::uint64_t piece = std::min(length, row_ - within);
            function(destination_.subspan(((position / row_) * pitch_) + within, piece));
            position += piece;
            length -= piece;
        }
    }

    /// Copies data, the bytes of the region from position on, into place.
    void put(std::uint64_t position, std::span<const std::byte> data) const noexcept;

    /// The byte of the destination that the byte at position of the region goes to.
    [[nodiscard]] std::byte& at(std::uint64_t position) const noexcept
    {
        return pitch_ == row_ ? destination_[position] : destination_[((position / row_) * pitch_) + (position % row_)];
    }

    /// The destination of the bytes [position, position + length) of the region when it is one piece, or an empty span.
    [[nodiscard]] std::span<std::byte> contiguous(std::uint64_t position, std::uint64_t length) const noexcept;

private:
    std::span<std::byte> destination_;
    std::uint64_t row_;
    std::uint64_t pitch_;
};

/// Where the bytes of a region come from, in the order of its runs: from data whose rows of row bytes start pitch bytes
/// apart (region_rows), pitch at least row, as region_sink puts them.
class region_source
{
public:
    region_source(std::span<const std::byte> data, std::uint64_t row, std::uint64_t pitch) noexcept
        : data_(data), row_(row), pitch_(pitch)
    {}

    /// Calls function with each piece of data of the bytes [position, position + length) of the region, in order: one
    /// piece within each row.
    template <typename Function>
    void for_each_piece(std::uint64_t position, std::uint64_t length, const Function& function) const
    {
        if (pitch_ == row_) {
            function(data_.subspan(position, length));
            return;
        }
        while (length > 0) {
            const std::uint64_t within = position % row_;
            const std::uint64_t piece = std::min(length, row_ - within);
            function(data_.subspan(((position / row_) * pitch_) + within, piece));
            position += piece;
            length -= piece;
        }
    }

private:
    std::span<const std::byte> data_;
    std::uint64_t row_;
    std::uint64_t pitch_;
};

} // namespace openxisf::detail
