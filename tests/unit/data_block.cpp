// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The data blocks of a unit: which elements have one, where its bytes are, and the rules that tie the attributes of a
// block to each other and to the unit (spec §10). The checksum rules are in conformance/checksum.cpp, and those of the
// byte order in conformance/byte_order.cpp.

#include "container/data_block.h"

#include <openxisf/error.h>
#include <openxisf/reader.h>

#include "model/ancillary_budget.h"
#include "model/unit.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"
#include "support/throws.h"
#include "support/virtual_source.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::severity;
using openxisf::detail::block_descriptor;
using openxisf::detail::data_block;
using openxisf::detail::location_kind;
using openxisf::detail::subblock;
using openxisf::detail::unit;
using openxisf::test::block_at;
using openxisf::test::block_paths;
using openxisf::test::bytes;
using openxisf::test::descriptor_at;
using openxisf::test::file_with_attachments;
using openxisf::test::header_xml;
using openxisf::test::monolithic_file;
using openxisf::test::no_diagnostics;
using openxisf::test::open_internal;
using openxisf::test::single_diagnostic;
using openxisf::test::stored_block;
using openxisf::test::throws;
using openxisf::test::value_of;

// A unit whose root element holds body after its Metadata element.
unit open_body(std::string_view body)
{
    return open_internal(monolithic_file(header_xml(body)));
}

// A ByteArray property of length bytes, with the given attributes and content.
std::string bytes_property(std::size_t length, std::string_view attributes, std::string_view content = {})
{
    return R"(<Property id="Test:Bytes" type="ByteArray" length=")" + std::to_string(length) + "\" " +
           std::string(attributes) + ">" + std::string(content) + "</Property>";
}

// A gray 8-bit image of one row of width pixels, with the given attributes and content.
std::string row_image(std::size_t width, std::string_view attributes, std::string_view content = {})
{
    return R"(<Image geometry=")" + std::to_string(width) + R"(:1:1" sampleFormat="UInt8" colorSpace="Gray" )" +
           std::string(attributes) + ">" + std::string(content) + "</Image>";
}

// The block at path is unavailable because of the only diagnostic of the unit, an error with code about element.
testing::AssertionResult unavailable(const unit& opened, std::string_view path, errc code, std::string_view element)
{
    testing::AssertionResult found = single_diagnostic(opened.diagnostics, severity::error, code, element);
    if (!found) {
        return found;
    }
    const data_block& block = block_at(opened, path);
    if (block.descriptor || !block.problem || block.problem->code != code ||
        block.problem->message != opened.diagnostics.front().message) {
        return testing::AssertionFailure() << "the block at " << path << " is not unavailable for that error";
    }
    return testing::AssertionSuccess();
}

std::vector<std::byte> abc()
{
    return bytes("abc");
}

// ---------------------------------------------------------------------------------------------------------------------
// Which elements have blocks

