// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §9.5: a detached XML signature after the root element, and spec §10.5: the checksums that a signed unit needs,
// on constructed units. Signatures are read and returned, never verified, and never prevent reading a unit.

#include <openxisf/error.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>
#include <openxisf/writer.h>

#include "core/diagnostic_log.h"
#include "model/header.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/temp_directory.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openxisf::diagnostic;
using openxisf::errc;
using openxisf::reader;
using openxisf::severity;
using openxisf::signature_status;
using openxisf::test::bytes;
using openxisf::test::header_xml;
using openxisf::test::open_header;
using openxisf::test::open_unit;
using openxisf::test::single_diagnostic;
using openxisf::test::throws;

// The form of the example of spec §9.5, shortened. It names the root element by the id of signed_header().
constexpr std::string_view signature = R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#">
<SignedInfo>
   <CanonicalizationMethod Algorithm="http://www.w3.org/2006/12/xml-c14n11"/>
   <SignatureMethod Algorithm="http://www.w3.org/2000/09/xmldsig#rsa-sha1"/>
   <Reference URI="#XISFRootElement">
      <DigestMethod Algorithm="http://www.w3.org/2000/09/xmldsig#sha1"/>
      <DigestValue>bm90IGEgZGlnZXN0</DigestValue>
   </Reference>
</SignedInfo>
<SignatureValue>bm90IGEgc2lnbmF0dXJl</SignatureValue>
</Signature>)";

constexpr std::string_view sha1_of_abc = "a9993e364706816aba3e25717850c26c9cd0d89d";

// A header whose root element has the id attribute by which a signature names it, as in the example of spec §9.5.
std::string signed_header(std::string_view body = {})
{
    std::string header = header_xml(body);
    constexpr std::string_view start = R"(<xisf version="1.0")";
    header.insert(header.find(start) + start.size(), R"( id="XISFRootElement")");
    return header;
}

// The root element of a unit, as written in it.
std::string_view root_element(std::string_view unit)
{
    constexpr std::string_view end_tag = "</xisf>";
    const std::size_t start = unit.find("<xisf ");
    return unit.substr(start, unit.find(end_tag) + end_tag.size() - start);
}

// A gray image of the 3 pixels "abc", stored in an embedded block.
std::string embedded_image()
{
    return R"(<Image geometry="3:1:1" sampleFormat="UInt8" colorSpace="Gray" location="embedded">)"
           R"(<Data encoding="base64">YWJj</Data></Image>)";
}

// Success when the diagnostics are the info that the unit is signed, about the signature at path, then one warning with
// code about element.
testing::AssertionResult signed_with_warning(std::span<const diagnostic> found, errc code, std::string_view element,
                                             std::string_view path = "/Signature[1]")
{
    if (found.size() == 2 && found[0].severity == severity::info && found[0].code == errc::signature_not_verified &&
        found[0].context.element == path && found[1].severity == severity::warning && found[1].code == code &&
        found[1].context.element == element) {
        return testing::AssertionSuccess();
    }
    return testing::AssertionFailure() << "expected the signature at '" << path << "' and a warning of code "
                                       << static_cast<int>(code) << " about '" << element << "', got:\n"
                                       << openxisf::test::describe(found);
}

// Where opening a monolithic file that holds header fails with invalid_data_error.
openxisf::error_context refusal(std::string_view header)
{
    try {
        (void)open_header(header);
    } catch (const openxisf::invalid_data_error& failure) {
        return failure.context();
    }
    ADD_FAILURE() << "the header is not refused";
    return {};
}

// The signature as parse_header() isolates it from a header.
std::optional<std::string> isolated_signature(std::string_view header)
{
    openxisf::detail::diagnostic_log log(false);
    std::optional<openxisf::detail::header_signature> found =
        openxisf::detail::parse_header(header, 16, {}, log).signature;
    if (!found) {
        return std::nullopt;
    }
    return found->element;
}

