// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/error.h>
#include <openxisf/io.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

// Fault injection: a source and a sink that forward to real ones until a chosen call, which fails. Every error path of
// the code that uses them can be reached without a failing disk.

namespace openxisf::test {

/// What the failing call does.
enum class fault
{
    error,          ///< Throws io_error, like a medium that fails.
    short_transfer, ///< Transfers the first half of the bytes, then throws io_error, like a medium that ends or fills.
    foreign_exception, ///< Throws injected_fault, like a callback of an application.
};

/// The exception of fault::foreign_exception, which is not an openxisf error.
class injected_fault : public std::runtime_error
{
public:
    injected_fault() : std::runtime_error("injected fault") {}
};

/// A source that reads from another one and fails at its n-th read, counting from 1.
class faulty_source final : public input_source
{
public:
    faulty_source(const input_source& inner, std::size_t failing_read, fault kind)
        : inner_(inner), failing_read_(failing_read), kind_(kind)
    {}

    std::uint64_t size() const override
    {
        return inner_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        if (++reads_ == failing_read_) {
            switch (kind_) {
            case fault::error:
                throw io_error(errc::read_failed, "injected read failure", {}, {.offset = offset});
            case fault::short_transfer:
                inner_.read(offset, destination.first(destination.size() / 2));
                throw io_error(errc::end_of_data, "injected short read", {}, {.offset = offset});
            case fault::foreign_exception:
                throw injected_fault();
            }
        }
        inner_.read(offset, destination);
    }

    bool supports_concurrent_reads() const override
    {
        return inner_.supports_concurrent_reads();
    }

    /// The number of reads so far, the failing one included.
    [[nodiscard]] std::size_t reads() const noexcept
    {
        return reads_;
    }

private:
    const input_source& inner_;
    std::size_t failing_read_;
    fault kind_;
    mutable std::atomic<std::size_t> reads_{0};
};

/// A sink that writes to another one and fails at its n-th call of write(), rewrite() or finish(), counting from 1.
class faulty_sink final : public output_sink
{
public:
    faulty_sink(output_sink& inner, std::size_t failing_call, fault kind)
        : inner_(inner), failing_call_(failing_call), kind_(kind)
    {}

    void write(std::span<const std::byte> data) override
    {
        if (++calls_ == failing_call_) {
            fail([&](std::span<const std::byte> part) { inner_.write(part); }, data);
        }
        inner_.write(data);
    }

    std::uint64_t position() const override
    {
        return inner_.position();
    }

    bool can_rewrite() const override
    {
        return inner_.can_rewrite();
    }

    void rewrite(std::uint64_t offset, std::span<const std::byte> data) override
    {
        if (++calls_ == failing_call_) {
            fail([&](std::span<const std::byte> part) { inner_.rewrite(offset, part); }, data);
        }
        inner_.rewrite(offset, data);
    }

    void finish() override
    {
        if (++calls_ == failing_call_) {
            fail([](std::span<const std::byte> /*part*/) {}, {});
        }
        inner_.finish();
    }

private:
    template <typename Transfer> [[noreturn]] void fail(Transfer transfer, std::span<const std::byte> data) const
    {
        switch (kind_) {
        case fault::error:
            break;
        case fault::short_transfer:
            transfer(data.first(data.size() / 2));
            break;
        case fault::foreign_exception:
            throw injected_fault();
        }
        throw io_error(errc::write_failed, "injected write failure");
    }

    output_sink& inner_;
    std::size_t failing_call_;
    fault kind_;
    std::size_t calls_ = 0;
};

/// A device that writes to another one and fails at its n-th call of write() or flush(), counting from 1. Its reads go
/// through. It takes the concurrency of the other device.
class faulty_device final : public random_access_device
{
public:
    faulty_device(random_access_device& inner, std::size_t failing_call, fault kind)
        : inner_(inner), failing_call_(failing_call), kind_(kind)
    {}

    std::uint64_t size() const override
    {
        return inner_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        inner_.read(offset, destination);
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        if (++calls_ == failing_call_) {
            switch (kind_) {
            case fault::error:
                throw io_error(errc::write_failed, "injected write failure", {}, {.offset = offset});
            case fault::short_transfer:
                inner_.write(offset, data.first(data.size() / 2));
                throw io_error(errc::write_failed, "injected short write", {}, {.offset = offset});
            case fault::foreign_exception:
                throw injected_fault();
            }
        }
        inner_.write(offset, data);
    }

    void flush() override
    {
        if (++calls_ == failing_call_) {
            if (kind_ == fault::foreign_exception) {
                throw injected_fault();
            }
            throw io_error(errc::write_failed, "injected flush failure");
        }
        inner_.flush();
    }

    bool supports_concurrent_reads() const override
    {
        return inner_.supports_concurrent_reads();
    }

    bool supports_concurrent_writes() const override
    {
        return inner_.supports_concurrent_writes();
    }

    /// The number of calls of write() and flush() so far, the failing one included.
    [[nodiscard]] std::size_t calls() const noexcept
    {
        return calls_;
    }

private:
    random_access_device& inner_;
    std::size_t failing_call_;
    fault kind_;
    std::atomic<std::size_t> calls_{0};
};

} // namespace openxisf::test
