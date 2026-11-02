// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Reads the pixels of the first image of a unit into memory that the application owns, interleaved (the normal storage
// model, the samples of each pixel together), while a progress function reports how far the read is, and computes the
// mean of each channel. Then it reads a region of interest, the central part of the first channel, into a tile with a
// margin around it, as a filter that needs the neighbours of each pixel reads it, and checks it against the whole
// image. Last, it starts a read again and cancels it from the progress function, as a user interface does when its
// user changes their mind.
//
//   read_pixels <file.xisf>

#include <openxisf/openxisf.h>

#include "arguments.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

// Prints the progress of a read in whole percents, each one once.
class progress_report
{
public:
    bool operator()(std::uint64_t done, std::uint64_t total)
    {
        const std::uint64_t percent = total == 0 ? 100 : done * 100 / total;
        if (percent != last_) {
            std::cout << "  read " << percent << "%\n";
            last_ = percent;
        }
        return true;
    }

private:
    std::uint64_t last_ = 101;
};

// The samples of the image, interleaved, as values of T, its sample format, in a buffer of the application, and the
// mean of each channel.
template <openxisf::pixel_sample T> std::vector<double> channel_means(const openxisf::reader& file)
{
    const openxisf::image_info& info = file.image(0);
    std::vector<T> samples(info.geometry.sample_count());
    file.read_pixels(0, std::span(samples),
                     {.storage = openxisf::pixel_storage::normal, .progress = progress_report()});

    // Interleaved: sample c of pixel p is at p × channels + c.
    const std::size_t channels = info.geometry.channels;
    std::vector<double> sums(channels);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        sums[i % channels] += static_cast<double>(samples[i]);
    }
    const auto pixels = static_cast<double>(info.geometry.pixel_count());
    for (double& sum : sums) {
        sum /= pixels;
    }
    return sums;
}

std::vector<double> channel_means_of(const openxisf::reader& file)
{
    switch (file.image(0).sample_format) {
    case openxisf::sample_format::uint8:
        return channel_means<std::uint8_t>(file);
    case openxisf::sample_format::uint16:
        return channel_means<std::uint16_t>(file);
    case openxisf::sample_format::uint32:
        return channel_means<std::uint32_t>(file);
    case openxisf::sample_format::uint64:
        return channel_means<std::uint64_t>(file);
    case openxisf::sample_format::float32:
        return channel_means<float>(file);
    case openxisf::sample_format::float64:
        return channel_means<double>(file);
    case openxisf::sample_format::complex32:
    case openxisf::sample_format::complex64:
        break;
    }
    std::cout << "  complex samples have no mean here\n";
    return {};
}

// Reads the central part of the first channel of the image, half of its length along each axis, into a tile with a
// margin of 2 samples around it: its rows land in the tile pitch samples apart, after the margin, and the margin stays
// as it was. True when the region holds the samples of the whole image there.
bool read_region(const openxisf::reader& file)
{
    const openxisf::image_info& info = file.image(0);
    openxisf::pixel_region region{.channel_count = 1};
    for (const std::uint64_t length : info.geometry.dimensions) {
        region.origin.push_back(length / 4);
        region.size.push_back(std::max<std::uint64_t>(1, length / 2));
    }
    constexpr std::uint64_t margin = 2;
    const std::uint64_t pitch = region.size[0] + (2 * margin);
    // The rows of the region: one for each pixel along the other axes.
    const std::uint64_t rows = region.sample_count(info.geometry) / region.size[0];
    const std::size_t size = openxisf::sample_size(info.sample_format);
    std::vector<std::byte> tile((rows + (2 * margin)) * pitch * size);
    const std::size_t start = ((margin * pitch) + margin) * size;
    file.read_pixels(0, std::span(tile).subspan(start), {.region = region, .pitch = pitch});

    // Channel 0 of the whole image, in which pixel p is sample p.
    const std::vector<std::byte> whole = file.read_pixels(0, {.storage = openxisf::pixel_storage::planar});
    for (std::uint64_t r = 0; r < rows; ++r) {
        // The pixel where row r of the region starts: its coordinates along the axes after the first.
        std::uint64_t p = region.origin[0];
        std::uint64_t rest = r;
        std::uint64_t scale = info.geometry.dimensions[0];
        for (std::size_t a = 1; a < region.size.size(); ++a) {
            p += (region.origin[a] + (rest % region.size[a])) * scale;
            rest /= region.size[a];
            scale *= info.geometry.dimensions[a];
        }
        const auto in_tile = tile.begin() + static_cast<std::ptrdiff_t>(start + (r * pitch * size));
        const auto in_image = whole.begin() + static_cast<std::ptrdiff_t>(p * size);
        if (!std::equal(in_tile, in_tile + static_cast<std::ptrdiff_t>(region.size[0] * size), in_image)) {
            return false;
        }
    }
    std::cout << "read a region of " << region.size[0] << " x " << rows << " pixels of channel 0 into a tile of "
              << pitch << " x " << rows + (2 * margin) << '\n';
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> arguments = example::utf8_arguments(argc, argv);
        if (arguments.size() != 2) {
            std::cerr << "usage: read_pixels <file.xisf>\n";
            return 2;
        }
        const std::string& path = arguments[1];
        const openxisf::reader file(path);
        if (file.images().empty()) {
            std::cerr << "the unit has no image\n";
            return 1;
        }

        std::cout << "reading " << file.image(0).data_size() << " bytes of pixel data\n";
        const std::vector<double> means = channel_means_of(file);
        for (std::size_t c = 0; c < means.size(); ++c) {
            std::cout << "  mean of channel " << c << ": " << means[c] << '\n';
        }
        if (!read_region(file)) {
            std::cerr << "the region does not match the whole image\n";
            return 1;
        }
        std::cout << "the region matches the whole image\n";

        // Returning false from the progress function cancels the read, which then throws cancelled_error.
        try {
            (void)file.read_pixels(0, {.progress = [](std::uint64_t, std::uint64_t) { return false; }});
            std::cerr << "the read was not cancelled\n";
            return 1;
        } catch (const openxisf::cancelled_error&) {
            std::cout << "the second read was cancelled\n";
        }
        return 0;
    } catch (const std::exception& failure) {
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