TEST(data_block, every_element_that_can_serialize_a_block_has_one_when_it_has_a_location)
{
    const unit opened = open_body(
        row_image(3, R"(location="embedded")",
                  R"(<Data encoding="base64">YWJj</Data>)"
                  R"(<Property id="Test:Value" type="UInt8" value="1"/>)" +
                      bytes_property(3, R"(location="inline:hex")", "616263") +
                      R"(<ICCProfile location="inline:base64">YWJj</ICCProfile>)"
                      R"(<Thumbnail geometry="3:1:1" sampleFormat="UInt8" colorSpace="Gray" location="embedded">)"
                      R"(<Data encoding="hex">616263</Data></Thumbnail>)") +
        // Other elements do not have blocks, and a location attribute means nothing to them.
        R"(<FITSKeyword name="OBSERVER" value="'Test'" comment="" location="inline:base64"/>)"
        R"(<Table id="Test:Table"><Structure><Field id="s" type="String"/></Structure>)"
        R"(<Row><Cell location="inline:base64">YWJj</Cell></Row></Table>)");

    // abc is no ICC profile, which the reader keeps with a warning.
    EXPECT_TRUE(openxisf::test::single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_icc_profile,
                                                  "/xisf/Image[1]/ICCProfile[1]"));
    EXPECT_EQ(block_paths(opened),
              (std::vector<std::string>{"/xisf/Image[1]", "/xisf/Image[1]/Property[2]", "/xisf/Image[1]/ICCProfile[1]",
                                        "/xisf/Image[1]/Thumbnail[1]", "/xisf/Table[1]/Row[1]/Cell[1]"}));
    for (const data_block& block : opened.blocks) {
        EXPECT_EQ(stored_block(opened, block.path), abc()) << block.path;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Inline and embedded blocks (spec §10.3)

TEST(data_block, an_inline_block_is_the_character_data_of_its_element)
{
    // White space in the data is ignored, and CDATA sections are character data too.
    const unit opened =
        open_body(bytes_property(14, R"(location="inline:base64")", "\n  VGhpcyBp\n  <![CDATA[cyBhIH]]>Rlc3Q=\n"));

    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    const block_descriptor& descriptor = descriptor_at(opened, "/xisf/Property[1]");
    EXPECT_EQ(descriptor.location.kind, location_kind::inline_data);
    EXPECT_EQ(descriptor.data, bytes("This is a test"));
}

TEST(data_block, an_inline_hexadecimal_block_is_decoded)
{
    const unit opened = open_body(bytes_property(3, R"(location="inline:hex")", "61 62\n63"));
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    EXPECT_EQ(stored_block(opened, "/xisf/Property[1]"), abc());
}

TEST(data_block, an_image_or_a_thumbnail_cannot_have_an_inline_block)
{
    // Spec §11.5, §11.12: their elements can have child elements. The character data are the block all the same.
    const unit image =
        open_body(row_image(3, R"(location="inline:base64")", R"(YW<Resolution horizontal="72" vertical="72"/>Jj)"));
    EXPECT_TRUE(single_diagnostic(image.diagnostics, severity::warning, errc::invalid_location, "/xisf/Image[1]"));
    EXPECT_EQ(stored_block(image, "/xisf/Image[1]"), abc());

    const unit thumbnail = open_body(
        R"(<Thumbnail geometry="3:1:1" sampleFormat="UInt8" colorSpace="Gray" location="inline:base64">YWJj</Thumbnail>)");
    EXPECT_TRUE(
        single_diagnostic(thumbnail.diagnostics, severity::warning, errc::invalid_location, "/xisf/Thumbnail[1]"));
    EXPECT_EQ(stored_block(thumbnail, "/xisf/Thumbnail[1]"), abc());
}

TEST(data_block, another_element_with_an_inline_block_cannot_have_child_elements)
{
    // Spec §10.3. The character data are the block all the same, and the Data element means nothing.
    const unit opened =
        open_body(bytes_property(3, R"(location="inline:base64")", R"(YW<Data encoding="base64">AA==</Data>Jj)"));

    ASSERT_EQ(opened.diagnostics.size(), 2U) << openxisf::test::describe(opened.diagnostics);
    EXPECT_EQ(opened.diagnostics[0].context.element, "/xisf/Property[1]/Data[1]");
    EXPECT_EQ(opened.diagnostics[1].severity, severity::warning);
    EXPECT_EQ(opened.diagnostics[1].code, errc::invalid_location);
    EXPECT_EQ(opened.diagnostics[1].context.element, "/xisf/Property[1]");
    EXPECT_EQ(stored_block(opened, "/xisf/Property[1]"), abc());
}

TEST(data_block, an_embedded_block_is_the_character_data_of_its_data_element)
{
    for (const std::string_view data :
         {R"(<Data encoding="base64"> YW Jj </Data>)", R"(<Data encoding="hex">616263</Data>)",
          R"(<Data encoding="base64"><![CDATA[YWJj]]></Data>)"}) {
        const unit opened = open_body(row_image(3, R"(location="embedded")", data));
        EXPECT_TRUE(no_diagnostics(opened.diagnostics)) << data;
        EXPECT_EQ(stored_block(opened, "/xisf/Image[1]"), abc()) << data;
    }
}

TEST(data_block, an_embedded_block_needs_exactly_one_data_element)
{
    const unit without = open_body(row_image(3, R"(location="embedded")"));
    EXPECT_TRUE(unavailable(without, "/xisf/Image[1]", errc::invalid_location, "/xisf/Image[1]"));

    const unit twice = open_body(row_image(
        3, R"(location="embedded")", R"(<Data encoding="base64">YWJj</Data><Data encoding="base64">YWJj</Data>)"));
    EXPECT_TRUE(unavailable(twice, "/xisf/Image[1]", errc::invalid_location, "/xisf/Image[1]"));
}

TEST(data_block, the_data_element_of_an_embedded_block_needs_an_encoding)
{
    for (const std::string_view data : {"<Data>YWJj</Data>", R"(<Data encoding="base16">616263</Data>)"}) {
        const unit opened = open_body(row_image(3, R"(location="embedded")", data));
        EXPECT_TRUE(unavailable(opened, "/xisf/Image[1]", errc::invalid_location, "/xisf/Image[1]/Data[1]")) << data;
    }
}

TEST(data_block, data_elements_that_hold_no_embedded_block_are_ignored_with_a_warning)
{
    const unit inline_block =
        open_body(bytes_property(3, R"(location="inline:base64")", R"(YWJj<Data encoding="base64">AAAA</Data>)"));
    // The child element of the inline block is reported too.
    ASSERT_EQ(inline_block.diagnostics.size(), 2U);
    EXPECT_EQ(inline_block.diagnostics[0].code, errc::invalid_location);
    EXPECT_EQ(inline_block.diagnostics[0].context.element, "/xisf/Property[1]/Data[1]");
    EXPECT_EQ(stored_block(inline_block, "/xisf/Property[1]"), abc());

    const std::vector<std::byte> file = file_with_attachments(
        header_xml(row_image(3, R"(location="attachment:{0}")", R"(<Data encoding="base64">AAAA</Data>)")), {abc()});
    const unit attached = open_internal(file);
    EXPECT_TRUE(
        single_diagnostic(attached.diagnostics, severity::warning, errc::invalid_location, "/xisf/Image[1]/Data[1]"));
    EXPECT_EQ(stored_block(attached, "/xisf/Image[1]"), abc());

    const unit no_block =
        open_body(R"(<Property id="Test:Value" type="UInt8" value="1"><Data encoding="base64">AAAA</Data></Property>)");
    EXPECT_TRUE(single_diagnostic(no_block.diagnostics, severity::warning, errc::invalid_location,
                                  "/xisf/Property[1]/Data[1]"));
    EXPECT_TRUE(no_block.blocks.empty());
}

TEST(data_block, block_attributes_of_an_element_without_a_block_are_ignored_with_a_warning)
{
    const unit opened = open_body(R"(<Property id="Test:Value" type="UInt8" value="1" byteOrder="big")"
                                  R"( checksum="sha1:0" compression="zlib:1" subblocks="1,1"/>)");

    ASSERT_EQ(opened.diagnostics.size(), 4U);
    const std::vector<errc> codes{errc::invalid_byte_order, errc::invalid_checksum, errc::invalid_compression,
                                  errc::invalid_subblocks};
    for (std::size_t i = 0; i < codes.size(); ++i) {
        EXPECT_EQ(opened.diagnostics[i].severity, severity::warning);
        EXPECT_EQ(opened.diagnostics[i].code, codes[i]);
    }
    EXPECT_TRUE(opened.blocks.empty());
}

// PixInsight writes the compression and the checksum of an embedded block on its Data element, which spec §10.6
// requires for the compression.
TEST(data_block, the_attributes_of_an_embedded_block_belong_to_its_data_element)
{
    const unit opened = open_body(row_image(
        3, R"(location="embedded")", R"(<Data encoding="base64" compression="zstd:3" subblocks="3,3">YWJj</Data>)"));
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    const block_descriptor& descriptor = descriptor_at(opened, "/xisf/Image[1]");
    EXPECT_EQ(value_of(descriptor.compression).subblocks,
              (std::vector<subblock>{{.compressed_size = 3, .uncompressed_size = 3}}));
}

TEST(data_block, an_attribute_of_an_embedded_block_on_its_element_is_used_with_a_warning)
{
    const unit misplaced = open_body(
        row_image(3, R"(location="embedded" compression="zstd:3")", R"(<Data encoding="base64">YWJj</Data>)"));
    EXPECT_TRUE(
        single_diagnostic(misplaced.diagnostics, severity::warning, errc::invalid_compression, "/xisf/Image[1]"));
    const block_descriptor& descriptor = descriptor_at(misplaced, "/xisf/Image[1]");
    EXPECT_EQ(value_of(descriptor.compression).uncompressed_size, 3U);

    // The attribute of the Data element applies when both have one.
    const unit both = open_body(row_image(3, R"(location="embedded" compression="zstd:30")",
                                          R"(<Data encoding="base64" compression="zstd:3">YWJj</Data>)"));
    EXPECT_TRUE(single_diagnostic(both.diagnostics, severity::warning, errc::invalid_compression, "/xisf/Image[1]"));
    const block_descriptor& chosen = descriptor_at(both, "/xisf/Image[1]");
    EXPECT_EQ(value_of(chosen.compression).uncompressed_size, 3U);
}

// ---------------------------------------------------------------------------------------------------------------------
// The text of inline and embedded blocks: the reader leniency rule

TEST(data_block, base64_data_without_padding_are_accepted_with_a_warning)
{
    struct unpadded
    {
        std::string_view text{};
        std::string_view data{};
    };
    for (const unpadded& entry :
         {unpadded{.text = "YWJjZA", .data = "abcd"}, unpadded{.text = "YWJjZGU", .data = "abcde"},
          unpadded{.text = "YW Jj ZA\n", .data = "abcd"}}) {
        const unit opened = open_body(bytes_property(entry.data.size(), R"(location="inline:base64")", entry.text));
        EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_base64, "/xisf/Property[1]"))
            << entry.text;
        EXPECT_EQ(stored_block(opened, "/xisf/Property[1]"), bytes(entry.data)) << entry.text;
    }
}

