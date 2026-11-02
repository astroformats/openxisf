// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §10.5 and §10.6.2: the checksums of data blocks, verified before a block is used. On constructed units whose
// blocks hold "abc", with the digests of the NIST examples.

#include <openxisf/error.h>
#include <openxisf/reader.h>

#include "model/unit.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::severity;
using openxisf::detail::unit;
using openxisf::test::bytes;
using openxisf::test::file_with_attachments;
using openxisf::test::header_xml;
using openxisf::test::no_diagnostics;
using openxisf::test::open_internal;
using openxisf::test::single_diagnostic;
using openxisf::test::stored_block;
using openxisf::test::throws;

std::vector<std::byte> abc()
{
    return bytes("abc");
}

constexpr std::string_view sha1_of_abc = "a9993e364706816aba3e25717850c26c9cd0d89d";

struct algorithm_name
{
    std::string_view name{};
    std::string_view digest_of_abc{};
};

// Every name and alternate name of spec §10.5, Table 9.
constexpr std::array algorithm_names{
    algorithm_name{.name = "sha-1", .digest_of_abc = sha1_of_abc},
    algorithm_name{.name = "sha1", .digest_of_abc = sha1_of_abc},
    algorithm_name{.name = "sha-256",
                   .digest_of_abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    algorithm_name{.name = "sha256",
                   .digest_of_abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
    algorithm_name{.name = "sha-512",
                   .digest_of_abc = "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                                    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
    algorithm_name{.name = "sha512",
                   .digest_of_abc = "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
                                    "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f"},
    algorithm_name{.name = "sha3-256",
                   .digest_of_abc = "3a985da74fe225b2045c172d6bd390bd855f086e3e9d525b46bfe24511431532"},
    algorithm_name{.name = "sha3-512",
                   .digest_of_abc = "b751850b1a57168a5693cd924b6b096e08f621827444f70d884f5d0240d2712e"
                                    "10e116e9192af3c91a7ec57647e3934057340b4cf408d5a56592f8274eec53f0"},
};

std::string checksum_attribute(std::string_view algorithm, std::string_view digest)
{
    return "checksum=\"" + std::string(algorithm) + ":" + std::string(digest) + "\"";
}

// A ByteArray property of 3 bytes, with the given attributes and content.
std::string property(std::string_view id, std::string_view attributes, std::string_view content = {})
{
    return R"(<Property id="Test:)" + std::string(id) + R"(" type="ByteArray" length="3" )" + std::string(attributes) +
           ">" + std::string(content) + "</Property>";
}

// A gray 8-bit image of 3 pixels in an embedded block, whose Data element has the given attributes.
std::string embedded_image(std::string_view data_attributes, std::string_view text = "YWJj")
{
    return R"(<Image geometry="3:1:1" sampleFormat="UInt8" colorSpace="Gray" location="embedded"><Data encoding="base64" )" +
           std::string(data_attributes) + ">" + std::string(text) + "</Data></Image>";
}

unit open_with_attachment(std::string_view body, openxisf::read_options options = {})
{
    return open_internal(file_with_attachments(header_xml(body), {abc()}), std::move(options));
}

TEST(conformance_checksum, every_algorithm_verifies_inline_embedded_and_attached_blocks)
{
    for (const algorithm_name& algorithm : algorithm_names) {
        const std::string checksum = checksum_attribute(algorithm.name, algorithm.digest_of_abc);
        const unit opened = open_with_attachment(property("Inline", R"(location="inline:base64" )" + checksum, "YWJj") +
                                                 embedded_image(checksum) +
                                                 property("Attached", R"(location="attachment:{0}" )" + checksum));

        EXPECT_TRUE(no_diagnostics(opened.diagnostics)) << algorithm.name;
        for (const std::string_view path : {"/xisf/Property[1]", "/xisf/Image[1]", "/xisf/Property[2]"}) {
            EXPECT_EQ(stored_block(opened, path), abc()) << algorithm.name << " " << path;
        }
    }
}

TEST(conformance_checksum, the_digest_of_an_inline_block_is_computed_over_its_decoded_bytes)
{
    // "abc" written in hexadecimal: the digest of "abc" matches, and that of the text "616263" does not.
    const unit decoded = open_with_attachment(
        property("Value", R"(location="inline:hex" )" + checksum_attribute("sha1", sha1_of_abc), "616263"));
    EXPECT_TRUE(no_diagnostics(decoded.diagnostics));

    const unit text = open_with_attachment(property(
        "Value", R"(location="inline:hex" checksum="sha1:c3d8b80f92eaf79c90a1b99f37a62c84b1494a38")", "616263"));
    EXPECT_TRUE(single_diagnostic(text.diagnostics, severity::error, errc::checksum_mismatch, "/xisf/Property[1]"));
}

TEST(conformance_checksum, an_inline_or_embedded_block_that_fails_its_checksum_is_unavailable)
{
    // The digest of "abd": one bit differs.
    const std::string wrong = checksum_attribute("sha1", "cb4cc28df0fdbe0ecf9d9662e294b118092a5735");
    const unit opened =
        open_with_attachment(property("Inline", R"(location="inline:base64" )" + wrong, "YWJj") +
                             embedded_image(wrong) + property("Valid", R"(location="inline:base64")", "YWJj"));

    ASSERT_EQ(opened.diagnostics.size(), 2U);
    EXPECT_EQ(opened.diagnostics[0].code, errc::checksum_mismatch);
    EXPECT_EQ(opened.diagnostics[0].context.element, "/xisf/Property[1]");
    EXPECT_EQ(opened.diagnostics[1].code, errc::checksum_mismatch);
    EXPECT_EQ(opened.diagnostics[1].context.element, "/xisf/Image[1]/Data[1]");
    EXPECT_EQ(opened.diagnostics[1].context.attribute, "checksum");
    for (const std::string_view path : {"/xisf/Property[1]", "/xisf/Image[1]"}) {
        EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch, [&opened, path] {
            (void)stored_block(opened, path);
        })) << path;
    }
    EXPECT_EQ(stored_block(opened, "/xisf/Property[2]"), abc());
}

TEST(conformance_checksum, strict_reading_fails_on_a_checksum_mismatch)
{
    const std::vector<std::byte> unit = openxisf::test::monolithic_file(
        header_xml(embedded_image(checksum_attribute("sha1", "cb4cc28df0fdbe0ecf9d9662e294b118092a5735"))));
    EXPECT_TRUE(throws<openxisf::integrity_error>(
        errc::checksum_mismatch, [&unit] { (void)openxisf::test::open_unit(unit, {.strict = true}); }));
}

TEST(conformance_checksum, an_attached_block_is_verified_before_it_is_returned)
{
    // Spec §10.5 allows verification on demand: the pixels of an image are read only when they are asked for.
    const unit opened = open_with_attachment(
        R"(<Image geometry="3:1:1" sampleFormat="UInt8" colorSpace="Gray" location="attachment:{0}" )" +
        checksum_attribute("sha1", "cb4cc28df0fdbe0ecf9d9662e294b118092a5735") + "/>");
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    try {
        (void)stored_block(opened, "/xisf/Image[1]");
        ADD_FAILURE() << "the block was returned";
    } catch (const openxisf::integrity_error& failure) {
        EXPECT_EQ(failure.code(), errc::checksum_mismatch);
        EXPECT_EQ(failure.context().element, "/xisf/Image[1]");
        EXPECT_EQ(failure.context().offset, 4096U);
    }
}

TEST(conformance_checksum, the_attached_block_of_a_property_is_verified_when_the_unit_opens)
{
    // Property values are loaded when the unit opens, so a block that fails its checksum makes the property
    // unavailable then.
    const unit opened = open_with_attachment(
        property("Attached", R"(location="attachment:{0}" )" +
                                 checksum_attribute("sha1", "cb4cc28df0fdbe0ecf9d9662e294b118092a5735")));
    EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::error, errc::checksum_mismatch, "/xisf/Property[1]"));
    EXPECT_EQ(opened.diagnostics.front().context.offset, 4096U);
    EXPECT_TRUE(opened.properties.standalone.empty());
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch, [] {
        (void)open_with_attachment(
            property("Attached", R"(location="attachment:{0}" )" + checksum_attribute("sha1", std::string(40, '0'))),
            {.strict = true});
    }));
}

