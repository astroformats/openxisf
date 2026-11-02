// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <string>
#include <string_view>

// Paths of local files, for the resolver of external data blocks. A unit writes them in UNIX syntax (spec §10.3); the
// system takes them in its own. Every path is UTF-8.

namespace openxisf::detail {

/// Throws usage_error unless path, a path that the application gives, is not empty (errc::invalid_argument) and is
/// valid UTF-8 (errc::invalid_utf8). what names it in the messages, as in "the path of the data blocks file".
void check_path_argument(std::string_view path, std::string_view what);

/// The directory of the file at path, in the syntax of the system: "." for a file name alone, and a root with its
/// separator, such as "/" or, on Windows, "C:\".
[[nodiscard]] std::string parent_directory(std::string_view path);

/// The name of the file at path, in the syntax of the system: what follows its directory.
[[nodiscard]] std::string_view file_name(std::string_view path) noexcept;

/// The path of the system for relative, a path in UNIX syntax from directory. On Windows, throws unsupported_error with
/// errc::unsupported_location when relative holds a character that no file name can hold there, such as a backslash or
/// a colon.
[[nodiscard]] std::string relative_system_path(std::string_view directory, std::string_view relative);

/// The path of the system for absolute, an absolute path in UNIX syntax: absolute itself, except on Windows, where
/// /c/dir/file names C:/dir/file. There, throws unsupported_error with errc::location_not_allowed for a network path,
/// which starts with two slashes, and with errc::unsupported_location for any other path that names no drive, or that
/// holds a character that no file name can hold.
[[nodiscard]] std::string absolute_system_path(std::string_view absolute);

/// True when path is inside directory, both canonical paths of the system (native_file::find()).
[[nodiscard]] bool is_inside(std::string_view path, std::string_view directory) noexcept;

#if defined(_WIN32)
/// True when path, absolute and normalized as Windows normalizes a path (GetFullPathNameW()), names a device: a path of
/// the device namespace, \\.\ or \\?\, that does not go on with a file system, a drive and a separator (C:\), UNC\ or a
/// volume (Volume{...}\). \\.\NUL, which a device name such as NUL becomes in any directory, \\.\COM1, \\.\pipe\name,
/// \\.\C:, the volume itself, and \\?\GLOBALROOT\Device\Null are devices.
[[nodiscard]] bool names_a_device(std::string_view path) noexcept;
#endif

} // namespace openxisf::detail