TEST(data_block, uppercase_hexadecimal_data_are_accepted_with_a_warning)
{
    const unit opened = open_body(bytes_property(3, R"(location="inline:hex")", "6A6b6C"));
    EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_hex, "/xisf/Property[1]"));
    EXPECT_EQ(stored_block(opened, "/xisf/Property[1]"), bytes("jkl"));
}

TEST(data_block, text_that_is_not_base64_or_hexadecimal_data_makes_the_block_unavailable)
{
    struct invalid_text
    {
        std::string_view location{};
        std::string_view text{};
        errc code = errc::invalid_base64;
    };
    for (const invalid_text& entry : {
             invalid_text{.location = "inline:base64", .text = "YW!j", .code = errc::invalid_base64},
             invalid_text{.location = "inline:base64", .text = "Y", .code = errc::invalid_base64},
             invalid_text{.location = "inline:base64", .text = "YWJj=", .code = errc::invalid_base64},
             invalid_text{.location = "inline:hex", .text = "61626", .code = errc::invalid_hex},
             invalid_text{.location = "inline:hex", .text = "6162xy", .code = errc::invalid_hex},
         }) {
        const unit opened =
            open_body(bytes_property(3, "location=\"" + std::string(entry.location) + "\"", entry.text));
        EXPECT_TRUE(unavailable(opened, "/xisf/Property[1]", entry.code, "/xisf/Property[1]")) << entry.text;
    }
}

