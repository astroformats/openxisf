// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

namespace openxisf::test {

/// The UTF-8 form of a path. std::filesystem::path::string() would give the ANSI code page on Windows.
[[nodiscard]] std::string utf8(const std::filesystem::path& path);

/// The path that UTF-8 text names, built without the ANSI code page on Windows.
[[nodiscard]] std::filesystem::path path_of(std::string_view text);

/// An absolute path in the form that the file functions of Windows accept beyond 260 characters, with the \\?\ prefix.
/// Unchanged on other systems.
[[nodiscard]] std::filesystem::path long_form(const std::filesystem::path& path);

/// Whether Windows makes the UTF-8 path a path of its device namespace, \\.\NAME, as it makes NUL in any directory, and
/// before Windows 11 also CON, AUX, PRN, COM1 to COM9 and LPT1 to LPT9, with or without an extension. False on other
/// systems.
[[nodiscard]] bool is_device_name(std::string_view path);

/// A new, empty directory in the temporary directory of the system, removed with its content on destruction, so that
/// a test leaves nothing behind whether it passes or fails.
class temp_directory
{
public:
    temp_directory();
    ~temp_directory();

    temp_directory(const temp_directory&) = delete;
    temp_directory& operator=(const temp_directory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept
    {
        return path_;
    }

    /// The UTF-8 path of the entry called name, which is UTF-8 too.
    [[nodiscard]] std::string file(std::string_view name) const;

    /// The number of entries in the directory.
    [[nodiscard]] std::size_t entries() const;

private:
    std::filesystem::path path_;
};

/// Makes a directory the working directory of the process while it exists.
class working_directory
{
public:
    explicit working_directory(const std::filesystem::path& path);
    ~working_directory();

    working_directory(const working_directory&) = delete;
    working_directory& operator=(const working_directory&) = delete;

private:
    std::filesystem::path previous_;
};

} // namespace openxisf::test
