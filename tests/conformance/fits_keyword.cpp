// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §11.6: FITSKeyword elements, on constructed units.

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/reader.h>

#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::fits_keyword;
using openxisf::reader;
using openxisf::severity;
using openxisf::test::header_xml;
using openxisf::test::image_xml;
using openxisf::test::no_diagnostics;
using openxisf::test::single_diagnostic;

reader open_body(std::string_view body, openxisf::read_options options = {})
{
    return openxisf::test::open_header(header_xml(body), std::move(options));
}

// An image whose only content is the keyword with the given attributes.
reader open_keyword(std::string_view attributes, openxisf::read_options options = {})
{
    return open_body(image_xml({}, "<FITSKeyword " + std::string(attributes) + "/>"), std::move(options));
}

TEST(conformance_fits_keyword, an_image_has_its_keywords_in_order)
{
    // The examples of spec §11.6.1.
    const reader file = open_body(image_xml(
        {}, R"(<FITSKeyword name="DATE-OBS" value="'2012-03-15T02:55:15'" comment="Observation start time, UT"/>)"
            R"(<FITSKeyword name="EXPTIME" value="300" comment="Exposure time in seconds"/>)"
            R"(<FITSKeyword name="XBINNING" value="1" comment="Binning factor, X-axis"/>)"
            R"(<FITSKeyword name="HISTORY" value="" comment="Processed with magic techniques"/>)"));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    const std::vector<fits_keyword> expected{
        {.name = "DATE-OBS", .value = "'2012-03-15T02:55:15'", .comment = "Observation start time, UT"},
        {.name = "EXPTIME", .value = "300", .comment = "Exposure time in seconds"},
        {.name = "XBINNING", .value = "1", .comment = "Binning factor, X-axis"},
        {.name = "HISTORY", .value = "", .comment = "Processed with magic techniques"}};
    EXPECT_EQ(file.image(0).fits_keywords, expected);
}

TEST(conformance_fits_keyword, values_and_comments_are_kept_as_written)
{
    // Padding of values is optional (spec §11.6.1), so it is kept, like any other text of a keyword.
    const reader file = open_keyword(R"(name="OBSERVER" value="'Nandu   '" comment="  spaces kept ")");
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.image(0).fits_keywords,
              (std::vector<fits_keyword>{{.name = "OBSERVER", .value = "'Nandu   '", .comment = "  spaces kept "}}));
}

TEST(conformance_fits_keyword, text_outside_printable_ascii_is_kept_with_a_warning)
{
    // FITS 4.0 §4.1.1: a header holds printable ASCII, which the writer requires; capture software writes degree signs.
    const reader file = open_keyword(R"(name="CCD-TEMP" value="-10&#9;" comment="Sensor temperature in °C")");
    ASSERT_EQ(file.diagnostics().size(), 2U) << openxisf::test::describe(file.diagnostics());
    for (const openxisf::diagnostic& entry : file.diagnostics()) {
        EXPECT_EQ(entry.severity, severity::warning);
        EXPECT_EQ(entry.code, errc::invalid_fits_keyword);
        EXPECT_EQ(entry.context.element, "/xisf/Image[1]/FITSKeyword[1]");
    }
    EXPECT_EQ(file.diagnostics()[0].context.attribute, "value");
    EXPECT_EQ(file.diagnostics()[1].context.attribute, "comment");
    EXPECT_EQ(
        file.image(0).fits_keywords,
        (std::vector<fits_keyword>{{.name = "CCD-TEMP", .value = "-10\t", .comment = "Sensor temperature in °C"}}));
}

TEST(conformance_fits_keyword, a_keyword_without_a_name_is_unavailable)
{
    const reader file =
        open_body(image_xml({}, R"(<FITSKeyword value="1" comment="no name"/>)" + openxisf::test::keyword_xml()));
    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::error, errc::invalid_fits_keyword,
                                  "/xisf/Image[1]/FITSKeyword[1]"));
    ASSERT_EQ(file.image(0).fits_keywords.size(), 1U);
    EXPECT_EQ(file.image(0).fits_keywords[0].name, "OBSERVER");

    EXPECT_TRUE(openxisf::test::throws<openxisf::invalid_data_error>(errc::invalid_fits_keyword, [] {
        (void)openxisf::test::open_header(header_xml(image_xml({}, R"(<FITSKeyword value="1" comment=""/>)")),
                                          {.strict = true});
    }));
}

TEST(conformance_fits_keyword, a_name_outside_the_fits_grammar_is_kept_with_a_warning)
{
    // FITS 4.0: at most eight upper-case letters, digits, hyphens and underscores; no padding in XISF. The names
    // outside the grammar are in unit/ancillary_attributes.cpp.
    const reader file = open_keyword(R"(name="object" value="1" comment="")");
    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_fits_keyword,
                                  "/xisf/Image[1]/FITSKeyword[1]"));
    ASSERT_EQ(file.image(0).fits_keywords.size(), 1U);
    EXPECT_EQ(file.image(0).fits_keywords[0].name, "object");
}

