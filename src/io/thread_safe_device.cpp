// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "io/thread_safe_device.h"

#include <openxisf/error.h>

#include "io/range_check.h"

#include <utility>

namespace openxisf::detail {

namespace {

std::unique_ptr<random_access_device> require(std::unique_ptr<random_access_device> device)
{
    if (!device) {
        throw usage_error(errc::invalid_argument, "the device is null");
    }
    return device;
}

// Reads through a device, whose own rules decide which reads run at once.
class device_source final : public input_source
{
public:
    explicit device_source(const thread_safe_device& device) noexcept : device_(device) {}

    [[nodiscard]] std::uint64_t size() const override
    {
        return device_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        device_.read(offset, destination);
    }

    [[nodiscard]] bool supports_concurrent_reads() const override
    {
        return true;
    }

    [[nodiscard]] std::string description() const override
    {
        return device_.description();
    }

private:
    const thread_safe_device& device_;
};

} // namespace

thread_safe_device::thread_safe_device(std::unique_ptr<random_access_device> device)
    : device_(require(std::move(device))), size_(device_->size()), description_(device_->description()),
      concurrent_reads_(device_->supports_concurrent_reads()), concurrent_writes_(device_->supports_concurrent_writes())
{}

void thread_safe_device::read(std::uint64_t offset, std::span<std::byte> destination) const
{
    check_read_range(size_, offset, destination.size(), description_);
    if (destination.empty()) {
        return;
    }
    if (locks_reads()) {
        const std::scoped_lock lock(mutex_);
        device_->read(offset, destination);
    } else {
        device_->read(offset, destination);
    }
}

void thread_safe_device::write(std::uint64_t offset, std::span<const std::byte> data) const
{
    check_write_range(size_, offset, data.size(), description_);
    if (data.empty()) {
        return;
    }
    if (concurrent_writes_) {
        device_->write(offset, data);
    } else {
        const std::scoped_lock lock(mutex_);
        device_->write(offset, data);
    }
}

void thread_safe_device::flush() const
{
    if (concurrent_writes_) {
        device_->flush();
    } else {
        const std::scoped_lock lock(mutex_);
        device_->flush();
    }
}

std::unique_ptr<input_source> thread_safe_device::source() const
{
    return std::make_unique<device_source>(*this);
}

} // namespace openxisf::detail