TEST(data_block, only_an_inline_block_can_be_empty)
{
    // Spec §10: the inline block of an empty vector or matrix has no bytes; any other block has at least one.
    const unit inline_block = open_body(bytes_property(0, R"(location="inline:base64")"));
    EXPECT_TRUE(no_diagnostics(inline_block.diagnostics));
    EXPECT_TRUE(stored_block(inline_block, "/xisf/Property[1]").empty());

    const unit embedded = open_body(bytes_property(0, R"(location="embedded")", R"(<Data encoding="hex"/>)"));
    EXPECT_TRUE(
        single_diagnostic(embedded.diagnostics, severity::warning, errc::invalid_location, "/xisf/Property[1]"));
    EXPECT_TRUE(stored_block(embedded, "/xisf/Property[1]").empty());

    const unit attached = open_internal(file_with_attachments(
        header_xml(bytes_property(0, R"(location="attachment:{0}")")), std::vector<std::vector<std::byte>>(1)));
    EXPECT_TRUE(
        single_diagnostic(attached.diagnostics, severity::warning, errc::invalid_location, "/xisf/Property[1]"));
    EXPECT_TRUE(stored_block(attached, "/xisf/Property[1]").empty());
}

// ---------------------------------------------------------------------------------------------------------------------
// Attached blocks (spec §10.1)

TEST(data_block, an_attached_block_is_read_from_its_position_in_the_file)
{
    const std::vector<std::byte> first = openxisf::test::pattern(1000);
    const std::vector<std::byte> second = bytes("second");
    const unit opened = open_internal(file_with_attachments(
        header_xml(row_image(1000, R"(location="attachment:{0}")") + bytes_property(6, R"(location="attachment:{1}")")),
        {first, second}));

    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    const block_descriptor& descriptor = descriptor_at(opened, "/xisf/Image[1]");
    EXPECT_EQ(descriptor.location.position, 4096U);
    EXPECT_EQ(descriptor.location.size, 1000U);
    EXPECT_EQ(stored_block(opened, "/xisf/Image[1]"), first);
    EXPECT_EQ(stored_block(opened, "/xisf/Property[1]"), second);
}

TEST(data_block, elements_can_share_an_attached_block)
{
    const unit opened = open_internal(file_with_attachments(
        header_xml(row_image(3, R"(location="attachment:{0}")") + row_image(3, R"(location="attachment:{0}")")),
        {abc()}));
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    EXPECT_EQ(stored_block(opened, "/xisf/Image[1]"), abc());
    EXPECT_EQ(stored_block(opened, "/xisf/Image[2]"), abc());
}

