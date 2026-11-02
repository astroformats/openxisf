// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "uncached_file.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace openxisf::bench {

namespace {

// The unit of the reads, a multiple of the sector size of every storage device: 512 or 4096 bytes.
constexpr std::uint64_t sector = 4096;

// The most that one call reads, a whole number of sectors: ReadFile() takes a 32-bit length, and Linux transfers at
// most 2 GiB - 4 KiB in one call.
constexpr std::uint64_t max_transfer = std::uint64_t{1} << 30;

constexpr std::uint64_t round_up(std::uint64_t value)
{
    return (value + sector - 1) / sector * sector;
}

// The error of the last call of the system that failed.
std::error_code last_error() noexcept
{
#if defined(_WIN32)
    return {static_cast<int>(GetLastError()), std::system_category()};
#else
    return {errno, std::generic_category()};
#endif
}

[[noreturn]] void fail(std::error_code error, const char* what)
{
    throw std::system_error(error, what);
}

// size bytes of the buffer of this thread, aligned to a sector, as reads around the cache need them.
std::span<std::byte> thread_buffer(std::size_t size)
{
    thread_local std::vector<std::byte> storage;
    if (storage.size() < size + sector) {
        storage.resize(size + sector);
    }
    void* start = storage.data();
    std::size_t space = storage.size();
    (void)std::align(sector, size, start, space);
    return {static_cast<std::byte*>(start), size};
}

#if defined(_WIN32)
using native_handle = HANDLE;
#else
using native_handle = int;
#endif

// Opens the file at path for reads around the cache of the system.
native_handle open_around_cache(const std::filesystem::path& path)
{
#if defined(_WIN32)
    const HANDLE handle =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        fail(last_error(), "cannot open the file");
    }
#else
#if defined(O_DIRECT)
    int handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECT);
    if (handle < 0 && errno == EINVAL) {
        // A file system without direct I/O.
        handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    }
#else
    const int handle = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
#endif
    if (handle < 0) {
        fail(last_error(), "cannot open the file");
    }
#if defined(F_NOCACHE)
    // macOS: no cache for the reads of this descriptor.
    (void)::fcntl(handle, F_NOCACHE, 1);
#endif
#endif
    return handle;
}

class uncached_file final : public input_source
{
public:
    explicit uncached_file(const std::filesystem::path& path) : handle_(open_around_cache(path))
    {
#if defined(_WIN32)
        LARGE_INTEGER size{};
        if (GetFileSizeEx(handle_, &size) == FALSE) {
            const std::error_code error = last_error();
            (void)CloseHandle(handle_);
            fail(error, "cannot find the size of the file");
        }
        size_ = static_cast<std::uint64_t>(size.QuadPart);
#else
        struct stat status{};
        if (::fstat(handle_, &status) != 0) {
            const std::error_code error = last_error();
            (void)::close(handle_);
            fail(error, "cannot find the size of the file");
        }
        size_ = static_cast<std::uint64_t>(status.st_size);
#endif
    }

    uncached_file(const uncached_file&) = delete;
    uncached_file& operator=(const uncached_file&) = delete;
    uncached_file(uncached_file&&) = delete;
    uncached_file& operator=(uncached_file&&) = delete;

    ~uncached_file() override
    {
#if defined(_WIN32)
        (void)CloseHandle(handle_);
#else
        (void)::close(handle_);
#endif
    }

    [[nodiscard]] std::uint64_t size() const override
    {
        return size_;
    }

    // The library asks for no byte beyond size(); the sectors read may go past it, where the file ends.
    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (destination.empty()) {
            return;
        }
        const std::uint64_t first = offset - (offset % sector);
        const std::uint64_t needed = offset + destination.size() - first;
        const std::span<std::byte> buffer = thread_buffer(round_up(needed));
        for (std::uint64_t done = 0; done < needed;) {
            const std::uint64_t got = read_some(
                first + done, buffer.subspan(done, std::min<std::uint64_t>(buffer.size() - done, max_transfer)));
            // A read around the cache moves whole sectors, unless the file ends.
            if (got == 0 || (got % sector != 0 && done + got < needed)) {
                throw std::runtime_error("the file ends before the bytes asked for");
            }
            done += got;
        }
        std::memcpy(destination.data(), buffer.data() + (offset - first), destination.size());
    }

    [[nodiscard]] bool supports_concurrent_reads() const override
    {
        return true;
    }

private:
    // Reads what it can of buffer from offset on: what the call transferred, 0 at the end of the file.
    std::uint64_t read_some(std::uint64_t offset, std::span<std::byte> buffer) const
    {
#if defined(_WIN32)
        OVERLAPPED position{};
        position.Offset = static_cast<DWORD>(offset & 0xFFFF'FFFFU);
        position.OffsetHigh = static_cast<DWORD>(offset >> 32U);
        DWORD transferred = 0;
        if (ReadFile(handle_, buffer.data(), static_cast<DWORD>(buffer.size()), &transferred, &position) == FALSE) {
            const std::error_code error = last_error();
            if (error.value() == ERROR_HANDLE_EOF) {
                return 0;
            }
            fail(error, "cannot read the file");
        }
        return transferred;
#else
        while (true) {
            const ssize_t count = ::pread(handle_, buffer.data(), buffer.size(), static_cast<off_t>(offset));
            if (count >= 0) {
                return static_cast<std::uint64_t>(count);
            }
            if (errno != EINTR) {
                fail(last_error(), "cannot read the file");
            }
        }
#endif
    }

    native_handle handle_;
    std::uint64_t size_ = 0;
};

} // namespace

std::unique_ptr<input_source> open_uncached(const std::filesystem::path& path)
{
    return std::make_unique<uncached_file>(path);
}

} // namespace openxisf::bench
