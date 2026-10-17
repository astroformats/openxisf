// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/error.h>

#include "core/utf16.h"
#include "io/native_file.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// UTF-8 paths become UTF-16 here, at the boundary of the system calls, and nowhere else.

namespace openxisf::detail {

namespace {

// ReadFile() and WriteFile() take a 32-bit length.
constexpr std::size_t max_transfer = std::size_t{1} << 30;

std::error_code last_error() noexcept
{
    return {static_cast<int>(GetLastError()), std::system_category()};
}

// The system's description of an error. std::error_code::message() would give it in the ANSI code page.
std::string describe(std::error_code error)
{
    std::array<wchar_t, 512> buffer{};
    const DWORD length =
        FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                       static_cast<DWORD>(error.value()), 0, buffer.data(), static_cast<DWORD>(buffer.size()), nullptr);
    std::u16string text(buffer.data(), buffer.data() + length);
    while (!text.empty() &&
           (text.back() == u'\r' || text.back() == u'\n' || text.back() == u' ' || text.back() == u'.')) {
        text.pop_back();
    }
    std::string fallback = "system error " + std::to_string(error.value());
    if (text.empty()) {
        return fallback;
    }
    try {
        return utf16_to_utf8(text);
    } catch (const invalid_data_error&) {
        return fallback;
    }
}

[[noreturn]] void fail(errc code, const std::string& message, std::error_code system, error_context context = {})
{
    throw io_error(code, message + ": " + describe(system), system, std::move(context));
}

[[noreturn]] void fail_not_regular(const std::string& path)
{
    throw io_error(errc::not_a_regular_file, path + " is not a regular file");
}

// path, absolute and normalized as Windows normalizes every path. Returns an empty string when GetFullPathNameW()
// fails, and GetLastError() tells why.
std::wstring full_path(const std::string& path)
{
    const std::u16string utf16 = utf8_to_utf16(path);
    const std::wstring given(utf16.begin(), utf16.end());
    DWORD length = GetFullPathNameW(given.c_str(), 0, nullptr, nullptr);
    if (length == 0) {
        return {};
    }
    std::wstring full(length, L'\0');
    length = GetFullPathNameW(given.c_str(), length, full.data(), nullptr);
    if (length == 0 || length >= full.size()) {
        return {};
    }
    full.resize(length);
    return full;
}

// The path for the wide functions of the system: absolute, normalized as Windows normalizes every path, and with the
// \\?\ prefix, which lifts the limit of MAX_PATH (260) characters. Device paths (\\.\) keep their form. Returns an
// empty string when GetFullPathNameW() fails, and GetLastError() tells why.
std::wstring system_path(const std::string& path)
{
    std::wstring full = full_path(path);
    if (full.empty()) {
        return {};
    }
    if (full.starts_with(LR"(\\?\)") || full.starts_with(LR"(\\.\)")) {
        return full;
    }
    if (full.starts_with(LR"(\\)")) {
        return LR"(\\?\UNC\)" + full.substr(2);
    }
    return LR"(\\?\)" + full;
}

OVERLAPPED at(std::uint64_t offset) noexcept
{
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFF'FFFFU);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    return overlapped;
}

bool is_directory(const std::wstring& path) noexcept
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

// True when GetFullPathNameW() made path, which did not name the device namespace, a path of it: a device name such as
// NUL in any directory, and before Windows 11 also CON, COM1 or LPT1, with or without an extension, becomes \\.\NUL.
// Such a path is refused before it is opened, since opening a device can act on it, as opening a serial port resets
// some controllers.
bool made_a_device(const std::string& path, const std::wstring& wide) noexcept
{
    const auto separator = [](char c) { return c == '\\' || c == '/'; };
    const bool given =
        path.size() >= 4 && separator(path[0]) && separator(path[1]) && path[2] == '.' && separator(path[3]);
    return !given && wide.starts_with(LR"(\\.\)");
}

// Renames an open file over target with POSIX semantics, which replace a target that other programs have open, as a
// rename does on POSIX systems, provided that they opened it with FILE_SHARE_DELETE, as file_source does; the rename
// fails otherwise. Windows 10 1607 and later support it on NTFS. Returns false where it is not supported.
bool rename_by_handle(HANDLE handle, const std::wstring& target, const std::string& name)
{
    // FILE_RENAME_INFO ends with the name, which extends beyond the structure.
    const std::size_t name_bytes = target.size() * sizeof(wchar_t);
    const std::size_t size = offsetof(FILE_RENAME_INFO, FileName) + name_bytes + sizeof(wchar_t);
    std::vector<FILE_RENAME_INFO> buffer((size + sizeof(FILE_RENAME_INFO) - 1) / sizeof(FILE_RENAME_INFO));
    FILE_RENAME_INFO& info = buffer.front();
    info.Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
    info.RootDirectory = nullptr;
    info.FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(reinterpret_cast<std::byte*>(buffer.data()) + offsetof(FILE_RENAME_INFO, FileName), target.c_str(),
                name_bytes + sizeof(wchar_t));

    if (SetFileInformationByHandle(handle, FileRenameInfoEx, &info, static_cast<DWORD>(size)) != FALSE) {
        return true;
    }
    const std::error_code error = last_error();
    if (error.value() == ERROR_INVALID_PARAMETER || error.value() == ERROR_NOT_SUPPORTED ||
        error.value() == ERROR_INVALID_FUNCTION) {
        return false;
    }
    fail(errc::write_failed, "cannot replace " + name, error);
}

} // namespace

