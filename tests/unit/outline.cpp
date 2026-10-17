// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/outline.h"

#include "core/diagnostic_log.h"
#include "support/diagnostics.h"
#include "xml/xml_document.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

using openxisf::detail::build_outline;
using openxisf::detail::element_kind;
using openxisf::detail::is_core_element;
using openxisf::detail::is_unique_element_id;
using openxisf::detail::no_element;
using openxisf::detail::unit_outline;
using openxisf::test::no_diagnostics;

// An outline with the document it points into.
struct outlined
{
    std::unique_ptr<pugi::xml_document> document{};
    unit_outline outline{};
    std::vector<openxisf::diagnostic> diagnostics{};
};

outlined outline_of(std::string_view root)
{
    outlined result{.document = openxisf::detail::parse_xml(root, 0, {})};
    openxisf::detail::diagnostic_log log(false);
    result.outline = build_outline(result.document->document_element(), log);
    result.diagnostics = log.release();
    return result;
}

std::vector<std::string> paths(const unit_outline& outline)
{
    std::vector<std::string> result;
    result.reserve(outline.elements.size());
    for (std::size_t i = 0; i < outline.elements.size(); ++i) {
        result.push_back(outline.path(i));
    }
    return result;
}

TEST(outline, lists_the_elements_in_document_order_with_their_structure)
{
    const outlined unit = outline_of(R"(<xisf version="1.0">)"
                                     R"(<Image><Property/><Thumbnail><Data/></Thumbnail><Property/></Image>)"
                                     R"(<Table><Structure><Field/></Structure><Row><Cell/></Row></Table>)"
                                     R"(</xisf>)");
    const auto& elements = unit.outline.elements;
    ASSERT_TRUE(no_diagnostics(unit.diagnostics));
    ASSERT_EQ(elements.size(), 10U);

    struct expected
    {
        element_kind kind{};
        std::size_t parent = 0;
        std::size_t position = 0;
        std::size_t end = 0;
    };
    const std::vector<expected> structure{
        {.kind = element_kind::image, .parent = no_element, .position = 1, .end = 5},
        {.kind = element_kind::property, .parent = 0, .position = 1, .end = 2},
        {.kind = element_kind::thumbnail, .parent = 0, .position = 1, .end = 4},
        {.kind = element_kind::data, .parent = 2, .position = 1, .end = 4},
        {.kind = element_kind::property, .parent = 0, .position = 2, .end = 5},
        {.kind = element_kind::table, .parent = no_element, .position = 1, .end = 10},
        {.kind = element_kind::structure, .parent = 5, .position = 1, .end = 8},
        {.kind = element_kind::field, .parent = 6, .position = 1, .end = 8},
        {.kind = element_kind::row, .parent = 5, .position = 1, .end = 10},
        {.kind = element_kind::cell, .parent = 8, .position = 1, .end = 10},
    };
    for (std::size_t i = 0; i < structure.size(); ++i) {
        EXPECT_EQ(elements[i].kind, structure[i].kind) << i;
        EXPECT_EQ(elements[i].parent, structure[i].parent) << i;
        EXPECT_EQ(elements[i].position, structure[i].position) << i;
        EXPECT_EQ(elements[i].end, structure[i].end) << i;
        EXPECT_EQ(elements[i].target, no_element) << i;
    }
    EXPECT_STREQ(elements[3].node.name(), "Data");
}

TEST(outline, paths_count_the_siblings_of_the_same_name)
{
    // The XISF namespace has two prefixes here, x and the default one; the steps of a path are local names.
    const outlined unit = outline_of(R"(<x:xisf xmlns:x="http://www.pixinsight.com/xisf" version="1.0">)"
                                     R"(<x:Image><x:Property/><x:FITSKeyword/><x:Property/></x:Image>)"
                                     R"(<x:Metadata/><Image xmlns="http://www.pixinsight.com/xisf"><Property/>)"
                                     R"(<x:Property/></Image></x:xisf>)");

    // The count starts again in each element.
    const std::vector<std::string> expected{
        "/xisf/Image[1]",
        "/xisf/Image[1]/Property[1]",
        "/xisf/Image[1]/FITSKeyword[1]",
        "/xisf/Image[1]/Property[2]",
        "/xisf/Metadata[1]",
        "/xisf/Image[2]",
        "/xisf/Image[2]/Property[1]",
        "/xisf/Image[2]/Property[2]",
    };
    EXPECT_EQ(paths(unit.outline), expected);
}