// A monolithic file signed as spec §9.5 and §10.5 require: an image and a property in attached blocks with checksums,
// and a property in an inline block, covered by the signature itself. Comments follow the signature.
std::vector<std::byte> signed_file()
{
    const std::string checksum = R"(checksum="sha1:)" + std::string(sha1_of_abc) + "\"";
    const std::string body =
        R"(<Image geometry="3:1:1" sampleFormat="UInt8" colorSpace="Gray" location="attachment:{0}" )" + checksum +
        "/>" + R"(<Property id="Test:Attached" type="ByteArray" length="3" location="attachment:{1}" )" + checksum +
        "/>" + R"(<Property id="Test:Inline" type="ByteArray" length="3" location="inline:base64">YWJj</Property>)";
    return openxisf::test::file_with_attachments(signed_header(body) + "\n" + std::string(signature) +
                                                     "\n<!-- after the signature -->\n",
                                                 {bytes("abc"), bytes("abc")});
}

std::string value_text(const reader& file, std::string_view id)
{
    const openxisf::property* found = file.properties().find(id);
    if (found == nullptr) {
        ADD_FAILURE() << "no property " << id;
        return {};
    }
    return openxisf::test::text(std::as_bytes(found->value.elements<std::uint8_t>()));
}

TEST(conformance_signature, a_signed_unit_is_read_and_its_signature_is_returned_unchanged_and_not_verified)
{
    const std::vector<std::byte> unit = signed_file();
    const reader file = open_unit(unit, {.strict = true});

    EXPECT_EQ(file.signature(), signature_status::not_verified);
    EXPECT_EQ(file.signature_xml(), signature);
    EXPECT_EQ(file.signed_xml(), root_element(openxisf::test::text(unit)));
    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::info, errc::signature_not_verified, "/Signature[1]"));
    EXPECT_EQ(file.read_pixels(0), bytes("abc"));
    EXPECT_EQ(value_text(file, "Test:Attached"), "abc");
    EXPECT_EQ(value_text(file, "Test:Inline"), "abc");
}

TEST(conformance_signature, a_unit_that_xml_signature_tooling_signed_is_read_without_warnings)
{
    // tests/data/signature/signed.xisf, written by make_signed.py: signxml signed it with RSA and SHA-256 over
    // Canonical XML 1.1 of the root element, and verified what it wrote.
    const std::string path = std::string(OPENXISF_TEST_DATA_DIR) + "/signature/signed.xisf";
    const std::string unit = openxisf::test::read_file(openxisf::test::path_of(path));
    const reader file(path, {.strict = true});

    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::info, errc::signature_not_verified, "/Signature[1]"));
    EXPECT_EQ(file.signed_xml(), root_element(unit));
    constexpr std::string_view end_tag = "</ds:Signature>";
    const std::size_t start = unit.find("<ds:Signature ");
    EXPECT_EQ(file.signature_xml(), std::string_view(unit).substr(start, unit.find(end_tag) + end_tag.size() - start));
    // The pixels, (x + 37y) mod 256, in an attached block with its checksum.
    std::vector<std::uint8_t> pixels(std::size_t{37} * 23);
    for (std::size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<std::uint8_t>(i % 256);
    }
    EXPECT_EQ(file.read_pixels<std::uint8_t>(0), pixels);
}

TEST(conformance_signature, an_unsigned_unit_has_no_signature_to_return)
{
    const reader file = open_header(header_xml());
    EXPECT_EQ(file.signature(), signature_status::none);
    EXPECT_TRUE(file.signature_xml().empty());
    EXPECT_TRUE(file.signed_xml().empty());
}

