// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §10.6: compressed data blocks in units, in every location, with subblocks, and verified before they are
// decompressed (spec §10.6.2). The codecs themselves are tested in unit/codecs.cpp and unit/compressed_block.cpp.

#include <openxisf/error.h>
#include <openxisf/reader.h>

#include "codec/codecs.h"
#include "codec/compressed_block.h"
#include "core/data_encoding.h"
#include "crypto/hash.h"
#include "model/unit.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::severity;
using openxisf::detail::compress_block;
using openxisf::detail::compressed_block;
using openxisf::detail::compression_codec;
using openxisf::detail::unit;
using openxisf::test::block_data;
using openxisf::test::bytes;
using openxisf::test::file_with_attachments;
using openxisf::test::header_xml;
using openxisf::test::monolithic_file;
using openxisf::test::no_diagnostics;
using openxisf::test::open_internal;
using openxisf::test::single_diagnostic;
using openxisf::test::throws;

unit open_body(std::string_view body)
{
    return open_internal(monolithic_file(header_xml(body)));
}

// The value of a compression attribute for a block.
std::string compression_of(const compressed_block& block, std::string_view name)
{
    std::string text = std::string(name) + ":" + std::to_string(block.compression.uncompressed_size);
    if (block.compression.item_size != 0) {
        text += ":" + std::to_string(block.compression.item_size);
    }
    return text;
}

std::string subblocks_of(const compressed_block& block)
{
    std::string text;
    for (const openxisf::detail::subblock& part : block.compression.subblocks) {
        text += (text.empty() ? "" : ":") + std::to_string(part.compressed_size) + "," +
                std::to_string(part.uncompressed_size);
    }
    return text;
}

std::string sha256_of(const std::vector<std::byte>& data)
{
    return "sha256:" + openxisf::detail::encode_hex(
                           openxisf::detail::compute_digest(openxisf::detail::hash_algorithm::sha256, data));
}

// 3 * 200 pixels of 16 bits: a gray image of one row.
std::vector<std::byte> pixels()
{
    std::vector<std::byte> data(1200);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::byte>(i % 2 == 0 ? (i / 6) & 0xFFU : i / 600);
    }
    return data;
}

constexpr std::string_view row_image = R"(<Image geometry="600:1:1" sampleFormat="UInt16" colorSpace="Gray" )";

// ---------------------------------------------------------------------------------------------------------------------
// The examples of spec §10.6.4

TEST(conformance_compression, the_embedded_zlib_image_of_the_specification_decodes)
{
    const unit opened = open_body(
        R"(<Image geometry="6:6:3" sampleFormat="UInt8" colorSpace="RGB" location="embedded">)"
        R"(<Data compression="zlib:108" encoding="base64">eJxjYGBg+A+GEPCfAYkJFQZSUPZ/KBtTBFMXOuc/AwCjKyPd</Data></Image>)");

    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    // Decoded by Python's zlib module.
    EXPECT_EQ(block_data(opened, "/xisf/Image[1]"),
              openxisf::detail::decode_hex(
                  "00000000ff00ff00ff00000000000000ff0000ff00000000000000ff00ff00ff00000000ff00ffff00ff00ff0000ff00ff00"
                  "ffff00ffff00ffff00ff00ff0000ff00ff00ffff00ff00ff00000000000000ff00ff00ff0000000000000000ff00ff00ff00"
                  "000000000000ff00"));
}

TEST(conformance_compression, the_inline_zlib_property_of_the_specification_decodes)
{
    const unit opened =
        open_body(R"(<Property id="Test" type="ByteArray" length="34" compression="zlib:34" location="inline:base64">)"
                  "\n   eNoLycgsVgCiRIWS1OISBV2FENfgECBlaGRsYmpmbmFpAACzWQkd\n</Property>");

    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    EXPECT_EQ(block_data(opened, "/xisf/Property[1]"), bytes("This is a test - TEST - 1234567890"));
}