TEST(conformance_checksum, the_digest_of_a_compressed_block_is_computed_over_the_compressed_bytes)
{
    // Spec §10.6.2. The stored bytes "abc" stand for the compressed pixels of a row of 100. A digest that fails is
    // detected when the stored bytes are read, so they never reach a decompressor.
    const auto image = [](std::string_view attributes, std::string_view content = {}) {
        return R"(<Image geometry="100:1:1" sampleFormat="UInt8" colorSpace="Gray" )" + std::string(attributes) + ">" +
               std::string(content) + "</Image>";
    };
    const std::string compressed = R"(compression="zlib:100" )";
    const std::string wrong = checksum_attribute("sha1", "cb4cc28df0fdbe0ecf9d9662e294b118092a5735");
    const unit opened = open_with_attachment(
        image(R"(location="embedded")",
              R"(<Data encoding="base64" )" + compressed + checksum_attribute("sha1", sha1_of_abc) + ">YWJj</Data>") +
        image(R"(location="attachment:{0}" )" + compressed + checksum_attribute("sha1", sha1_of_abc)) +
        image(R"(location="attachment:{0}" )" + compressed + wrong));

    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    EXPECT_EQ(stored_block(opened, "/xisf/Image[1]"), abc());
    EXPECT_EQ(stored_block(opened, "/xisf/Image[2]"), abc());
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch,
                                                  [&opened] { (void)stored_block(opened, "/xisf/Image[3]"); }));
}

