// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Writes and reads of regions from 16 threads at once through one editor, which its contract allows for regions that
// share no sample: the tiles of a reserved frame, each read back by the thread that wrote it while the others write,
// then committed and read whole. ThreadSanitizer runs these tests in CI.

#include <openxisf/editor.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>
#include <openxisf/writer.h>

#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/regions.h"
#include "support/temp_directory.h"
#include "support/threads.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

constexpr std::size_t thread_count = 16;
constexpr std::uint64_t width = 256;
constexpr std::uint64_t height = 192;
constexpr std::uint64_t tile = 32;
// The geometry of the frame.
openxisf::geometry frame()
{
    return {.dimensions = {width, height}, .channels = 3};
}

// The planar samples of the frame: channel c of pixel p is 7c + p, wrapped to 16 bits.
std::vector<std::byte> stacked_frame()
{
    const std::uint64_t pixels = width * height;
    std::vector<std::uint16_t> values(pixels * 3);
    for (std::uint64_t c = 0; c < 3; ++c) {
        for (std::uint64_t p = 0; p < pixels; ++p) {
            values[(c * pixels) + p] = static_cast<std::uint16_t>((7 * c) + p);
        }
    }
    std::vector<std::byte> bytes(values.size() * 2);
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
}

// The unit of the frame reserved with a SHA-256 checksum, in memory.
std::vector<std::byte> reserved_frame()
{
    openxisf::image_info info;
    info.geometry = frame();
    info.color_space = openxisf::color_space::rgb;
    openxisf::writer output({.creator_application = "OpenXISF tests 1.0",
                             .creation_time = openxisf::date_time{.year = 2026},
                             .checksum = openxisf::checksum_algorithm::sha256});
    (void)output.add_reserved_image(info);
    openxisf::memory_sink sink;
    output.save(sink);
    return sink.release();
}

// The tiles of the frame, of tile x tile pixels with every channel.
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

// Fills the frame through edit from thread_count threads, each writing every thread_count-th tile and reading it back
// with the verification asked for. Returns the number of tiles that did not read back as written, failures included.
int fill_from_threads(openxisf::editor& edit, std::span<const std::byte> stacked)
{
    const std::vector<openxisf::pixel_region> regions = tiles();
    std::atomic<int> wrong{0};
    openxisf::test::run_threads(thread_count, [&](std::size_t t) {
        for (std::size_t k = t; k < regions.size(); k += thread_count) {
            try {
                const std::vector<std::byte> data = openxisf::test::region_of(
                    stacked, frame(), 2, openxisf::pixel_storage::planar, regions[k], openxisf::pixel_storage::planar);
                edit.write_pixels(0, data, {.region = regions[k]});
                if (edit.read_pixels(0, {.region = regions[k]}) != data) {
                    ++wrong;
                }
            } catch (const std::exception&) {
                ++wrong;
            }
        }
    });
    return wrong;
}

// A device that forwards to another one and does not support concurrent writes, which the editor then calls one call
// at a time; it tells when two calls overlapped.
class serial_device final : public openxisf::random_access_device
{
public:
    explicit serial_device(std::span<std::byte> data) : inner_(data) {}

    std::uint64_t size() const override
    {
        return inner_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        const scope inside(active_, overlapped_);
        inner_.read(offset, destination);
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        const scope inside(active_, overlapped_);
        inner_.write(offset, data);
    }

    [[nodiscard]] bool overlapped() const noexcept
    {
        return overlapped_;
    }

private:
    // Counts the calls in progress for its lifetime.
    class scope
    {
    public:
        scope(std::atomic<int>& active, std::atomic<bool>& overlapped) : active_(active)
        {
            if (++active_ > 1) {
                overlapped = true;
            }
        }

        scope(const scope&) = delete;
        scope& operator=(const scope&) = delete;

        ~scope()
        {
            --active_;
        }

    private:
        std::atomic<int>& active_;
    };

    openxisf::memory_device inner_;
    mutable std::atomic<int> active_{0};
    mutable std::atomic<bool> overlapped_{false};
};

TEST(concurrency_editor, threads_fill_the_tiles_of_one_frame_through_one_editor)
{
    const std::vector<std::byte> stacked = stacked_frame();
    std::vector<std::byte> file = reserved_frame();
    {
        openxisf::editor edit(std::make_unique<openxisf::memory_device>(file));
        EXPECT_EQ(fill_from_threads(edit, stacked), 0);
        edit.commit();
        EXPECT_EQ(edit.read_pixels(0), stacked);
    }
    EXPECT_EQ(openxisf::test::open_unit(file, {.strict = true}).read_pixels(0), stacked);
}

TEST(concurrency_editor, a_device_without_concurrent_writes_is_called_one_call_at_a_time)
{
    const std::vector<std::byte> stacked = stacked_frame();
    std::vector<std::byte> file = reserved_frame();
    auto device = std::make_unique<serial_device>(file);
    const serial_device& serial = *device;
    {
        openxisf::editor edit(std::move(device));
        EXPECT_EQ(fill_from_threads(edit, stacked), 0);
        edit.commit();
        EXPECT_FALSE(serial.overlapped());
    }
    EXPECT_EQ(openxisf::test::open_unit(file, {.strict = true}).read_pixels(0), stacked);
}

TEST(concurrency_editor, threads_fill_the_tiles_of_one_file_through_one_editor)
{
    const openxisf::test::temp_directory directory;
    const std::string path = directory.file("stack.xisf");
    openxisf::test::write_file(openxisf::test::path_of(path), reserved_frame());
    const std::vector<std::byte> stacked = stacked_frame();
    {
        openxisf::editor edit(path);
        EXPECT_EQ(fill_from_threads(edit, stacked), 0);
        edit.commit();
    }
    EXPECT_EQ(openxisf::reader(path, {.strict = true}).read_pixels(0), stacked);
}

} // namespace