TEST(conformance_compression, a_codec_name_in_another_case_is_accepted_with_a_warning)
{
    // The specification names codecs in lowercase; another case names the same codec. Shuffling items of one byte
    // changes nothing.
    for (const std::string_view compression : {"ZLIB:34", "Zlib+SH:34:1"}) {
        const unit opened =
            open_body(R"(<Property id="Test" type="ByteArray" length="34" compression=")" + std::string(compression) +
                      R"(" location="inline:base64">)"
                      "eNoLycgsVgCiRIWS1OISBV2FENfgECBlaGRsYmpmbmFpAACzWQkd</Property>");
        EXPECT_TRUE(
            single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_compression, "/xisf/Property[1]"))
            << compression;
        EXPECT_EQ(block_data(opened, "/xisf/Property[1]"), bytes("This is a test - TEST - 1234567890")) << compression;
    }

    // A name that is no codec in any case is reported as written, with no warning about its case.
    const unit unknown = open_body(R"(<Property id="Test" type="ByteArray" length="3" compression="LZMA:3" )"
                                   R"(location="inline:base64">YWJj</Property>)");
    ASSERT_TRUE(
        single_diagnostic(unknown.diagnostics, severity::error, errc::unsupported_compression, "/xisf/Property[1]"));
    EXPECT_NE(unknown.diagnostics.front().message.find("'LZMA'"), std::string::npos)
        << unknown.diagnostics.front().message;
}

// ---------------------------------------------------------------------------------------------------------------------
// Compressed blocks in units

TEST(conformance_compression, compressed_blocks_decode_in_every_location)
{
    const std::vector<std::byte> data = pixels();
    const compressed_block block = compress_block(data, {.codec = compression_codec::zstd, .item_size = 2});
    const std::string compression = compression_of(block, "zstd+sh");
    const std::string base64 = openxisf::detail::encode_base64(block.data);

    const std::vector<std::byte> file = file_with_attachments(
        header_xml(
            std::string(row_image) + R"(location="attachment:{0}" compression=")" + compression + "\"/>" +
            std::string(row_image) + R"(location="embedded"><Data encoding="base64" compression=")" + compression +
            "\">" + base64 + "</Data></Image>" +
            R"(<Property id="Test:Bytes" type="ByteArray" length="1200" location="inline:base64" compression=")" +
            compression + "\">" + base64 + "</Property>"),
        {block.data});
    const unit opened = open_internal(file, {.strict = true});

    for (const std::string_view path : {"/xisf/Image[1]", "/xisf/Image[2]", "/xisf/Property[1]"}) {
        EXPECT_EQ(block_data(opened, path), data) << path;
    }
}

