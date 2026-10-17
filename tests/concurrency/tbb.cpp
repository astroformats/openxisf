// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The parallel work of a build with oneTBB, called from the oneTBB tasks of an application: from a task group that the
// application cancelled, or whose other task threw, where the library still does its work whole, and from the
// application's own parallel loop, whose tasks a thread that waits for the library does not take. Built with
// OPENXISF_WITH_TBB only; ThreadSanitizer runs these tests in CI.

#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>
#include <openxisf/types.h>
#include <openxisf/writer.h>

#include "support/faulty_io.h"

#include <gtest/gtest.h>
#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <thread>
#include <vector>

namespace {

using openxisf::pixel_storage;

// A planar RGB UInt16 image of 1.5 MiB, which a read converts to the normal storage model in two pieces of about
// 1 MiB, and which the writer compresses in 48 subblocks.
openxisf::image_info test_image()
{
    openxisf::image_info info;
    info.geometry = {.dimensions = {512, 512}, .channels = 3};
    info.sample_format = openxisf::sample_format::uint16;
    info.color_space = openxisf::color_space::rgb;
    return info;
}

// The samples in native byte order, which the writer swaps on a big-endian host.
std::vector<std::byte> samples(const openxisf::image_info& info)
{
    std::vector<std::uint16_t> values(info.geometry.sample_count());
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = static_cast<std::uint16_t>((i * 7919U) ^ (i >> 5U));
    }
    std::vector<std::byte> bytes(values.size() * sizeof(std::uint16_t));
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
}

// The unit of the image, compressed with Zstandard and byte shuffling in subblocks of 32 KiB, the same byte for byte
// at every save.
std::vector<std::byte> save(const openxisf::image_info& info, std::span<const std::byte> pixels,
                            const openxisf::progress_function& progress = {})
{
    openxisf::writer output({.creator_application = "OpenXISF tests 1.0",
                             .creation_time = openxisf::date_time{.year = 2026, .month = 10, .day = 14},
                             .codec = openxisf::codec::zstd,
                             .byte_shuffle = true,
                             .subblock_size = std::uint64_t{32} << 10U,
                             .progress = progress});
    (void)output.add_image(info, pixels);
    openxisf::memory_sink sink;
    output.save(sink);
    return sink.release();
}

// What the library gives within a task of the application: the unit saved again, and its pixels read in their own
// storage model and in the normal one, which each run loops of the library in parallel.
struct results
{
    std::vector<std::byte> unit{};
    std::vector<std::byte> planar{};
    std::vector<std::byte> normal{};
};

results work(const openxisf::image_info& info, std::span<const std::byte> pixels, const openxisf::reader& file)
{
    results done;
    done.unit = save(info, pixels);
    done.planar = file.read_pixels(0);
    done.normal = file.read_pixels(0, {.storage = pixel_storage::normal});
    return done;
}

// The image, its unit, and the results of the library outside any task of the application.
struct encoded_image
{
    openxisf::image_info info = test_image();
    std::vector<std::byte> pixels = samples(info);
    std::vector<std::byte> unit = save(info, pixels);
    openxisf::reader file{std::make_unique<openxisf::memory_source>(std::span<const std::byte>(unit))};
    results expected = work(info, pixels, file);
};

void expect_whole(const results& done, const encoded_image& encoded)
{
    EXPECT_TRUE(done.unit == encoded.unit) << "the unit saved differs";
    EXPECT_TRUE(done.planar == encoded.pixels) << "the pixels read differ";
    EXPECT_TRUE(done.normal == encoded.expected.normal) << "the pixels converted differ";
}

// Waits until condition() holds, for a minute at most. False when it does not.
template <typename Condition> bool wait_until(const Condition& condition)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

TEST(concurrency_tbb, the_work_of_the_library_is_done_whole_in_a_cancelled_task_group)
{
    // The application cancels its group, whose task then calls the library. Bound to that group, the loops of the
    // library would run none of their iterations, and return as if they had.
    const encoded_image encoded;
    ASSERT_TRUE(encoded.expected.planar == encoded.pixels);
    results inside;
    oneapi::tbb::task_group group;
    group.run([&] {
        group.cancel();
        inside = work(encoded.info, encoded.pixels, encoded.file);
    });
    EXPECT_EQ(group.wait(), oneapi::tbb::task_group_status::canceled);
    expect_whole(inside, encoded);
}

TEST(concurrency_tbb, the_work_of_the_library_is_done_whole_after_another_task_of_the_group_threw)
{
    // One task of the group calls the library once the other has thrown, which cancels the group.
    if (oneapi::tbb::this_task_arena::max_concurrency() < 2) {
        GTEST_SKIP() << "the two tasks need two threads";
    }
    const encoded_image encoded;
    std::atomic<bool> started{false};
    bool cancelled = false;
    results inside;
    oneapi::tbb::task_group group;
    group.run([&] {
        started = true;
        cancelled = wait_until([] { return oneapi::tbb::is_current_task_group_canceling(); });
        inside = work(encoded.info, encoded.pixels, encoded.file);
    });
    group.run([&started] {
        if (wait_until([&started] { return started.load(); })) {
            throw openxisf::test::injected_fault();
        }
    });
    EXPECT_THROW(group.wait(), openxisf::test::injected_fault);
    EXPECT_TRUE(cancelled) << "the group was not cancelled before the library was called";
    expect_whole(inside, encoded);
}

TEST(concurrency_tbb, a_thread_that_waits_for_the_library_takes_no_task_of_the_application)
{
    // While a thread waits in a read for the tasks of the library that other threads took, it would otherwise take
    // another iteration of the application's loop, and run it on top of the read, which has not returned: where the
    // application may still hold a lock that the iteration takes again.
    const encoded_image encoded;
    oneapi::tbb::enumerable_thread_specific<int> depth(0);
    std::atomic<int> nested{0};
    std::atomic<int> wrong{0};
    oneapi::tbb::parallel_for(0, 64, [&](int /*i*/) {
        int& reads = depth.local();
        if (reads > 0) {
            ++nested;
        }
        ++reads;
        if (encoded.file.read_pixels(0) != encoded.pixels) {
            ++wrong;
        }
        --reads;
    });
    EXPECT_EQ(nested.load(), 0);
    EXPECT_EQ(wrong.load(), 0);
}

TEST(concurrency_tbb, progress_functions_run_on_the_calling_thread)
{
    // io.h: the subblocks are compressed and decompressed in parallel, and their progress is reported on the thread
    // that saves or reads.
    const encoded_image encoded;
    const std::thread::id caller = std::this_thread::get_id();
    std::atomic<int> calls{0};
    std::atomic<int> elsewhere{0};
    const openxisf::progress_function progress = [&](std::uint64_t /*done*/, std::uint64_t /*total*/) {
        ++calls;
        if (std::this_thread::get_id() != caller) {
            ++elsewhere;
        }
        return true;
    };
    EXPECT_TRUE(save(encoded.info, encoded.pixels, progress) == encoded.unit);
    EXPECT_TRUE(encoded.file.read_pixels(0, {.progress = progress}) == encoded.pixels);
    EXPECT_TRUE(encoded.file.read_pixels(0, {.storage = pixel_storage::normal, .progress = progress}) ==
                encoded.expected.normal);
    EXPECT_GT(calls.load(), 3 * 48);
    EXPECT_EQ(elsewhere.load(), 0);
}

} // namespace
