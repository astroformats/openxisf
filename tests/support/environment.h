// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#if defined(_WIN32)
#include "core/utf16.h"

#include <windows.h>
#else
#include <cstdlib>
#endif

#include <optional>
#include <string>
#include <string_view>

namespace openxisf::test {

/// The value of an environment variable, in UTF-8, or nothing when it is not set or empty, as an empty variable of a CI
/// workflow is. Tests read only two, both optional: OPENXISF_LARGE_SAMPLES_DIR and OPENXISF_WRITTEN_UNITS_DIR.
[[nodiscard]] inline std::optional<std::string> environment_variable(std::string_view name)
{
#if defined(_WIN32)
    // The wide function, since a narrow one gives the value in the ANSI code page.
    const std::u16string wide_name = detail::utf8_to_utf16(name);
    const auto* key = reinterpret_cast<const wchar_t*>(wide_name.c_str());
    const DWORD size = GetEnvironmentVariableW(key, nullptr, 0);
    if (size == 0) {
        return std::nullopt;
    }
    std::u16string value(size, u'\0');
    value.resize(GetEnvironmentVariableW(key, reinterpret_cast<wchar_t*>(value.data()), size));
    if (value.empty()) {
        return std::nullopt;
    }
    return detail::utf16_to_utf8(value);
#else
    // No test sets environment variables, so reading one cannot race.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* value = std::getenv(std::string(name).c_str());
    if (value == nullptr || *value == '\0') {
        return std::nullopt;
    }
    return std::string(value);
#endif
}

} // namespace openxisf::test
