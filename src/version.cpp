// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/version.h>

namespace openxisf {

std::string_view version() noexcept
{
    return OPENXISF_VERSION_STRING;
}

version_info version_numbers() noexcept
{
    return {.major = OPENXISF_VERSION_MAJOR, .minor = OPENXISF_VERSION_MINOR, .patch = OPENXISF_VERSION_PATCH};
}

} // namespace openxisf
