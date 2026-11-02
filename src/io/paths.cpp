// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "io/paths.h"

#include <openxisf/error.h>

#include "core/quote.h"
#include "core/utf8.h"

#include <algorithm>
#include <cstddef>

namespace openxisf::detail {

namespace {

#if defined(_WIN32)
constexpr std::string_view separators = "/\\";

// Windows refuses control characters and these in file names; the backslash and the colon also separate its paths,
// which UNIX syntax does not.
bool is_forbidden(char c) noexcept
{
    return static_cast<unsigned char>(c) < 0x20 || std::string_view(R"(<>:"\|?*)").find(c) != std::string_view::npos;
}

void check_characters(std::string_view path, std::string_view whole)
{
    if (std::ranges::any_of(path, is_forbidden)) {
        throw unsupported_error(errc::unsupported_location,
                                "the path " + quote(whole) +
                                    " names no file on Windows, whose file names cannot hold control characters or "
                                    "any of < > : \" \\ | ? *");
    }
}

bool is_ascii_letter(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
#else
constexpr std::string_view separators = "/";
#endif

bool is_separator(char c) noexcept
{
    return separators.find(c) != std::string_view::npos;
}

} // namespace

void check_path_argument(std::string_view path, std::string_view what)
{
    if (path.empty()) {
        throw usage_error(errc::invalid_argument, std::string(what) + " is empty");
    }
    if (!is_valid_utf8(path)) {
        throw usage_error(errc::invalid_utf8, std::string(what) + ", " + quote(path) + ", is not valid UTF-8");
    }
}

std::string parent_directory(std::string_view path)
{
    const std::size_t separator = path.find_last_of(separators);
    if (separator == std::string_view::npos) {
#if defined(_WIN32)
        // C:name names a file in the current directory of drive C.
        if (path.size() >= 2 && path[1] == ':') {
            return std::string(path.substr(0, 2)) + ".";
        }
#endif
        return ".";
    }
    const std::string_view directory = path.substr(0, separator);
    // A root keeps its separator: "/", and on Windows "\" or "C:\", which "C:" alone would not name.
    bool root = directory.empty();
#if defined(_WIN32)
    root = root || directory.ends_with(':');
#endif
    return std::string(root ? path.substr(0, separator + 1) : directory);
}

std::string_view file_name(std::string_view path) noexcept
{
    std::size_t start = path.find_last_of(separators);
    start = start == std::string_view::npos ? 0 : start + 1;
#if defined(_WIN32)
    // C:name
    if (start == 0 && path.size() >= 2 && path[1] == ':') {
        start = 2;
    }
#endif
    return path.substr(start);
}

std::string relative_system_path(std::string_view directory, std::string_view relative)
{
#if defined(_WIN32)
    check_characters(relative, relative);
#endif
    std::string path(directory.empty() ? std::string_view(".") : directory);
    if (!is_separator(path.back())) {
        path += '/';
    }
    path += relative;
    return path;
}

std::string absolute_system_path(std::string_view absolute)
{
#if defined(_WIN32)
    if (absolute.starts_with("//")) {
        throw unsupported_error(errc::location_not_allowed,
                                "the path " + quote(absolute) + " names a network resource, which is not opened");
    }
    // /c/dir/file, the form of MSYS2: the drive, then the path on it.
    if (absolute.size() < 2 || absolute.front() != '/' || !is_ascii_letter(absolute[1]) ||
        (absolute.size() > 2 && absolute[2] != '/')) {
        throw unsupported_error(errc::unsupported_location,
                                "the absolute path " + quote(absolute) +
                                    " names no drive, as an absolute path on Windows must: /c/dir/file names "
                                    "C:\\dir\\file");
    }
    const std::string_view rest = absolute.substr(2);
    check_characters(rest, absolute);
    const char drive = absolute[1] >= 'a' ? static_cast<char>(absolute[1] - 'a' + 'A') : absolute[1];
    return std::string(1, drive) + ":" + (rest.empty() ? std::string("/") : std::string(rest));
#else
    return std::string(absolute);
#endif
}

bool is_inside(std::string_view path, std::string_view directory) noexcept
{
    if (directory.empty() || path.size() <= directory.size() || !path.starts_with(directory)) {
        return false;
    }
    return is_separator(directory.back()) || is_separator(path[directory.size()]);
}

#if defined(_WIN32)
namespace {

// True when text starts with prefix, an ASCII word, in any case, as the names of the object manager compare.
bool starts_with_word(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() &&
           std::ranges::equal(text.substr(0, prefix.size()), prefix, [](char a, char b) {
               return (a >= 'A' && a <= 'Z' ? a - 'A' + 'a' : a) == (b >= 'A' && b <= 'Z' ? b - 'A' + 'a' : b);
           });
}

} // namespace

bool names_a_device(std::string_view path) noexcept
{
    if (path.size() < 4 || !is_separator(path[0]) || !is_separator(path[1]) || (path[2] != '.' && path[2] != '?') ||
        !is_separator(path[3])) {
        return false;
    }
    const std::string_view rest = path.substr(4);
    if (rest.size() >= 3 && is_ascii_letter(rest[0]) && rest[1] == ':' && is_separator(rest[2])) {
        return false;
    }
    if (starts_with_word(rest, "UNC") && rest.size() > 3 && is_separator(rest[3])) {
        return false;
    }
    if (starts_with_word(rest, "Volume{")) {
        const std::size_t end = rest.find('}');
        return end == std::string_view::npos || end + 1 >= rest.size() || !is_separator(rest[end + 1]);
    }
    return true;
}
#endif

} // namespace openxisf::detail
