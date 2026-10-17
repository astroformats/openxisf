// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/error.h>
#include <openxisf/limits.h>

#include "container/data_block.h"
#include "core/diagnostic_log.h"
#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openxisf::detail {

/// The data that a unit holds beyond its header, counted against limits::max_ancillary_data: the data blocks loaded
/// when it is opened (the values of properties and table cells, ICC profiles and thumbnails), and the copies that
/// Reference elements make of what they name.
class ancillary_budget
{
public:
    /// A budget of limit bytes, where 0 means no limit.
    explicit ancillary_budget(std::uint64_t limit) noexcept : limit_(limit) {}

    /// Counts a data block of cost bytes that is about to be loaded. False when it would take the data beyond the
    /// limit, which is an error in log about context, and nothing is counted.
    [[nodiscard]] bool load(std::uint64_t cost, error_context context, diagnostic_log& log);

    /// Counts a copy of cost bytes of what, such as "the property 'Test:Name'". False when it would take the data
    /// beyond the limit, which is an error in log about context, and nothing is counted.
    [[nodiscard]] bool copy(std::uint64_t cost, std::string_view what, error_context context, diagnostic_log& log);

    /// Gives back cost bytes counted by load() for a data block that could not be loaded, or whose data were refused.
    void release(std::uint64_t cost) noexcept;

private:
    [[nodiscard]] bool fits(std::uint64_t cost) const noexcept;

    std::uint64_t limit_;
    std::uint64_t used_ = 0;
};

/// What a data block counts against the budget when it is loaded: the larger of its stored and its data size, which
/// the load holds at once.
[[nodiscard]] std::uint64_t block_cost(const block_descriptor& descriptor) noexcept;

/// The data of an available block, decompressed, loaded within the budget, which counts its block_cost(). Nothing when
/// it cannot be read, which is an error in log about the element at path, and costs nothing then: an io_error of the
/// source of an external block is such an error, with its class and system error for a strict log. What the unit's
/// own source throws passes through.
[[nodiscard]] std::optional<std::vector<std::byte>> load_block(const thread_safe_source& source,
                                                               const block_descriptor& descriptor, const limits& limits,
                                                               ancillary_budget& budget, const std::string& path,
                                                               diagnostic_log& log);

} // namespace openxisf::detail
