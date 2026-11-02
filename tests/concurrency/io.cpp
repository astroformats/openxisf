// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Reads from 16 threads at once, on sources that support concurrent reads and on sources that the library serializes,
// and reads and writes of devices. ThreadSanitizer runs these tests in CI.

#include <openxisf/io.h>
#include <openxisf/stream_io.h>

#include "core/xoshiro.h"
#include "io/thread_safe_device.h"
#include "io/thread_safe_source.h"
#include "support/bytes.h"
#include "support/cursor_source.h"
#include "support/files.h"
#include "support/temp_directory.h"
#include "support/threads.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

using openxisf::detail::thread_safe_source;
using openxisf::test::pattern;
using openxisf::test::temp_directory;

constexpr std::size_t thread_count = 16;
constexpr int reads_per_thread = 200;
constexpr std::size_t max_read = std::size_t{64} << 10;

// Every thread reads ranges of its own and compares them with the expected bytes. Returns the number of wrong reads,
// failed reads included.
template <typename Source> int read_from_threads(const Source& source, std::span<const std::byte> expected)
{
    std::atomic<int> wrong{0};
    openxisf::test::run_threads(thread_count, [&source, expected, &wrong](std::size_t t) {
        openxisf::detail::xoshiro256starstar random({1, 2, 3, t + 1});
        std::vector<std::byte> destination;
        for (int i = 0; i < reads_per_thread; ++i) {
            const std::size_t offset = random() % expected.size();
            const std::size_t length = random() % std::min(max_read, expected.size() - offset);
            destination.resize(length);
            try {
                source.read(offset, destination);
                if (!std::ranges::equal(destination, expected.subspan(offset, length))) {
                    ++wrong;
                }
            } catch (const std::exception&) {
                ++wrong;
            }
        }
    });
    return wrong;
}

TEST(concurrency_io, file_source)
{
    const temp_directory directory;
    const std::vector<std::byte> data = pattern(std::size_t{1} << 20);
    openxisf::test::write_file(directory.path() / "data.bin", data);
    const std::string path = directory.file("data.bin");

    const openxisf::file_source direct(path);
    const thread_safe_source wrapped(std::make_unique<openxisf::file_source>(path));

    EXPECT_EQ(read_from_threads(direct, data), 0);
    EXPECT_EQ(read_from_threads(wrapped, data), 0);
}

TEST(concurrency_io, memory_source)
{
    const std::vector<std::byte> data = pattern(std::size_t{1} << 20);
    const openxisf::memory_source source{std::span<const std::byte>(data)};

    EXPECT_EQ(read_from_threads(source, data), 0);
}

TEST(concurrency_io, a_source_without_concurrent_reads_is_called_one_read_at_a_time)
{
    const std::vector<std::byte> data = pattern(std::size_t{1} << 20);
    auto inner = std::make_unique<openxisf::test::cursor_source>(data);
    const openxisf::test::cursor_source& cursor = *inner;
    const thread_safe_source source(std::move(inner));

    EXPECT_EQ(read_from_threads(source, data), 0);
    EXPECT_FALSE(cursor.overlapped());
}

// Every thread writes ranges of its own stripe of the device, of thread_count stripes, with bytes of its own, and reads
// each range back. Returns the number of wrong reads, failed reads and writes included; the device then holds the last
// bytes written in each stripe, which content follows.
template <typename Device> int write_from_threads(Device& device, std::vector<std::byte>& content)
{
    const std::size_t stripe = content.size() / thread_count;
    std::atomic<int> wrong{0};
    openxisf::test::run_threads(thread_count, [&device, &content, stripe, &wrong](std::size_t t) {
        openxisf::detail::xoshiro256starstar random({4, 5, 6, t + 1});
        std::vector<std::byte> data;
        std::vector<std::byte> read;
        for (int i = 0; i < reads_per_thread; ++i) {
            const std::size_t offset = t * stripe + (random() % stripe);
            const std::size_t length = random() % std::min(max_read, ((t + 1) * stripe) - offset);
            data.assign(length, static_cast<std::byte>(random()));
            read.resize(length);
            try {
                device.write(offset, data);
                device.read(offset, read);
                // Each thread has its own stripe of content.
                std::ranges::copy(data, content.begin() + static_cast<std::ptrdiff_t>(offset));
                if (read != data) {
                    ++wrong;
                }
            } catch (const std::exception&) {
                ++wrong;
            }
        }
    });
    return wrong;
}

TEST(concurrency_io, file_device)
{
    const temp_directory directory;
    std::vector<std::byte> content = pattern(std::size_t{1} << 20);
    openxisf::test::write_file(directory.path() / "data.bin", content);
    const std::string path = directory.file("data.bin");

    openxisf::file_device direct(path);
    EXPECT_EQ(write_from_threads(direct, content), 0);
    const openxisf::detail::thread_safe_device wrapped(std::make_unique<openxisf::file_device>(path));
    EXPECT_EQ(write_from_threads(wrapped, content), 0);
    EXPECT_EQ(read_from_threads(wrapped, content), 0);
}

TEST(concurrency_io, memory_device)
{
    std::vector<std::byte> buffer = pattern(std::size_t{1} << 20);
    std::vector<std::byte> content = buffer;
    openxisf::memory_device device(buffer);

    EXPECT_EQ(write_from_threads(device, content), 0);
    EXPECT_EQ(buffer, content);
}

TEST(concurrency_io, stream_sources)
{
    const temp_directory directory;
    const std::vector<std::byte> data = pattern(std::size_t{1} << 20);
    openxisf::test::write_file(directory.path() / "data.bin", data);
    const openxisf::test::c_file file(directory.path() / "data.bin", "rb");
    std::istringstream stream(openxisf::test::text(data));

    const thread_safe_source from_file(std::make_unique<openxisf::stdio_source>(file.get()));
    const thread_safe_source from_stream(std::make_unique<openxisf::istream_source>(stream));

    EXPECT_EQ(read_from_threads(from_file, data), 0);
    EXPECT_EQ(read_from_threads(from_stream, data), 0);
}

} // namespace