TEST(conformance_compression, the_subblocks_of_a_block_decode_one_after_the_other)
{
    const std::vector<std::byte> data = pixels();
    for (const auto& [codec, name] :
         {std::pair{compression_codec::zlib, "zlib+sh"}, std::pair{compression_codec::lz4, "lz4+sh"},
          std::pair{compression_codec::lz4hc, "lz4hc+sh"}, std::pair{compression_codec::zstd, "zstd+sh"}}) {
        const compressed_block block = compress_block(data, {.codec = codec, .item_size = 2, .max_subblock_size = 500});
        ASSERT_EQ(block.compression.subblocks.size(), 3U);
        const unit opened = open_internal(
            file_with_attachments(header_xml(std::string(row_image) + R"(location="attachment:{0}" compression=")" +
                                             compression_of(block, name) + R"(" subblocks=")" + subblocks_of(block) +
                                             "\"/>"),
                                  {block.data}),
            {.strict = true});
        EXPECT_EQ(block_data(opened, "/xisf/Image[1]"), data) << name;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Uncompressed subblocks (spec §10.6.1, Revision 2): the stored ones are tested with the encoder in
// unit/compressed_block and conformance/baseline_decoder; here, what a decoder owes the units of encoders of earlier
// revisions.

TEST(conformance_compression, a_subblock_longer_than_its_data_is_decompressed)
{
    // An encoder of an earlier revision could compress a subblock that got longer, which is decompressed all the
    // same; equal sizes alone mean data stored as they are. The first 400 bytes are noise, which Zstandard makes
    // longer; the rest compress.
    std::vector<std::byte> data = pixels();
    std::uint32_t state = 5;
    for (std::size_t i = 0; i < 400; ++i) {
        state = (state * 1'664'525U) + 1'013'904'223U;
        data[i] = static_cast<std::byte>(state >> 24U);
    }
    std::vector<std::byte> longer;
    openxisf::detail::zstd_compress(std::span(data).first(400), 1, longer);
    ASSERT_GT(longer.size(), 400U);
    std::vector<std::byte> shorter;
    openxisf::detail::zstd_compress(std::span(data).subspan(400), 1, shorter);
    ASSERT_LT(shorter.size(), 800U);
    std::vector<std::byte> stored(longer.size() + shorter.size());
    std::ranges::copy(shorter, std::ranges::copy(longer, stored.begin()).out);
    const unit opened = open_internal(
        file_with_attachments(
            header_xml(std::string(row_image) + R"(location="attachment:{0}" compression="zstd:1200" subblocks=")" +
                       std::to_string(longer.size()) + ",400:" + std::to_string(shorter.size()) + ",800\"/>"),
            {stored}),
        {.strict = true});
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    EXPECT_EQ(block_data(opened, "/xisf/Image[1]"), data);
}

TEST(conformance_compression, a_block_that_does_not_decode_to_its_size_is_invalid)
{
    // Each compressed subblock decompresses to exactly its uncompressed size, or the block is invalid: the stream
    // decodes to 1200 bytes, and the block declares the 1199 that its image needs.
    const compressed_block block = compress_block(pixels(), {.codec = compression_codec::zlib});
    const unit opened = open_internal(file_with_attachments(
        header_xml(
            R"(<Image geometry="1199:1:1" sampleFormat="UInt8" location="attachment:{0}" compression="zlib:1199"/>)"),
        {block.data}));
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::corrupt_compressed_data,
                                                  [&opened] { (void)block_data(opened, "/xisf/Image[1]"); }));
}

// ---------------------------------------------------------------------------------------------------------------------
// Corrupt data (spec §10.6.2)

// Compressed data with a changed byte, which the codec rejects.
std::vector<std::byte> corrupt_block()
{
    compressed_block block = compress_block(pixels(), {.codec = compression_codec::zlib});
    block.data[block.data.size() - 1] ^= std::byte{0xFF};
    return block.data;
}

TEST(conformance_compression, a_compressed_block_that_fails_its_checksum_is_never_decompressed)
{
    // The data are corrupt, and the checksum is of other data: verification comes first, so the error is the checksum.
    const std::vector<std::byte> corrupt = corrupt_block();
    const std::string attributes = R"(compression="zlib:1200" checksum=")" + sha256_of(pixels()) + "\"";

    const unit attached = open_internal(file_with_attachments(
        header_xml(std::string(row_image) + R"(location="attachment:{0}" )" + attributes + "/>"), {corrupt}));
    EXPECT_TRUE(no_diagnostics(attached.diagnostics));
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch,
                                                  [&attached] { (void)block_data(attached, "/xisf/Image[1]"); }));

    const unit embedded = open_body(std::string(row_image) + R"(location="embedded"><Data encoding="base64" )" +
                                    attributes + ">" + openxisf::detail::encode_base64(corrupt) + "</Data></Image>");
    EXPECT_TRUE(
        single_diagnostic(embedded.diagnostics, severity::error, errc::checksum_mismatch, "/xisf/Image[1]/Data[1]"));
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch,
                                                  [&embedded] { (void)block_data(embedded, "/xisf/Image[1]"); }));
}

