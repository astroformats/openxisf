// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/error.h>
#include <openxisf/orientation.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <utility>
#include <vector>

namespace openxisf {

namespace {

void check_two_dimensional(const geometry& size)
{
    if (size.dimensions.size() != 2) {
        throw usage_error(errc::invalid_argument, "an orientation applies to two-dimensional images");
    }
}

// The orientations that rotate by 90 degrees, which exchange the width and the height.
bool exchanges_axes(orientation turn) noexcept
{
    return turn == orientation::rotate_90 || turn == orientation::rotate_90_flip ||
           turn == orientation::rotate_minus_90 || turn == orientation::rotate_minus_90_flip;
}

// Where pixel (x, y) of an image of the given width and height goes in the turned image (spec §11.5.2), the origin
// being the top-left corner and Y growing downwards. A rotation by 90 degrees counter-clockwise takes the top-right
// corner to the top-left one; a horizontal flip exchanges left and right.
std::pair<std::size_t, std::size_t> turned_position(orientation turn, std::size_t x, std::size_t y, std::size_t width,
                                                    std::size_t height) noexcept
{
    const std::size_t right = width - 1 - x;
    const std::size_t bottom = height - 1 - y;
    switch (turn) {
    case orientation::none:
        break;
    case orientation::flip:
        return {right, y};
    case orientation::rotate_90:
        return {y, right};
    case orientation::rotate_90_flip:
        return {bottom, right};
    case orientation::rotate_minus_90:
        return {bottom, x};
    case orientation::rotate_minus_90_flip:
        return {y, x};
    case orientation::rotate_180:
        return {right, bottom};
    case orientation::rotate_180_flip:
        return {x, bottom};
    }
    return {x, y};
}

} // namespace

geometry oriented_geometry(const geometry& size, orientation turn)
{
    check_two_dimensional(size);
    geometry result = size;
    if (exchanges_axes(turn)) {
        std::swap(result.dimensions[0], result.dimensions[1]);
    }
    return result;
}

std::vector<std::byte> orient_pixels(std::span<const std::byte> pixels, const geometry& size, std::size_t sample_size,
                                     pixel_storage storage, orientation turn)
{
    check_two_dimensional(size);
    const std::uint64_t samples = size.sample_count();
    if (sample_size == 0 || samples > std::numeric_limits<std::uint64_t>::max() / sample_size ||
        samples * sample_size != pixels.size()) {
        throw usage_error(errc::invalid_argument, "the pixel data do not hold the samples of the geometry");
    }
    if (pixels.empty()) {
        return {};
    }
    const std::size_t width = size.dimensions[0];
    const std::size_t height = size.dimensions[1];
    const std::size_t turned_width = exchanges_axes(turn) ? height : width;
    // What moves as one: a sample of one channel in planar storage, the samples of a whole pixel in normal storage.
    const std::size_t planes = storage == pixel_storage::planar ? size.channels : 1;
    const std::size_t unit = pixels.size() / planes / width / height;
    const std::size_t plane_size = width * height * unit;
    std::vector<std::byte> result(pixels.size());
    for (std::size_t plane = 0; plane < planes; ++plane) {
        const std::byte* from = pixels.data() + (plane * plane_size);
        std::byte* to = result.data() + (plane * plane_size);
        for (std::size_t y = 0; y < height; ++y) {
            for (std::size_t x = 0; x < width; ++x) {
                const auto [turned_x, turned_y] = turned_position(turn, x, y, width, height);
                std::memcpy(to + (((turned_y * turned_width) + turned_x) * unit), from + (((y * width) + x) * unit),
                            unit);
            }
        }
    }
    return result;
}

} // namespace openxisf