TEST(conformance_fits_keyword, a_name_padded_with_spaces_is_read_without_them_with_a_warning)
{
    // Spec §11.6.1: names must not be padded, which the first edition did not say, so older encoders may pad them to
    // eight characters. FITS names are left-justified without spaces inside, so trailing spaces are padding.
    const reader file = open_keyword(R"(name="EXPTIME " value="300." comment="")", {.strict = true});
    ASSERT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_fits_keyword,
                                  "/xisf/Image[1]/FITSKeyword[1]"));
    EXPECT_EQ(file.diagnostics()[0].context.attribute, "name");
    EXPECT_NE(file.diagnostics()[0].message.find("padded"), std::string::npos) << file.diagnostics()[0].message;
    ASSERT_EQ(file.image(0).fits_keywords.size(), 1U);
    EXPECT_EQ(file.image(0).fits_keywords[0].name, "EXPTIME");

    // A name that only padding makes is the blank keyword, and one outside the grammar stays outside it.
    for (const auto& [attributes, name] : std::initializer_list<std::pair<std::string_view, std::string_view>>{
             {R"(name="        " value="" comment="")", ""}, {R"(name="ob ject  " value="1" comment="")", "ob ject"}}) {
        const reader other = open_keyword(attributes);
        EXPECT_TRUE(single_diagnostic(other.diagnostics(), severity::warning, errc::invalid_fits_keyword,
                                      "/xisf/Image[1]/FITSKeyword[1]"))
            << attributes;
        EXPECT_EQ(other.image(0).fits_keywords.at(0).name, name);
    }
}

TEST(conformance_fits_keyword, the_blank_keyword_is_kept_with_a_warning)
{
    // FITS has a blank keyword, whose name is empty without its padding, but the XML schema of XISF requires a name of
    // one to eight characters.
    const reader file = open_keyword(R"(name="" value="" comment="commentary text")");
    ASSERT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_fits_keyword,
                                  "/xisf/Image[1]/FITSKeyword[1]"));
    EXPECT_EQ(file.diagnostics()[0].context.attribute, "name");
    EXPECT_EQ(file.image(0).fits_keywords,
              (std::vector<fits_keyword>{{.name = "", .value = "", .comment = "commentary text"}}));

    // Like COMMENT and HISTORY, it has no value.
    const reader valued = open_keyword(R"(name="" value="text" comment="")");
    ASSERT_EQ(valued.diagnostics().size(), 2U) << openxisf::test::describe(valued.diagnostics());
    EXPECT_EQ(valued.diagnostics()[0].context.attribute, "name");
    EXPECT_EQ(valued.diagnostics()[1].code, errc::invalid_fits_keyword);
    EXPECT_EQ(valued.diagnostics()[1].context.attribute, "value");
    EXPECT_EQ(valued.image(0).fits_keywords.at(0).value, "text");
}

TEST(conformance_fits_keyword, a_missing_value_or_comment_reads_as_empty_with_a_warning)
{
    const reader without_value = open_keyword(R"(name="OBJECT" comment="c")");
    EXPECT_TRUE(single_diagnostic(without_value.diagnostics(), severity::warning, errc::invalid_fits_keyword,
                                  "/xisf/Image[1]/FITSKeyword[1]"));
    EXPECT_EQ(without_value.diagnostics()[0].context.attribute, "value");
    EXPECT_EQ(without_value.image(0).fits_keywords,
              (std::vector<fits_keyword>{{.name = "OBJECT", .value = "", .comment = "c"}}));

    const reader without_comment = open_keyword(R"(name="OBJECT" value="'M31'")");
    EXPECT_TRUE(single_diagnostic(without_comment.diagnostics(), severity::warning, errc::invalid_fits_keyword,
                                  "/xisf/Image[1]/FITSKeyword[1]"));
    EXPECT_EQ(without_comment.diagnostics()[0].context.attribute, "comment");
    EXPECT_EQ(without_comment.image(0).fits_keywords,
              (std::vector<fits_keyword>{{.name = "OBJECT", .value = "'M31'", .comment = ""}}));
}

TEST(conformance_fits_keyword, history_and_comment_keywords_have_an_empty_value)
{
    // Spec §11.6.1: the value attribute of HISTORY and COMMENT is an empty string. Another value is kept.
    for (const std::string_view name : {"HISTORY", "COMMENT"}) {
        const reader file = open_keyword(R"(name=")" + std::string(name) + R"(" value="text" comment="")");
        EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_fits_keyword,
                                      "/xisf/Image[1]/FITSKeyword[1]"))
            << name;
        EXPECT_EQ(file.diagnostics()[0].context.attribute, "value");
        EXPECT_EQ(file.image(0).fits_keywords.at(0).value, "text");
    }
}

TEST(conformance_fits_keyword, a_thumbnail_has_keywords_of_its_own)
{
    // Spec §11.12: a Thumbnail element is an Image element under another name.
    const reader file = open_body(
        image_xml({}, openxisf::test::keyword_xml() +
                          openxisf::test::thumbnail_xml({}, R"(<FITSKeyword name="THUMB" value="T" comment=""/>)")));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.image(0).fits_keywords.size(), 1U);
    EXPECT_EQ(file.image(0).fits_keywords[0].name, "OBSERVER");
    const openxisf::thumbnail& small = file.image(0).thumbnail.value_or(openxisf::thumbnail{});
    ASSERT_EQ(small.fits_keywords.size(), 1U);
    EXPECT_EQ(small.fits_keywords[0].name, "THUMB");
}

} // namespace