TEST(conformance_compression, data_that_do_not_decode_fail_with_the_element_of_the_block)
{
    // With a checksum of the corrupt data themselves, the codec is the one to find the problem.
    const std::vector<std::byte> corrupt = corrupt_block();
    const unit opened = open_internal(file_with_attachments(
        header_xml(std::string(row_image) + R"(location="attachment:{0}" compression="zlib:1200" checksum=")" +
                   sha256_of(corrupt) + "\"/>"),
        {corrupt}));
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    try {
        (void)block_data(opened, "/xisf/Image[1]");
        ADD_FAILURE() << "no error";
    } catch (const openxisf::integrity_error& failure) {
        EXPECT_EQ(failure.code(), errc::corrupt_compressed_data);
        EXPECT_EQ(failure.context().element, "/xisf/Image[1]");
        EXPECT_EQ(failure.context().offset, 4096U);
    }
}

TEST(conformance_compression, a_block_that_decompresses_beyond_the_allocation_limit_is_not_read)
{
    const compressed_block block = compress_block(pixels(), {});
    const std::vector<std::byte> file = file_with_attachments(
        header_xml(std::string(row_image) + R"(location="attachment:{0}" compression="zstd:1200"/>)"), {block.data});

    const unit limited = open_internal(file, {.limits = {.max_allocation = 1199}});
    EXPECT_TRUE(no_diagnostics(limited.diagnostics));
    try {
        (void)block_data(limited, "/xisf/Image[1]");
        ADD_FAILURE() << "no error";
    } catch (const openxisf::limit_error& failure) {
        EXPECT_EQ(failure.code(), errc::allocation_too_large);
        EXPECT_EQ(failure.context().element, "/xisf/Image[1]");
    }
    EXPECT_EQ(block_data(open_internal(file, {.limits = {.max_allocation = 1200}}), "/xisf/Image[1]"), pixels());
}

TEST(conformance_compression, a_zstd_block_beyond_the_limit_of_its_expansion_is_unavailable)
{
    // The 1200 bytes of the pixels compress to fewer than 600, so a limit of 2 bytes for each stored byte refuses them,
    // and the default of the format, 32768, does not.
    const compressed_block block = compress_block(pixels(), {.codec = compression_codec::zstd});
    ASSERT_LT(block.data.size(), 600U);
    const std::vector<std::byte> file = file_with_attachments(
        header_xml(std::string(row_image) + R"(location="attachment:{0}" compression="zstd:1200"/>)"), {block.data});

    const unit limited = open_internal(file, {.limits = {.max_zstd_expansion = 2}});
    ASSERT_TRUE(
        single_diagnostic(limited.diagnostics, severity::error, errc::zstd_expansion_too_large, "/xisf/Image[1]"));
    EXPECT_EQ(limited.diagnostics.front().context.attribute, "compression");
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::zstd_expansion_too_large,
                                              [&limited] { (void)block_data(limited, "/xisf/Image[1]"); }));
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::zstd_expansion_too_large, [&file] {
        (void)open_internal(file, {.strict = true, .limits = {.max_zstd_expansion = 2}});
    }));
    EXPECT_EQ(block_data(open_internal(file), "/xisf/Image[1]"), pixels());
    EXPECT_EQ(block_data(open_internal(file, {.limits = {.max_zstd_expansion = 0}}), "/xisf/Image[1]"), pixels());
}

TEST(conformance_compression, a_block_too_large_for_its_decoder_without_subblocks_is_unavailable)
{
    // LZ4 decodes at most 2^31 - 1 bytes at once, so a larger block must be divided into subblocks (spec §10.6).
    const unit opened = open_body(std::string(row_image) +
                                  R"(location="embedded"><Data encoding="base64" compression="lz4:2147483648">)"
                                  R"(AAAA</Data></Image>)");
    EXPECT_TRUE(
        single_diagnostic(opened.diagnostics, severity::error, errc::invalid_subblocks, "/xisf/Image[1]/Data[1]"));
    EXPECT_EQ(opened.diagnostics.front().context.attribute, "compression");
}

} // namespace
