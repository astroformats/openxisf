// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The grammar of the attributes of data blocks (spec §10.3 to §10.6), one value at a time.

#include "container/block_attributes.h"

#include <openxisf/error.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::invalid_data_error;
using openxisf::unsupported_error;
using openxisf::detail::block_compression;
using openxisf::detail::block_encoding;
using openxisf::detail::block_location;
using openxisf::detail::byte_order;
using openxisf::detail::compression_codec;
using openxisf::detail::hash_algorithm;
using openxisf::detail::location_kind;
using openxisf::detail::parse_byte_order;
using openxisf::detail::parse_checksum;
using openxisf::detail::parse_compression;
using openxisf::detail::parse_encoding;
using openxisf::detail::parse_location;
using openxisf::detail::parse_subblocks;
using openxisf::detail::subblock;
using openxisf::test::throws;

// ---------------------------------------------------------------------------------------------------------------------
// Spec §10.3: location

TEST(block_attributes, parses_every_form_of_location)
{
    struct valid
    {
        std::string_view text{};
        block_location location{};
    };
    for (const valid& entry : {
             valid{.text = "inline:base64", .location = {.kind = location_kind::inline_data}},
             valid{.text = "inline:hex",
                   .location = {.kind = location_kind::inline_data, .encoding = block_encoding::hex}},
             valid{.text = "embedded", .location = {.kind = location_kind::embedded}},
             valid{.text = "attachment:4096:6220800",
                   .location = {.kind = location_kind::attachment, .position = 4096, .size = 6220800}},
             // Spec §8.3.2: any plain text representation of an unsigned integer.
             valid{.text = "attachment:0x1000:0b101",
                   .location = {.kind = location_kind::attachment, .position = 4096, .size = 5}},
             valid{.text = "attachment:18446744073709551615:0",
                   .location = {.kind = location_kind::attachment, .position = UINT64_MAX, .size = 0}},
             valid{.text = "url(http://mysite.example.com/myfile.bin)",
                   .location = {.kind = location_kind::url, .reference = "http://mysite.example.com/myfile.bin"}},
             valid{.text = "url(file:///data/huge-things.xisb):0x7a73526b584c6167",
                   .location = {.kind = location_kind::url,
                                .reference = "file:///data/huge-things.xisb",
                                .index_id = 0x7a73526b584c6167U}},
             valid{.text = "path(/Documents/description.txt)",
                   .location = {.kind = location_kind::path, .reference = "/Documents/description.txt"}},
             valid{.text = "path(@header_dir/sample-screenshots.xisb):0x4d373e33756e480f",
                   .location = {.kind = location_kind::path,
                                .reference = "@header_dir/sample-screenshots.xisb",
                                .index_id = 0x4d373e33756e480fU}},
             valid{.text = "path(@header_dir/astrometry/solution.dat):12",
                   .location = {.kind = location_kind::path,
                                .reference = "@header_dir/astrometry/solution.dat",
                                .index_id = 12}},
         }) {
        EXPECT_EQ(parse_location(entry.text), entry.location) << entry.text;
    }
}

TEST(block_attributes, a_url_or_a_path_extends_to_the_last_closing_parenthesis)
{
    EXPECT_EQ(parse_location("url(ftp://ftp.example.com/public/example(2016).dat)").reference,
              "ftp://ftp.example.com/public/example(2016).dat");
    EXPECT_EQ(parse_location("path(/Documents/description(draft).txt)").reference, "/Documents/description(draft).txt");
    EXPECT_EQ(parse_location("path(@header_dir/description(draft).txt)").reference,
              "@header_dir/description(draft).txt");
    // What looks like an index-id followed by a parenthesis is still a part of the URL.
    const block_location location = parse_location("url(name):5)");
    EXPECT_EQ(location.reference, "name):5");
    EXPECT_FALSE(location.index_id);
}