TEST(data_block, an_attached_block_lies_between_the_header_and_the_end_of_the_file)
{
    const std::vector<std::byte> file =
        file_with_attachments(header_xml(row_image(3, R"(location="attachment:{0}")")), {abc()});
    // file_with_attachments() puts the block at 4096, so the header ends before.
    const std::uint64_t end = file.size();
    // One pixel, so that the last byte of the file is a whole image.
    const auto open_with = [&file](std::string_view location) {
        const std::string text = header_xml(row_image(1, "location=\"" + std::string(location) + "\""));
        std::vector<std::byte> changed = monolithic_file(text);
        changed.resize(file.size());
        std::copy(file.begin() + 4096, file.end(), changed.begin() + 4096);
        return open_internal(changed);
    };

    const unit last_byte = open_with("attachment:" + std::to_string(end - 1) + ":1");
    EXPECT_TRUE(no_diagnostics(last_byte.diagnostics));
    EXPECT_EQ(stored_block(last_byte, "/xisf/Image[1]"), bytes("c"));

    for (const std::string& location :
         {"attachment:" + std::to_string(end - 2) + ":3", "attachment:" + std::to_string(end + 1) + ":0",
          std::string("attachment:4096:18446744073709551615"), std::string("attachment:18446744073709551615:1"),
          std::string("attachment:0:3"), std::string("attachment:16:3")}) {
        const unit opened = open_with(location);
        EXPECT_TRUE(unavailable(opened, "/xisf/Image[1]", errc::block_out_of_bounds, "/xisf/Image[1]")) << location;
    }
}

TEST(data_block, an_attached_block_starts_after_the_bytes_that_the_header_length_counts)
{
    // The header length counts zeros after the XML, which the reader drops; a block that starts among them is still
    // inside the header.
    const std::string header = header_xml(row_image(3, R"(location="attachment:1000:3")"));
    std::vector<std::byte> file = monolithic_file(header + std::string(1000 - 16 - header.size() + 1, '\0'));
    const std::vector<std::byte> block = abc();
    file.insert(file.end(), block.begin(), block.end());
    const unit opened = open_internal(file);
    ASSERT_EQ(opened.diagnostics.size(), 2U);
    EXPECT_EQ(opened.diagnostics[1].code, errc::block_out_of_bounds);
}

TEST(data_block, attached_blocks_beyond_4_gib)
{
    // A file of 5 GiB whose bytes are computed from their offset, but for the header, which needs no memory for the
    // rest; the source records where it is read.
    constexpr std::uint64_t size = std::uint64_t{5} << 30;
    constexpr std::uint64_t beyond = std::uint64_t{1} << 32;
    auto reads = std::make_shared<std::vector<std::uint64_t>>();
    const auto open_at = [&reads](std::uint64_t position) {
        const std::string header =
            header_xml(row_image(10, "location=\"attachment:" + std::to_string(position) + ":10\""));
        return unit(openxisf::test::virtual_source(size, {{.position = 0, .data = monolithic_file(header)}}, reads),
                    {});
    };

    // A block whose position, cut to 32 bits, would be inside the header, and one that ends where the file does.
    for (const std::uint64_t position : {beyond + 16, size - 10}) {
        const unit opened = open_at(position);
        EXPECT_TRUE(no_diagnostics(opened.diagnostics)) << position;
        EXPECT_EQ(stored_block(opened, "/xisf/Image[1]"), openxisf::test::virtual_bytes(position, 10)) << position;
    }
    const unit past_the_end = open_at(size - 9);
    EXPECT_TRUE(unavailable(past_the_end, "/xisf/Image[1]", errc::block_out_of_bounds, "/xisf/Image[1]"));

    // Read in pieces when its progress is reported.
    const unit opened = open_at(beyond + 16);
    reads->clear();
    std::vector<std::uint64_t> calls;
    EXPECT_EQ(openxisf::detail::read_block(
                  opened.source, block_at(opened, "/xisf/Image[1]"), opened.limits,
                  [&calls](std::uint64_t done) { calls.push_back(done); }, 4),
              openxisf::test::virtual_bytes(beyond + 16, 10));
    EXPECT_EQ(*reads, (std::vector<std::uint64_t>{beyond + 16, beyond + 20, beyond + 24}));
    EXPECT_EQ(calls, (std::vector<std::uint64_t>{4, 8, 10}));
}

// ---------------------------------------------------------------------------------------------------------------------
// Monolithic and distributed units (spec §10.1, §10.2)

