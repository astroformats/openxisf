// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "support/temp_directory.h"

#include "core/xoshiro.h"

#include <gtest/gtest.h>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace openxisf::test {

std::string utf8(const std::filesystem::path& path)
{
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::filesystem::path path_of(std::string_view text)
{
    std::u8string converted;
    converted.reserve(text.size());
    for (const char c : text) {
        converted += static_cast<char8_t>(c);
    }
    return converted;
}

std::filesystem::path long_form(const std::filesystem::path& path)
{
#if defined(_WIN32)
    std::filesystem::path preferred = path;
    preferred.make_preferred();
    return {LR"(\\?\)" + preferred.native()};
#else
    return path;
#endif
}

bool is_device_name(std::string_view path)
{
#if defined(_WIN32)
    const std::wstring wide = path_of(path).native();
    std::array<wchar_t, 1024> full{};
    const DWORD length = GetFullPathNameW(wide.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    return length != 0 && length < full.size() && std::wstring_view(full.data(), length).starts_with(LR"(\\.\)");
#else
    (void)path;
    return false;
#endif
}

namespace {

#if defined(_WIN32)
// Removes a directory and its content, one entry at a time. std::filesystem::remove_all() cannot be used on paths
// beyond 260 characters with every standard library: the libstdc++ of MinGW lists the wrong directory for a \\?\ path
// and never returns. The functions of the system take such paths as they are. Links are removed, not followed.
bool remove_tree(const std::wstring& path)
{
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((path + LR"(\*)").c_str(), &entry);
    bool removed = true;
    if (search != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = entry.cFileName;
            if (name == L"." || name == L"..") {
                continue;
            }
            const std::wstring child = path + LR"(\)" + name;
            const DWORD attributes = entry.dwFileAttributes;
            if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                removed = remove_tree(child) && removed;
            } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                removed = RemoveDirectoryW(child.c_str()) != FALSE && removed;
            } else {
                removed = DeleteFileW(child.c_str()) != FALSE && removed;
            }
        } while (FindNextFileW(search, &entry) != FALSE);
        (void)FindClose(search);
    }
    return RemoveDirectoryW(path.c_str()) != FALSE && removed;
}
#endif

bool remove_tree(const std::filesystem::path& path)
{
#if defined(_WIN32)
    return remove_tree(long_form(path).native());
#else
    std::error_code error;
    std::filesystem::remove_all(path, error);
    return !error;
#endif
}

} // namespace

temp_directory::temp_directory()
{
    detail::xoshiro256starstar random = detail::xoshiro256starstar::from_random_device();
    const std::filesystem::path base = std::filesystem::temp_directory_path();
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::filesystem::path candidate = base / ("openxisf-test-" + std::to_string(random()));
        if (std::filesystem::create_directory(candidate)) {
            path_ = std::filesystem::absolute(candidate);
            return;
        }
    }
    throw std::runtime_error("cannot create a temporary directory in " + utf8(base));
}

temp_directory::~temp_directory()
{
    // A directory left behind fails the test. A destructor throws nothing, so a failure to remove the directory or to
    // write the message is reported without one.
    try {
        if (!remove_tree(path_)) {
            ADD_FAILURE() << "the temporary directory " << utf8(path_) << " could not be removed";
        }
    } catch (...) {
        ADD_FAILURE();
    }
}

working_directory::working_directory(const std::filesystem::path& path) : previous_(std::filesystem::current_path())
{
    std::filesystem::current_path(path);
}

working_directory::~working_directory()
{
    std::error_code ignored;
    std::filesystem::current_path(previous_, ignored);
}

std::string temp_directory::file(std::string_view name) const
{
    return utf8(path_ / path_of(name));
}

std::size_t temp_directory::entries() const
{
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path_)) {
        (void)entry;
        ++count;
    }
    return count;
}

} // namespace openxisf::test