TEST(conformance_checksum, a_digest_in_uppercase_digits_is_accepted_with_a_warning)
{
    // Spec §10.5 asks for lowercase digits; uppercase ones name the same digest.
    const unit opened = open_with_attachment(property(
        "Value", R"(location="inline:base64" checksum="sha1:A9993E364706816ABA3E25717850C26C9CD0D89D")", "YWJj"));
    EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_checksum, "/xisf/Property[1]"));
    EXPECT_EQ(stored_block(opened, "/xisf/Property[1]"), abc());
}

TEST(conformance_checksum, an_algorithm_name_in_another_case_is_accepted_with_a_warning)
{
    // Spec §10.5 names the algorithms in lowercase; another case names the same algorithm.
    for (const std::string_view algorithm : {"SHA1", "Sha-1"}) {
        const unit opened =
            open_with_attachment(property("Value",
                                          R"(location="inline:base64" checksum=")" + std::string(algorithm) +
                                              R"(:a9993e364706816aba3e25717850c26c9cd0d89d")",
                                          "YWJj"));
        EXPECT_TRUE(
            single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_checksum, "/xisf/Property[1]"))
            << algorithm;
        EXPECT_EQ(stored_block(opened, "/xisf/Property[1]"), abc()) << algorithm;
    }

    // A name that is no algorithm in any case is reported as written, with no warning about its case or its digits.
    const unit unknown = open_with_attachment(
        property("Value", R"(location="inline:base64" checksum="MD5:900150983CD24FB0D6963F7D28E17F72")", "YWJj"));
    ASSERT_TRUE(
        single_diagnostic(unknown.diagnostics, severity::error, errc::unsupported_checksum, "/xisf/Property[1]"));
    EXPECT_NE(unknown.diagnostics.front().message.find("'MD5'"), std::string::npos)
        << unknown.diagnostics.front().message;
}

} // namespace
