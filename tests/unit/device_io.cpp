// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The devices that an editor reads and writes in place, and the checks that the library makes before a device sees a
// call.

#include <openxisf/error.h>
#include <openxisf/io.h>

#include "io/thread_safe_device.h"
#include "support/bytes.h"
#include "support/faulty_io.h"
#include "support/files.h"
#include "support/temp_directory.h"
#include "support/threads.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

using openxisf::errc;
using openxisf::file_device;
using openxisf::io_error;
using openxisf::memory_device;
using openxisf::usage_error;
using openxisf::detail::thread_safe_device;
using openxisf::test::bytes;
using openxisf::test::pattern;
using openxisf::test::read_file;
using openxisf::test::temp_directory;
using openxisf::test::text;
using openxisf::test::throws;
using openxisf::test::utf8;
using openxisf::test::write_file;

// Writes every range of device, each with bytes of its own, and reads it back; expected holds the content of the
// device, which it follows.
void write_and_read_every_range(openxisf::random_access_device& device, std::vector<std::byte>& expected)
{
    std::uint8_t mark = 0;
    for (std::size_t offset = 0; offset <= expected.size(); ++offset) {
        for (std::size_t length = 0; offset + length <= expected.size(); ++length) {
            const std::vector<std::byte> data(length, static_cast<std::byte>(++mark));
            device.write(offset, data);
            std::ranges::copy(data, expected.begin() + static_cast<std::ptrdiff_t>(offset));
            std::vector<std::byte> read(length);
            device.read(offset, read);
            ASSERT_EQ(read, data) << offset << ' ' << length;
        }
    }
    std::vector<std::byte> whole(expected.size());
    device.read(0, whole);
    EXPECT_EQ(whole, expected);
}

TEST(file_device, reads_and_writes_every_range_of_a_file)
{
    const temp_directory directory;
    std::vector<std::byte> expected = pattern(37);
    write_file(directory.path() / "data.bin", expected);
    {
        file_device device(directory.file("data.bin"));
        ASSERT_EQ(device.size(), expected.size());
        write_and_read_every_range(device, expected);
    }
    EXPECT_EQ(bytes(read_file(directory.path() / "data.bin")), expected);
}

TEST(file_device, writes_past_the_end_are_refused_before_the_file_sees_them)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    file_device device(directory.file("data.bin"));
    const std::vector<std::byte> data = bytes("abcd");

    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(7, data); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(11, {}); }));
    device.write(10, {});
    EXPECT_EQ(read_file(directory.path() / "data.bin"), "0123456789");
    EXPECT_EQ(device.size(), 10U);
}

TEST(file_device, reads_past_the_end_are_end_of_data)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    const file_device device(directory.file("data.bin"));
    std::vector<std::byte> destination(4);

    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { device.read(7, destination); }));
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { device.read(11, {}); }));
}

TEST(file_device, describes_itself_with_its_path_and_supports_concurrent_access)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    const file_device device(directory.file("data.bin"));

    EXPECT_EQ(device.description(), directory.file("data.bin"));
    EXPECT_TRUE(device.supports_concurrent_reads());
    EXPECT_TRUE(device.supports_concurrent_writes());
}

TEST(file_device, a_missing_file_is_open_failed_and_is_not_created)
{
    const temp_directory directory;

    try {
        const file_device device(directory.file("missing.xisf"));
        ADD_FAILURE() << "no exception";
    } catch (const io_error& failure) {
        EXPECT_EQ(failure.code(), errc::open_failed);
        EXPECT_EQ(failure.system_code(), std::errc::no_such_file_or_directory);
        EXPECT_NE(std::string_view(failure.what()).find(directory.file("missing.xisf")), std::string_view::npos);
    }
    EXPECT_EQ(directory.entries(), 0U);
}

TEST(file_device, refuses_directories_and_devices)
{
    const temp_directory directory;
#if defined(_WIN32)
    const std::string device = "NUL";
#else
    const std::string device = "/dev/null";
#endif

    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&] { file_device opened(utf8(directory.path())); }));
    EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&] { file_device opened(device); }));
}

#if defined(_WIN32)
TEST(file_device, refuses_the_device_names_of_windows_in_any_directory)
{
    // As file_source refuses them, before they are opened.
    const temp_directory directory;
    for (const std::string_view name : {"NUL", "nul.xisf", "CON", "COM9", "LPT1"}) {
        const std::string path = directory.file(name);
        if (openxisf::test::is_device_name(path)) {
            EXPECT_TRUE(throws<io_error>(errc::not_a_regular_file, [&path] { file_device opened(path); })) << name;
        } else {
            EXPECT_TRUE(throws<io_error>(errc::open_failed, [&path] { file_device opened(path); })) << name;
        }
    }
}
#endif