TEST(conformance_signature, the_signature_is_kept_exactly_as_written)
{
    // Markup in comments, CDATA sections, processing instructions and attribute values does not end the signature,
    // and what follows it is not part of it.
    const std::string tricky = R"(<ds:Signature xmlns:ds="http://www.w3.org/2000/09/xmldsig#" a="/>" b='>'>)"
                               R"(<!-- > </ds:Signature> --><ds:SignatureValue><![CDATA[ > </ds:Signature>]]>)"
                               R"(</ds:SignatureValue><?pi /> </ds:Signature>?><ds:Object/>)"
                               "\r\n\t</ds:Signature>";
    EXPECT_EQ(isolated_signature(header_xml() + tricky + "\n<!-- after -->\n"), tricky);
}

struct malformed_case
{
    std::string_view name{};
    /// The signature as written, which ends the header.
    std::string_view signature{};
};

constexpr std::array malformed_signatures{
    malformed_case{.name = "a misspelled end tag",
                   .signature = R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#"><SignedInfo/>)"
                                R"(<SignatureValue>bm90</SignatureValu></Signature>)"},
    malformed_case{.name = "no end tag",
                   .signature = R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#"><SignedInfo>)"},
    malformed_case{.name = "an attribute value without quotes",
                   .signature = R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#"><SignedInfo Id=a/>)"
                                R"(</Signature>)"},
    malformed_case{.name = "a reference to U+0000",
                   .signature = R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#"><SignedInfo/>)"
                                R"(<SignatureValue>&#0;</SignatureValue></Signature>)"},
    malformed_case{.name = "an attribute given twice",
                   .signature = R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#"><SignedInfo Id="a" Id="b"/>)"
                                R"(</Signature>)"},
    // Its path leaves out the prefix, as every path does.
    malformed_case{.name = "a prefix",
                   .signature = R"(<ds:Signature xmlns:ds="http://www.w3.org/2000/09/xmldsig#"><ds:SignedInfo>)"
                                R"(</ds:Signature>)"},
};

TEST(conformance_signature, a_signature_that_is_not_well_formed_is_returned_as_written_and_reading_goes_on)
{
    for (const malformed_case& tested : malformed_signatures) {
        const std::string header = signed_header(embedded_image()) + "\n" + std::string(tested.signature);
        const reader file = open_header(header, {.strict = true});

        EXPECT_EQ(file.signature(), signature_status::not_verified) << tested.name;
        EXPECT_EQ(file.signature_xml(), tested.signature) << tested.name;
        EXPECT_EQ(file.signed_xml(), root_element(header)) << tested.name;
        EXPECT_TRUE(signed_with_warning(file.diagnostics(), errc::invalid_signature, "/Signature[1]")) << tested.name;
        ASSERT_EQ(file.diagnostics().size(), 2U) << tested.name;
        EXPECT_TRUE(file.diagnostics()[1].context.offset.has_value()) << tested.name;
        EXPECT_EQ(file.read_pixels(0), bytes("abc")) << tested.name;
    }
}

TEST(conformance_signature, a_signature_that_does_not_start_as_an_xml_signature_does_is_a_warning)
{
    // XML Signature §4.1: SignedInfo, then SignatureValue, both in the namespace of XML signatures.
    for (const std::string_view content :
         {"", "<SignedInfo/>", "<SignedInfo/><KeyInfo/>", "<SignatureValue/><SignedInfo/>",
          R"(<SignedInfo xmlns="urn:example"/><SignatureValue/>)"}) {
        const std::string written =
            R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#">)" + std::string(content) + "</Signature>";
        const reader file = open_header(signed_header() + written);
        EXPECT_EQ(file.signature(), signature_status::not_verified) << written;
        EXPECT_EQ(file.signature_xml(), written);
        EXPECT_TRUE(signed_with_warning(file.diagnostics(), errc::invalid_signature, "/Signature[1]")) << written;
    }
}

TEST(conformance_signature, the_signature_names_the_root_element_by_its_id_attribute)
{
    const auto with_reference = [](std::string_view reference) {
        return R"(<ds:Signature xmlns:ds="http://www.w3.org/2000/09/xmldsig#"><ds:SignedInfo>)" +
               std::string(reference) + R"(</ds:SignedInfo><ds:SignatureValue/></ds:Signature>)";
    };
    // A bare name, and the XPointers of XML Signature §4.4.3.3.
    for (const std::string_view reference :
         {R"(<ds:Reference URI="#XISFRootElement"/>)", R"x(<ds:Reference URI="#xpointer(id('XISFRootElement'))"/>)x",
          R"x(<ds:Reference URI='#xpointer(id("XISFRootElement"))'/>)x",
          R"(<ds:Reference URI="#Object"/><ds:Reference URI="#XISFRootElement"/>)",
          R"(<ds:Reference URI="#XISFRootElement"/><ds:Reference URI="#Object"/>)"}) {
        const reader file = open_header(signed_header() + with_reference(reference));
        EXPECT_TRUE(
            single_diagnostic(file.diagnostics(), severity::info, errc::signature_not_verified, "/Signature[1]"))
            << reference;
    }

    // Another element, the whole document, a Reference element of another namespace, no Reference element at all.
    for (const std::string_view reference :
         {R"(<ds:Reference URI="#Object"/>)", R"(<ds:Reference URI=""/>)",
          R"(<Reference xmlns="urn:example" URI="#XISFRootElement"/>)", R"(<ds:Reference/>)", ""}) {
        const reader file = open_header(signed_header() + with_reference(reference));
        EXPECT_TRUE(signed_with_warning(file.diagnostics(), errc::invalid_signature, "/Signature[1]/SignedInfo[1]",
                                        "/Signature[1]"))
            << reference;
    }

    // A root element without an id cannot be named.
    const reader file = open_header(header_xml() + std::string(signature));
    EXPECT_TRUE(signed_with_warning(file.diagnostics(), errc::invalid_signature, "/xisf"));
    EXPECT_EQ(file.diagnostics()[1].context.attribute, "id");
}

struct trailing_case
{
    std::string_view name{};
    /// What follows the signature.
    std::string_view content{};
    /// The diagnostic about it, after the info about the signature, if there is one.
    std::optional<errc> code{};
    std::string_view element{};
};

constexpr std::array trailing_content{
    trailing_case{.name = "comments, processing instructions and white space",
                  .content = "\n<!-- a comment -->\r\n<?instruction data?>\n"},
    trailing_case{.name = "text", .content = "\n text \n", .code = errc::invalid_xml},
    trailing_case{.name = "an extension element",
                  .content = R"(<Extension xmlns="urn:example"/>)",
                  .code = errc::unknown_element,
                  .element = "/Extension[1]"},
    trailing_case{.name = "an element without its end tag",
                  .content = R"(<Extension xmlns="urn:example"><Inner/>)",
                  .code = errc::invalid_xml,
                  .element = "/Extension[1]"},
    trailing_case{.name = "an end tag without its element", .content = "</Extension>", .code = errc::invalid_xml},
    trailing_case{.name = "a comment without its end", .content = "<!-- never ends", .code = errc::invalid_xml},
    trailing_case{.name = "a reference to U+0000 in the text before an element",
                  .content = R"( &#0; <Extension xmlns="urn:example"/>)",
                  .code = errc::invalid_xml},
    trailing_case{.name = "a comment before an element without its end tag",
                  .content = R"(<!-- a comment --><Extension xmlns="urn:example">)",
                  .code = errc::invalid_xml,
                  .element = "/Extension[1]"},
    trailing_case{.name = "a second signature",
                  .content = R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#"/>)",
                  .code = errc::unknown_element,
                  .element = "/Signature[2]"},
    trailing_case{.name = "a second signature that is not well-formed",
                  .content = R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#">)",
                  .code = errc::invalid_xml,
                  .element = "/Signature[2]"},
};

TEST(conformance_signature, what_follows_the_signature_is_not_part_of_it)
{
    for (const trailing_case& tested : trailing_content) {
        const reader file =
            open_header(signed_header(embedded_image()) + std::string(signature) + std::string(tested.content));
        EXPECT_EQ(file.signature_xml(), signature) << tested.name;
        if (tested.code) {
            EXPECT_TRUE(signed_with_warning(file.diagnostics(), *tested.code, tested.element)) << tested.name;
        } else {
            EXPECT_TRUE(
                single_diagnostic(file.diagnostics(), severity::info, errc::signature_not_verified, "/Signature[1]"))
                << tested.name;
        }
        EXPECT_EQ(file.read_pixels(0), bytes("abc")) << tested.name;
    }
}

TEST(conformance_signature, what_follows_an_element_that_is_not_well_formed_is_not_read)
{
    const std::string_view malformed = malformed_signatures.front().signature;
    const reader after_signature =
        open_header(signed_header() + std::string(malformed) + R"(<Extension xmlns="urn:x"/>)");
    EXPECT_EQ(after_signature.signature_xml(), malformed);
    EXPECT_TRUE(signed_with_warning(after_signature.diagnostics(), errc::invalid_signature, "/Signature[1]"));

    // The element without its end tag extends to the end of the header, the signature with it.
    const reader file =
        open_header(signed_header(embedded_image()) + R"(<Extension xmlns="urn:example">)" + std::string(signature));
    EXPECT_EQ(file.signature(), signature_status::none);
    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_xml, "/Extension[1]"));
    EXPECT_EQ(file.read_pixels(0), bytes("abc"));
}

TEST(conformance_signature, a_signature_whose_start_tag_is_not_well_formed_is_not_a_signature)
{
    // The namespace that makes an element a signature is declared in its start tag, which cannot be read then: the
    // element is where the header stops being well-formed XML, and the unit is not signed.
    for (const std::string_view start_tag : {R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#" Id=a>)",
                                             R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#" xmlns="">)"}) {
        const reader file =
            open_header(signed_header(embedded_image()) + std::string(start_tag) + "<SignedInfo/></Signature>");
        EXPECT_EQ(file.signature(), signature_status::none) << start_tag;
        EXPECT_TRUE(file.signature_xml().empty()) << start_tag;
        EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_xml, "/Signature[1]"))
            << start_tag;
        EXPECT_EQ(file.read_pixels(0), bytes("abc")) << start_tag;
    }
}

