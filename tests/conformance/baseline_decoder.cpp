// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §7.2: the abilities of a baseline decoder, each checked on a constructed unit read through the public API, in
// strict mode. The behaviours themselves are tested in detail in the suites of their own sections.

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>
#include <openxisf/types.h>

#include "codec/compressed_block.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::property_value;
using openxisf::reader;
using openxisf::detail::compress_block;
using openxisf::detail::compressed_block;
using openxisf::detail::compression_codec;
using openxisf::test::bytes;
using openxisf::test::file_with_attachments;
using openxisf::test::header_xml;
using openxisf::test::no_diagnostics;
using openxisf::test::throws;

reader open_attached(std::string_view body, const std::vector<std::vector<std::byte>>& blocks)
{
    return openxisf::test::open_unit(file_with_attachments(header_xml(body), blocks), {.strict = true});
}

std::vector<std::byte> block(std::initializer_list<std::uint8_t> values)
{
    std::vector<std::byte> data;
    data.reserve(values.size());
    for (const std::uint8_t value : values) {
        data.push_back(static_cast<std::byte>(value));
    }
    return data;
}

TEST(conformance_baseline_decoder, reads_monolithic_files)
{
    const reader file =
        open_attached(R"(<Image geometry="2:1:1" sampleFormat="UInt8" location="attachment:{0}"/>)", {bytes("ab")});
    EXPECT_EQ(file.storage(), openxisf::unit_storage::monolithic);
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.read_pixels<std::uint8_t>(0), (std::vector<std::uint8_t>{'a', 'b'}));
}

TEST(conformance_baseline_decoder, reads_properties_of_every_scalar_type_of_8_to_64_bits)
{
    const reader file =
        openxisf::test::open_header(header_xml(R"(<Property id="Test:Boolean" type="Boolean" value="true"/>)"
                                               R"(<Property id="Test:Int8" type="Int8" value="-1"/>)"
                                               R"(<Property id="Test:UInt8" type="UInt8" value="2"/>)"
                                               R"(<Property id="Test:Int16" type="Int16" value="-3"/>)"
                                               R"(<Property id="Test:UInt16" type="UInt16" value="4"/>)"
                                               R"(<Property id="Test:Int32" type="Int32" value="-5"/>)"
                                               R"(<Property id="Test:UInt32" type="UInt32" value="6"/>)"
                                               R"(<Property id="Test:Int64" type="Int64" value="-7"/>)"
                                               R"(<Property id="Test:UInt64" type="UInt64" value="8"/>)"
                                               R"(<Property id="Test:Float32" type="Float32" value="9.5"/>)"
                                               R"(<Property id="Test:Float64" type="Float64" value="-10.25"/>)"),
                                    {.strict = true});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    const openxisf::property_list& properties = file.properties();
    EXPECT_EQ(properties.at("Test:Boolean").value, property_value(true));
    EXPECT_EQ(properties.at("Test:Int8").value, property_value(std::int8_t{-1}));
    EXPECT_EQ(properties.at("Test:UInt8").value, property_value(std::uint8_t{2}));
    EXPECT_EQ(properties.at("Test:Int16").value, property_value(std::int16_t{-3}));
    EXPECT_EQ(properties.at("Test:UInt16").value, property_value(std::uint16_t{4}));
    EXPECT_EQ(properties.at("Test:Int32").value, property_value(std::int32_t{-5}));
    EXPECT_EQ(properties.at("Test:UInt32").value, property_value(std::uint32_t{6}));
    EXPECT_EQ(properties.at("Test:Int64").value, property_value(std::int64_t{-7}));
    EXPECT_EQ(properties.at("Test:UInt64").value, property_value(std::uint64_t{8}));
    EXPECT_EQ(properties.at("Test:Float32").value, property_value(9.5F));
    EXPECT_EQ(properties.at("Test:Float64").value, property_value(-10.25));
}

TEST(conformance_baseline_decoder, reads_several_images_from_one_file)
{
    const reader file =
        open_attached(R"(<Image id="a" geometry="1:1:1" sampleFormat="UInt8" location="attachment:{0}"/>)"
                      R"(<Image id="b" geometry="2:1:1" sampleFormat="UInt8" location="attachment:{1}"/>)"
                      R"(<Image id="c" geometry="3:1:1" sampleFormat="UInt8" location="attachment:{2}"/>)",
                      {bytes("a"), bytes("bc"), bytes("def")});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.images().size(), 3U);
    EXPECT_EQ(file.image(0).id, "a");
    EXPECT_EQ(file.image(2).id, "c");
    EXPECT_EQ(file.read_pixels<std::uint8_t>(1), (std::vector<std::uint8_t>{'b', 'c'}));
    EXPECT_EQ(file.read_pixels<std::uint8_t>(2), (std::vector<std::uint8_t>{'d', 'e', 'f'}));
}