TEST(block_attributes, refuses_a_malformed_location)
{
    for (const std::string_view text : {
             "",
             "Embedded",
             " embedded",
             "embedded ",
             "inline",
             "inline:",
             "inline:base16",
             "inline:Base64",
             "attachment",
             "attachment:",
             "attachment:4096",
             "attachment:4096:10:20",
             "attachment:-1:10",
             "attachment:4096:x",
             "attachment:04096:10",
             "attachment:18446744073709551616:10",
             "url(",
             "url()",
             "url(x",
             "url(x)y",
             // An index-id follows a colon, not any other character.
             "url(x)x5",
             "url(x):",
             "url(x):id",
             "url(x):-1",
             "path(relative/file.dat)",
             "path(/)",
             "path(@header_dir/)",
             "path(@header_dir)",
             "path(@header_directory/file.dat)",
             "file(/data/file.dat)",
         }) {
        EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_location, [text] { (void)parse_location(text); }))
            << "'" << text << "'";
    }
}

TEST(block_attributes, parses_the_encodings_of_inline_and_embedded_blocks)
{
    EXPECT_EQ(parse_encoding("base64"), block_encoding::base64);
    EXPECT_EQ(parse_encoding("hex"), block_encoding::hex);
    // Spec §10.3 names Base16 data hex.
    for (const std::string_view text : {"", "Base64", "HEX", "base16", "hex "}) {
        EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_location, [text] { (void)parse_encoding(text); }))
            << "'" << text << "'";
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Spec §10.4: byteOrder

TEST(block_attributes, parses_a_byte_order)
{
    EXPECT_EQ(parse_byte_order("little"), byte_order::little);
    EXPECT_EQ(parse_byte_order("big"), byte_order::big);
    for (const std::string_view text : {"", "Big", "LITTLE", "network", "le", "big "}) {
        EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_byte_order, [text] { (void)parse_byte_order(text); }))
            << "'" << text << "'";
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Spec §10.5: checksum

std::string digits(std::size_t count)
{
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        text += "0123456789abcdef"[i % 16];
    }
    return text;
}

TEST(block_attributes, parses_a_checksum_of_every_algorithm_by_each_of_its_names)
{
    struct name
    {
        std::string_view text{};
        hash_algorithm algorithm{};
        std::size_t digest_size = 0;
    };
    for (const name& entry : {
             name{.text = "sha-1", .algorithm = hash_algorithm::sha1, .digest_size = 20},
             name{.text = "sha1", .algorithm = hash_algorithm::sha1, .digest_size = 20},
             name{.text = "sha-256", .algorithm = hash_algorithm::sha256, .digest_size = 32},
             name{.text = "sha256", .algorithm = hash_algorithm::sha256, .digest_size = 32},
             name{.text = "sha-512", .algorithm = hash_algorithm::sha512, .digest_size = 64},
             name{.text = "sha512", .algorithm = hash_algorithm::sha512, .digest_size = 64},
             name{.text = "sha3-256", .algorithm = hash_algorithm::sha3_256, .digest_size = 32},
             name{.text = "sha3-512", .algorithm = hash_algorithm::sha3_512, .digest_size = 64},
         }) {
        const auto checksum = parse_checksum(std::string(entry.text) + ":" + digits(2 * entry.digest_size));
        EXPECT_EQ(checksum.algorithm, entry.algorithm) << entry.text;
        ASSERT_EQ(checksum.digest.size(), entry.digest_size) << entry.text;
        EXPECT_EQ(checksum.digest[0], std::byte{0x01});
        EXPECT_EQ(checksum.digest[7], std::byte{0xef});
    }
}

TEST(block_attributes, refuses_a_malformed_checksum)
{
    const std::string valid_digest = digits(40);
    for (const std::string& text : {
             std::string(),
             std::string("sha1"),
             ":" + valid_digest,
             std::string("sha1:"),
             "sha1:" + digits(38),
             "sha1:" + digits(42),
             "sha1:" + digits(39) + "g",
             // Spec §10.5: lowercase digits. The reader accepts uppercase ones with a warning (describe_blocks()).
             "sha1:" + digits(39) + "A",
             "sha1: " + digits(39),
             "sha1:" + digits(40) + " ",
         }) {
        EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_checksum, [&text] { (void)parse_checksum(text); }))
            << "'" << text << "'";
    }
}

