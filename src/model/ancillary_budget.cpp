// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/ancillary_budget.h"

#include <algorithm>
#include <string>
#include <utility>

namespace openxisf::detail {

bool ancillary_budget::load(std::uint64_t cost, error_context context, diagnostic_log& log)
{
    if (!fits(cost)) {
        log.error(errc::ancillary_data_too_large,
                  "the data block of " + std::to_string(cost) +
                      " bytes would take the data loaded when the unit is opened beyond the limit of " +
                      std::to_string(limit_) + " bytes",
                  std::move(context));
        return false;
    }
    used_ += cost;
    return true;
}

bool ancillary_budget::copy(std::uint64_t cost, std::string_view what, error_context context, diagnostic_log& log)
{
    if (!fits(cost)) {
        log.error(errc::ancillary_data_too_large,
                  "a copy of " + std::string(what) + " of " + std::to_string(cost) +
                      " bytes would take the data held by the unit beyond the limit of " + std::to_string(limit_) +
                      " bytes",
                  std::move(context));
        return false;
    }
    used_ += cost;
    return true;
}

void ancillary_budget::release(std::uint64_t cost) noexcept
{
    used_ -= cost;
}

bool ancillary_budget::fits(std::uint64_t cost) const noexcept
{
    return limit_ == 0 || cost <= limit_ - used_;
}

std::uint64_t block_cost(const block_descriptor& descriptor) noexcept
{
    return std::max(stored_size(descriptor), data_size(descriptor));
}

std::optional<std::vector<std::byte>> load_block(const thread_safe_source& source, const block_descriptor& descriptor,
                                                 const limits& limits, ancillary_budget& budget,
                                                 const std::string& path, diagnostic_log& log)
{
    error_context context{.element = path};
    if (descriptor.location.kind == location_kind::attachment) {
        context.offset = descriptor.location.position;
    }
    // Counted before the read, which holds that much memory.
    const std::uint64_t cost = block_cost(descriptor);
    if (!budget.load(cost, context, log)) {
        return std::nullopt;
    }
    try {
        return read_block_data(source, descriptor, limits);
    } catch (const integrity_error& failure) {
        log.error(failure.code(), failure.what(), std::move(context));
    } catch (const limit_error& failure) {
        log.error(failure.code(), failure.what(), std::move(context));
    } catch (const unsupported_error& failure) {
        log.error(failure.code(), failure.what(), std::move(context));
    } catch (const invalid_data_error& failure) {
        log.error(failure.code(), failure.what(), std::move(context));
    } catch (const io_error& failure) {
        // The source of an external file fails as when its block index is read: the block is unavailable, and a strict
        // open throws the io_error with its code and system error. One of the unit's own source fails the open.
        if (!is_external(descriptor.location.kind)) {
            throw;
        }
        log.error(failure.code(), failure.what(), std::move(context),
                  {.raised = error_origin::kind::io, .system_code = failure.system_code()});
    }
    // Nothing is held, so a block that fails takes no room from those after it.
    budget.release(cost);
    return std::nullopt;
}

} // namespace openxisf::detail