TEST(conformance_signature, a_signature_of_another_namespace_is_not_a_signature)
{
    // Another namespace, and none, as in the signature that PixInsight 1.9.5 adds to an XML document after its root
    // element (Security.generateXMLSignature()): an Ed25519 signature of its own form.
    for (const std::string_view other :
         {R"(<Signature xmlns="urn:example"/>)",
          R"(<Signature developerId="Example" timestamp="2026-10-06T00:41:41.089Z" encoding="Base64">)"
          R"(m+jZJxt+Wam3CDLuv6CThUeP2GjK5QyuCi+cYyfGcfW8aVlEHsTHG5qNeefdqGYnmZU/+h+t86pKlO0G7V+zCA==</Signature>)"}) {
        const reader file = open_header(signed_header() + std::string(other));
        EXPECT_EQ(file.signature(), signature_status::none) << other;
        EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::unknown_element, "/Signature[1]"))
            << other;
    }
}

TEST(conformance_signature, the_root_element_must_be_well_formed_whatever_follows_it)
{
    // The image has no end tag. The error is that of the root element, which follows it or not.
    const std::string broken = signed_header(R"(<Image geometry="1:1:1" sampleFormat="UInt8" location="embedded">)");
    for (const std::string_view after : {signature, std::string_view(R"(<Signature xmlns="urn:x"><Open>)")}) {
        EXPECT_TRUE(throws<openxisf::invalid_data_error>(errc::invalid_xml,
                                                         [&] { (void)open_header(broken + std::string(after)); }));
        // The error is that of the root element: at the image that has no end tag, or after it.
        const std::uint64_t image = 16 + broken.find("<Image");
        EXPECT_GE(refusal(broken + std::string(after)).offset.value_or(0), image);
        EXPECT_LE(refusal(broken + std::string(after)).offset.value_or(0), 16 + broken.size());
    }

    // A second root element is refused, well-formed or not.
    for (const std::string_view after : {"<xisf version=\"1.0\"/>", "<xisf version=\"1.0\"><Open>"}) {
        const std::string header = signed_header() + std::string(signature) + std::string(after);
        EXPECT_TRUE(
            throws<openxisf::invalid_data_error>(errc::invalid_root_element, [&] { (void)open_header(header); }));
        EXPECT_EQ(refusal(header).element, "/xisf[2]");
    }
}

