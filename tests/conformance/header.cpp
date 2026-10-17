// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §9.5 (the XML declaration, the root element, namespaces and extension elements) and spec §11 (core element
// names and unique element identifiers), on constructed units.

#include <openxisf/error.h>
#include <openxisf/reader.h>

#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using openxisf::errc;
using openxisf::invalid_data_error;
using openxisf::reader;
using openxisf::severity;
using openxisf::test::header_xml;
using openxisf::test::image_xml;
using openxisf::test::keyword_xml;
using openxisf::test::mandatory_metadata;
using openxisf::test::no_diagnostics;
using openxisf::test::open_header;
using openxisf::test::root_start_tag;
using openxisf::test::single_diagnostic;
using openxisf::test::throws;
using openxisf::test::xml_declaration;

// A header with the given XML declaration and root element, which holds a Metadata element and body.
std::string header_with(std::string_view declaration, std::string_view root_start, std::string_view body = {},
                        std::string_view root_end = "</xisf>")
{
    return std::string(declaration) + std::string(root_start) + "<Metadata>" + std::string(mandatory_metadata) +
           "</Metadata>" + std::string(body) + std::string(root_end);
}

testing::AssertionResult refuses(errc code, const std::string& header)
{
    return throws<invalid_data_error>(code, [&header] { (void)open_header(header); });
}

TEST(conformance_header, a_minimal_unit_opens_without_diagnostics)
{
    const reader file = open_header(header_xml());

    EXPECT_EQ(file.storage(), openxisf::unit_storage::monolithic);
    EXPECT_EQ(file.signature(), openxisf::signature_status::none);
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
}

// ---------------------------------------------------------------------------------------------------------------------
// The XML declaration

TEST(conformance_header, a_header_without_xml_declaration_is_read_with_a_warning)
{
    const reader file = open_header(header_with("", root_start_tag));

    ASSERT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_xml_declaration));
    EXPECT_EQ(file.diagnostics().front().context.offset, 16U);
}

TEST(conformance_header, another_xml_declaration_is_read_with_a_warning)
{
    for (const std::string_view declaration : {
             R"(<?xml version="1.0"?>)",
             R"(<?xml version='1.0' encoding='UTF-8'?>)",
             R"(<?xml version="1.0" encoding="utf-8"?>)",
             R"(<?xml version="1.0"  encoding="UTF-8"?>)",
             R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>)",
             R"(<?xml version="1.1" encoding="UTF-8"?>)",
             R"(<?xml version="1.0" encoding="US-ASCII"?>)",
         }) {
        const reader file = open_header(header_with(declaration, root_start_tag));
        EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_xml_declaration))
            << declaration;
    }
}

TEST(conformance_header, a_header_beyond_ascii_cannot_declare_another_encoding)
{
    const std::string_view latin1 = R"(<?xml version="1.0" encoding="ISO-8859-1"?>)";
    // In ASCII, the encoding makes no difference.
    EXPECT_TRUE(single_diagnostic(open_header(header_with(latin1, root_start_tag)).diagnostics(), severity::warning,
                                  errc::invalid_xml_declaration));
    EXPECT_TRUE(refuses(errc::invalid_xml, header_with(latin1, root_start_tag,
                                                       R"(<Property id="Test:Name" type="String">Ñandú</Property>)")));
}

