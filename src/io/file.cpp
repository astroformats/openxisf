// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/error.h>
#include <openxisf/io.h>

#include "core/hex.h"
#include "core/xoshiro.h"
#include "io/native_file.h"
#include "io/paths.h"
#include "io/range_check.h"

#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace openxisf {

namespace {

// A name in the directory of target, which is absolute and so has a separator: .openxisf-<16 hexadecimal digits>.tmp. A
// fixed length keeps it within the limits of the file system whatever the name of the target.
std::string temporary_path(std::string_view target, std::uint64_t random)
{
#if defined(_WIN32)
    const std::size_t separator = target.find_last_of("/\\");
#else
    const std::size_t separator = target.find_last_of('/');
#endif
    std::string path(target.substr(0, separator + 1));
    path += ".openxisf-";
    path += detail::fixed_width_hex(random);
    path += ".tmp";
    return path;
}

} // namespace

struct file_source::state
{
    detail::native_file file;
    std::uint64_t size = 0;
};

file_source::file_source(std::string_view path)
{
    detail::check_path_argument(path, "the path");
    detail::native_file file = detail::native_file::open_for_reading(std::string(path));
    const std::uint64_t size = file.size();
    state_ = std::make_unique<const state>(state{.file = std::move(file), .size = size});
}

file_source::~file_source() = default;

std::uint64_t file_source::size() const
{
    return state_->size;
}

void file_source::read(std::uint64_t offset, std::span<std::byte> destination) const
{
    detail::check_read_range(state_->size, offset, destination.size(), state_->file.name());
    state_->file.read_at(offset, destination);
}

bool file_source::supports_concurrent_reads() const
{
    return true;
}

std::string file_source::description() const
{
    return state_->file.name();
}

struct file_sink::state
{
    state(std::string target_path, std::string temporary_path, detail::native_file temporary_file, bool flush)
        : target(std::move(target_path)), temporary(std::move(temporary_path)), file(std::move(temporary_file)),
          flush_to_disk(flush)
    {}

    state(const state&) = delete;
    state& operator=(const state&) = delete;

    // Unless finish() replaced the target with it, the temporary file holds an incomplete unit.
    ~state()
    {
        discard();
    }

    void discard() noexcept
    {
        if (!temporary.empty()) {
            file.discard(temporary);
            temporary.clear();
        }
    }

    void check_writable() const
    {
        if (finished) {
            throw usage_error(errc::invalid_argument, "the file sink for " + target + " has finished");
        }
    }

    std::string target;
    std::string temporary;
    detail::native_file file;
    std::uint64_t position = 0;
    bool flush_to_disk;
    bool finished = false;
};

file_sink::file_sink(std::string_view path, file_sink_options options)
{
    detail::check_path_argument(path, "the path");
    // finish() replaces the file that the path names now, whatever the current directory is by then.
    std::string target = detail::absolute_path(std::string(path));
    // A rename would replace a FIFO, a socket or a device, and fails on a directory only once the unit is written.
    detail::check_replaceable(target);
    // A name taken by another file is tried again with another random number, which only a crowded directory needs.
    detail::xoshiro256starstar random = detail::xoshiro256starstar::from_random_device();
    for (int attempt = 0; attempt < 16; ++attempt) {
        std::string temporary = temporary_path(target, random());
        std::optional<detail::native_file> file = detail::native_file::create_new(temporary, target);
        if (file) {
            // The file exists from here, and must not stay behind when the state cannot be allocated. Nothing is moved
            // before the allocation succeeds.
            try {
                state_ = std::make_unique<state>(std::move(target), std::move(temporary), std::move(*file),
                                                 options.flush_to_disk);
            } catch (...) {
                file->discard(temporary);
                throw;
            }
            return;
        }
    }
    throw io_error(errc::open_failed, "cannot create a temporary file next to " + target + ": every name tried exists");
}

file_sink::~file_sink() = default;

void file_sink::write(std::span<const std::byte> data)
{
    state_->check_writable();
    state_->file.write_at(state_->position, data);
    state_->position += data.size();
}

std::uint64_t file_sink::position() const
{
    return state_->position;
}

bool file_sink::can_rewrite() const
{
    return true;
}

void file_sink::rewrite(std::uint64_t offset, std::span<const std::byte> data)
{
    state_->check_writable();
    detail::check_rewrite_range(state_->position, offset, data.size());
    state_->file.write_at(offset, data);
}

void file_sink::finish()
{
    state_->check_writable();
    state_->finished = true;
    try {
        state_->file.commit(state_->temporary, state_->target, state_->flush_to_disk);
        state_->temporary.clear();
    } catch (...) {
        state_->discard();
        throw;
    }
}

} // namespace openxisf