TEST(conformance_baseline_decoder, reads_pixel_data_from_inline_embedded_and_attached_blocks)
{
    // An inline block in an Image element is tolerated, with a warning (spec §11.5).
    const reader file = open_attached(
        R"(<Image geometry="2:1:1" sampleFormat="UInt8" location="inline:hex">0102</Image>)"
        R"(<Image geometry="2:1:1" sampleFormat="UInt8" location="embedded"><Data encoding="base64">AwQ=</Data></Image>)"
        R"(<Image geometry="2:1:1" sampleFormat="UInt8" location="attachment:{0}"/>)",
        {block({5, 6})});
    ASSERT_EQ(file.images().size(), 3U);
    EXPECT_EQ(file.read_pixels<std::uint8_t>(0), (std::vector<std::uint8_t>{1, 2}));
    EXPECT_EQ(file.read_pixels<std::uint8_t>(1), (std::vector<std::uint8_t>{3, 4}));
    EXPECT_EQ(file.read_pixels<std::uint8_t>(2), (std::vector<std::uint8_t>{5, 6}));
}

TEST(conformance_baseline_decoder, reads_data_blocks_in_both_byte_orders)
{
    const reader file =
        open_attached(R"(<Image geometry="2:1:1" sampleFormat="UInt16" location="attachment:{0}"/>)"
                      R"(<Image geometry="2:1:1" sampleFormat="UInt16" byteOrder="big" location="attachment:{0}"/>)",
                      {block({1, 2, 3, 4})});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.read_pixels<std::uint16_t>(0), (std::vector<std::uint16_t>{0x0201, 0x0403}));
    EXPECT_EQ(file.read_pixels<std::uint16_t>(1), (std::vector<std::uint16_t>{0x0102, 0x0304}));
}

TEST(conformance_baseline_decoder, decompresses_every_standard_codec)
{
    // Spec §10.6.3 to §10.6.10, Table 10: each codec, with and without byte shuffling.
    // The blocks are little-endian, the default byte order, whatever the host's.
    std::vector<std::uint16_t> samples(300);
    std::vector<std::byte> data;
    data.reserve(samples.size() * 2);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<std::uint16_t>(i * 7);
        data.push_back(static_cast<std::byte>(samples[i] & 0xFFU));
        data.push_back(static_cast<std::byte>(samples[i] >> 8U));
    }

    std::string body;
    std::vector<std::vector<std::byte>> blocks;
    for (const auto& [codec, name] :
         {std::pair{compression_codec::zlib, "zlib"}, std::pair{compression_codec::lz4, "lz4"},
          std::pair{compression_codec::lz4hc, "lz4hc"}, std::pair{compression_codec::zstd, "zstd"}}) {
        for (const unsigned item_size : {0U, 2U}) {
            const compressed_block compressed = compress_block(data, {.codec = codec, .item_size = item_size});
            std::string attribute = std::string(name) + (item_size == 0 ? "" : "+sh") + ":" +
                                    std::to_string(compressed.compression.uncompressed_size);
            if (item_size != 0) {
                attribute += ":" + std::to_string(compressed.compression.item_size);
            }
            body += R"(<Image geometry="300:1:1" sampleFormat="UInt16" location="attachment:{)" +
                    std::to_string(blocks.size()) + R"(}" compression=")" + attribute + "\"/>";
            blocks.push_back(compressed.data);
        }
    }
    const reader file = open_attached(body, blocks);
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.images().size(), 8U);
    for (std::size_t i = 0; i < 8; ++i) {
        EXPECT_EQ(file.read_pixels<std::uint16_t>(i), samples) << i;
    }
}

