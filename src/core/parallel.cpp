// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/parallel.h"

#if defined(OPENXISF_WITH_TBB)
#include <oneapi/tbb/parallel_for.h>
#include <oneapi/tbb/task_arena.h>
#include <oneapi/tbb/task_group.h>
#endif

namespace openxisf::detail {

std::size_t concurrency()
{
#if defined(OPENXISF_WITH_TBB)
    const int threads = oneapi::tbb::this_task_arena::max_concurrency();
    return threads > 1 ? static_cast<std::size_t>(threads) : 1;
#else
    return 1;
#endif
}

void parallel_for(std::size_t count, const std::function<void(std::size_t i)>& work)
{
#if defined(OPENXISF_WITH_TBB)
    if (count > 1) {
        // Each call is a large piece of work, such as a subblock, so each gets a task of its own. oneTBB rethrows the
        // exception of a task on the calling thread, and cancels the tasks that have not started.
        //
        // The calling thread may run a task of the application, whose oneTBB work is kept apart from the loop. The
        // loop has a context of its own, isolated, which a cancellation of the application's group does not reach:
        // bound to that group, the loop would run none of its calls and return as if it had. And the calling thread,
        // while it waits for the calls that other threads took, runs only calls of the loop, not the application's
        // tasks, which would run on top of a call of the library that has not returned, where the application may
        // hold a lock that they take.
        oneapi::tbb::task_group_context context(oneapi::tbb::task_group_context::isolated);
        oneapi::tbb::this_task_arena::isolate([count, &work, &context] {
            oneapi::tbb::parallel_for(
                oneapi::tbb::blocked_range<std::size_t>(0, count, 1),
                [&work](const oneapi::tbb::blocked_range<std::size_t>& range) {
                    for (std::size_t i = range.begin(); i != range.end(); ++i) {
                        work(i);
                    }
                },
                oneapi::tbb::simple_partitioner(), context);
        });
        return;
    }
#endif
    for (std::size_t i = 0; i < count; ++i) {
        work(i);
    }
}

} // namespace openxisf::detail
