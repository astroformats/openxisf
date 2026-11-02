// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/types.h>

#include "model/pixel_layout.h"

#include <limits>

namespace openxisf {

namespace {

std::uint64_t multiply(std::uint64_t a, std::uint64_t b)
{
    if (a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a) {
        throw usage_error(errc::arithmetic_overflow, "the size of the image does not fit in 64 bits");
    }
    return a * b;
}

} // namespace

std::uint64_t geometry::pixel_count() const
{
    std::uint64_t count = 1;
    for (const std::uint64_t length : dimensions) {
        count = multiply(count, length);
    }
    return count;
}

std::uint64_t geometry::sample_count() const
{
    return multiply(pixel_count(), channels);
}

std::uint64_t pixel_region::sample_count(const geometry& image) const
{
    return detail::resolve_region(*this, image).samples;
}

std::uint64_t image_info::data_size() const
{
    return multiply(geometry.sample_count(), sample_size(sample_format));
}

std::optional<openxisf::bounds> image_info::representable_range() const noexcept
{
    if (bounds) {
        return bounds;
    }
    // Spec §8.5.5: [0, 2^k - 1] for k-bit unsigned integers; no default for floating point and complex samples.
    switch (sample_format) {
    case openxisf::sample_format::uint8:
        return openxisf::bounds{.lower = 0.0, .upper = 255.0};
    case openxisf::sample_format::uint16:
        return openxisf::bounds{.lower = 0.0, .upper = 65535.0};
    case openxisf::sample_format::uint32:
        return openxisf::bounds{.lower = 0.0, .upper = 4294967295.0};
    case openxisf::sample_format::uint64:
        return openxisf::bounds{.lower = 0.0, .upper = static_cast<double>(std::numeric_limits<std::uint64_t>::max())};
    case openxisf::sample_format::float32:
    case openxisf::sample_format::float64:
    case openxisf::sample_format::complex32:
    case openxisf::sample_format::complex64:
        break;
    }
    return std::nullopt;
}

} // namespace openxisf