TEST(conformance_baseline_decoder, verifies_checksums_of_sha1_sha256_and_sha512)
{
    // The NIST digests of "abc".
    const reader file =
        open_attached(R"(<Image geometry="3:1:1" sampleFormat="UInt8" location="attachment:{0}" )"
                      R"(checksum="sha-1:a9993e364706816aba3e25717850c26c9cd0d89d"/>)"
                      R"(<Image geometry="3:1:1" sampleFormat="UInt8" location="attachment:{0}" )"
                      R"(checksum="sha-256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"/>)"
                      R"(<Image geometry="3:1:1" sampleFormat="UInt8" location="attachment:{0}" checksum="sha-512:)"
                      "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                      R"(2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"/>)",
                      {bytes("abc")});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(file.read_pixels<std::uint8_t>(i), (std::vector<std::uint8_t>{'a', 'b', 'c'})) << i;
    }

    // Data that fail their checksum are not read (spec §10.5).
    const reader corrupt = openxisf::test::open_unit(
        file_with_attachments(header_xml(R"(<Image geometry="3:1:1" sampleFormat="UInt8" location="attachment:{0}" )"
                                         R"(checksum="sha-1:a9993e364706816aba3e25717850c26c9cd0d89e"/>)"),
                              {bytes("abc")}));
    EXPECT_TRUE(
        throws<openxisf::integrity_error>(errc::checksum_mismatch, [&corrupt] { (void)corrupt.read_pixels(0); }));
}

TEST(conformance_baseline_decoder, reads_pixel_data_in_both_storage_models)
{
    // An RGB image of 2 x 1 pixels, whose sample of channel c at x is 10c + x, stored in each model.
    const reader file = open_attached(
        R"(<Image geometry="2:1:3" sampleFormat="UInt8" colorSpace="RGB" location="attachment:{0}"/>)"
        R"(<Image geometry="2:1:3" sampleFormat="UInt8" colorSpace="RGB" pixelStorage="Normal" location="attachment:{1}"/>)",
        {block({0, 1, 10, 11, 20, 21}), block({0, 10, 20, 1, 11, 21})});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.image(0).pixel_storage, openxisf::pixel_storage::planar);
    EXPECT_EQ(file.image(1).pixel_storage, openxisf::pixel_storage::normal);
    const std::vector<std::uint8_t> planar{0, 1, 10, 11, 20, 21};
    const std::vector<std::uint8_t> normal{0, 10, 20, 1, 11, 21};
    EXPECT_EQ(file.read_pixels<std::uint8_t>(0), planar);
    EXPECT_EQ(file.read_pixels<std::uint8_t>(1), normal);
    // Either is read in the other model on request.
    EXPECT_EQ(file.read_pixels<std::uint8_t>(0, {.storage = openxisf::pixel_storage::normal}), normal);
    EXPECT_EQ(file.read_pixels<std::uint8_t>(1, {.storage = openxisf::pixel_storage::planar}), planar);
}

TEST(conformance_baseline_decoder, reads_uint8_uint16_and_float32_samples)
{
    const reader file =
        open_attached(R"(<Image geometry="2:1:1" sampleFormat="UInt8" location="attachment:{0}"/>)"
                      R"(<Image geometry="2:1:1" sampleFormat="UInt16" location="attachment:{1}"/>)"
                      R"(<Image geometry="2:1:1" sampleFormat="Float32" bounds="0:1" location="attachment:{2}"/>)",
                      {block({1, 255}), block({0x34, 0x12, 0xFF, 0xFF}), block({0, 0, 0, 0x3F, 0, 0, 0x80, 0xBF})});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.read_pixels<std::uint8_t>(0), (std::vector<std::uint8_t>{1, 255}));
    EXPECT_EQ(file.read_pixels<std::uint16_t>(1), (std::vector<std::uint16_t>{0x1234, 0xFFFF}));
    EXPECT_EQ(file.read_pixels<float>(2), (std::vector<float>{0.5F, -1.0F}));
}

TEST(conformance_baseline_decoder, reads_grayscale_and_rgb_images)
{
    const reader file =
        open_attached(R"(<Image geometry="1:1:1" sampleFormat="UInt8" colorSpace="Gray" location="attachment:{0}"/>)"
                      R"(<Image geometry="1:1:3" sampleFormat="UInt8" colorSpace="RGB" location="attachment:{1}"/>)",
                      {block({7}), block({1, 2, 3})});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.image(0).color_space, openxisf::color_space::gray);
    EXPECT_EQ(file.image(1).color_space, openxisf::color_space::rgb);
    EXPECT_EQ(file.image(1).geometry.channels, 3U);
    EXPECT_EQ(file.read_pixels<std::uint8_t>(0), (std::vector<std::uint8_t>{7}));
    EXPECT_EQ(file.read_pixels<std::uint8_t>(1), (std::vector<std::uint8_t>{1, 2, 3}));
}

} // namespace
