// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/pixel_layout.h"

#include <openxisf/error.h>

#include "core/endian.h"
#include "core/parallel.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <string>

namespace openxisf::detail {

std::size_t byte_order_item_size(sample_format format) noexcept
{
    const std::size_t size = sample_size(format);
    return format == sample_format::complex32 || format == sample_format::complex64 ? size / 2 : size;
}

void to_native_byte_order(std::span<std::byte> data, sample_format format, byte_order order) noexcept
{
    const bool native = (order == byte_order::little) == (std::endian::native == std::endian::little);
    if (!native) {
        swap_byte_order(data, byte_order_item_size(format));
    }
}

namespace {

// A conversion between the storage models: pixel_count × channels samples of sample_size bytes, stored in the model
// from at source, copied in the other one to destination.
struct conversion
{
    const std::byte* source = nullptr;
    std::byte* destination = nullptr;
    pixel_storage from = pixel_storage::planar;
    std::size_t pixel_count = 0;
    std::size_t channels = 0;
    std::size_t sample_size = 0;
};

// Converts the samples of the pixels [first, end). Sample p of channel c is at c × pixel_count + p in the planar model,
// and at p × channels + c in the normal one; the copy walks the destination in order. SampleSize is the sample size
// when it is known at compile time, which makes each copy a single move, and 0 otherwise.
template <std::size_t SampleSize>
void convert_pixels(const conversion& job, std::size_t first, std::size_t end) noexcept
{
    const std::size_t size = SampleSize == 0 ? job.sample_size : SampleSize;
    if (job.from == pixel_storage::planar) {
        std::byte* out = job.destination + (first * job.channels * size);
        for (std::size_t p = first; p < end; ++p) {
            for (std::size_t c = 0; c < job.channels; ++c) {
                std::memcpy(out, job.source + (((c * job.pixel_count) + p) * size), size);
                out += size;
            }
        }
    } else {
        for (std::size_t c = 0; c < job.channels; ++c) {
            std::byte* out = job.destination + (((c * job.pixel_count) + first) * size);
            for (std::size_t p = first; p < end; ++p) {
                std::memcpy(out, job.source + (((p * job.channels) + c) * size), size);
                out += size;
            }
        }
    }
}

void convert_pixels(const conversion& job, std::size_t first, std::size_t end) noexcept
{
    // The sizes of the sample formats.
    switch (job.sample_size) {
    case 1:
        convert_pixels<1>(job, first, end);
        return;
    case 2:
        convert_pixels<2>(job, first, end);
        return;
    case 4:
        convert_pixels<4>(job, first, end);
        return;
    case 8:
        convert_pixels<8>(job, first, end);
        return;
    case 16:
        convert_pixels<16>(job, first, end);
        return;
    default:
        convert_pixels<0>(job, first, end);
        return;
    }
}

} // namespace

void convert_storage(std::span<const std::byte> source, std::span<std::byte> destination, pixel_storage from,
                     std::size_t pixel_count, std::size_t channels, std::size_t sample_size)
{
    if (channels == 1) {
        // Both models store a single channel the same way.
        std::ranges::copy(source, destination.begin());
        return;
    }
    const conversion job{.source = source.data(),
                         .destination = destination.data(),
                         .from = from,
                         .pixel_count = pixel_count,
                         .channels = channels,
                         .sample_size = sample_size};
    // The pixels are converted in pieces of about 1 MiB, in parallel with oneTBB.
    const std::size_t piece = std::max<std::size_t>(1, (std::size_t{1} << 20) / (channels * sample_size));
    parallel_for((pixel_count + piece - 1) / piece, [&job, piece, pixel_count](std::size_t k) {
        convert_pixels(job, k * piece, std::min(pixel_count, (k + 1) * piece));
    });
}

namespace {

// A conversion of a region between rows that start source_pitch bytes apart and rows that start destination_pitch bytes
// apart: a conversion of pixel_count pixels in rows of row_length.
struct row_conversion
{
    conversion pixels{};
    std::size_t row_length = 0;
    std::size_t source_pitch = 0;
    std::size_t destination_pitch = 0;
};

// Converts the rows [first, end) of pixels, as convert_pixels() converts pixels. Row r holds the pixels from
// r × row_length on; on the planar side, channel c has its rows after those of channel c - 1.
template <std::size_t SampleSize>
void convert_rows(const row_conversion& job, std::size_t first, std::size_t end) noexcept
{
    const conversion& pixels = job.pixels;
    const std::size_t size = SampleSize == 0 ? pixels.sample_size : SampleSize;
    const std::size_t rows = pixels.pixel_count / job.row_length;
    for (std::size_t r = first; r < end; ++r) {
        if (pixels.from == pixel_storage::planar) {
            std::byte* out = pixels.destination + (r * job.destination_pitch);
            for (std::size_t i = 0; i < job.row_length; ++i) {
                for (std::size_t c = 0; c < pixels.channels; ++c) {
                    std::memcpy(out, pixels.source + (((c * rows) + r) * job.source_pitch) + (i * size), size);
                    out += size;
                }
            }
        } else {
            const std::byte* in = pixels.source + (r * job.source_pitch);
            for (std::size_t c = 0; c < pixels.channels; ++c) {
                std::byte* out = pixels.destination + (((c * rows) + r) * job.destination_pitch);
                for (std::size_t i = 0; i < job.row_length; ++i) {
                    std::memcpy(out, in + (((i * pixels.channels) + c) * size), size);
                    out += size;
                }
            }
        }
    }
}

void convert_rows(const row_conversion& job, std::size_t first, std::size_t end) noexcept
{
    switch (job.pixels.sample_size) {
    case 1:
        convert_rows<1>(job, first, end);
        return;
    case 2:
        convert_rows<2>(job, first, end);
        return;
    case 4:
        convert_rows<4>(job, first, end);
        return;
    case 8:
        convert_rows<8>(job, first, end);
        return;
    case 16:
        convert_rows<16>(job, first, end);
        return;
    default:
        convert_rows<0>(job, first, end);
        return;
    }
}

} // namespace

void convert_storage(std::span<const std::byte> source, std::span<std::byte> destination, pixel_storage from,
                     std::size_t pixel_count, std::size_t channels, std::size_t sample_size, std::size_t row_length,
                     std::size_t source_pitch, std::size_t destination_pitch)
{
    const row_conversion job{.pixels = {.source = source.data(),
                                        .destination = destination.data(),
                                        .from = from,
                                        .pixel_count = pixel_count,
                                        .channels = channels,
                                        .sample_size = sample_size},
                             .row_length = row_length,
                             .source_pitch = source_pitch,
                             .destination_pitch = destination_pitch};
    // The rows are converted in pieces of about 1 MiB, in parallel with oneTBB.
    const std::size_t rows = pixel_count / row_length;
    const std::size_t piece = std::max<std::size_t>(1, (std::size_t{1} << 20) / (row_length * channels * sample_size));
    parallel_for((rows + piece - 1) / piece,
                 [&job, piece, rows](std::size_t k) { convert_rows(job, k * piece, std::min(rows, (k + 1) * piece)); });
}

namespace {

// The product, or usage_error when it does not fit in 64 bits.
std::uint64_t multiply(std::uint64_t a, std::uint64_t b)
{
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        throw usage_error(errc::arithmetic_overflow, "the number of samples of the region does not fit in 64 bits");
    }
    return a * b;
}

[[noreturn]] void refuse_region(const std::string& message)
{
    throw usage_error(errc::invalid_argument, message);
}

} // namespace

