// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "xml/xml_document.h"

#include <openxisf/error.h>
#include <openxisf/limits.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using namespace std::string_view_literals;
using openxisf::errc;
using openxisf::invalid_data_error;
using openxisf::limit_error;
using openxisf::detail::character_data;
using openxisf::detail::element_length;
using openxisf::detail::is_xml_white_space;
using openxisf::detail::parse_xml;
using openxisf::detail::tag_end;
using openxisf::test::throws;

std::unique_ptr<pugi::xml_document> parse(std::string_view text, const openxisf::limits& limits = {})
{
    return parse_xml(text, 0, limits);
}

testing::AssertionResult refuses(errc code, std::string_view text)
{
    return throws<invalid_data_error>(code, [text] { (void)parse(text); });
}

// count elements nested in one another.
std::string nested(std::size_t count)
{
    std::string text;
    for (std::size_t i = 0; i < count; ++i) {
        text += "<e>";
    }
    for (std::size_t i = 0; i < count; ++i) {
        text += "</e>";
    }
    return text;
}

TEST(xml_document, accepts_several_elements_at_the_top_level)
{
    // As in a signed header, where the signature follows the root element.
    const auto document = parse("<a><b/></a>\n<c/>");

    EXPECT_STREQ(document->document_element().name(), "a");
    EXPECT_STREQ(document->document_element().next_sibling().name(), "c");
}

TEST(xml_document, refuses_malformed_xml)
{
    for (const std::string_view text :
         {""sv, "text"sv, "<a>"sv, "<a></b>"sv, "<a b=c/>"sv, "<a/><"sv, "<a><!-- </a>"sv}) {
        EXPECT_TRUE(refuses(errc::invalid_xml, text)) << text;
    }
}

TEST(xml_document, places_an_error_in_the_source)
{
    for (const std::uint64_t offset : {0U, 100U}) {
        try {
            (void)parse_xml("<a><b></a>", offset, {});
            ADD_FAILURE();
        } catch (const invalid_data_error& failure) {
            ASSERT_TRUE(failure.context().offset.has_value());
            EXPECT_GE(*failure.context().offset, offset + 6);
            EXPECT_LE(*failure.context().offset, offset + 10);
        }
    }
}

TEST(xml_document, refuses_text_that_is_not_utf8)
{
    for (const std::string_view text : {"<a>\xC0\x80</a>"sv, "<a>\xFF</a>"sv, "<a>\xED\xA0\x80</a>"sv, "<a>\0</a>"sv}) {
        EXPECT_TRUE(refuses(errc::invalid_utf8, text)) << text;
    }
}

TEST(xml_document, refuses_a_document_type_declaration)
{
    // The entities of a document type declaration are the way to external resources and to exponential expansion.
    for (const std::string_view text : {
             R"(<!DOCTYPE xisf><xisf/>)"sv,
             R"(<!DOCTYPE xisf SYSTEM "http://example.com/xisf.dtd"><xisf/>)"sv,
             R"(<!DOCTYPE xisf [<!ENTITY a "aaaa"><!ENTITY b "&a;&a;&a;&a;">]><xisf>&b;</xisf>)"sv,
         }) {
        EXPECT_TRUE(refuses(errc::doctype_not_allowed, text)) << text;
    }
}