TEST(data_block, a_header_file_cannot_have_attached_blocks)
{
    const unit opened = open_internal(bytes(header_xml(row_image(3, R"(location="attachment:0:3")"))));
    EXPECT_EQ(opened.storage, openxisf::unit_storage::distributed);
    EXPECT_TRUE(unavailable(opened, "/xisf/Image[1]", errc::invalid_location, "/xisf/Image[1]"));
}

TEST(data_block, a_monolithic_file_cannot_have_external_blocks)
{
    for (const std::string_view location : {"url(http://example.com/data.bin)", "path(@header_dir/data.xisb):1"}) {
        const unit opened = open_body(row_image(3, "location=\"" + std::string(location) + "\""));
        EXPECT_TRUE(unavailable(opened, "/xisf/Image[1]", errc::invalid_location, "/xisf/Image[1]")) << location;
    }
}

// The blocks are located through the resolver of the options, which a unit opened from memory has not.
TEST(data_block, the_external_blocks_of_a_header_file_need_a_resolver)
{
    const unit opened = open_internal(bytes(header_xml(row_image(3, R"x(location="path(@header_dir/data.bin)")x"))));
    EXPECT_TRUE(unavailable(opened, "/xisf/Image[1]", errc::unsupported_location, "/xisf/Image[1]"));
    EXPECT_TRUE(throws<openxisf::unsupported_error>(errc::unsupported_location,
                                                    [&opened] { (void)stored_block(opened, "/xisf/Image[1]"); }));
}

// ---------------------------------------------------------------------------------------------------------------------
// Malformed and unsupported attributes

TEST(data_block, a_malformed_or_unsupported_attribute_makes_the_block_unavailable)
{
    struct invalid_attribute
    {
        std::string_view attributes{};
        errc code = errc::invalid_location;
    };
    for (const invalid_attribute& entry : {
             invalid_attribute{.attributes = R"(location="inline:base16")", .code = errc::invalid_location},
             invalid_attribute{.attributes = R"(location="inline:base64" byteOrder="middle")",
                               .code = errc::invalid_byte_order},
             invalid_attribute{.attributes = R"(location="inline:base64" checksum="sha1:abc")",
                               .code = errc::invalid_checksum},
             invalid_attribute{.attributes =
                                   R"(location="inline:base64" checksum="md5:900150983cd24fb0d6963f7d28e17f72")",
                               .code = errc::unsupported_checksum},
             invalid_attribute{.attributes = R"(location="inline:base64" compression="zlib")",
                               .code = errc::invalid_compression},
             invalid_attribute{.attributes = R"(location="inline:base64" compression="bzip2:3")",
                               .code = errc::unsupported_compression},
             invalid_attribute{.attributes = R"(location="inline:base64" compression="zlib:3" subblocks="3")",
                               .code = errc::invalid_subblocks},
         }) {
        const unit opened = open_body(bytes_property(3, entry.attributes, "YWJj"));
        EXPECT_TRUE(unavailable(opened, "/xisf/Property[1]", entry.code, "/xisf/Property[1]")) << entry.attributes;
    }
}

TEST(data_block, reading_an_unavailable_block_throws_its_error)
{
    const unit malformed = open_body(bytes_property(3, R"(location="inline:base16")", "YWJj"));
    EXPECT_TRUE(throws<openxisf::invalid_data_error>(
        errc::invalid_location, [&malformed] { (void)stored_block(malformed, "/xisf/Property[1]"); }));

    const unit unsupported = open_body(bytes_property(3, R"(location="inline:base64" compression="bzip2:3")", "YWJj"));
    EXPECT_TRUE(throws<openxisf::unsupported_error>(
        errc::unsupported_compression, [&unsupported] { (void)stored_block(unsupported, "/xisf/Property[1]"); }));
}

// ---------------------------------------------------------------------------------------------------------------------
// Compression and subblocks (spec §10.6)

