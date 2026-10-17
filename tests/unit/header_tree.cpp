// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The elements of the metadata that the writer generates, which cannot have a data block, and the pixel data that the
// tree borrows. The header that the writer writes is tested through the writer, in conformance/writer.cpp.

#include "model/header_tree.h"

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/property.h>

#include "model/unit_contents.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::property;
using openxisf::detail::block_source;
using openxisf::detail::header_tree;
using openxisf::detail::property_elements;

TEST(header_tree, the_generated_metadata_cannot_have_a_data_block)
{
    // They are written with the data blocks of the unit, which an element of theirs with a block would name: a String
    // longer than 1 MiB, or one that XML cannot hold, and a vector take one.
    for (const property& item : {property{.id = "XISF:CreatorApplication", .value = std::string((1U << 20U) + 1, 'a')},
                                 property{.id = "XISF:CreatorApplication", .value = "My\x01"},
                                 property{.id = "XISF:Test", .value = std::vector<std::uint8_t>{1, 2}}}) {
        EXPECT_TRUE(openxisf::test::throws<openxisf::validation_error>(errc::invalid_metadata, [&item] {
            (void)property_elements(std::span(&item, 1));
        })) << item.id;
    }
}

TEST(header_tree, pixel_data_are_borrowed_until_they_are_made_little_endian)
{
    // The writer copies the samples that a big-endian host swaps only once it has checked the size of the unit, so the
    // tree borrows the pixel data, and marks the blocks of samples of more than one byte that need the copy there.
    const std::array<std::uint16_t, 2> wide{0x0102, 0x0304};
    const std::array<std::uint8_t, 2> narrow{1, 2};
    openxisf::image_info image;
    image.geometry = {.dimensions = {2, 1}, .channels = 1};
    image.sample_format = openxisf::sample_format::uint16;
    openxisf::detail::unit_contents unit;
    unit.images = {image, image};
    unit.images[1].sample_format = openxisf::sample_format::uint8;
    unit.pixels = {std::as_bytes(std::span(wide)), std::as_bytes(std::span(narrow))};
    const std::vector<std::string> uuids(2);
    header_tree tree = openxisf::detail::build_header_tree(unit, {}, uuids);
    ASSERT_EQ(tree.blocks.size(), 2U);
    EXPECT_EQ(tree.blocks[0].data().data(), unit.pixels[0].data());
    EXPECT_EQ(tree.blocks[1].data().data(), unit.pixels[1].data());
    EXPECT_EQ(tree.blocks[0].native_order, std::endian::native == std::endian::big);
    EXPECT_FALSE(tree.blocks[1].native_order);

    // A marked block gets a copy with the bytes of each sample in the other order, which it then holds.
    const std::span<const std::byte> native = unit.pixels[0];
    block_source& block = tree.blocks[0];
    block.native_order = true;
    openxisf::detail::to_little_endian(block);
    EXPECT_FALSE(block.native_order);
    EXPECT_FALSE(block.borrowed.has_value());
    EXPECT_EQ(block.owned, (std::vector<std::byte>{native[1], native[0], native[3], native[2]}));
    EXPECT_EQ(block.data().data(), block.owned.data());
    // Another block is left as it is.
    openxisf::detail::to_little_endian(tree.blocks[1]);
    EXPECT_EQ(tree.blocks[1].data().data(), unit.pixels[1].data());
}

} // namespace