TEST(file_device, a_file_that_cannot_be_written_is_open_failed)
{
#if !defined(_WIN32)
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root may write any file";
    }
#endif
    const temp_directory directory;
    const std::filesystem::path path = directory.path() / "data.bin";
    write_file(path, bytes("0123456789"));
    constexpr std::filesystem::perms writable = std::filesystem::perms::owner_write |
                                                std::filesystem::perms::group_write |
                                                std::filesystem::perms::others_write;
    std::filesystem::permissions(path, writable, std::filesystem::perm_options::remove);

    // On Windows, the read-only attribute refuses write access.
    EXPECT_TRUE(throws<io_error>(errc::open_failed, [&] { file_device device(directory.file("data.bin")); }));
    std::filesystem::permissions(path, std::filesystem::perms::owner_write, std::filesystem::perm_options::add);
    EXPECT_EQ(read_file(path), "0123456789");
}

TEST(file_device, refuses_empty_and_invalid_utf8_paths)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { file_device device(""); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_utf8, [] { file_device device("a\xC3.xisf"); }));
}

TEST(file_device, flushes_what_it_wrote_to_the_storage_device)
{
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    {
        file_device device(directory.file("data.bin"));
        device.write(2, bytes("ab"));
        device.flush();
        device.write(8, bytes("yz"));
        device.flush();
    }
    EXPECT_EQ(read_file(directory.path() / "data.bin"), "01ab4567yz");
}

TEST(file_device, other_programs_can_read_and_write_the_file_while_it_is_open)
{
    // As a reader of the same file does, before and after the device opens it.
    const temp_directory directory;
    write_file(directory.path() / "data.bin", bytes("0123456789"));
    const openxisf::file_source before(directory.file("data.bin"));
    file_device device(directory.file("data.bin"));
    const openxisf::file_source after(directory.file("data.bin"));
    file_device other(directory.file("data.bin"));

    device.write(0, bytes("ab"));
    other.write(8, bytes("yz"));
    for (const openxisf::input_source* source :
         {static_cast<const openxisf::input_source*>(&before), static_cast<const openxisf::input_source*>(&after)}) {
        std::vector<std::byte> content(10);
        source->read(0, content);
        EXPECT_EQ(text(content), "ab234567yz");
    }
}

TEST(memory_device, reads_and_writes_every_range_of_a_buffer)
{
    std::vector<std::byte> buffer = pattern(37);
    std::vector<std::byte> expected = buffer;
    memory_device device(buffer);
    ASSERT_EQ(device.size(), buffer.size());
    write_and_read_every_range(device, expected);
    EXPECT_EQ(buffer, expected);
}

TEST(memory_device, writes_past_the_end_are_refused_and_reads_past_it_are_end_of_data)
{
    std::vector<std::byte> buffer = bytes("0123456789");
    memory_device device(buffer);
    std::vector<std::byte> destination(4);

    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(7, bytes("abcd")); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(11, {}); }));
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { device.read(7, destination); }));
    EXPECT_EQ(text(buffer), "0123456789");
}

TEST(memory_device, supports_concurrent_access_and_flushes_without_doing_anything)
{
    std::vector<std::byte> buffer = bytes("0123456789");
    memory_device device(buffer);

    EXPECT_TRUE(device.supports_concurrent_reads());
    EXPECT_TRUE(device.supports_concurrent_writes());
    EXPECT_EQ(device.description(), "");
    device.flush();
    EXPECT_EQ(text(buffer), "0123456789");
}

// A device that records its calls, and tells when two of them overlapped in time.
class recording_device final : public openxisf::random_access_device
{
public:
    explicit recording_device(std::size_t size, bool concurrent = false) : content_(size), concurrent_(concurrent) {}

    std::uint64_t size() const override
    {
        return content_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        const call_scope scope(*this);
        record("read", offset, destination.size());
        std::copy_n(content_.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(), destination.begin());
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        const call_scope scope(*this);
        record("write", offset, data.size());
        std::ranges::copy(data, content_.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    void flush() override
    {
        const call_scope scope(*this);
        record("flush", 0, 0);
    }

    bool supports_concurrent_reads() const override
    {
        return concurrent_;
    }

    bool supports_concurrent_writes() const override
    {
        return concurrent_;
    }

    std::string description() const override
    {
        return "the recording device";
    }

    [[nodiscard]] std::vector<std::string> calls() const
    {
        const std::scoped_lock lock(mutex_);
        return calls_;
    }

    [[nodiscard]] bool overlapped() const noexcept
    {
        return overlapped_;
    }

private:
    // Counts the calls in progress for its lifetime, and waits a little inside, so that two threads that are let in at
    // once overlap. A sleep would last a whole tick of the timer on Windows.
    class call_scope
    {
    public:
        explicit call_scope(const recording_device& device) : device_(device)
        {
            if (++device_.active_ > 1) {
                device_.overlapped_ = true;
            }
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(100);
            while (std::chrono::steady_clock::now() < until) {
            }
        }

        call_scope(const call_scope&) = delete;
        call_scope& operator=(const call_scope&) = delete;

        ~call_scope()
        {
            --device_.active_;
        }

    private:
        const recording_device& device_;
    };

    void record(std::string_view what, std::uint64_t offset, std::size_t length) const
    {
        const std::scoped_lock lock(mutex_);
        calls_.push_back(std::string(what) + ' ' + std::to_string(offset) + ' ' + std::to_string(length));
    }

    std::vector<std::byte> content_;
    bool concurrent_;
    mutable std::mutex mutex_;
    mutable std::vector<std::string> calls_;
    mutable std::atomic<int> active_{0};
    mutable std::atomic<bool> overlapped_{false};
};

TEST(thread_safe_device, refuses_a_null_device)
{
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [] { thread_safe_device device(nullptr); }));
}