TEST(data_block, the_subblocks_account_for_every_byte_of_their_block)
{
    // An attached block and an embedded block of 3 stored bytes, each declared as 10 bytes uncompressed. The rules of
    // the sizes are in unit/compressed_block.cpp.
    for (const bool embedded : {false, true}) {
        const auto open_with = [embedded](std::string_view subblocks) {
            const std::string attributes = R"(compression="lz4:10" subblocks=")" + std::string(subblocks) + "\"";
            const std::string image = embedded ? row_image(10, R"(location="embedded")",
                                                           R"(<Data encoding="base64" )" + attributes + ">YWJj</Data>")
                                               : row_image(10, R"(location="attachment:{0}" )" + attributes);
            return open_internal(file_with_attachments(header_xml(image), {abc()}));
        };
        const std::string_view element = embedded ? "/xisf/Image[1]/Data[1]" : "/xisf/Image[1]";

        const unit valid = open_with("1,4:2,6");
        EXPECT_TRUE(no_diagnostics(valid.diagnostics)) << element;
        const block_descriptor& descriptor = descriptor_at(valid, "/xisf/Image[1]");
        EXPECT_EQ(value_of(descriptor.compression).subblocks,
                  (std::vector<subblock>{{.compressed_size = 1, .uncompressed_size = 4},
                                         {.compressed_size = 2, .uncompressed_size = 6}}));

        EXPECT_TRUE(unavailable(open_with("1,4:2,5"), "/xisf/Image[1]", errc::invalid_subblocks, element)) << element;
    }
}

// Descriptors that no open makes, which a read still refuses, with the context of the block: an external block that
// was never located, and subblocks that do not add up to the stored block. load_block() records the same errors, and
// gives back the budget of the block.
TEST(data_block, a_descriptor_that_cannot_be_read_is_refused_with_the_context_of_its_block)
{
    const unit opened =
        open_internal(file_with_attachments(header_xml(row_image(3, R"(location="attachment:{0}")")), {abc()}));
    const block_descriptor& attached = descriptor_at(opened, "/xisf/Image[1]");
    block_descriptor unlocated = attached;
    unlocated.location.kind = location_kind::path;
    block_descriptor uneven = attached;
    uneven.compression =
        openxisf::detail::block_compression{.codec = openxisf::detail::compression_codec::zlib,
                                            .uncompressed_size = 3,
                                            .subblocks = {{.compressed_size = 1, .uncompressed_size = 3}}};
    struct refused
    {
        block_descriptor descriptor{};
        errc code = errc::unsupported_location;
        std::optional<std::uint64_t> offset{};
    };
    for (const refused& entry :
         {refused{.descriptor = unlocated, .code = errc::unsupported_location},
          refused{.descriptor = uneven, .code = errc::invalid_subblocks, .offset = attached.location.position}}) {
        const data_block block{.path = "/xisf/Image[1]", .descriptor = entry.descriptor};
        try {
            (void)openxisf::detail::read_block(opened.source, block, opened.limits);
            ADD_FAILURE() << "read " << static_cast<int>(entry.code);
        } catch (const openxisf::error& failure) {
            EXPECT_EQ(failure.code(), entry.code);
            EXPECT_EQ(failure.context().element, "/xisf/Image[1]");
            EXPECT_EQ(failure.context().offset, entry.offset);
        }

        openxisf::detail::ancillary_budget budget(openxisf::detail::block_cost(entry.descriptor));
        openxisf::detail::diagnostic_log log(false);
        EXPECT_FALSE(
            openxisf::detail::load_block(opened.source, entry.descriptor, opened.limits, budget, "/xisf/Image[1]", log)
                .has_value());
        EXPECT_TRUE(single_diagnostic(log.entries(), severity::error, entry.code, "/xisf/Image[1]"));
        EXPECT_TRUE(budget.load(openxisf::detail::block_cost(entry.descriptor), {}, log));
    }
}

TEST(data_block, subblocks_without_compression_are_ignored_with_a_warning)
{
    const unit opened = open_body(bytes_property(3, R"(location="inline:base64" subblocks="3,3")", "YWJj"));
    EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_subblocks, "/xisf/Property[1]"));
    EXPECT_EQ(stored_block(opened, "/xisf/Property[1]"), abc());
}

TEST(data_block, the_stored_bytes_of_a_compressed_block_are_returned_as_they_are)
{
    // Decompression is the next step, not a part of reading the stored block.
    const unit opened = open_body(
        row_image(100, R"(location="embedded")", R"(<Data encoding="base64" compression="zlib:100">YWJj</Data>)"));
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    EXPECT_EQ(stored_block(opened, "/xisf/Image[1]"), abc());
}

// ---------------------------------------------------------------------------------------------------------------------
// Progress

TEST(data_block, an_attachment_is_read_in_pieces_when_its_progress_is_reported)
{
    const std::vector<std::byte> data = openxisf::test::pattern(10);
    const unit opened =
        open_internal(file_with_attachments(header_xml(row_image(10, R"(location="attachment:{0}")")), {data}));
    std::vector<std::uint64_t> calls;
    const auto progress = [&calls](std::uint64_t done) { calls.push_back(done); };
    EXPECT_EQ(
        openxisf::detail::read_block(opened.source, block_at(opened, "/xisf/Image[1]"), opened.limits, progress, 3),
        data);
    EXPECT_EQ(calls, (std::vector<std::uint64_t>{3, 6, 9, 10}));
}