std::string absolute_path(const std::string& path)
{
    const std::wstring full = full_path(path);
    if (full.empty()) {
        fail(errc::open_failed, "cannot make " + path + " absolute", last_error());
    }
    try {
        return utf16_to_utf8(std::u16string(full.begin(), full.end()));
    } catch (const invalid_data_error&) {
        throw io_error(errc::open_failed, "the absolute path of " + path + " is not valid UTF-16");
    }
}

void check_replaceable(const std::string& path)
{
    const std::wstring wide = system_path(path);
    if (wide.empty()) {
        return;
    }
    // GetFullPathNameW() makes a device name such as NUL, COM1 or CON, in any directory, a device of the device
    // namespace: \\.\NUL, with no separator after the prefix.
    if (wide.starts_with(LR"(\\.\)") && wide.find(L'\\', 4) == std::wstring::npos) {
        fail_not_regular(path);
    }
    // The attributes of a symbolic link or a junction are its own, with FILE_ATTRIBUTE_REPARSE_POINT: a directory that
    // is one is a link, which a rename replaces.
    const DWORD attributes = GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    const bool directory =
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
    if (directory || (attributes & FILE_ATTRIBUTE_DEVICE) != 0) {
        fail_not_regular(path);
    }
}

bool is_directory(const std::string& path)
{
    // A junction, or a symbolic link to a directory, has FILE_ATTRIBUTE_DIRECTORY itself.
    const std::wstring wide = system_path(path);
    return !wide.empty() && is_directory(wide);
}

native_file native_file::open_for_reading(const std::string& path)
{
    const std::wstring wide = system_path(path);
    if (wide.empty()) {
        fail(errc::open_failed, "cannot open " + path, last_error());
    }
    if (made_a_device(path, wide)) {
        fail_not_regular(path);
    }
    // Other programs may go on using the file, as they could on POSIX systems.
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const std::error_code error = last_error();
        // Opening a directory needs FILE_FLAG_BACKUP_SEMANTICS. Without it, the failure is a denied access, but for the
        // root of a drive (ERROR_PATH_NOT_FOUND) or of a share (ERROR_INVALID_NAME).
        if (is_directory(wide)) {
            fail_not_regular(path);
        }
        fail(errc::open_failed, "cannot open " + path, error);
    }
    native_file file(handle, path);
    // A device such as NUL or a named pipe is not a disk file.
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        fail_not_regular(path);
    }
    return file;
}