TEST(conformance_header, references_in_the_xml_declaration_are_read_as_in_attribute_values)
{
    // pugixml reads them, so a reference to U+0000 would end the name of an encoding before it starts, which would pass
    // for none.
    EXPECT_TRUE(
        refuses(errc::invalid_xml, header_with(R"(<?xml version="1.0" encoding="&#0;ISO-8859-1"?>)", root_start_tag,
                                               R"(<Property id="Test:Name" type="String">Ñandú</Property>)")));
    EXPECT_TRUE(refuses(errc::invalid_xml, header_with(R"(<?xml version="1.0&#xD800;"?>)", root_start_tag)));
    // A control character gets the warning that it gets elsewhere.
    const std::string header = header_with(R"(<?xml version="1.0" encoding="UTF-8&#1;"?>)", root_start_tag);
    const reader file = open_header(header);
    ASSERT_EQ(file.diagnostics().size(), 2U) << openxisf::test::describe(file.diagnostics());
    EXPECT_EQ(file.diagnostics()[0].code, errc::invalid_xml_declaration);
    EXPECT_EQ(file.diagnostics()[1].code, errc::invalid_character);
    EXPECT_EQ(file.diagnostics()[1].context.offset, 16 + header.find("&#1;"));
}

TEST(conformance_header, a_byte_order_mark_before_the_header_is_skipped)
{
    EXPECT_TRUE(no_diagnostics(open_header("\xEF\xBB\xBF" + header_xml()).diagnostics()));
}

TEST(conformance_header, the_header_is_utf8)
{
    // Latin-1 for Ñ.
    EXPECT_TRUE(refuses(errc::invalid_utf8, header_xml("<Property id=\"Test:Name\" type=\"String\">\xD1</Property>")));
}

TEST(conformance_header, a_character_that_xml_does_not_allow_is_read_with_a_warning)
{
    // XML 1.0, Legal Character: a control character other than tab, line feed and carriage return means what it says,
    // so the value keeps it, and the header has a warning about the first one.
    const std::string header = header_xml(R"(<Property id="Test:Bell" type="String">a&#7;b</Property>)"
                                          "<Property id=\"Test:Escape\" type=\"String\">\x1B</Property>");
    const reader file = open_header(header);
    ASSERT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_character));
    EXPECT_EQ(file.diagnostics().front().context.offset, 16 + header.find("&#7;"));
    EXPECT_EQ(file.properties().at("Test:Bell").value, openxisf::property_value("a\ab"));
    EXPECT_EQ(file.properties().at("Test:Escape").value, openxisf::property_value("\x1B"));

    // What follows where the header stops being well-formed XML, after the root element, is not read: the warning
    // about it is the only one.
    for (const std::string_view after :
         {"<Extension xmlns=\"urn:example\">\x01", "<Extension xmlns=\"urn:example\">&#1;"}) {
        EXPECT_TRUE(single_diagnostic(open_header(header_xml() + std::string(after)).diagnostics(), severity::warning,
                                      errc::invalid_xml, "/Extension[1]"))
            << after;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The root element

TEST(conformance_header, the_root_element_is_xisf_in_the_xisf_namespace)
{
    struct root
    {
        std::string_view start{};
        std::string_view end{};
    };
    for (const root& element : {
             root{.start = R"(<XISF version="1.0" xmlns="http://www.pixinsight.com/xisf">)", .end = "</XISF>"},
             root{.start = R"(<Image version="1.0" xmlns="http://www.pixinsight.com/xisf">)", .end = "</Image>"},
             root{.start = R"(<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf/">)", .end = "</xisf>"},
             root{.start = R"(<x:xisf version="1.0" xmlns:x="urn:example">)", .end = "</x:xisf>"},
             root{.start = R"(<x:xisf version="1.0">)", .end = "</x:xisf>"},
         }) {
        EXPECT_TRUE(refuses(errc::invalid_root_element, header_with(xml_declaration, element.start, {}, element.end)))
            << element.start;
    }
}

TEST(conformance_header, the_root_element_may_have_a_prefix_for_the_xisf_namespace)
{
    const std::string header =
        std::string(xml_declaration) + R"(<x:xisf version="1.0" xmlns:x="http://www.pixinsight.com/xisf">)" +
        R"(<x:Metadata><x:Property id="XISF:CreationTime" type="TimePoint" value="2026-10-02T12:00:00Z"/>)" +
        R"(<x:Property id="XISF:CreatorApplication" type="String">OpenXISF tests 1.0</x:Property></x:Metadata>)" +
        R"(<x:FITSKeyword name="OBSERVER" value="'Test'" comment=""/></x:xisf>)";
    EXPECT_TRUE(no_diagnostics(open_header(header).diagnostics()));
}

TEST(conformance_header, elements_without_namespace_are_accepted)
{
    EXPECT_TRUE(no_diagnostics(
        open_header(header_with(xml_declaration, R"(<xisf version="1.0">)", image_xml())).diagnostics()));
}

TEST(conformance_header, the_root_element_has_a_version)
{
    try {
        (void)open_header(header_with(xml_declaration, R"(<xisf xmlns="http://www.pixinsight.com/xisf">)"));
        ADD_FAILURE();
    } catch (const invalid_data_error& failure) {
        EXPECT_EQ(failure.code(), errc::invalid_root_element);
        EXPECT_EQ(failure.context().element, "/xisf");
        EXPECT_EQ(failure.context().attribute, "version");
    }
}

TEST(conformance_header, only_version_1_0_is_supported)
{
    for (const std::string_view version : {"2.0", "1.1", "1.00", "1", " 1.0", ""}) {
        const std::string root =
            R"(<xisf xmlns="http://www.pixinsight.com/xisf" version=")" + std::string(version) + R"(">)";
        EXPECT_TRUE(throws<openxisf::unsupported_error>(errc::unsupported_version, [&root] {
            (void)open_header(header_with(xml_declaration, root));
        })) << version;
    }
}

TEST(conformance_header, character_data_in_the_root_element_is_ignored_with_a_warning)
{
    for (const std::string_view text : {"text", "<![CDATA[text]]>"}) {
        const reader file = open_header(header_xml(text));
        EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_root_element, "/xisf"))
            << text;
    }
}

TEST(conformance_header, white_space_between_elements_is_not_significant)
{
    const reader file =
        open_header("\xEF\xBB\xBF" + std::string(xml_declaration) + "\n<!-- comment -->\n" +
                    std::string(root_start_tag) + "\n  <Metadata>\n    " + std::string(mandatory_metadata) +
                    "\n  </Metadata>\n  <![CDATA[ \t ]]>" + image_xml() + "\r\n</xisf>\n");
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
}

TEST(conformance_header, a_second_root_element_is_refused)
{
    EXPECT_TRUE(refuses(errc::invalid_root_element, header_xml() + header_xml().substr(xml_declaration.size())));
}

TEST(conformance_header, elements_after_the_root_element_are_ignored_with_a_warning)
{
    const reader file = open_header(header_xml() + "<Other/>");
    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::unknown_element, "/Other[1]"));
}

TEST(conformance_header, character_data_outside_the_root_element_is_ignored_with_a_warning)
{
    // XML allows markup and white space alone around the root element, and pugixml skips any text there.
    const std::string root = header_xml().substr(xml_declaration.size());
    for (const std::string& header : {std::string(xml_declaration) + "text" + root, header_xml() + "\ntext\n",
                                      header_xml() + "<!-- --> &amp;", header_xml() + "<![CDATA[ ]]>"}) {
        const reader file = open_header(header);
        EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_xml)) << header;
    }
    const reader before = open_header(std::string(xml_declaration) + "\n text" + root);
    ASSERT_FALSE(before.diagnostics().empty());
    EXPECT_EQ(before.diagnostics().front().context.offset, 16 + xml_declaration.size() + 2);
}

// ---------------------------------------------------------------------------------------------------------------------
// Core elements and extension elements

TEST(conformance_header, core_element_names_are_case_sensitive)
{
    const reader file = open_header(header_xml("<image/>"));
    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::unknown_element, "/xisf/image[1]"));
}