TEST(xml_document, replaces_only_predefined_entities_and_character_references)
{
    const auto document = parse(R"(<a b="&lt;&#x41;&gt;">&amp;&#66;&quot;&apos;&undeclared;</a>)");
    const pugi::xml_node element = document->document_element();

    EXPECT_STREQ(element.attribute("b").value(), "<A>");
    EXPECT_STREQ(element.child_value(), "&B\"'&undeclared;");
}

TEST(xml_document, refuses_character_references_to_what_xml_does_not_allow)
{
    // XML 1.0, Legal Character: U+0000, surrogates and numbers beyond U+10FFFF, which pugixml would write into the
    // text.
    for (const std::string_view text :
         {"<a>&#0;</a>", "<a>&#x0;</a>", "<a>x&#00000;y</a>", R"(<a b="&#0;"/>)", "<a>&#xD800;</a>", "<a>&#xdfff;</a>",
          "<a>&#55296;</a>", "<a>&#x110000;</a>", "<a>&#1114112;</a>", "<a>&#4294967296;</a>", "<a>&#x100000000;</a>",
          "<a>&#99999999999999999999;</a>"}) {
        EXPECT_TRUE(refuses(errc::invalid_xml, text)) << text;
    }
    // pugixml accepts '<' in attribute values, so what starts a comment, a CDATA section or a processing instruction
    // there is part of the value, and hides no reference after it.
    for (const std::string_view text :
         {R"(<a b="<!--">x&#0;y</a>)", R"(<a b='<![CDATA['>&#0;</a>)", R"(<a b="<?">&#0;</a>)",
          R"(<a b="<!--" c="&#0;"/>)", R"(<a b="'<!--'"><c d="&#0;"/></a>)"}) {
        EXPECT_TRUE(refuses(errc::invalid_xml, text)) << text;
    }
    // pugixml reads the pseudo-attributes of an XML declaration, whose target is xml in any case, as attribute values.
    for (const std::string_view text : {R"(<?xml version="1.0" encoding="&#0;ISO-8859-1"?><a/>)",
                                        R"(<?XML version='1.0&#xD800;'?><a/>)", R"(<?xml?><a>&#0;</a>)"}) {
        EXPECT_TRUE(refuses(errc::invalid_xml, text)) << text;
    }
    // The last code point and those around the surrogates are characters; references in comments, CDATA sections and
    // processing instructions are text, whatever quotes and tags they hold; and what pugixml does not read as a
    // reference stays as written.
    for (const std::string_view text :
         {"<a>&#x10FFFF;&#xD7FF;&#xE000;&#1;&#x9;</a>", "<a><!-- &#0; --></a>", "<a><![CDATA[&#0;]]></a>",
          "<a><?pi &#0;?></a>", R"(<a><!-- " <b c=' --><![CDATA[ " &#0; ]]><?pi ' &#0;?></a>)", "<a>&#X0;</a>",
          "<a>&#;</a>", "<a>&#x;</a>", "<a>&#0</a>", "<a>&#0x1;</a>", "<a>&#fffffff;</a>",
          "<?xml-stylesheet href='&#0;'?><a/>", "<?xml0 &#0;?><a/>"}) {
        EXPECT_NO_THROW((void)parse(text)) << text;
    }
    EXPECT_STREQ(parse("<a>&#x10FFFF;</a>")->document_element().child_value(), "\xF4\x8F\xBF\xBF");
}

TEST(xml_document, finds_the_first_character_that_xml_does_not_allow)
{
    using openxisf::detail::first_restricted_character;
    const auto found = [](std::string_view text) {
        const std::optional<openxisf::detail::restricted_character> first = first_restricted_character(text);
        return first ? std::make_pair(first->offset, first->code_point) : std::make_pair(text.size(), std::uint32_t{});
    };
    // XML 1.0, Legal Character: control characters but tab, line feed and carriage return, U+FFFE and U+FFFF, written
    // as they are anywhere, comments included.
    EXPECT_EQ(found("<a>x\x01</a>"), std::make_pair(std::size_t{4}, std::uint32_t{1}));
    EXPECT_EQ(found("<a b=\"\x1F\"/>"), std::make_pair(std::size_t{6}, std::uint32_t{0x1F}));
    EXPECT_EQ(found("<a><!-- \x0B --></a>"), std::make_pair(std::size_t{8}, std::uint32_t{0x0B}));
    EXPECT_EQ(found("<a>\xEF\xBF\xBE</a>"), std::make_pair(std::size_t{3}, std::uint32_t{0xFFFE}));
    EXPECT_EQ(found("<a>\xEF\xBF\xBF</a>"), std::make_pair(std::size_t{3}, std::uint32_t{0xFFFF}));
    // Or as character references, where pugixml reads them; the first in the text, whatever its form.
    EXPECT_EQ(found("<a>&#1;</a>"), std::make_pair(std::size_t{3}, std::uint32_t{1}));
    EXPECT_EQ(found(R"(<a b="&#x1f;"/>)"), std::make_pair(std::size_t{6}, std::uint32_t{0x1F}));
    EXPECT_EQ(found("<a>&#xFFFF;\x02</a>"), std::make_pair(std::size_t{3}, std::uint32_t{0xFFFF}));
    EXPECT_EQ(found("<a>\x02&#xFFFF;</a>"), std::make_pair(std::size_t{3}, std::uint32_t{2}));
    EXPECT_EQ(found(R"(<?xml version="1.0" encoding="UTF-8&#1;"?><a/>)"),
              std::make_pair(std::size_t{35}, std::uint32_t{1}));
    // Characters that XML allows, and references in comments, CDATA sections and processing instructions, which are
    // text.
    const std::string_view allowed = "<a b=\"\t\">\r\n&#9;&#xA;&#13;\x7F\xC2\x80\xEF\xBF\xBD&#xFFFD;&#x10FFFF;"
                                     "<!-- &#1; --><![CDATA[&#2;]]><?pi &#3;?></a>";
    EXPECT_EQ(found(allowed), std::make_pair(allowed.size(), std::uint32_t{}));
}

TEST(xml_document, finds_character_data_outside_the_elements)
{
    using openxisf::detail::first_text_outside_elements;
    // XML allows markup and white space alone at the top level (Document), and pugixml accepts the rest: text, entity
    // references among it, and CDATA sections.
    for (const std::string_view text : {"x<a/>", "<a/>x", "<?xml version=\"1.0\"?>\n x<a/>", "<a/><!-- c -->y<b/>",
                                        "<a>t</a> &amp;", "<a/><![CDATA[ ]]>", "<?pi?>x<a/>"}) {
        EXPECT_NO_THROW((void)parse(text)) << text;
        EXPECT_TRUE(first_text_outside_elements(text).has_value()) << text;
    }
    EXPECT_EQ(first_text_outside_elements("<a/>\n\t x"), 7U);
    EXPECT_EQ(first_text_outside_elements("<a/><![CDATA[x]]>"), 4U);
    // White space, and what elements, comments and processing instructions hold, quotes and tags included.
    for (const std::string_view text : {"<a>x</a>", " \r\n\t<a/>\n", "<a b='>x'>y<![CDATA[z]]></a><!-- x -->",
                                        "<?xml version=\"1.0\"?>\n<?pi > x?>\n<a><b>y</b><c/></a>\n<d e=\"<f>\"/>"}) {
        EXPECT_EQ(first_text_outside_elements(text), std::nullopt) << text;
    }
}

TEST(xml_document, refuses_an_element_that_repeats_an_attribute)
{
    EXPECT_TRUE(refuses(errc::invalid_xml, R"(<a x="1" x="2"/>)"));
    try {
        (void)parse_xml(R"(<a><b y="1" x="2" y="1"/></a>)", 100, {});
        ADD_FAILURE();
    } catch (const invalid_data_error& failure) {
        EXPECT_EQ(failure.code(), errc::invalid_xml);
        EXPECT_EQ(failure.context().offset, 103U);
    }
}

TEST(xml_document, attributes_with_different_prefixes_are_different)
{
    EXPECT_NO_THROW((void)parse(R"(<a xmlns:p="urn:p" xmlns:q="urn:q" x="1" p:x="2" q:x="3"/>)"));
}

TEST(xml_document, keeps_white_space_that_is_the_only_content_of_an_element)
{
    // The value of a String property (spec §11.1.6).
    const auto document = parse("<a><b>  </b> <c> x </c>\n</a>");
    const pugi::xml_node a = document->document_element();

    EXPECT_STREQ(a.child("b").child_value(), "  ");
    EXPECT_STREQ(a.child("c").child_value(), " x ");
    // White space between elements is dropped.
    EXPECT_EQ(a.first_child().type(), pugi::node_element);
    EXPECT_EQ(a.last_child().type(), pugi::node_element);
}

TEST(xml_document, takes_the_character_data_around_markup_from_the_text)
{
    // pugixml drops the white space beside a comment, a processing instruction or a CDATA section, which XML keeps:
    // the character data of such an element come from the text as XML defines them. The expected values are those of
    // Python's xml.etree (expat): the text of the element and the tails of its children.
    struct sample
    {
        std::string_view content{};
        std::string_view data{};
    };
    for (const sample& entry : {
             sample{.content = " <!--c-->a", .data = " a"},
             sample{.content = "a<!--c--> ", .data = "a "},
             sample{.content = " <!--c--> ", .data = "  "},
             sample{.content = "a<?pi x?> ", .data = "a "},
             sample{.content = "  <![CDATA[x]]>", .data = "  x"},
             sample{.content = "<![CDATA[ <&> ]]>", .data = " <&> "},
             sample{.content = "a&amp;<!---->&#x41;&lt;b", .data = "a&A<b"},
             sample{.content = "line\r\n<!---->\rend", .data = "line\n\nend"},
             sample{.content = "<![CDATA[a\r\nb\rc]]>", .data = "a\nb\nc"},
             sample{.content = "x<!--a-->y<!--b-->z", .data = "xyz"},
             sample{.content = "a<b>t</b><!---->c", .data = "ac"},
             sample{.content = " \t\n<!---->", .data = " \t\n"},
             sample{.content = "&#9;<!---->&#10;&#13;", .data = "\t\n\r"},
             sample{.content = "]]&gt;<?a?>", .data = "]]>"},
             sample{.content = "<![CDATA[]]]]><![CDATA[>]]>", .data = "]]>"},
             sample{.content = "<?pi a > b ?>x", .data = "x"},
             sample{.content = "<!-- a - b -->x", .data = "x"},
             sample{.content = "\xC3\xA9<!---->\xC3\xBC", .data = "\xC3\xA9\xC3\xBC"},
             sample{.content = "<!-- <Property/> -->a", .data = "a"},
             sample{.content = "&quot;&apos;<!---->&gt;", .data = "\"'>"},
             sample{.content = "<!---->", .data = ""},
             sample{.content = "&#x1F600;<?x?>", .data = "\xF0\x9F\x98\x80"},
             // An undeclared entity, which expat refuses, stays as written, as pugixml leaves it.
             sample{.content = "&foo;<!---->&#;", .data = "&foo;&#;"},
         }) {
        const std::string text = "<r><e a=\"1\">" + std::string(entry.content) + "</e><f/></r>";
        const auto document = parse(text);
        EXPECT_EQ(character_data(document->document_element().child("e"), text), entry.data) << entry.content;
    }
    // Without such markup, the character data are those that pugixml gives, whatever the text says.
    const std::string text = "<r><e>  </e><f/><g> x&amp;y </g></r>";
    const auto document = parse(text);
    const pugi::xml_node root = document->document_element();
    EXPECT_EQ(character_data(root.child("e"), text), "  ");
    EXPECT_EQ(character_data(root.child("f"), text), "");
    EXPECT_EQ(character_data(root.child("g"), text), " x&y ");
}

TEST(xml_document, normalizes_line_ends_and_white_space_in_attributes)
{
    const auto document = parse("<a b=\"1\t2\n3\">x\r\ny</a>");
    EXPECT_STREQ(document->document_element().attribute("b").value(), "1 2 3");
    EXPECT_STREQ(document->document_element().child_value(), "x\ny");
}

TEST(xml_document, limits_the_nesting_depth)
{
    EXPECT_NO_THROW((void)parse(nested(5), {.max_xml_depth = 5}));
    EXPECT_TRUE(throws<limit_error>(errc::xml_too_deep, [] { (void)parse(nested(6), {.max_xml_depth = 5}); }));

    // The depth goes down again after each element.
    const std::string_view siblings = "<a><b><c/></b><d><e/></d></a><f><g/></f>";
    EXPECT_NO_THROW((void)parse(siblings, {.max_xml_depth = 3}));
    EXPECT_TRUE(throws<limit_error>(errc::xml_too_deep, [siblings] { (void)parse(siblings, {.max_xml_depth = 2}); }));
}

TEST(xml_document, limits_the_number_of_elements)
{
    // Every element counts, also those after the first top-level one.
    EXPECT_NO_THROW((void)parse("<a><b/><c/></a>", {.max_xml_elements = 3}));
    EXPECT_TRUE(throws<limit_error>(errc::too_many_xml_elements,
                                    [] { (void)parse("<a><b/><c/></a><d/>", {.max_xml_elements = 3}); }));
}

TEST(xml_document, checks_the_limits_on_the_tags_before_it_builds_the_document)
{
    // The elements are counted from the tags of the text before pugixml builds a tree of them, which costs memory for
    // each: text beyond a limit fails with the limit, also when it is not well-formed, which the parse would report.
    EXPECT_TRUE(throws<limit_error>(errc::too_many_xml_elements,
                                    [] { (void)parse("<a><b/><c/><d/>", {.max_xml_elements = 3}); }));
    EXPECT_TRUE(throws<limit_error>(errc::xml_too_deep, [] { (void)parse("<a><b><c><d>", {.max_xml_depth = 3}); }));
    EXPECT_TRUE(refuses(errc::invalid_xml, "<a><b/><c/>"));
    try {
        (void)parse("<a><b/><c/><d/>", {.max_xml_elements = 3});
        FAIL() << "no exception";
    } catch (const limit_error& failure) {
        EXPECT_EQ(failure.context().offset, 11U); // The '<' of the element beyond the limit.
    }

    // Comments, CDATA sections, processing instructions, a declaration and a '>' in an attribute value are not tags;
    // a self-closing tag and an end tag close their element.
    EXPECT_NO_THROW((void)parse("<a><!-- <b/><c/> --><![CDATA[<d/>]]><?pi <e/> ?></a>", {.max_xml_elements = 1}));
    EXPECT_NO_THROW((void)parse("<?xml version=\"1.0\"?><a/>", {.max_xml_elements = 1}));
    EXPECT_NO_THROW((void)parse(R"(<a b="/>" c='>'><d/></a>)", {.max_xml_depth = 2, .max_xml_elements = 2}));
    EXPECT_NO_THROW((void)parse("<a/><b/><c><d/></c>", {.max_xml_depth = 2}));
    EXPECT_TRUE(
        throws<limit_error>(errc::xml_too_deep, [] { (void)parse("<a/><b/><c><d/></c>", {.max_xml_depth = 1}); }));
    // Text that is not XML at all is for the parse to refuse.
    EXPECT_TRUE(refuses(errc::invalid_xml, "<<<<"));
    EXPECT_TRUE(refuses(errc::invalid_xml, "<a><b"));
}

TEST(xml_document, zero_means_no_limit)
{
    // Deep enough to exhaust the stack of any recursive walk.
    EXPECT_NO_THROW((void)parse(nested(100'000), {.max_xml_depth = 0, .max_xml_elements = 0}));
}

TEST(xml_document, recognizes_xml_white_space)
{
    EXPECT_TRUE(is_xml_white_space(" \t\r\n"));
    EXPECT_TRUE(is_xml_white_space(""));
    for (const std::string_view text : {"x"sv, "\v"sv, "\f"sv, "\xC2\xA0"sv}) {
        EXPECT_FALSE(is_xml_white_space(text)) << text;
    }
}

TEST(xml_document, finds_the_end_of_an_element)
{
    struct example
    {
        std::string_view text{};
        std::size_t start = 0;
        std::string_view element{};
    };
    for (const example& e : {
             example{.text = "<a/>tail", .start = 0, .element = "<a/>"},
             example{.text = "<a></a>tail", .start = 0, .element = "<a></a>"},
             example{.text = "<x/><a b='1'><a/><a>t</a></a>tail", .start = 4, .element = "<a b='1'><a/><a>t</a></a>"},
             // Each of these holds what would end the element if it were read as tags.
             example{.text = R"(<a b="/>" c='>'></a>tail)", .start = 0, .element = R"(<a b="/>" c='>'></a>)"},
             example{.text = "<a><!-- > </a> --></a>tail", .start = 0, .element = "<a><!-- > </a> --></a>"},
             example{.text = "<a><![CDATA[ > </a> ]]></a>tail", .start = 0, .element = "<a><![CDATA[ > </a> ]]></a>"},
             example{.text = "<a><?p /> </a>?></a>tail", .start = 0, .element = "<a><?p /> </a>?></a>"},
             example{.text = "<a>no end", .start = 0, .element = "<a>no end"},
             // Text that is not well-formed: the tags nest without regard to their names.
             example{.text = "<a><b></a></b>tail", .start = 0, .element = "<a><b></a></b>"},
             example{.text = "</a>tail", .start = 0, .element = "</a>"},
             example{.text = "<a b='>", .start = 0, .element = "<a b='>"},
         }) {
        EXPECT_EQ(e.text.substr(e.start, element_length(e.text, e.start)), e.element);
    }
}

TEST(xml_document, finds_the_end_of_a_tag)
{
    EXPECT_EQ(tag_end(R"(x<a b='>' c=">">y)", 1), 15U);
    EXPECT_EQ(tag_end("<a/>", 0), 3U);
    EXPECT_FALSE(tag_end("<a b='>", 0).has_value());
    EXPECT_FALSE(tag_end("<a", 0).has_value());
}

} // namespace
