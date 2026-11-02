// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/error.h>
#include <openxisf/types.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using openxisf::errc;
using openxisf::geometry;
using openxisf::pixel_region;
using openxisf::usage_error;
using openxisf::test::throws;

constexpr std::uint64_t max64 = std::numeric_limits<std::uint64_t>::max();

// An image of 4 x 3 pixels of 2 channels, and one of 4 x 3 x 2 pixels of 3 channels.
geometry flat_image()
{
    return {.dimensions = {4, 3}, .channels = 2};
}

geometry deep_image()
{
    return {.dimensions = {4, 3, 2}, .channels = 3};
}

TEST(pixel_region, a_region_without_origin_and_size_is_the_whole_image)
{
    const geometry flat = flat_image();
    const geometry deep = deep_image();
    EXPECT_EQ(pixel_region{}.sample_count(flat), 24U);
    EXPECT_EQ(pixel_region{}.sample_count(deep), 72U);
}

TEST(pixel_region, counts_the_samples_of_its_box_and_its_channels)
{
    const geometry flat = flat_image();
    const geometry deep = deep_image();
    EXPECT_EQ((pixel_region{.origin = {1, 1}, .size = {2, 2}}).sample_count(flat), 8U);
    EXPECT_EQ(
        (pixel_region{.origin = {1, 1}, .size = {2, 2}, .first_channel = 1, .channel_count = 1}).sample_count(flat),
        4U);
    EXPECT_EQ((pixel_region{.origin = {0, 1, 1}, .size = {4, 2, 1}, .channel_count = 2}).sample_count(deep), 16U);
    // A single pixel, and a single sample.
    EXPECT_EQ((pixel_region{.origin = {3, 2}, .size = {1, 1}}).sample_count(flat), 2U);
    EXPECT_EQ((pixel_region{.origin = {3, 2}, .size = {1, 1}, .first_channel = 1}).sample_count(flat), 1U);
}

TEST(pixel_region, an_empty_size_reaches_the_end_of_every_axis)
{
    const geometry flat = flat_image();
    // From the origin to the end: 3 x 2 pixels.
    EXPECT_EQ((pixel_region{.origin = {1, 1}}).sample_count(flat), 12U);
    // From the first pixel, without an origin.
    EXPECT_EQ((pixel_region{.size = {2, 1}}).sample_count(flat), 4U);
}

TEST(pixel_region, a_channel_count_of_0_takes_every_channel_from_the_first)
{
    const geometry flat = flat_image();
    const geometry deep = deep_image();
    EXPECT_EQ((pixel_region{.first_channel = 1}).sample_count(flat), 12U);
    EXPECT_EQ((pixel_region{.first_channel = 1, .channel_count = 0}).sample_count(deep), 48U);
    EXPECT_EQ((pixel_region{.first_channel = 2, .channel_count = 1}).sample_count(deep), 24U);
}

TEST(pixel_region, a_region_that_does_not_lie_within_the_image_is_a_usage_error)
{
    const geometry flat = flat_image();
    for (const pixel_region& region : {
             // An entry for each axis, or none.
             pixel_region{.origin = {1}},
             pixel_region{.origin = {0, 0, 0}},
             pixel_region{.size = {4}},
             pixel_region{.size = {4, 3, 1}},
             // An origin beyond the last pixel.
             pixel_region{.origin = {4, 0}},
             pixel_region{.origin = {0, 3}},
             // No pixels along an axis.
             pixel_region{.size = {0, 3}},
             pixel_region{.origin = {1, 1}, .size = {2, 0}},
             // Past the end of an axis, without and with an overflow of the end.
             pixel_region{.origin = {3, 0}, .size = {2, 1}},
             pixel_region{.origin = {0, 1}, .size = {4, 3}},
             pixel_region{.origin = {1, 0}, .size = {max64, 1}},
             // Channels beyond the last.
             pixel_region{.first_channel = 2},
             pixel_region{.first_channel = 1, .channel_count = 2},
             pixel_region{.channel_count = 3},
             pixel_region{.first_channel = max64, .channel_count = 2},
         }) {
        EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&region, &flat] { (void)region.sample_count(flat); }))
            << "origin " << region.origin.size() << ", size " << region.size.size() << ", channel "
            << region.first_channel << " + " << region.channel_count;
    }
}

TEST(pixel_region, a_count_beyond_64_bits_is_a_usage_error)
{
    // A geometry of more samples than 64 bits count, which no image has.
    const geometry huge{.dimensions = {max64, 2}, .channels = 2};
    EXPECT_TRUE(throws<usage_error>(errc::arithmetic_overflow, [&huge] { (void)pixel_region{}.sample_count(huge); }));
    EXPECT_TRUE(throws<usage_error>(errc::arithmetic_overflow,
                                    [&huge] { (void)(pixel_region{.size = {max64, 1}}).sample_count(huge); }));
    // A part of it that fits.
    EXPECT_EQ((pixel_region{.size = {max64, 1}, .channel_count = 1}).sample_count(huge), max64);
}

TEST(pixel_region, regions_are_equal_when_their_members_are)
{
    EXPECT_EQ((pixel_region{.origin = {1, 2}, .size = {3, 4}, .first_channel = 1, .channel_count = 1}),
              (pixel_region{.origin = {1, 2}, .size = {3, 4}, .first_channel = 1, .channel_count = 1}));
    EXPECT_NE((pixel_region{.origin = {1, 2}}), (pixel_region{.origin = {2, 1}}));
    // Two regions of the same pixels differ when their members do: an empty size stands for every pixel from the
    // origin on.
    EXPECT_NE((pixel_region{}), (pixel_region{.origin = {0, 0}, .size = {4, 3}}));
}

} // namespace