TEST(conformance_header, an_extension_element_in_another_namespace_is_ignored)
{
    struct extension
    {
        std::string_view xml{};
        std::string_view path{};
    };
    for (const extension& element : {
             // Nothing inside an extension is read.
             extension{.xml = R"(<e:Extension xmlns:e="urn:e"><Image uid="a"/><Image uid="a"/></e:Extension>)",
                       .path = "/xisf/Extension[1]"},
             extension{.xml = R"(<Extension xmlns="urn:example"><Unknown/></Extension>)", .path = "/xisf/Extension[1]"},
             // A prefix that is not declared names no namespace that could be the XISF one.
             extension{.xml = R"(<u:Extension/>)", .path = "/xisf/Extension[1]"},
         }) {
        const reader file = open_header(header_xml(element.xml));
        EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::info, errc::unknown_element, element.path))
            << element.xml;
    }
}

TEST(conformance_header, an_element_in_the_xisf_namespace_that_xisf_does_not_define_is_ignored_with_a_warning)
{
    // Extensions should have a namespace of their own (spec §9.5).
    const reader file = open_header(header_xml("<Extension/><Extension/>"));
    ASSERT_EQ(file.diagnostics().size(), 2U);
    EXPECT_EQ(file.diagnostics()[1].severity, severity::warning);
    EXPECT_EQ(file.diagnostics()[1].code, errc::unknown_element);
    EXPECT_EQ(file.diagnostics()[1].context.element, "/xisf/Extension[2]");
}

// ---------------------------------------------------------------------------------------------------------------------
// Unique element identifiers (spec §11)

TEST(conformance_header, a_uid_must_be_a_unique_element_identifier)
{
    const reader file = open_header(header_xml(image_xml(R"(uid="1st")")));
    ASSERT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_uid, "/xisf/Image[1]"));
    EXPECT_EQ(file.diagnostics().front().context.attribute, "uid");
}

TEST(conformance_header, a_uid_is_unique_in_the_unit)
{
    // Also between elements at different levels.
    const std::string header = header_xml(image_xml(R"(uid="a")", keyword_xml(R"(uid="b")")) + image_xml(R"(uid="b")"));

    const reader file = open_header(header);
    ASSERT_TRUE(single_diagnostic(file.diagnostics(), severity::error, errc::duplicate_uid, "/xisf/Image[2]"));
    EXPECT_EQ(file.diagnostics().front().context.attribute, "uid");

    EXPECT_TRUE(
        throws<invalid_data_error>(errc::duplicate_uid, [&header] { (void)open_header(header, {.strict = true}); }));
}

TEST(conformance_header, uids_are_case_sensitive)
{
    EXPECT_TRUE(
        no_diagnostics(open_header(header_xml(image_xml(R"(uid="a")") + image_xml(R"(uid="A")"))).diagnostics()));
}

TEST(conformance_header, an_attribute_with_a_prefix_is_not_a_core_attribute)
{
    // Attributes in a namespace belong to extensions, even with the name of a core attribute.
    const std::string_view prefixed_uid = R"(xmlns:e="urn:example" e:uid="a")";
    const reader file = open_header(header_xml(image_xml(prefixed_uid) + image_xml(prefixed_uid) +
                                               image_xml({}, R"(<Reference xmlns:e="urn:e" e:ref="a" ref="b"/>)") +
                                               keyword_xml(R"(uid="b")")));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
}

} // namespace
