// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Stacks frames region by region, as an application that combines many frames does without holding them: it writes a
// few input frames as frames meant for region access are written, compressed in subblocks of 1 MiB and without a
// checksum, reserves the stacked frame in a unit of its own, planar and with a checksum, fills it tile by tile from a
// few threads through one editor, each tile the mean of the same tile of every input, read as a region of each, commits
// it, and reads a region of it back.
//
//   stack_regions <directory>

#include <openxisf/openxisf.h>

#include "arguments.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t width = 512;
constexpr std::uint64_t height = 384;
constexpr std::uint64_t channels = 3;
constexpr std::uint64_t tile = 128;
constexpr std::size_t frame_count = 4;
constexpr std::size_t thread_count = 4;

// The sky that every frame shows, at pixel (x, y) of channel c.
float sky(std::uint64_t x, std::uint64_t y, std::uint64_t c)
{
    const double r = std::hypot(static_cast<double>(x) - 200.0, static_cast<double>(y) - 150.0);
    return static_cast<float>((0.1 + (0.05 * static_cast<double>(c))) + (0.75 * std::exp(-r * r / 800.0)));
}

// Frame k of the sky, planar, with an offset of its own, which the mean of the frames cancels.
std::vector<float> render_frame(std::size_t k)
{
    const float offset = (static_cast<float>(k) - 1.5F) * 0.01F;
    std::vector<float> samples(width * height * channels);
    for (std::uint64_t c = 0; c < channels; ++c) {
        for (std::uint64_t y = 0; y < height; ++y) {
            for (std::uint64_t x = 0; x < width; ++x) {
                samples[(((c * height) + y) * width) + x] = sky(x, y, c) + offset;
            }
        }
    }
    return samples;
}

openxisf::image_info frame_info()
{
    openxisf::image_info info;
    info.geometry = {.dimensions = {width, height}, .channels = channels};
    info.sample_format = openxisf::sample_format::float32;
    info.color_space = openxisf::color_space::rgb;
    info.bounds = openxisf::bounds{.lower = 0.0, .upper = 1.0};
    return info;
}

// The tiles of the frame, every channel of tile x tile pixels.
std::vector<openxisf::pixel_region> tiles()
{
    std::vector<openxisf::pixel_region> regions;
    for (std::uint64_t y = 0; y < height; y += tile) {
        for (std::uint64_t x = 0; x < width; x += tile) {
            regions.push_back({.origin = {x, y}, .size = {tile, tile}});
        }
    }
    return regions;
}

// Writes each input frame as a frame meant for region access: compressed with byte shuffling in subblocks of 1 MiB,
// which a region read decodes alone, and without a checksum, which would make it read the whole frame.
std::vector<std::string> write_inputs(const std::string& directory)
{
    std::vector<std::string> paths;
    for (std::size_t k = 0; k < frame_count; ++k) {
        const std::vector<float> samples = render_frame(k);
        openxisf::writer output({.creator_application = "stack_regions sample 1.0",
                                 .codec = openxisf::codec::zstd,
                                 .byte_shuffle = true,
                                 .subblock_size = std::uint64_t{1} << 20});
        output.add_image(frame_info(), std::span(samples));
        paths.push_back(directory + "/frame-" + std::to_string(k + 1) + ".xisf");
        output.save(paths.back());
    }
    return paths;
}

// Fills the stacked frame tile by tile, each tile the mean of the same tile of every input: the threads take every
// thread_count-th tile, and write it through the one editor, which takes regions that share no sample at once.
void stack(const std::vector<openxisf::reader>& inputs, openxisf::editor& output)
{
    const std::vector<openxisf::pixel_region> regions = tiles();
    std::vector<std::exception_ptr> failures(thread_count);
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (std::size_t t = 0; t < thread_count; ++t) {
        threads.emplace_back([&, t] {
            try {
                for (std::size_t k = t; k < regions.size(); k += thread_count) {
                    std::vector<float> sum(regions[k].sample_count(inputs.front().image(0).geometry));
                    for (const openxisf::reader& input : inputs) {
                        const std::vector<float> region = input.read_pixels<float>(0, {.region = regions[k]});
                        for (std::size_t i = 0; i < sum.size(); ++i) {
                            sum[i] += region[i] / static_cast<float>(inputs.size());
                        }
                    }
                    output.write_pixels(0, std::span(sum), {.region = regions[k]});
                }
            } catch (...) {
                failures[t] = std::current_exception();
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (const std::exception_ptr& failure : failures) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    try {
        const std::vector<std::string> arguments = example::utf8_arguments(argc, argv);
        if (arguments.size() != 2) {
            std::cerr << "usage: stack_regions <directory>\n";
            return 2;
        }
        const std::string& directory = arguments[1];
        std::vector<openxisf::reader> inputs;
        for (const std::string& path : write_inputs(directory)) {
            inputs.emplace_back(path);
        }

        // The output, reserved: its zeros are written now, uncompressed, with the checksum that the editor replaces.
        const std::string stacked = directory + "/stack.xisf";
        openxisf::writer reserved(
            {.creator_application = "stack_regions sample 1.0", .checksum = openxisf::checksum_algorithm::sha256});
        reserved.add_reserved_image(frame_info());
        reserved.save(stacked);

        openxisf::editor output(stacked);
        stack(inputs, output);
        // The new checksum, written in place; until then a reader finds the frame corrupt.
        output.commit();
        std::cout << "stacked " << frame_count << " frames of " << width << " x " << height << " pixels in "
                  << tiles().size() << " tiles from " << thread_count << " threads\n";

        // A region of the stack, read back by a reader that verifies the checksum, holds the sky.
        const openxisf::reader result(stacked, {.strict = true});
        const openxisf::pixel_region centre{.origin = {150, 100}, .size = {100, 100}, .channel_count = 1};
        const std::vector<float> values = result.read_pixels<float>(0, {.region = centre});
        for (std::uint64_t y = 0; y < 100; ++y) {
            for (std::uint64_t x = 0; x < 100; ++x) {
                if (std::abs(values[(y * 100) + x] - sky(150 + x, 100 + y, 0)) > 1e-5F) {
                    std::cerr << "the stack differs from the sky at (" << 150 + x << ", " << 100 + y << ")\n";
                    return 1;
                }
            }
        }
        std::cout << "the region read back holds the sky\n";
        return 0;
    } catch (const std::exception& failure) {
        (void)std::fputs(failure.what(), stderr);
        (void)std::fputs("\n", stderr);
        return 1;
    }
}