region_box resolve_region(const pixel_region& region, const geometry& image)
{
    const std::size_t axes = image.dimensions.size();
    if (!region.origin.empty() && region.origin.size() != axes) {
        refuse_region("the origin of the region has " + std::to_string(region.origin.size()) +
                      " coordinates, and the image " + std::to_string(axes) + " axes");
    }
    if (!region.size.empty() && region.size.size() != axes) {
        refuse_region("the size of the region has " + std::to_string(region.size.size()) + " lengths, and the image " +
                      std::to_string(axes) + " axes");
    }
    region_box box{.origin = region.origin.empty() ? std::vector<std::uint64_t>(axes) : region.origin,
                   .size = std::vector<std::uint64_t>(axes)};
    std::uint64_t samples = 1;
    for (std::size_t a = 0; a < axes; ++a) {
        const std::uint64_t extent = image.dimensions[a];
        const std::uint64_t start = box.origin[a];
        if (start >= extent) {
            refuse_region("the region starts at pixel " + std::to_string(start) + " of axis " + std::to_string(a) +
                          ", which has " + std::to_string(extent));
        }
        const std::uint64_t length = region.size.empty() ? extent - start : region.size[a];
        if (length == 0) {
            refuse_region("the region has no pixels along axis " + std::to_string(a));
        }
        if (length > extent - start) {
            refuse_region("the region of " + std::to_string(length) + " pixels from pixel " + std::to_string(start) +
                          " goes past the end of axis " + std::to_string(a) + ", which has " + std::to_string(extent));
        }
        box.size[a] = length;
        samples = multiply(samples, length);
    }
    if (region.first_channel >= image.channels) {
        refuse_region("the region starts at channel " + std::to_string(region.first_channel) + ", and the image has " +
                      std::to_string(image.channels) + " channels");
    }
    const std::uint64_t channels = image.channels - region.first_channel;
    if (region.channel_count > channels) {
        refuse_region("the region has " + std::to_string(region.channel_count) + " channels from channel " +
                      std::to_string(region.first_channel) + ", and the image " + std::to_string(image.channels));
    }
    box.first_channel = region.first_channel;
    box.channel_count = region.channel_count == 0 ? channels : region.channel_count;
    box.samples = multiply(samples, box.channel_count);
    return box;
}

