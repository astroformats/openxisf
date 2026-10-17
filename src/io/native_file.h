// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

// The platform layer of file_source and file_sink: native_file_win32.cpp or native_file_posix.cpp. Paths are UTF-8 and
// valid, which the public classes check. Every failure is an io_error whose message names the file.

namespace openxisf::detail {

/// path made absolute from the current directory, so that it names the same file after the current directory changes:
/// on Windows as the system makes a path absolute, which normalizes it as every path (GetFullPathNameW()), and on POSIX
/// systems the current directory followed by a relative path. Throws io_error with errc::open_failed when the current
/// directory cannot be found, or its path is not valid UTF-8.
[[nodiscard]] std::string absolute_path(const std::string& path);

/// Throws io_error with errc::not_a_regular_file when what the absolute path names is something that a file must not
/// replace: a directory, a FIFO, a socket or a device, which on Windows includes a device name such as NUL in any
/// directory. A link, a symbolic link or a junction on Windows, is not followed, since a file that replaces it replaces
/// the link alone. Nothing at path, and what cannot be looked at, is left to create_new() and commit(), which fail
/// where the system refuses them.
void check_replaceable(const std::string& path);

/// True when path names a directory, or a link to one. False when it names something else, or cannot be looked at.
[[nodiscard]] bool is_directory(const std::string& path);

/// A file of the operating system, closed on destruction. Reads and writes take explicit offsets, so that any number
/// of threads can read one file at once.
class native_file
{
public:
#if defined(_WIN32)
    using handle_type = void*;
    static constexpr handle_type closed_handle = nullptr;
#else
    using handle_type = int;
    static constexpr handle_type closed_handle = -1;
#endif

    /// Opens a regular file for reading. What the system tells is not one is refused before it is opened, since opening
    /// a device can act on it: on Windows a device name that the system makes a path of its device namespace, such as
    /// NUL in any directory, and on POSIX systems what stat() does not call a regular file. Throws io_error with
    /// errc::open_failed or errc::not_a_regular_file, which a directory, the root of a drive or a share included, is.
    [[nodiscard]] static native_file open_for_reading(const std::string& path);

    /// Creates a file for writing, or returns nothing when something exists at path already. name is the path of the
    /// file it becomes, which error messages use. Throws io_error with errc::open_failed.
    [[nodiscard]] static std::optional<native_file> create_new(const std::string& path, std::string name);

    /// Finds the file or directory at path without access to its content, and names the result by the canonical path
    /// of what it found: absolute, with every symbolic link resolved (and every junction, on Windows), as the system
    /// names it. On POSIX systems the result holds that path alone, and no file. Throws io_error with
    /// errc::open_failed when nothing is there or the path cannot be resolved, and on Windows with
    /// errc::not_a_regular_file for a device, which has no such path; a device name is refused so before it is opened,
    /// as open_for_reading() refuses it.
    [[nodiscard]] static native_file find(const std::string& path);

    native_file(native_file&& other) noexcept
        : handle_(std::exchange(other.handle_, closed_handle)), name_(std::move(other.name_))
    {}

    native_file& operator=(native_file&& other) noexcept
    {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, closed_handle);
            name_ = std::move(other.name_);
        }
        return *this;
    }

    native_file(const native_file&) = delete;
    native_file& operator=(const native_file&) = delete;

    ~native_file()
    {
        reset();
    }

    /// The path, or the name given to create_new().
    [[nodiscard]] const std::string& name() const noexcept
    {
        return name_;
    }

    /// Opens for reading the regular file that find() found, with the same name. On Windows it is the file found,
    /// through its handle, whatever its path names by then; on POSIX systems it is the file at the canonical path,
    /// which a change of the directory tree since find() can make another one. Throws io_error with errc::open_failed
    /// or errc::not_a_regular_file.
    [[nodiscard]] native_file reopen_for_reading() const;

    [[nodiscard]] std::uint64_t size() const;

    /// Fills destination with the bytes at offset. A file that ends first is an io_error with errc::end_of_data.
    void read_at(std::uint64_t offset, std::span<std::byte> destination) const;

    void write_at(std::uint64_t offset, std::span<const std::byte> data) const;

    /// Closes the file, which create_new() made at path, and moves it over target, which it replaces atomically, also
    /// while other programs read target. With flush_to_disk, the content and the rename reach the storage device
    /// before it returns. Throws io_error with errc::write_failed. The file is then either still at path, to be
    /// discarded, or already in place of the target.
    void commit(const std::string& path, const std::string& target, bool flush_to_disk);

    /// Closes the file, which create_new() made at path, and removes it, unless commit() moved it to its target.
    void discard(const std::string& path) noexcept;

    /// Closes the file, ignoring failures.
    void reset() noexcept;

private:
    native_file(handle_type handle, std::string name) noexcept : handle_(handle), name_(std::move(name)) {}

    void flush_to_disk() const;
    // Closes the file and reports a failure, which may be a delayed write error.
    void close();

    handle_type handle_;
    std::string name_;
};

} // namespace openxisf::detail
