// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "support/links.h"

#include "support/temp_directory.h"

#if defined(_WIN32)
// clang-format off: the I/O control codes of winioctl.h need the types of windows.h.
#include <windows.h>
#include <winioctl.h>
// clang-format on
#endif

#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace openxisf::test {

namespace {

#if defined(_WIN32)
// Windows refuses a symbolic link to a process without the privilege or developer mode: ERROR_PRIVILEGE_NOT_HELD with
// the standard library of MSVC, EPERM with libc++, which maps the errors of Windows to those of POSIX, and libstdc++
// creates none on Windows (ENOSYS, and ENOTSUP with the libstdc++ of GCC 11).
bool refused_by_windows(const std::error_code& error)
{
    return (error.category() == std::system_category() && error.value() == ERROR_PRIVILEGE_NOT_HELD) ||
           error == std::errc::operation_not_permitted || error == std::errc::function_not_supported ||
           error == std::errc::not_supported;
}
#endif

} // namespace

bool create_symbolic_link(const std::filesystem::path& target, const std::filesystem::path& link)
{
    std::error_code error;
    if (std::filesystem::is_directory(target)) {
        std::filesystem::create_directory_symlink(target, link, error);
    } else {
        std::filesystem::create_symlink(target, link, error);
    }
    if (!error) {
        return true;
    }
#if defined(_WIN32)
    if (refused_by_windows(error)) {
        return false;
    }
#endif
    throw std::system_error(error, "cannot create the symbolic link " + utf8(link));
}

#if defined(_WIN32)
void create_junction(const std::filesystem::path& target, const std::filesystem::path& link)
{
    std::filesystem::create_directory(link);
    // The reparse data of a mount point, REPARSE_DATA_BUFFER of the driver kit: a header, then the target as an NT
    // path, \??\C:\dir, and as it is printed, each with its terminating null. An absolute target is kept as it is:
    // std::filesystem::absolute() would drop a dot that ends one of its steps.
    const std::wstring printed = (target.is_absolute() ? target : std::filesystem::absolute(target)).native();
    const std::wstring substitute = LR"(\??\)" + printed;
    const std::size_t substitute_size = substitute.size() * sizeof(wchar_t);
    const std::size_t printed_size = printed.size() * sizeof(wchar_t);
    constexpr std::size_t header_size = 16;
    std::vector<std::byte> buffer(header_size + substitute_size + printed_size + (2 * sizeof(wchar_t)));
    const auto put = [&buffer](std::size_t offset, const void* data, std::size_t size) {
        std::memcpy(buffer.data() + offset, data, size);
    };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    // The length of what follows the first 8 bytes, a reserved field, then the offset and length of each name.
    const std::array<WORD, 6> words{static_cast<WORD>(buffer.size() - 8),
                                    0,
                                    0,
                                    static_cast<WORD>(substitute_size),
                                    static_cast<WORD>(substitute_size + sizeof(wchar_t)),
                                    static_cast<WORD>(printed_size)};
    put(0, &tag, sizeof(tag));
    put(4, words.data(), words.size() * sizeof(WORD));
    put(header_size, substitute.c_str(), substitute_size + sizeof(wchar_t));
    put(header_size + substitute_size + sizeof(wchar_t), printed.c_str(), printed_size + sizeof(wchar_t));

    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("cannot open the directory of the junction");
    }
    DWORD returned = 0;
    const BOOL created = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                                         static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    (void)CloseHandle(handle);
    if (created == FALSE) {
        throw std::runtime_error("cannot create the junction");
    }
}
#endif

} // namespace openxisf::test
