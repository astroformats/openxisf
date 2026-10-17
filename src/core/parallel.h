// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <cstddef>
#include <functional>

// The only parallelism of the library: oneTBB in a build with OPENXISF_WITH_TBB, in the task arena of the calling
// thread, without any change to the global settings of oneTBB, and apart from the oneTBB work of the application.
// Without it the work runs on the calling thread.

namespace openxisf::detail {

/// The number of threads that parallel_for() can use: the concurrency of the task arena of the calling thread with
/// oneTBB, and 1 without it.
[[nodiscard]] std::size_t concurrency();

/// Calls work(i) for every i from 0 to count - 1: in parallel in the task arena of the calling thread with oneTBB, and
/// in order on the calling thread without it. The calls must be independent of each other. When one throws, the calls
/// that have not started may be skipped, and once the others are done, one of the exceptions is rethrown on the calling
/// thread. With oneTBB, every call is made even when the calling thread runs a task of a cancelled task group of the
/// application, and the calling thread runs no task of the application while it waits for the calls.
void parallel_for(std::size_t count, const std::function<void(std::size_t i)>& work);

} // namespace openxisf::detail