bool covers_image(const region_box& box, const geometry& image) noexcept
{
    return box.first_channel == 0 && box.channel_count == image.channels && box.size == image.dimensions &&
           std::ranges::all_of(box.origin, [](std::uint64_t start) { return start == 0; });
}

region_rows rows_of(const region_box& box, std::size_t sample_size, pixel_storage storage, std::uint64_t pitch)
{
    // A single channel is stored the same way in both models.
    const bool planar = storage == pixel_storage::planar || box.channel_count == 1;
    // The samples of the region fit in 64 bits, and so does each of its parts.
    std::uint64_t rows = planar ? box.channel_count : 1;
    for (std::size_t a = 1; a < box.size.size(); ++a) {
        rows *= box.size[a];
    }
    const std::uint64_t row = box.size.empty() ? box.channel_count : box.size[0] * (planar ? 1 : box.channel_count);
    if (pitch != 0 && pitch < row) {
        throw usage_error(errc::invalid_argument, "the pitch of " + std::to_string(pitch) +
                                                      " samples is shorter than a row of the region, " +
                                                      std::to_string(row));
    }
    // The pitch of a single row does not matter.
    const std::uint64_t step = pitch == 0 || rows == 1 ? row : pitch;
    constexpr std::uint64_t most = std::numeric_limits<std::uint64_t>::max();
    // The span in samples, ((rows - 1) × step) + row, then in bytes.
    if ((rows > 1 && step > (most - row) / (rows - 1)) || ((((rows - 1) * step) + row) > most / sample_size)) {
        throw usage_error(errc::arithmetic_overflow, "the rows of the region with a pitch of " + std::to_string(pitch) +
                                                         " samples do not fit in 64 bits");
    }
    return {.row = row * sample_size, .count = rows, .pitch = step * sample_size};
}

region_layout layout_of(const image_info& info, const std::optional<pixel_region>& region,
                        std::optional<pixel_storage> storage, std::uint64_t pitch)
{
    const std::size_t size = sample_size(info.sample_format);
    region_layout layout{.box = resolve_region(region.value_or(pixel_region{}), info.geometry)};
    const pixel_storage model = storage.value_or(info.pixel_storage);
    layout.rows = rows_of(layout.box, size, model, pitch);
    layout.converts = model != info.pixel_storage && layout.box.channel_count > 1;
    layout.whole = covers_image(layout.box, info.geometry) && layout.rows.pitch == layout.rows.row;
    // The samples of the region are within those of the image, whose data size fits in 64 bits.
    layout.size = pitch == 0 ? layout.box.samples * size : layout.rows.span();
    return layout;
}

