// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Byte order conversions. XISF structures are little-endian (spec §8.2), and data blocks may be stored in either
// byte order (spec §10.4). Nothing here assumes the byte order of the host.

namespace openxisf::detail {

static_assert(std::endian::native == std::endian::little || std::endian::native == std::endian::big,
              "mixed-endian hosts are not supported");

/// value with its bytes in reverse order (std::byteswap is C++23).
template <std::unsigned_integral T> [[nodiscard]] constexpr T byte_swap(T value) noexcept
{
    // In 64 bits, so that integer promotion does not change the type, and no cast is needed when T is 64 bits wide.
    std::uint64_t swapped = 0;
    std::uint64_t remaining = value;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        swapped = (swapped << 8U) | (remaining & 0xFFU);
        remaining >>= 8U;
    }
    if constexpr (sizeof(T) == sizeof(std::uint64_t)) {
        return swapped;
    } else {
        return static_cast<T>(swapped);
    }
}

/// Loads an unsigned integer stored in little-endian byte order.
template <std::unsigned_integral T>
[[nodiscard]] T load_little_endian(std::span<const std::byte, sizeof(T)> bytes) noexcept
{
    T value = 0;
    std::memcpy(&value, bytes.data(), sizeof(T));
    if constexpr (std::endian::native == std::endian::big) {
        value = byte_swap(value);
    }
    return value;
}

/// Loads an unsigned integer stored in big-endian byte order.
template <std::unsigned_integral T>
[[nodiscard]] T load_big_endian(std::span<const std::byte, sizeof(T)> bytes) noexcept
{
    T value = 0;
    std::memcpy(&value, bytes.data(), sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        value = byte_swap(value);
    }
    return value;
}

/// Stores an unsigned integer in little-endian byte order.
template <std::unsigned_integral T> void store_little_endian(std::span<std::byte, sizeof(T)> bytes, T value) noexcept
{
    if constexpr (std::endian::native == std::endian::big) {
        value = byte_swap(value);
    }
    std::memcpy(bytes.data(), &value, sizeof(T));
}

/// Stores an unsigned integer in big-endian byte order.
template <std::unsigned_integral T> void store_big_endian(std::span<std::byte, sizeof(T)> bytes, T value) noexcept
{
    if constexpr (std::endian::native == std::endian::little) {
        value = byte_swap(value);
    }
    std::memcpy(bytes.data(), &value, sizeof(T));
}

/// Reverses the byte order of every item of item_size bytes, in place. Bytes after the last complete item are left
/// unchanged, as byte shuffling does with them (spec §10.6.3).
void swap_byte_order(std::span<std::byte> data, std::size_t item_size) noexcept;

} // namespace openxisf::detail