std::optional<native_file> native_file::create_new(const std::string& path, std::string name)
{
    const std::wstring wide = system_path(path);
    if (wide.empty()) {
        fail(errc::open_failed, "cannot create a temporary file next to " + name, last_error());
    }
    // DELETE access lets commit() rename the file and discard() delete it through the handle, which stays open until
    // then. FILE_SHARE_DELETE lets MoveFileExW() move the file while the handle is open.
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const std::error_code error = last_error();
        if (error.value() == ERROR_FILE_EXISTS || error.value() == ERROR_ALREADY_EXISTS) {
            return std::nullopt;
        }
        fail(errc::open_failed, "cannot create a temporary file next to " + name, error);
    }
    return native_file(handle, std::move(name));
}

native_file native_file::find(const std::string& path)
{
    const std::wstring wide = system_path(path);
    if (wide.empty()) {
        fail(errc::open_failed, "cannot find " + path, last_error());
    }
    if (made_a_device(path, wide)) {
        fail_not_regular(path);
    }
    // A handle without access to the content only names the file; backup semantics let it name a directory too.
    // Symbolic links and junctions are followed.
    HANDLE handle = CreateFileW(wide.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        fail(errc::open_failed, "cannot find " + path, last_error());
    }
    native_file found(handle, path);
    if (GetFileType(handle) != FILE_TYPE_DISK) {
        fail_not_regular(path);
    }

    // The name of the volume as a drive letter, or as a volume GUID for a volume without one.
    std::wstring final_path(MAX_PATH, L'\0');
    for (const DWORD volume : {DWORD{VOLUME_NAME_DOS}, DWORD{VOLUME_NAME_GUID}}) {
        DWORD length = GetFinalPathNameByHandleW(handle, final_path.data(), static_cast<DWORD>(final_path.size()),
                                                 FILE_NAME_NORMALIZED | volume);
        if (length >= final_path.size()) {
            final_path.resize(length);
            length = GetFinalPathNameByHandleW(handle, final_path.data(), static_cast<DWORD>(final_path.size()),
                                               FILE_NAME_NORMALIZED | volume);
        }
        if (length != 0 && length < final_path.size()) {
            final_path.resize(length);
            try {
                found.name_ = utf16_to_utf8(std::u16string(final_path.begin(), final_path.end()));
            } catch (const invalid_data_error&) {
                throw io_error(errc::open_failed, "the canonical path of " + path + " is not valid UTF-16");
            }
            return found;
        }
    }
    fail(errc::open_failed, "cannot resolve " + path, last_error());
}

native_file native_file::reopen_for_reading() const
{
    // The file of the handle, not the file that its path names: Windows trims the dots and spaces that end each step
    // of a path that it normalizes, so the canonical path of a directory named "sub." would open "sub" instead.
    BY_HANDLE_FILE_INFORMATION information{};
    if (GetFileInformationByHandle(handle_, &information) == FALSE) {
        fail(errc::open_failed, "cannot open " + name_, last_error());
    }
    if ((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        fail_not_regular(name_);
    }
    HANDLE handle = ReOpenFile(handle_, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0);
    if (handle == INVALID_HANDLE_VALUE) {
        fail(errc::open_failed, "cannot open " + name_, last_error());
    }
    return {handle, name_};
}

std::uint64_t native_file::size() const
{
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle_, &size) == FALSE) {
        fail(errc::read_failed, "cannot read the size of " + name_, last_error());
    }
    return static_cast<std::uint64_t>(size.QuadPart);
}