TEST(block_attributes, an_unknown_checksum_algorithm_is_unsupported)
{
    // Names are compared exactly, as Table 9 of spec §10.5 writes them.
    for (const std::string_view name : {"md5", "sha224", "SHA1", "SHA-256", "sha3_256", "sha-3-256", "x"}) {
        const std::string text = std::string(name) + ":" + digits(40);
        EXPECT_TRUE(throws<unsupported_error>(errc::unsupported_checksum, [&text] { (void)parse_checksum(text); }))
            << text;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Spec §10.6: compression and subblocks

TEST(block_attributes, parses_the_compression_of_every_codec)
{
    struct valid
    {
        std::string_view text{};
        block_compression compression{};
    };
    for (const valid& entry : {
             valid{.text = "zlib:6220800",
                   .compression = {.codec = compression_codec::zlib, .uncompressed_size = 6220800}},
             valid{.text = "zlib+sh:6220800:4",
                   .compression = {.codec = compression_codec::zlib, .uncompressed_size = 6220800, .item_size = 4}},
             valid{.text = "lz4:34", .compression = {.codec = compression_codec::lz4, .uncompressed_size = 34}},
             valid{.text = "lz4+sh:34:2",
                   .compression = {.codec = compression_codec::lz4, .uncompressed_size = 34, .item_size = 2}},
             valid{.text = "lz4hc:0", .compression = {.codec = compression_codec::lz4hc}},
             valid{.text = "lz4hc+sh:16:16",
                   .compression = {.codec = compression_codec::lz4hc, .uncompressed_size = 16, .item_size = 16}},
             valid{.text = "zstd:67108864",
                   .compression = {.codec = compression_codec::zstd, .uncompressed_size = 67108864}},
             valid{.text = "zstd+sh:201326592:4",
                   .compression = {.codec = compression_codec::zstd, .uncompressed_size = 201326592, .item_size = 4}},
             valid{.text = "zstd+sh:0x100:0b1",
                   .compression = {.codec = compression_codec::zstd, .uncompressed_size = 256, .item_size = 1}},
         }) {
        EXPECT_EQ(parse_compression(entry.text), entry.compression) << entry.text;
    }
}

TEST(block_attributes, refuses_a_malformed_compression)
{
    for (const std::string_view text : {
             "",
             ":100",
             "zlib",
             "zlib:",
             "zlib:x",
             "zlib:-1",
             "zlib:100:4",
             "zlib+sh:100",
             "zlib+sh:100:4:1",
             "zstd+sh:100:x",
             // Byte shuffling needs items of one byte at least (spec §10.6.3).
             "zstd+sh:100:0",
         }) {
        EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_compression, [text] { (void)parse_compression(text); }))
            << "'" << text << "'";
    }
}

TEST(block_attributes, an_unknown_codec_is_unsupported)
{
    for (const std::string_view text : {"bzip2:100", "ZLIB:100", "zlib+shuffle:100:4", "lzma", "zstd-sh:100:4"}) {
        EXPECT_TRUE(throws<unsupported_error>(errc::unsupported_compression, [text] { (void)parse_compression(text); }))
            << text;
    }
}

TEST(block_attributes, parses_subblocks)
{
    EXPECT_EQ(parse_subblocks("1428362,6220800"),
              (std::vector<subblock>{{.compressed_size = 1428362, .uncompressed_size = 6220800}}));
    EXPECT_EQ(parse_subblocks("10,20:30,40:0x5,0"), (std::vector<subblock>{
                                                        {.compressed_size = 10, .uncompressed_size = 20},
                                                        {.compressed_size = 30, .uncompressed_size = 40},
                                                        {.compressed_size = 5, .uncompressed_size = 0},
                                                    }));
}