// A prefix of the XISF namespace tells nothing in a path, and one of any length can be declared.
TEST(outline, the_paths_of_diagnostics_leave_out_the_prefixes_of_the_elements)
{
    // The XISF namespace is bound to P, which stands for a prefix of a thousand characters.
    const std::string_view text = R"(<P:xisf xmlns:P="http://www.pixinsight.com/xisf" version="1.0"><P:Image uid="a">)"
                                  R"(<Foo xmlns="http://www.pixinsight.com/xisf"/></P:Image><P:Image uid="a"/>)"
                                  R"(<e:Extension xmlns:e="urn:e"/></P:xisf>)";
    const std::string prefix(1000, 'p');
    std::string root;
    for (const char c : text) {
        root += c == 'P' ? prefix : std::string(1, c);
    }
    const outlined unit = outline_of(root);

    ASSERT_EQ(unit.diagnostics.size(), 3U) << openxisf::test::describe(unit.diagnostics);
    EXPECT_EQ(unit.diagnostics[0].context.element, "/xisf/Image[1]/Foo[1]");
    EXPECT_EQ(unit.diagnostics[1].context.element, "/xisf/Image[2]");
    EXPECT_EQ(unit.diagnostics[1].message, "the uid 'a' is already the uid of /xisf/Image[1]");
    EXPECT_EQ(unit.diagnostics[2].context.element, "/xisf/Extension[1]");
}

TEST(outline, leaves_out_ignored_elements_with_their_content)
{
    const outlined unit = outline_of(R"(<xisf version="1.0"><e:Extension xmlns:e="urn:e"><Image/></e:Extension>)"
                                     R"(<Image><Unknown><Property/></Unknown></Image></xisf>)");

    EXPECT_EQ(paths(unit.outline), std::vector<std::string>{"/xisf/Image[1]"});
    EXPECT_EQ(unit.diagnostics.size(), 2U);
}

TEST(outline, a_reference_targets_the_element_it_names_before_or_after_it)
{
    const outlined unit = outline_of(R"(<xisf version="1.0"><Image><Reference ref="later"/></Image>)"
                                     R"(<FITSKeyword uid="later"/><Reference ref="first"/>)"
                                     R"(<Image uid="first"/></xisf>)");
    const auto& elements = unit.outline.elements;
    ASSERT_TRUE(no_diagnostics(unit.diagnostics));
    ASSERT_EQ(elements.size(), 5U);

    EXPECT_EQ(elements[1].target, 2U);
    EXPECT_EQ(elements[3].target, 4U);
}

TEST(outline, names_the_kinds_of_elements)
{
    EXPECT_EQ(openxisf::detail::element_name(element_kind::fits_keyword), "FITSKeyword");
    EXPECT_EQ(openxisf::detail::element_name(element_kind::data), "Data");
    EXPECT_TRUE(is_core_element(element_kind::reference));
    EXPECT_TRUE(is_core_element(element_kind::thumbnail));
    for (const element_kind kind : {element_kind::field, element_kind::row, element_kind::cell, element_kind::data}) {
        EXPECT_FALSE(is_core_element(kind)) << openxisf::detail::element_name(kind);
    }
}

// Spec §11: [_a-zA-Z][_a-zA-Z0-9]*
TEST(outline, unique_element_identifiers)
{
    for (const std::string_view id : {"a", "Z", "_", "_9", "RGBWS001", "foo_bar"}) {
        EXPECT_TRUE(is_unique_element_id(id)) << id;
    }
    for (const std::string_view id : {"", "9", "1a", "a-b", "a b", "a.b", "a:b", "\xC3\xB1"}) {
        EXPECT_FALSE(is_unique_element_id(id)) << id;
    }
}

} // namespace
