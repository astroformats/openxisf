// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/editor.h>

#include "container/file_layout.h"
#include "io/thread_safe_device.h"
#include "model/pixel_writes.h"
#include "model/pixels.h"
#include "model/unit.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace openxisf {

namespace {

// The unit in device, opened as a reader opens it. A header file is refused before it is read: its data blocks are in
// other files, which the editor does not edit.
detail::unit open_monolithic(const detail::thread_safe_device& device, read_options options)
{
    std::array<char, detail::monolithic_header_offset> start{};
    const std::size_t available = static_cast<std::size_t>(std::min<std::uint64_t>(device.size(), start.size()));
    device.read(0, std::as_writable_bytes(std::span(start).first(available)));
    if (detail::storage_of(std::string_view(start.data(), available)) == unit_storage::distributed) {
        throw unsupported_error(errc::not_editable, "the unit is a header file, whose data blocks are in other files: "
                                                    "an editor edits monolithic files alone");
    }
    // A monolithic file has no external data blocks, which a resolver would open. Other threads write the device
    // while it is read, at regions that share no sample: a read of a region reads the bytes of its runs alone.
    options.resolver = {};
    return {device.source(), std::move(options), true};
}

} // namespace

struct editor::state
{
    state(std::unique_ptr<random_access_device> input, read_options options)
        : device(std::move(input)), opened(open_monolithic(device, std::move(options))),
          overlapping(detail::overlapping_blocks(opened.blocks)), written(opened.blocks.size()),
          verified(opened.blocks.size()), verifying(opened.blocks.size())
    {}

    // True when the block was written since the open or the last commit(), so that its digest is stale.
    [[nodiscard]] bool is_written(std::size_t block) const
    {
        const std::scoped_lock lock(mutex);
        return written[block];
    }

    // Records that the block is being written, before any of its bytes are.
    void mark_written(std::size_t block)
    {
        const std::scoped_lock lock(mutex);
        written[block] = true;
    }

    // Verifies the block, when it has a checksum, once before its first write, so that commit() gives its digest to
    // bytes that were verified and those written alone; or, unless verify, takes it as verified, the choice of the
    // application. A write of the block on another thread meanwhile waits for the verification; a block that fails it
    // is verified again at its next write. The digest that commit() writes is the editor's own, so a block stays
    // verified after it.
    void verify_once(std::size_t block, bool verify)
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this, block] { return !verifying[block]; });
        if (verified[block]) {
            return;
        }
        if (!verify) {
            verified[block] = true;
            return;
        }
        verifying[block] = true;
        lock.unlock();
        try {
            detail::verify_digest(device, opened.blocks[block]);
        } catch (...) {
            end_verification(block, false);
            throw;
        }
        end_verification(block, true);
    }

    void end_verification(std::size_t block, bool sound)
    {
        {
            const std::scoped_lock lock(mutex);
            verifying[block] = false;
            verified[block] = sound;
        }
        changed.notify_all();
    }

    // Calls read with options, or with options that do not verify the block of the image at index when it was written
    // since the open or the last commit(). A read that verifies a block whose first write begins meanwhile can see part
    // of what is written, and fail the checksum: it is made again without verification, since the block is marked
    // before it is written.
    template <typename Read>
    decltype(auto) read(std::size_t index, const pixel_read_options& options, const Read& read) const
    {
        (void)detail::image_at(opened, index);
        const std::size_t block = opened.images.blocks[index];
        const std::optional<detail::block_descriptor>& descriptor = opened.blocks[block].descriptor;
        if (!options.verify_checksum || !descriptor || !descriptor->checksum) {
            return read(options);
        }
        if (!is_written(block)) {
            try {
                return read(options);
            } catch (const integrity_error& failure) {
                if (failure.code() != errc::checksum_mismatch || !is_written(block)) {
                    throw;
                }
            }
        }
        pixel_read_options unverified = options;
        unverified.verify_checksum = false;
        return read(unverified);
    }

    detail::thread_safe_device device;
    detail::unit opened;
    // For each block, whether it shares bytes with another one, which makes it not editable.
    std::vector<bool> overlapping;
    // For each block, whether it was written since the open or the last commit(), under the mutex.
    std::vector<bool> written;
    // For each block, whether it was verified before its first write, and whether a thread is verifying it, under the
    // mutex, whose changes the condition tells.
    std::vector<bool> verified;
    std::vector<bool> verifying;
    mutable std::mutex mutex;
    std::condition_variable changed;
};

editor::editor(std::string_view path, read_options options)
    : editor(std::make_unique<file_device>(path), std::move(options))
{}

editor::editor(std::unique_ptr<random_access_device> device, read_options options)
    : state_(std::make_unique<state>(std::move(device), std::move(options)))
{}

editor::editor(editor&& other) noexcept = default;
editor& editor::operator=(editor&& other) noexcept = default;
editor::~editor() = default;

std::span<const image_info> editor::images() const noexcept
{
    return state_->opened.images.infos;
}

const image_info& editor::image(std::size_t index) const
{
    return detail::image_at(state_->opened, index);
}

std::span<const diagnostic> editor::diagnostics() const noexcept
{
    return state_->opened.diagnostics;
}

std::vector<std::byte> editor::read_pixels(std::size_t index, const pixel_read_options& options) const
{
    return state_->read(index, options, [this, index](const pixel_read_options& chosen) {
        return detail::read_pixels(state_->opened, index, chosen);
    });
}

void editor::read_pixels(std::size_t index, std::span<std::byte> destination, const pixel_read_options& options) const
{
    state_->read(index, options, [this, index, destination](const pixel_read_options& chosen) {
        detail::read_pixels(state_->opened, index, destination, chosen);
    });
}

void editor::write_pixels(std::size_t index, std::span<const std::byte> data, const pixel_write_options& options)
{
    const detail::region_layout layout = detail::write_layout(state_->opened, index, data, options);
    detail::check_editable(state_->opened, index, state_->overlapping);
    const std::size_t block = state_->opened.images.blocks[index];
    state_->verify_once(block, options.verify_checksum);
    state_->mark_written(block);
    detail::write_pixels(state_->opened, state_->device, index, data, layout, options);
}

void editor::commit()
{
    for (std::size_t block = 0; block < state_->written.size(); ++block) {
        if (!state_->written[block]) {
            continue;
        }
        if (std::optional<detail::block_descriptor>& descriptor = state_->opened.blocks[block].descriptor) {
            detail::refresh_digest(state_->device, *descriptor);
        }
        state_->written[block] = false;
    }
    state_->device.flush();
}

std::size_t editor::sample_count_of(std::size_t index, sample_format format, const pixel_read_options& options) const
{
    return detail::typed_sample_count(state_->opened, index, format, options);
}

void editor::check_sample_format(std::size_t index, sample_format format) const
{
    (void)detail::typed_image_at(state_->opened, index, format);
}

} // namespace openxisf