TEST(thread_safe_device, checks_every_range_before_the_device_sees_it)
{
    auto owned = std::make_unique<recording_device>(10);
    const recording_device& recorded = *owned;
    const thread_safe_device device(std::move(owned));
    std::vector<std::byte> destination(4);
    const std::vector<std::byte> data = bytes("abcd");

    EXPECT_EQ(device.size(), 10U);
    EXPECT_EQ(device.description(), "the recording device");
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { device.read(7, destination); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(7, data); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { device.write(11, {}); }));
    // Empty calls within the device reach it not at all.
    device.read(10, {});
    device.write(10, {});
    device.write(6, data);
    device.read(6, destination);
    device.flush();
    EXPECT_EQ(destination, data);
    EXPECT_EQ(recorded.calls(), (std::vector<std::string>{"write 6 4", "read 6 4", "flush 0 0"}));
}

TEST(thread_safe_device, its_source_reads_through_the_device)
{
    auto owned = std::make_unique<recording_device>(10);
    const recording_device& recorded = *owned;
    const thread_safe_device device(std::move(owned));
    device.write(0, bytes("0123456789"));
    const std::unique_ptr<openxisf::input_source> source = device.source();
    std::vector<std::byte> destination(3);

    EXPECT_EQ(source->size(), 10U);
    EXPECT_EQ(source->description(), "the recording device");
    // The device decides which calls run at once.
    EXPECT_TRUE(source->supports_concurrent_reads());
    source->read(2, destination);
    EXPECT_EQ(text(destination), "234");
    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source->read(8, destination); }));
    EXPECT_EQ(recorded.calls(), (std::vector<std::string>{"write 0 10", "read 2 3"}));
}

// Reads and writes from 8 threads at once, each at its own range of 4 bytes, and checks what each read.
void use_from_threads(const thread_safe_device& device)
{
    std::atomic<int> wrong{0};
    openxisf::test::run_threads(8, [&device, &wrong](std::size_t t) {
        const std::vector<std::byte> data(4, static_cast<std::byte>(t));
        std::vector<std::byte> read(4);
        for (int i = 0; i < 20; ++i) {
            device.write(t * 4, data);
            device.read(t * 4, read);
            if (read != data) {
                ++wrong;
            }
        }
    });
    EXPECT_EQ(wrong, 0);
}

TEST(thread_safe_device, calls_a_device_without_concurrent_writes_one_at_a_time)
{
    auto owned = std::make_unique<recording_device>(32);
    const recording_device& recorded = *owned;
    const thread_safe_device device(std::move(owned));
    use_from_threads(device);
    EXPECT_FALSE(recorded.overlapped());
    EXPECT_EQ(recorded.calls().size(), 8U * 40U);
}

TEST(thread_safe_device, calls_a_device_with_concurrent_writes_from_several_threads_at_once)
{
    auto owned = std::make_unique<recording_device>(32, true);
    const recording_device& recorded = *owned;
    const thread_safe_device device(std::move(owned));
    use_from_threads(device);
    // 320 calls of 100 µs each, from 8 threads that start together: some overlap, unless the device were serialized.
    EXPECT_TRUE(recorded.overlapped());
}

TEST(thread_safe_device, passes_the_exceptions_of_the_device_through)
{
    std::vector<std::byte> buffer(10);
    memory_device inner(buffer);
    for (const openxisf::test::fault kind : {openxisf::test::fault::error, openxisf::test::fault::foreign_exception}) {
        const thread_safe_device device(std::make_unique<openxisf::test::faulty_device>(inner, 1, kind));
        if (kind == openxisf::test::fault::error) {
            EXPECT_TRUE(throws<io_error>(errc::write_failed, [&] { device.write(0, bytes("ab")); }));
        } else {
            EXPECT_THROW(device.write(0, bytes("ab")), openxisf::test::injected_fault);
        }
    }
}

} // namespace