TEST(conformance_signature, a_signature_before_the_root_element_takes_its_place)
{
    const std::string header = header_xml();
    const std::string misplaced = header.substr(0, openxisf::test::xml_declaration.size()) + std::string(signature) +
                                  header.substr(openxisf::test::xml_declaration.size());
    EXPECT_TRUE(throws<openxisf::invalid_data_error>(errc::invalid_root_element,
                                                     [&misplaced] { (void)open_header(misplaced); }));
}

TEST(conformance_signature, a_block_stored_outside_the_header_of_a_signed_unit_needs_a_checksum)
{
    // Spec §10.5: attached and external blocks. Inline and embedded blocks are in the header, which the signature
    // covers.
    const std::string inline_property = R"(<Property id="Test:Inline" type="ByteArray" length="3" )"
                                        R"(location="inline:base64">YWJj</Property>)";
    const reader attached = open_unit(openxisf::test::file_with_attachments(
        signed_header(R"(<Image geometry="3:1:1" sampleFormat="UInt8" colorSpace="Gray" )"
                      R"(location="attachment:{0}"/>)" +
                      embedded_image() + inline_property) +
            std::string(signature),
        {bytes("abc")}));
    EXPECT_TRUE(signed_with_warning(attached.diagnostics(), errc::missing_checksum, "/xisf/Image[1]"));
    EXPECT_EQ(attached.read_pixels(0), bytes("abc"));

    const reader external = open_unit(bytes(signed_header(R"(<Property id="Test:External" type="ByteArray" length="3" )"
                                                          R"(location="path(@header_dir/unit.xisb):1"/>)") +
                                            std::string(signature)),
                                      {.resolver = openxisf::test::memory_resolver(
                                           {{"unit.xisb", openxisf::test::blocks_file_of({bytes("abc")}, {1})}})});
    EXPECT_TRUE(signed_with_warning(external.diagnostics(), errc::missing_checksum, "/xisf/Property[1]"));
    EXPECT_EQ(value_text(external, "Test:External"), "abc");

    // The same blocks in a unit that is not signed need none.
    const reader unsigned_unit =
        open_unit(openxisf::test::file_with_attachments(header_xml(R"(<Image geometry="3:1:1" sampleFormat="UInt8" )"
                                                                   R"(colorSpace="Gray" location="attachment:{0}"/>)"),
                                                        {bytes("abc")}));
    EXPECT_TRUE(openxisf::test::no_diagnostics(unsigned_unit.diagnostics()));
}

TEST(conformance_signature, a_signed_unit_written_again_is_not_signed)
{
    // The writer writes a new header, which the signature does not cover.
    const reader file = open_unit(signed_file());
    const std::vector<std::byte> pixels = file.read_pixels(0);
    openxisf::writer output({.creator_application = "OpenXISF tests 1.0"});
    (void)output.add_image(file.image(0), std::span<const std::byte>(pixels));
    openxisf::memory_sink sink;
    output.save(sink);

    const reader written = open_unit(sink.release());
    EXPECT_EQ(written.signature(), signature_status::none);
    EXPECT_TRUE(written.signature_xml().empty());
    EXPECT_EQ(written.read_pixels(0), pixels);
}

} // namespace