void native_file::read_at(std::uint64_t offset, std::span<std::byte> destination) const
{
    // Each call names its offset in an OVERLAPPED structure, so concurrent reads do not share a file pointer. The
    // handle is synchronous, so ReadFile() returns when the read is complete.
    std::uint64_t position = offset;
    while (!destination.empty()) {
        const auto chunk = static_cast<DWORD>(std::min(destination.size(), max_transfer));
        OVERLAPPED overlapped = at(position);
        DWORD transferred = 0;
        if (ReadFile(handle_, destination.data(), chunk, &transferred, &overlapped) == FALSE) {
            const std::error_code error = last_error();
            if (error.value() != ERROR_HANDLE_EOF) {
                fail(errc::read_failed, "cannot read " + name_, error, {.offset = position});
            }
            transferred = 0;
        }
        if (transferred == 0) {
            throw io_error(errc::end_of_data, name_ + " ends before the data that was asked for", {},
                           {.offset = position});
        }
        destination = destination.subspan(transferred);
        position += transferred;
    }
}

void native_file::write_at(std::uint64_t offset, std::span<const std::byte> data) const
{
    std::uint64_t position = offset;
    while (!data.empty()) {
        const auto chunk = static_cast<DWORD>(std::min(data.size(), max_transfer));
        OVERLAPPED overlapped = at(position);
        DWORD transferred = 0;
        if (WriteFile(handle_, data.data(), chunk, &transferred, &overlapped) == FALSE) {
            fail(errc::write_failed, "cannot write " + name_, last_error(), {.offset = position});
        }
        data = data.subspan(transferred);
        position += transferred;
    }
}

void native_file::flush_to_disk() const
{
    if (FlushFileBuffers(handle_) == FALSE) {
        fail(errc::write_failed, "cannot flush " + name_ + " to disk", last_error());
    }
}

void native_file::commit(const std::string& path, const std::string& target, bool flush_to_disk)
{
    if (flush_to_disk) {
        this->flush_to_disk();
    }
    const std::wstring wide_target = system_path(target);
    if (wide_target.empty()) {
        fail(errc::write_failed, "cannot replace " + target, last_error());
    }
    // Once the file is in place of the target, the handle moves out of this object, so that a failure afterwards
    // leaves discard() nothing to delete.
    if (rename_by_handle(handle_, wide_target, target)) {
        native_file renamed = std::move(*this);
        if (flush_to_disk) {
            renamed.flush_to_disk();
        }
        renamed.close();
        return;
    }

    // Where POSIX semantics are not supported, such as on FAT file systems, the file is moved by path, which fails
    // while another program has the target open.
    const std::wstring wide_path = system_path(path);
    if (wide_path.empty()) {
        fail(errc::write_failed, "cannot replace " + target, last_error());
    }
    DWORD flags = MOVEFILE_REPLACE_EXISTING;
    if (flush_to_disk) {
        flags |= MOVEFILE_WRITE_THROUGH;
    }
    if (MoveFileExW(wide_path.c_str(), wide_target.c_str(), flags) == FALSE) {
        fail(errc::write_failed, "cannot replace " + target, last_error());
    }
    native_file moved = std::move(*this);
    moved.close();
}

void native_file::discard(const std::string& /*path*/) noexcept
{
    // Through the handle: no path to convert, and no file that took the name meanwhile can be hit.
    if (handle_ != closed_handle) {
        FILE_DISPOSITION_INFO disposition{};
        disposition.DeleteFile = TRUE;
        (void)SetFileInformationByHandle(handle_, FileDispositionInfo, &disposition, sizeof(disposition));
    }
    reset();
}

void native_file::close()
{
    HANDLE handle = std::exchange(handle_, closed_handle);
    if (handle != closed_handle && CloseHandle(handle) == FALSE) {
        fail(errc::write_failed, "cannot write " + name_, last_error());
    }
}

void native_file::reset() noexcept
{
    HANDLE handle = std::exchange(handle_, closed_handle);
    if (handle != closed_handle) {
        (void)CloseHandle(handle);
    }
}

} // namespace openxisf::detail
