// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/types.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

// Regions of interest computed from the whole pixel data of an image, sample by sample, as an oracle for the reads and
// the writes of regions, independent of the code of the library.

namespace openxisf::test {

/// Calls function(from, to) for each sample of region, in an image of geometry image whose pixel data are in the
/// storage model stored, with from its index there and to its index in rows of the model storage, pitch samples apart,
/// or one after the other when pitch is 0 (pixel_read_options::pitch). Returns the samples that the rows span. The
/// region lies within the image.
template <typename Function>
std::uint64_t for_each_region_sample(const geometry& image, pixel_storage stored, const pixel_region& region,
                                     pixel_storage storage, std::uint64_t pitch, const Function& function)
{
    const std::size_t axes = image.dimensions.size();
    const std::vector<std::uint64_t> origin = region.origin.empty() ? std::vector<std::uint64_t>(axes) : region.origin;
    std::vector<std::uint64_t> size(axes);
    std::uint64_t pixels = 1;
    for (std::size_t a = 0; a < axes; ++a) {
        size[a] = region.size.empty() ? image.dimensions[a] - origin[a] : region.size[a];
        pixels *= size[a];
    }
    const std::uint64_t first = region.first_channel;
    const std::uint64_t selected = region.channel_count == 0 ? image.channels - first : region.channel_count;
    // A single channel has rows of the planar model in either model.
    const bool planar = storage == pixel_storage::planar || selected == 1;
    const std::uint64_t pixel_rows = pixels / size[0];
    const std::uint64_t row = size[0] * (planar ? 1 : selected);
    const std::uint64_t rows = pixel_rows * (planar ? selected : 1);
    const std::uint64_t step = pitch == 0 ? row : pitch;
    const std::uint64_t image_pixels = image.pixel_count();
    std::vector<std::uint64_t> local(axes);
    for (std::uint64_t q = 0; q < pixels; ++q) {
        // Pixel q of the region, the first axis fastest, and its index p in the image.
        std::uint64_t rest = q;
        std::uint64_t p = 0;
        std::uint64_t scale = 1;
        for (std::size_t a = 0; a < axes; ++a) {
            local[a] = rest % size[a];
            rest /= size[a];
            p += (origin[a] + local[a]) * scale;
            scale *= image.dimensions[a];
        }
        for (std::uint64_t k = 0; k < selected; ++k) {
            const std::uint64_t c = first + k;
            const std::uint64_t from =
                stored == pixel_storage::planar ? (c * image_pixels) + p : (p * image.channels) + c;
            const std::uint64_t to = planar ? ((((k * pixel_rows) + (q / size[0])) * step) + local[0])
                                            : ((q / size[0]) * step) + (local[0] * selected) + k;
            function(from, to);
        }
    }
    return ((rows - 1) * step) + row;
}

/// What a read of region gives from whole, the pixel data of an image of geometry image, in the storage model stored
/// and native byte order: the samples of the region in rows of the model storage, pitch samples apart, or one after the
/// other when pitch is 0, with fill between the rows (pixel_read_options::pitch). The region lies within the image.
[[nodiscard]] inline std::vector<std::byte> region_of(std::span<const std::byte> whole, const geometry& image,
                                                      std::size_t sample_size, pixel_storage stored,
                                                      const pixel_region& region, pixel_storage storage,
                                                      std::uint64_t pitch = 0, std::byte fill = std::byte{0})
{
    const std::uint64_t span =
        for_each_region_sample(image, stored, region, storage, pitch, [](std::uint64_t, std::uint64_t) {});
    std::vector<std::byte> result(span * sample_size, fill);
    for_each_region_sample(image, stored, region, storage, pitch, [&](std::uint64_t from, std::uint64_t to) {
        std::memcpy(result.data() + (to * sample_size), whole.data() + (from * sample_size), sample_size);
    });
    return result;
}

/// What a write of region gives in whole, the pixel data of an image of geometry image in the storage model stored and
/// native byte order: whole with the samples of the region taken from data, which holds them as region_of() returns
/// them, in rows of the model storage, pitch samples apart, or one after the other when pitch is 0.
[[nodiscard]] inline std::vector<std::byte> with_region(std::span<const std::byte> whole, const geometry& image,
                                                        std::size_t sample_size, pixel_storage stored,
                                                        const pixel_region& region, std::span<const std::byte> data,
                                                        pixel_storage storage, std::uint64_t pitch = 0)
{
    std::vector<std::byte> result(whole.begin(), whole.end());
    for_each_region_sample(image, stored, region, storage, pitch, [&](std::uint64_t from, std::uint64_t to) {
        std::memcpy(result.data() + (from * sample_size), data.data() + (to * sample_size), sample_size);
    });
    return result;
}

/// Every region of an image of geometry image: each box of every origin and size, with each range of channels.
[[nodiscard]] inline std::vector<pixel_region> every_region(const geometry& image)
{
    std::vector<pixel_region> boxes{pixel_region{.origin = std::vector<std::uint64_t>(image.dimensions.size()),
                                                 .size = std::vector<std::uint64_t>(image.dimensions.size())}};
    // The boxes grow axis by axis: every start and length along the axis, for each box of the axes before it.
    for (std::size_t a = 0; a < image.dimensions.size(); ++a) {
        std::vector<pixel_region> grown;
        for (const pixel_region& box : boxes) {
            for (std::uint64_t start = 0; start < image.dimensions[a]; ++start) {
                for (std::uint64_t length = 1; start + length <= image.dimensions[a]; ++length) {
                    pixel_region next = box;
                    next.origin[a] = start;
                    next.size[a] = length;
                    grown.push_back(next);
                }
            }
        }
        boxes = std::move(grown);
    }
    std::vector<pixel_region> regions;
    for (const pixel_region& box : boxes) {
        for (std::uint64_t first = 0; first < image.channels; ++first) {
            for (std::uint64_t count = 1; first + count <= image.channels; ++count) {
                pixel_region next = box;
                next.first_channel = first;
                next.channel_count = count;
                regions.push_back(next);
            }
        }
    }
    return regions;
}

} // namespace openxisf::test
