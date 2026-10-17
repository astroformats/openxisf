// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §7: what a decoder does not recognize is ignored, and the rest of the unit stays readable. On constructed units.

#include <openxisf/error.h>
#include <openxisf/reader.h>

#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::reader;
using openxisf::severity;
using openxisf::test::header_xml;
using openxisf::test::image_xml;
using openxisf::test::keyword_xml;
using openxisf::test::open_header;
using openxisf::test::single_diagnostic;

struct misplaced
{
    std::string header{};
    std::string path{};
};

void expect_ignored_with_a_warning(const misplaced& element)
{
    const reader file = open_header(element.header);
    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::unknown_element, element.path))
        << element.header;
}

TEST(conformance_availability, an_element_that_a_core_element_cannot_contain_is_ignored_with_a_warning)
{
    for (const misplaced& element : {
             misplaced{.header = header_xml(image_xml({}, "<Metadata/>")), .path = "/xisf/Image[1]/Metadata[1]"},
             misplaced{.header = header_xml(image_xml({}, "<Image/>")), .path = "/xisf/Image[1]/Image[1]"},
             misplaced{.header = header_xml(image_xml({}, "<Field/>")), .path = "/xisf/Image[1]/Field[1]"},
             misplaced{.header =
                           header_xml(R"(<Property id="Test:Value" type="UInt8" value="1"><Property/></Property>)"),
                       .path = "/xisf/Property[1]/Property[1]"},
             misplaced{.header = header_xml(
                           R"(<FITSKeyword name="OBSERVER" value="'Test'" comment=""><Data/></FITSKeyword>)"),
                       .path = "/xisf/FITSKeyword[1]/Data[1]"},
             misplaced{.header =
                           header_xml(R"(<Table id="Test:Table"><Structure><Field id="n" type="UInt8"/></Structure>)"
                                      R"(<Row><Cell value="1"/><Field id="n" type="UInt8"/></Row></Table>)"),
                       .path = "/xisf/Table[1]/Row[1]/Field[1]"},
             misplaced{.header = header_xml(R"(<Structure uid="s"><Field id="n" type="UInt8"/><Cell/></Structure>)"),
                       .path = "/xisf/Structure[1]/Cell[1]"},
             misplaced{.header = header_xml(image_xml(R"(uid="i")") + R"(<Reference ref="i"><Data/></Reference>)"),
                       .path = "/xisf/Reference[1]/Data[1]"},
         }) {
        expect_ignored_with_a_warning(element);
    }
}

TEST(conformance_availability, the_metadata_element_holds_only_properties)
{
    // Spec §11.4: metadata properties are Property elements.
    expect_ignored_with_a_warning(
        {.header = header_xml({}, keyword_xml()), .path = "/xisf/Metadata[1]/FITSKeyword[1]"});
}

TEST(conformance_availability, the_parts_of_core_elements_are_not_allowed_at_the_top)
{
    for (const std::string_view name : {"Data", "Field", "Row", "Cell"}) {
        std::string element = "<";
        element += name;
        element += "/>";
        std::string path = "/xisf/";
        path += name;
        path += "[1]";
        expect_ignored_with_a_warning({.header = header_xml(element), .path = path});
    }
}

TEST(conformance_availability, an_extension_element_inside_a_core_element_is_ignored_with_a_warning)
{
    // Extension elements belong at the top of the header; core elements contain only elements of the specification.
    expect_ignored_with_a_warning(
        {.header = header_xml(image_xml({}, R"(<e:Note xmlns:e="urn:example"/>)")), .path = "/xisf/Image[1]/Note[1]"});
}

TEST(conformance_availability, what_an_ignored_element_contains_is_not_read)
{
    // A duplicate uid inside an ignored element is not a duplicate.
    expect_ignored_with_a_warning(
        {.header = header_xml(image_xml(R"(uid="a")", R"(<Unknown><Image uid="a"/></Unknown>)")),
         .path = "/xisf/Image[1]/Unknown[1]"});
}

// ---------------------------------------------------------------------------------------------------------------------
// Data blocks that this version cannot read

// Three images that each need a feature that OpenXISF does not have, and one that does not.
std::vector<std::byte> unit_with_unsupported_blocks()
{
    const auto image = [](std::string_view attributes) {
        return R"(<Image geometry="3:1:1" sampleFormat="UInt8" colorSpace="Gray" location="attachment:{0}" )" +
               std::string(attributes) + "/>";
    };
    return openxisf::test::file_with_attachments(
        header_xml(image(R"(compression="lzma:3")") + image(R"(checksum="md5:900150983cd24fb0d6963f7d28e17f72")") +
                   image({})),
        {openxisf::test::bytes("abc")});
}

TEST(conformance_availability, an_unsupported_codec_or_checksum_algorithm_leaves_the_rest_of_the_unit_readable)
{
    const reader file = openxisf::test::open_unit(unit_with_unsupported_blocks());

    const std::span<const openxisf::diagnostic> diagnostics = file.diagnostics();
    ASSERT_EQ(diagnostics.size(), 2U) << openxisf::test::describe(diagnostics);
    EXPECT_EQ(diagnostics[0].severity, severity::error);
    EXPECT_EQ(diagnostics[0].code, errc::unsupported_compression);
    EXPECT_EQ(diagnostics[0].context.element, "/xisf/Image[1]");
    EXPECT_EQ(diagnostics[1].severity, severity::error);
    EXPECT_EQ(diagnostics[1].code, errc::unsupported_checksum);
    EXPECT_EQ(diagnostics[1].context.element, "/xisf/Image[2]");
    // The images are listed, and the pixels of those that need what OpenXISF does not have cannot be read.
    ASSERT_EQ(file.images().size(), 3U);
    EXPECT_TRUE(openxisf::test::throws<openxisf::unsupported_error>(errc::unsupported_compression,
                                                                    [&file] { (void)file.read_pixels(0); }));
    EXPECT_TRUE(openxisf::test::throws<openxisf::unsupported_error>(errc::unsupported_checksum,
                                                                    [&file] { (void)file.read_pixels(1); }));
    EXPECT_EQ(file.read_pixels(2), openxisf::test::bytes("abc"));
}

TEST(conformance_availability, strict_reading_fails_on_an_unsupported_feature)
{
    EXPECT_TRUE(openxisf::test::throws<openxisf::unsupported_error>(errc::unsupported_compression, [] {
        (void)openxisf::test::open_unit(unit_with_unsupported_blocks(), {.strict = true});
    }));
}

} // namespace
