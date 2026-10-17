// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <filesystem>

// Links in the file system, for the tests of path confinement.

namespace openxisf::test {

/// Creates a symbolic link at link to target, a file or a directory. False when Windows does not let the process create
/// one, which needs a privilege or developer mode there; any other failure throws std::system_error, so that a test
/// fails rather than skips.
[[nodiscard]] bool create_symbolic_link(const std::filesystem::path& target, const std::filesystem::path& link);

#if defined(_WIN32)
/// Creates a junction at link to the directory target, which every user of Windows can create; an absolute target is
/// taken as it is, without the normalization that Windows applies to paths. Throws std::runtime_error when it cannot.
void create_junction(const std::filesystem::path& target, const std::filesystem::path& link);
#endif

} // namespace openxisf::test