TEST(block_attributes, refuses_malformed_subblocks)
{
    for (const std::string_view text : {"", ",", "1", "1,", ",1", "1,2,3", "1,2:", ":1,2", "1,2::3,4", "1;2", "a,1",
                                        "1,2:3", "-1,2", "1,18446744073709551616"}) {
        EXPECT_TRUE(throws<invalid_data_error>(errc::invalid_subblocks, [text] { (void)parse_subblocks(text); }))
            << "'" << text << "'";
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The values an encoder writes

TEST(block_attributes, formats_attachments_that_parse_back)
{
    constexpr std::uint64_t largest = 18'446'744'073'709'551'615U;
    EXPECT_EQ(openxisf::detail::format_attachment(4096, 6220800), "attachment:4096:6220800");
    EXPECT_EQ(parse_location(openxisf::detail::format_attachment(largest, 0)),
              (block_location{.kind = location_kind::attachment, .position = largest, .size = 0}));
}

TEST(block_attributes, formats_locations_in_data_blocks_files_that_parse_back)
{
    using openxisf::detail::format_index_id;
    using openxisf::detail::format_relative_location;
    // Spec §10.3: an index-id should be hexadecimal; every one has 16 digits.
    EXPECT_EQ(format_index_id(0x4d373e33756e480f), "0x4d373e33756e480f");
    EXPECT_EQ(format_index_id(1), "0x0000000000000001");
    EXPECT_EQ(format_index_id(UINT64_MAX), "0xffffffffffffffff");
    EXPECT_EQ(format_relative_location("blocks/a (1).xisb", 0x2a),
              "path(@header_dir/blocks/a (1).xisb):0x000000000000002a");
    EXPECT_EQ(parse_location(format_relative_location("unit.xisb", UINT64_MAX)),
              (block_location{.kind = location_kind::path,
                              .reference = "@header_dir/unit.xisb",
                              .index_id = std::uint64_t{UINT64_MAX}}));
}

TEST(block_attributes, formats_checksums_with_the_names_pixinsight_writes)
{
    struct expected_name
    {
        hash_algorithm algorithm = hash_algorithm::sha1;
        std::string_view name{};
    };
    for (const expected_name& entry : {expected_name{.algorithm = hash_algorithm::sha1, .name = "sha1"},
                                       expected_name{.algorithm = hash_algorithm::sha256, .name = "sha256"},
                                       expected_name{.algorithm = hash_algorithm::sha512, .name = "sha512"},
                                       expected_name{.algorithm = hash_algorithm::sha3_256, .name = "sha3-256"},
                                       expected_name{.algorithm = hash_algorithm::sha3_512, .name = "sha3-512"}}) {
        EXPECT_EQ(openxisf::detail::checksum_name(entry.algorithm), entry.name);
        const openxisf::detail::block_checksum checksum{
            .algorithm = entry.algorithm,
            .digest = std::vector<std::byte>(openxisf::detail::digest_size(entry.algorithm), std::byte{0xA5})};
        const std::string text = openxisf::detail::format_checksum(checksum);
        EXPECT_TRUE(text.starts_with(std::string(entry.name) + ":a5a5")) << text;
        EXPECT_EQ(parse_checksum(text), checksum);
    }
}

TEST(block_attributes, formats_the_compression_of_every_codec)
{
    for (const compression_codec codec :
         {compression_codec::zlib, compression_codec::lz4, compression_codec::lz4hc, compression_codec::zstd}) {
        for (const std::uint64_t item_size : {0U, 1U, 4U, 16U}) {
            const block_compression compression{.codec = codec, .uncompressed_size = 6220800, .item_size = item_size};
            const std::string text = openxisf::detail::format_compression(compression);
            EXPECT_EQ(text.find('+') != std::string::npos, item_size != 0) << text;
            EXPECT_EQ(parse_compression(text), compression) << text;
        }
    }
    EXPECT_EQ(openxisf::detail::format_compression(
                  {.codec = compression_codec::zstd, .uncompressed_size = 201326592, .item_size = 4}),
              "zstd+sh:201326592:4");
    EXPECT_EQ(openxisf::detail::compression_name({.codec = compression_codec::lz4hc}), "lz4hc");
}

TEST(block_attributes, formats_subblocks)
{
    const std::vector<subblock> subblocks{{.compressed_size = 10, .uncompressed_size = 20},
                                          {.compressed_size = 30, .uncompressed_size = 40}};
    EXPECT_EQ(openxisf::detail::format_subblocks(subblocks), "10,20:30,40");
    EXPECT_EQ(parse_subblocks(openxisf::detail::format_subblocks(subblocks)), subblocks);
}

} // namespace
