// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/io.h>

#include <filesystem>
#include <memory>

namespace openxisf::bench {

/// A source over the file at path that reads around the cache of the system, as if none of its bytes were in memory:
/// each read goes to the storage device, rounded out to whole sectors of 4 KiB, through a buffer of the thread. It
/// stands for a file read for the first time, which the memory of the machine would otherwise hold after the first
/// iteration of a benchmark. Where the file system cannot read around its cache (Linux tmpfs, for one), the reads go
/// through it. Any number of threads can read it at once.
/// @throws std::system_error when the file cannot be opened, and from read() when it cannot be read.
[[nodiscard]] std::unique_ptr<input_source> open_uncached(const std::filesystem::path& path);

} // namespace openxisf::bench