TEST(data_block, the_progress_of_a_block_counts_the_bytes_read_then_those_decompressed)
{
    // An embedded block is read at once.
    const unit opened = open_body(row_image(3, R"(location="embedded")", R"(<Data encoding="hex">616263</Data>)"));
    const block_descriptor& descriptor = descriptor_at(opened, "/xisf/Image[1]");
    EXPECT_EQ(openxisf::detail::work_size(descriptor), 3U);
    std::vector<std::uint64_t> calls;
    (void)openxisf::detail::read_block(opened.source, block_at(opened, "/xisf/Image[1]"), opened.limits,
                                       [&calls](std::uint64_t done) { calls.push_back(done); });
    EXPECT_EQ(calls, std::vector<std::uint64_t>{3});

    // A compressed block counts its stored and its uncompressed size, which saturates beyond 64 bits.
    block_descriptor compressed{.location = {.kind = location_kind::attachment, .position = 4096, .size = 10},
                                .compression = openxisf::detail::block_compression{.uncompressed_size = 100}};
    EXPECT_EQ(openxisf::detail::work_size(compressed), 110U);
    compressed.compression->uncompressed_size = std::numeric_limits<std::uint64_t>::max() - 5;
    EXPECT_EQ(openxisf::detail::work_size(compressed), std::numeric_limits<std::uint64_t>::max());
}

// ---------------------------------------------------------------------------------------------------------------------
// Reads into memory of the caller

TEST(data_block, a_block_read_into_memory_of_the_caller_is_verified_there)
{
    const std::string sha1_of_abc = "a9993e364706816aba3e25717850c26c9cd0d89d";
    const std::vector<std::byte> file = file_with_attachments(
        header_xml(row_image(3, R"(location="attachment:{0}" checksum="sha1:)" + sha1_of_abc + "\"") +
                   row_image(3, R"(location="attachment:{1}" checksum="sha1:)" + sha1_of_abc + "\"")),
        {abc(), bytes("abd")});
    const unit opened = open_internal(file);
    std::vector<std::byte> destination(3);
    openxisf::detail::read_block_into(opened.source, block_at(opened, "/xisf/Image[1]"), opened.limits, destination);
    EXPECT_EQ(destination, abc());

    // Spec §10.5: bytes that fail their checksum are not left in the destination.
    std::ranges::fill(destination, std::byte{0xEE});
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch, [&] {
        openxisf::detail::read_block_into(opened.source, block_at(opened, "/xisf/Image[2]"), opened.limits,
                                          destination);
    }));
    EXPECT_EQ(destination, std::vector<std::byte>(3));
}

TEST(data_block, a_block_is_read_into_memory_of_its_size)
{
    const unit opened = open_body(row_image(3, R"(location="inline:base64")", "YWJj"));
    for (const std::size_t size : {2U, 4U}) {
        std::vector<std::byte> destination(size);
        EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument, [&] {
            openxisf::detail::read_block_into(opened.source, block_at(opened, "/xisf/Image[1]"), opened.limits,
                                              destination);
        })) << size;
    }
    std::vector<std::byte> destination(3);
    openxisf::detail::read_block_into(opened.source, block_at(opened, "/xisf/Image[1]"), opened.limits, destination);
    EXPECT_EQ(destination, abc());
}

// ---------------------------------------------------------------------------------------------------------------------
// Limits

TEST(data_block, an_attached_block_larger_than_the_allocation_limit_is_not_read)
{
    const std::vector<std::byte> file =
        file_with_attachments(header_xml(row_image(3, R"(location="attachment:{0}")")), {abc()});
    const unit limited = open_internal(file, {.limits = {.max_allocation = 2}});
    EXPECT_TRUE(no_diagnostics(limited.diagnostics));
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::allocation_too_large,
                                              [&limited] { (void)stored_block(limited, "/xisf/Image[1]"); }));

    EXPECT_EQ(stored_block(open_internal(file, {.limits = {.max_allocation = 3}}), "/xisf/Image[1]"), abc());
    EXPECT_EQ(stored_block(open_internal(file, {.limits = {.max_allocation = 0}}), "/xisf/Image[1]"), abc());

    // Read into memory of the caller, the block allocates nothing.
    std::vector<std::byte> destination(3);
    openxisf::detail::read_block_into(limited.source, block_at(limited, "/xisf/Image[1]"), limited.limits, destination);
    EXPECT_EQ(destination, abc());
}

} // namespace
