// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "io/thread_safe_source.h"

#include <openxisf/error.h>
#include <openxisf/io.h>

#include "support/bytes.h"
#include "support/faulty_io.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::io_error;
using openxisf::detail::thread_safe_source;
using openxisf::test::bytes;
using openxisf::test::fault;
using openxisf::test::faulty_source;
using openxisf::test::injected_fault;
using openxisf::test::text;
using openxisf::test::throws;

// A source that counts the calls it receives.
class counting_source final : public openxisf::input_source
{
public:
    explicit counting_source(int& calls) : calls_(calls) {}

    std::uint64_t size() const override
    {
        ++calls_;
        return 10;
    }

    void read(std::uint64_t /*offset*/, std::span<std::byte> /*destination*/) const override
    {
        ++calls_;
    }

    std::string description() const override
    {
        ++calls_;
        return "the counting source";
    }

    std::span<const std::byte> bytes() const override
    {
        ++calls_;
        return {};
    }

private:
    int& calls_;
};

TEST(thread_safe_source, asks_the_source_for_its_size_bytes_and_description_once)
{
    int calls = 0;
    const thread_safe_source source(std::make_unique<counting_source>(calls));
    std::array<std::byte, 4> destination{};
    EXPECT_EQ(calls, 3);

    source.read(0, destination);
    source.read(6, destination);

    EXPECT_EQ(source.size(), 10U);
    EXPECT_TRUE(source.bytes().empty());
    EXPECT_EQ(source.description(), "the counting source");
    EXPECT_EQ(calls, 5);
}

TEST(thread_safe_source, gives_the_bytes_of_a_source_in_memory)
{
    const std::vector<std::byte> content = bytes("0123456789");
    const thread_safe_source source(std::make_unique<openxisf::memory_source>(std::span<const std::byte>(content)));
    EXPECT_EQ(source.bytes().data(), content.data());
    EXPECT_EQ(source.bytes().size(), content.size());
}

// A source of 10 bytes that gives 4 in memory.
class short_memory_source final : public openxisf::input_source
{
public:
    std::uint64_t size() const override
    {
        return 10;
    }

    void read(std::uint64_t /*offset*/, std::span<std::byte> /*destination*/) const override {}

    std::span<const std::byte> bytes() const override
    {
        return content_;
    }

private:
    std::array<std::byte, 4> content_{};
};

TEST(thread_safe_source, refuses_bytes_in_memory_of_another_size_than_the_source)
{
    EXPECT_TRUE(throws<openxisf::usage_error>(
        errc::invalid_argument, [] { const thread_safe_source source(std::make_unique<short_memory_source>()); }));
}

TEST(thread_safe_source, checks_reads_before_they_reach_the_source)
{
    int calls = 0;
    const thread_safe_source source(std::make_unique<counting_source>(calls));
    std::array<std::byte, 4> destination{};

    EXPECT_TRUE(throws<io_error>(errc::end_of_data, [&] { source.read(7, destination); }));
    source.read(10, {});
    EXPECT_EQ(calls, 3);
}

TEST(thread_safe_source, exceptions_of_the_source_pass_through_and_later_reads_work)
{
    // A source without concurrent reads, so that every read takes the lock, which an exception must release.
    const std::vector<std::byte> content = bytes("0123456789");
    const openxisf::callback_source data(content.size(), [&](std::uint64_t offset, std::span<std::byte> destination) {
        std::ranges::copy(std::span(content).subspan(offset, destination.size()), destination.begin());
    });
    std::array<std::byte, 4> destination{};

    for (const fault kind : {fault::error, fault::short_transfer, fault::foreign_exception}) {
        auto faulty = std::make_unique<faulty_source>(data, 1, kind);
        const faulty_source& inner = *faulty;
        const thread_safe_source source(std::move(faulty));
        ASSERT_FALSE(inner.supports_concurrent_reads());

        if (kind == fault::foreign_exception) {
            EXPECT_THROW(source.read(0, destination), injected_fault);
        } else {
            EXPECT_THROW(source.read(0, destination), io_error);
        }
        source.read(6, destination);
        EXPECT_EQ(text(destination), "6789");
        EXPECT_EQ(inner.reads(), 2U);
    }
}

TEST(thread_safe_source, refuses_a_null_source)
{
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument, [] { thread_safe_source source(nullptr); }));
}

} // namespace