region_runs::region_runs(const geometry& image, std::size_t sample_size, pixel_storage storage, const region_box& box)
{
    // The axes of the samples in the block, the fastest first: the pixel axes, then the channels in planar storage, and
    // the channels first in normal storage. Each has its extent, and the start and the length of the box along it.
    struct box_axis
    {
        std::uint64_t extent = 0;
        std::uint64_t start = 0;
        std::uint64_t length = 0;
    };
    std::vector<box_axis> axes;
    axes.reserve(image.dimensions.size() + 1);
    const box_axis channels{.extent = image.channels, .start = box.first_channel, .length = box.channel_count};
    if (storage == pixel_storage::normal) {
        axes.push_back(channels);
    }
    for (std::size_t a = 0; a < image.dimensions.size(); ++a) {
        axes.push_back({.extent = image.dimensions[a], .start = box.origin[a], .length = box.size[a]});
    }
    if (storage == pixel_storage::planar) {
        axes.push_back(channels);
    }
    // The runs extend along every axis that the box covers whole, and along the first one that it does not; the axes
    // after it repeat them.
    std::size_t along = 0;
    while (along + 1 < axes.size() && axes[along].length == axes[along].extent) {
        ++along;
    }
    std::uint64_t stride = sample_size;
    length_ = sample_size;
    for (std::size_t i = 0; i < axes.size(); ++i) {
        base_ += axes[i].start * stride;
        if (i <= along) {
            length_ *= axes[i].length;
        } else if (axes[i].length > 1) {
            axes_.push_back({.count = axes[i].length, .stride = stride});
            count_ *= axes[i].length;
        }
        stride *= axes[i].extent;
    }
    // The channels are the last axis in planar storage: when the runs repeat along it, each channel has its own.
    if (storage == pixel_storage::planar && along + 1 < axes.size() && box.channel_count > 1) {
        groups_ = box.channel_count;
    }
}

std::uint64_t region_runs::offset(std::uint64_t run) const noexcept
{
    std::uint64_t result = base_;
    for (const axis& along : axes_) {
        result += (run % along.count) * along.stride;
        run /= along.count;
    }
    return result;
}

std::uint64_t region_runs::first_ending_after(std::uint64_t position) const noexcept
{
    // The runs are in the order of their offsets.
    std::uint64_t low = 0;
    std::uint64_t high = count_;
    while (low < high) {
        const std::uint64_t middle = low + ((high - low) / 2);
        if (offset(middle) + length_ > position) {
            high = middle;
        } else {
            low = middle + 1;
        }
    }
    return low;
}

std::uint64_t region_runs::group_start(std::uint64_t group) const noexcept
{
    return offset(group * runs_per_group());
}

std::uint64_t region_runs::group_end(std::uint64_t group) const noexcept
{
    return offset(((group + 1) * runs_per_group()) - 1) + length_;
}

region_runs::cursor region_runs::at(std::uint64_t run) const
{
    cursor result(*this);
    result.move_to(run);
    return result;
}

region_runs::cursor::cursor(const region_runs& runs) : runs_(&runs), coordinates_(runs.axes_.size()) {}

void region_runs::cursor::next() noexcept
{
    ++index_;
    for (std::size_t a = 0; a < coordinates_.size(); ++a) {
        const axis& along = runs_->axes_[a];
        offset_ += along.stride;
        if (++coordinates_[a] < along.count) {
            return;
        }
        offset_ -= along.count * along.stride;
        coordinates_[a] = 0;
    }
}

void region_runs::cursor::move_to(std::uint64_t run) noexcept
{
    index_ = run;
    offset_ = runs_->base_;
    for (std::size_t a = 0; a < coordinates_.size(); ++a) {
        const axis& along = runs_->axes_[a];
        coordinates_[a] = run % along.count;
        offset_ += coordinates_[a] * along.stride;
        run /= along.count;
    }
}

void region_sink::put(std::uint64_t position, std::span<const std::byte> data) const noexcept
{
    for_each_piece(position, data.size(), [&data](std::span<std::byte> piece) {
        std::memcpy(piece.data(), data.data(), piece.size());
        data = data.subspan(piece.size());
    });
}

std::span<std::byte> region_sink::contiguous(std::uint64_t position, std::uint64_t length) const noexcept
{
    if (pitch_ == row_) {
        return destination_.subspan(position, length);
    }
    const std::uint64_t within = position % row_;
    if (within + length > row_) {
        return {};
    }
    return destination_.subspan(((position / row_) * pitch_) + within, length);
}

} // namespace openxisf::detail
