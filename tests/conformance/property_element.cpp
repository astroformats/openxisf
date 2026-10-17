// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §8.4 and §11.1: Property elements, their placement and their serialization by type, on constructed units.

#include <openxisf/error.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>

#include "codec/compressed_block.h"
#include "model/properties.h"
#include "model/unit.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::date_time;
using openxisf::errc;
using openxisf::property;
using openxisf::property_format;
using openxisf::property_list;
using openxisf::property_type;
using openxisf::property_value;
using openxisf::severity;
using openxisf::detail::unit;
using openxisf::test::bytes;
using openxisf::test::file_with_attachments;
using openxisf::test::header_xml;
using openxisf::test::monolithic_file;
using openxisf::test::no_diagnostics;
using openxisf::test::open_internal;
using openxisf::test::properties_of;
using openxisf::test::single_diagnostic;
using openxisf::test::throws;

// A unit whose root element holds body, after the Metadata element.
unit open_body(std::string_view body, openxisf::read_options options = {})
{
    return open_internal(monolithic_file(header_xml(body)), std::move(options));
}

// A unit whose root element holds body, with blocks attached as file_with_attachments() places them.
unit open_with_blocks(std::string_view body, const std::vector<std::vector<std::byte>>& blocks,
                      openxisf::read_options options = {})
{
    return open_internal(file_with_attachments(header_xml(body), blocks), std::move(options));
}

// The standalone property id of a unit opened without diagnostics.
const property& standalone(const unit& opened, std::string_view id)
{
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    const property* found = opened.properties.standalone.find(id);
    if (found == nullptr) {
        ADD_FAILURE() << "no property " << id;
        static const property none{};
        return none;
    }
    return *found;
}

// The value of the only standalone property of body, which must open without diagnostics.
property_value value_of(std::string_view body)
{
    const unit opened = open_body(body);
    EXPECT_EQ(opened.properties.standalone.size(), 1U);
    return standalone(opened, opened.properties.standalone.empty() ? "" : opened.properties.standalone.begin()->id)
        .value;
}

// -------------------------------------------------------------------------------------------------------------------
// Serialization by type

TEST(conformance_property_element, scalars_complex_numbers_and_time_points_are_in_the_value_attribute)
{
    // The examples of spec §11.1.4, §11.1.5 and §11.1.7.
    const unit opened = open_body(R"(<Property id="HasData" type="Boolean" value="true"/>)"
                                  R"(<Property id="Flags" type="UInt32" value="0x8000FFA0"/>)"
                                  R"(<Property id="Volume" type="Float64" value="1.1234e+04"/>)"
                                  R"x(<Property id="PeakElement" type="Complex32" value="(0.123,-0.735e-02)"/>)x"
                                  R"(<Property id="Observation:Time:Start" type="TimePoint" )"
                                  R"(value="2015-01-23T19:52:31.46Z"/>)");
    EXPECT_EQ(standalone(opened, "HasData").value, property_value(true));
    EXPECT_EQ(standalone(opened, "Flags").value, property_value(std::uint32_t{0x8000FFA0}));
    EXPECT_EQ(standalone(opened, "Volume").value, property_value(1.1234e+04));
    EXPECT_EQ(standalone(opened, "PeakElement").value, property_value(std::complex<float>(0.123F, -0.735e-02F)));
    EXPECT_EQ(
        standalone(opened, "Observation:Time:Start").value,
        property_value(date_time{
            .year = 2015, .month = 1, .day = 23, .hour = 19, .minute = 52, .second = 31, .nanosecond = 460'000'000}));
    // In document order.
    EXPECT_EQ(opened.properties.standalone.begin()->id, "HasData");
}

TEST(conformance_property_element, radix_literals_and_non_finite_values_in_the_value_attribute)
{
    // Spec §8.3: integers in binary, octal and hexadecimal, and each spelling of the values that are not finite.
    const unit opened = open_body(R"(<Property id="Binary" type="UInt8" value="0b10100101"/>)"
                                  R"(<Property id="Octal" type="Int16" value="0o777"/>)"
                                  R"(<Property id="Hexadecimal" type="Int64" value="0x7fffffffffffffff"/>)"
                                  R"(<Property id="NaN" type="Float64" value="NaN"/>)"
                                  R"(<Property id="LowerNan" type="Float32" value="nan"/>)"
                                  R"(<Property id="MinusNan" type="Float64" value="-nan"/>)"
                                  R"(<Property id="PlusInf" type="Float64" value="+Inf"/>)"
                                  R"(<Property id="MinusInf" type="Float32" value="-Inf"/>)"
                                  R"(<Property id="LowerInf" type="Float64" value="inf"/>)"
                                  R"(<Property id="MinusLowerInf" type="Float32" value="-inf"/>)"
                                  R"x(<Property id="Complex" type="Complex64" value="(NaN,-inf)"/>)x");
    EXPECT_EQ(standalone(opened, "Binary").value, property_value(std::uint8_t{0xA5}));
    EXPECT_EQ(standalone(opened, "Octal").value, property_value(std::int16_t{511}));
    EXPECT_EQ(standalone(opened, "Hexadecimal").value, property_value(std::numeric_limits<std::int64_t>::max()));
    EXPECT_TRUE(std::isnan(standalone(opened, "NaN").value.get<double>()));
    EXPECT_TRUE(std::isnan(standalone(opened, "LowerNan").value.get<float>()));
    EXPECT_TRUE(std::isnan(standalone(opened, "MinusNan").value.get<double>()));
    constexpr double infinity = std::numeric_limits<double>::infinity();
    EXPECT_EQ(standalone(opened, "PlusInf").value, property_value(infinity));
    EXPECT_EQ(standalone(opened, "MinusInf").value, property_value(-std::numeric_limits<float>::infinity()));
    EXPECT_EQ(standalone(opened, "LowerInf").value, property_value(infinity));
    EXPECT_EQ(standalone(opened, "MinusLowerInf").value, property_value(-std::numeric_limits<float>::infinity()));
    const auto complex = standalone(opened, "Complex").value.get<std::complex<double>>();
    EXPECT_TRUE(std::isnan(complex.real()));
    EXPECT_EQ(complex.imag(), -infinity);
}

TEST(conformance_property_element, every_alternate_type_name)
{
    // Spec Tables 3 to 8.
    struct alternate
    {
        std::string_view name{};
        std::string_view value_attribute{};
        property_type type{};
    };
    for (const alternate& entry : {
             alternate{.name = "Byte", .value_attribute = R"( value="1")", .type = property_type::uint8},
             alternate{.name = "Short", .value_attribute = R"( value="1")", .type = property_type::int16},
             alternate{.name = "UShort", .value_attribute = R"( value="1")", .type = property_type::uint16},
             alternate{.name = "Int", .value_attribute = R"( value="1")", .type = property_type::int32},
             alternate{.name = "UInt", .value_attribute = R"( value="1")", .type = property_type::uint32},
             alternate{.name = "Float", .value_attribute = R"( value="1")", .type = property_type::float32},
             alternate{.name = "Double", .value_attribute = R"( value="1")", .type = property_type::float64},
             alternate{.name = "Quad", .value_attribute = R"( value="1")", .type = property_type::float128},
             alternate{.name = "Complex", .value_attribute = R"x( value="(1,2)")x", .type = property_type::complex64},
             alternate{.name = "ByteArray", .value_attribute = R"( length="0")", .type = property_type::ui8_vector},
             alternate{.name = "IVector", .value_attribute = R"( length="0")", .type = property_type::i32_vector},
             alternate{.name = "UIVector", .value_attribute = R"( length="0")", .type = property_type::ui32_vector},
             alternate{.name = "Vector", .value_attribute = R"( length="0")", .type = property_type::f64_vector},
             alternate{.name = "ByteMatrix",
                       .value_attribute = R"( rows="0" columns="0")",
                       .type = property_type::ui8_matrix},
             alternate{
                 .name = "IMatrix", .value_attribute = R"( rows="0" columns="0")", .type = property_type::i32_matrix},
             alternate{
                 .name = "UIMatrix", .value_attribute = R"( rows="0" columns="0")", .type = property_type::ui32_matrix},
             alternate{
                 .name = "Matrix", .value_attribute = R"( rows="0" columns="0")", .type = property_type::f64_matrix},
         }) {
        const bool scalar = entry.value_attribute.starts_with(" value");
        const std::string element = R"(<Property id="A" type=")" + std::string(entry.name) + "\"" +
                                    std::string(entry.value_attribute) +
                                    (scalar ? "/>" : R"( location="inline:base64"></Property>)");
        EXPECT_EQ(value_of(element).type(), entry.type) << entry.name;
    }
}

TEST(conformance_property_element, strings_in_character_data_keep_all_their_white_space)
{
    // Spec §11.1.6.
    EXPECT_EQ(value_of(R"(<Property id="S" type="String">  Ñandú — 星雲 ✓  </Property>)").get<std::string>(),
              "  Ñandú — 星雲 ✓  ");
    EXPECT_EQ(value_of(R"(<Property id="S" type="String">   </Property>)").get<std::string>(), "   ");
    EXPECT_EQ(value_of("<Property id=\"S\" type=\"String\">\n line 1\n line 2\n</Property>").get<std::string>(),
              "\n line 1\n line 2\n");
    EXPECT_EQ(value_of(R"(<Property id="S" type="String"/>)").get<std::string>(), "");
    EXPECT_EQ(value_of(R"(<Property id="S" type="String">a &amp; &lt;b&gt; &#x10FFFF;</Property>)").get<std::string>(),
              "a & <b> \xF4\x8F\xBF\xBF");
    EXPECT_EQ(value_of(R"(<Property id="S" type="String">x<![CDATA[ <y> ]]>z</Property>)").get<std::string>(),
              "x <y> z");
    // Beside a comment, a processing instruction or a CDATA section too, as XML gives them.
    EXPECT_EQ(value_of(R"(<Property id="S" type="String"> <!-- c -->a</Property>)").get<std::string>(), " a");
    EXPECT_EQ(value_of(R"(<Property id="S" type="String">a<?pi x?> </Property>)").get<std::string>(), "a ");
    EXPECT_EQ(value_of(R"(<Property id="S" type="String">  <![CDATA[x]]></Property>)").get<std::string>(), "  x");
    // In the metadata, an image and a thumbnail, whose elements are not the first of the header.
    const unit nested = open_body(openxisf::test::image_xml(
        {}, R"(<Property id="S" type="String"> <!---->a </Property>)" +
                openxisf::test::thumbnail_xml({}, R"(<Property id="S" type="String"> <!---->b </Property>)")));
    EXPECT_TRUE(no_diagnostics(nested.diagnostics));
    EXPECT_EQ(properties_of(nested, "/xisf/Image[1]").at("S").value, property_value(" a "));
    EXPECT_EQ(properties_of(nested, "/xisf/Image[1]/Thumbnail[1]").at("S").value, property_value(" b "));
}

TEST(conformance_property_element, strings_in_data_blocks)
{
    // The example of spec §11.1.6, the same text embedded and attached, and a longer one compressed.
    const std::vector<std::byte> fox = bytes("The quick brown fox");
    std::string foxes;
    for (int i = 0; i < 20; ++i) {
        foxes += "The quick brown fox ";
    }
    const openxisf::detail::compressed_block compressed =
        openxisf::detail::compress_block(bytes(foxes), {.codec = openxisf::detail::compression_codec::zlib});
    ASSERT_LT(compressed.data.size(), foxes.size());
    const unit opened = open_with_blocks(
        R"(<Property id="Inline" type="String" location="inline:base64">VGhlIHF1aWNrIGJyb3duIGZveA==</Property>)"
        R"(<Property id="Hex" type="String" location="inline:hex">546865</Property>)"
        R"(<Property id="Embedded" type="String" location="embedded">)"
        R"(<Data encoding="base64">VGhlIHF1aWNrIGJyb3duIGZveA==</Data></Property>)"
        R"(<Property id="Attached" type="String" location="attachment:{0}"/>)"
        R"(<Property id="Compressed" type="String" location="attachment:{1}" compression="zlib:400"/>)",
        {fox, compressed.data});
    EXPECT_EQ(standalone(opened, "Inline").value.get<std::string>(), "The quick brown fox");
    EXPECT_EQ(standalone(opened, "Hex").value.get<std::string>(), "The");
    EXPECT_EQ(standalone(opened, "Embedded").value.get<std::string>(), "The quick brown fox");
    EXPECT_EQ(standalone(opened, "Attached").value.get<std::string>(), "The quick brown fox");
    EXPECT_EQ(standalone(opened, "Compressed").value.get<std::string>(), foxes);
}

TEST(conformance_property_element, a_string_in_a_data_block_ignores_the_character_data_of_its_element)
{
    // The character data of an element with an inline block are the block; beside another block they are ignored,
    // with a warning, unless they are white space.
    const unit opened = open_with_blocks(
        R"(<Property id="S" type="String" location="attachment:{0}">ignored</Property>)", {bytes("kept")});
    EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_property, "/xisf/Property[1]"));
    EXPECT_EQ(opened.properties.standalone.at("S").value.get<std::string>(), "kept");
    const unit spaced = open_with_blocks(R"(<Property id="S" type="String" location="attachment:{0}">)"
                                         "\n\t </Property>",
                                         {bytes("kept")});
    EXPECT_TRUE(no_diagnostics(spaced.diagnostics));
    EXPECT_EQ(standalone(spaced, "S").value.get<std::string>(), "kept");
}

TEST(conformance_property_element, a_string_in_a_data_block_must_be_utf8_without_nul)
{
    // Spec §8.4.4.3: no U+0000 and nothing that is not UTF-8. "YQBi" is a, U+0000, b; "wyg=" is C3 28.
    for (const std::string_view data : {"YQBi", "wyg="}) {
        const unit opened = open_body(R"(<Property id="S" type="String" location="inline:base64">)" +
                                      std::string(data) + "</Property>");
        EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::error, errc::invalid_utf8, "/xisf/Property[1]"));
        EXPECT_TRUE(opened.properties.standalone.empty());
    }
    // In character data, U+0000 can only be a character reference, which makes the header invalid XML.
    EXPECT_TRUE(throws<openxisf::invalid_data_error>(
        errc::invalid_xml, [] { (void)open_body(R"(<Property id="S" type="String">a&#0;b</Property>)"); }));
}

TEST(conformance_property_element, vectors_and_matrices_are_in_data_blocks)
{
    // The example of spec §11.1.8, a big-endian vector in hexadecimal, and an attached matrix.
    const std::vector<std::byte> matrix = {std::byte{1}, std::byte{0}, std::byte{2}, std::byte{0},
                                           std::byte{3}, std::byte{0}, std::byte{4}, std::byte{0}};
    const unit opened = open_with_blocks(
        "<Property id=\"TestProperty\" type=\"ByteArray\" length=\"34\" location=\"inline:base64\">\n"
        "   VGhpcyBpcyBhIHRlc3QgLSBURVNUIC0gMTIzNDU2Nzg5MA==\n</Property>"
        R"(<Property id="Big" type="I16Vector" length="2" location="inline:hex" byteOrder="big">fffe0102</Property>)"
        R"(<Property id="M" type="UI16Matrix" rows="2" columns="2" location="attachment:{0}"/>)"
        R"(<Property id="C" type="C64Vector" length="1" location="embedded"><Data encoding="hex">)"
        R"(000000000000f03f00000000000000c0</Data></Property>)",
        {matrix});
    const std::string_view text = "This is a test - TEST - 1234567890";
    EXPECT_EQ(standalone(opened, "TestProperty").value,
              property_value(std::vector<std::uint8_t>(text.begin(), text.end())));
    EXPECT_EQ(standalone(opened, "Big").value, property_value(std::vector<std::int16_t>{-2, 0x0102}));
    EXPECT_EQ(standalone(opened, "M").value, property_value::matrix(2, 2, std::vector<std::uint16_t>{1, 2, 3, 4}));
    EXPECT_EQ(standalone(opened, "C").value, property_value(std::vector<std::complex<double>>{{1.0, -2.0}}));
}

TEST(conformance_property_element, empty_vectors_and_matrices_are_empty_inline_blocks)
{
    // Spec §11.1.8 and §11.1.9.
    const unit opened = open_body(R"(<Property id="V" type="F64Vector" length="0" location="inline:base64"/>)"
                                  R"(<Property id="M" type="F64Matrix" rows="0" columns="3" location="inline:hex">)"
                                  "</Property>");
    EXPECT_EQ(standalone(opened, "V").value, property_value(std::vector<double>{}));
    EXPECT_EQ(standalone(opened, "M").value, property_value::matrix(0, 3, std::vector<double>{}));
}

TEST(conformance_property_element, an_empty_vector_or_matrix_without_a_data_block_is_tolerated)
{
    const unit vector = open_body(R"(<Property id="V" type="F64Vector" length="0"/>)");
    EXPECT_TRUE(single_diagnostic(vector.diagnostics, severity::warning, errc::invalid_property, "/xisf/Property[1]"));
    EXPECT_EQ(vector.properties.standalone.at("V").value, property_value(std::vector<double>{}));
    const unit matrix = open_body(R"(<Property id="M" type="I8Matrix" rows="2" columns="0"/>)");
    EXPECT_TRUE(single_diagnostic(matrix.diagnostics, severity::warning, errc::invalid_property, "/xisf/Property[1]"));
    EXPECT_EQ(matrix.properties.standalone.at("M").value.rows(), 2U);
}

TEST(conformance_property_element, a_vector_or_matrix_with_elements_needs_a_data_block)
{
    for (const std::string_view body : {R"(<Property id="V" type="F64Vector" length="2"/>)",
                                        R"(<Property id="M" type="I8Matrix" rows="1" columns="1"/>)"}) {
        const unit opened = open_body(body);
        EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::error, errc::invalid_property, "/xisf/Property[1]"))
            << body;
        EXPECT_TRUE(opened.properties.standalone.empty());
    }
}

TEST(conformance_property_element, length_rows_and_columns_must_agree_with_the_data_block)
{
    // "AQID" is 3 bytes; "AQIDBA==" is 4.
    struct mismatch
    {
        std::string_view element{};
        errc code = errc::invalid_property;
    };
    for (const mismatch& entry : {
             mismatch{.element =
                          R"(<Property id="P" type="ByteArray" length="4" location="inline:base64">AQID</Property>)",
                      .code = errc::invalid_property_length},
             mismatch{.element =
                          R"(<Property id="P" type="UI16Vector" length="1" location="inline:base64">AQID</Property>)",
                      .code = errc::invalid_property_length},
             mismatch{.element = R"(<Property id="P" type="UI8Matrix" rows="2" columns="1" location="inline:base64">)"
                                 "AQID</Property>",
                      .code = errc::invalid_property_length},
             // 2^32 × 2^32 elements wrap around to none, the size of the block.
             mismatch{.element = R"(<Property id="P" type="F64Matrix" rows="4294967296" columns="4294967296" )"
                                 R"(location="inline:base64"></Property>)",
                      .code = errc::invalid_property_length},
             mismatch{.element = R"(<Property id="P" type="UI64Vector" length="2305843009213693952" )"
                                 R"(location="inline:base64">AQIDBA==</Property>)",
                      .code = errc::invalid_property_length},
             mismatch{.element =
                          R"(<Property id="P" type="ByteArray" length="x" location="inline:base64">AQID</Property>)",
                      .code = errc::invalid_integer},
             mismatch{.element =
                          R"(<Property id="P" type="ByteArray" length="-1" location="inline:base64">AQID</Property>)",
                      .code = errc::value_out_of_range},
             mismatch{.element =
                          R"(<Property id="P" type="UI8Matrix" rows="3" location="inline:base64">AQID</Property>)",
                      .code = errc::invalid_property},
             mismatch{.element =
                          R"(<Property id="P" type="UI8Matrix" columns="3" location="inline:base64">AQID</Property>)",
                      .code = errc::invalid_property},
         }) {
        const unit opened = open_body(entry.element);
        EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::error, entry.code, "/xisf/Property[1]"))
            << entry.element;
        EXPECT_TRUE(opened.properties.standalone.empty()) << entry.element;
    }
}

TEST(conformance_property_element, a_vector_without_a_length_takes_it_from_its_block)
{
    const unit opened = open_body(R"(<Property id="V" type="UI16Vector" location="inline:base64">AQIDBA==</Property>)");
    EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_property, "/xisf/Property[1]"));
    EXPECT_EQ(opened.properties.standalone.at("V").value, property_value(std::vector<std::uint16_t>{0x0201, 0x0403}));
    // When the block does not hold a whole number of elements, the vector is unavailable.
    const unit odd = open_body(R"(<Property id="V" type="UI16Vector" location="inline:base64">AQID</Property>)");
    ASSERT_EQ(odd.diagnostics.size(), 2U);
    EXPECT_EQ(odd.diagnostics[1].code, errc::invalid_property_length);
    EXPECT_TRUE(odd.properties.standalone.empty());
}

TEST(conformance_property_element, the_value_attribute_belongs_to_scalars_complex_numbers_and_time_points)
{
    // A scalar without one is unavailable.
    const unit missing = open_body(R"(<Property id="P" type="Int32"/>)");
    EXPECT_TRUE(single_diagnostic(missing.diagnostics, severity::error, errc::invalid_property, "/xisf/Property[1]"));
    EXPECT_EQ(missing.diagnostics.front().context.attribute, "value");
    // A string, vector or matrix with one ignores it.
    const unit string = open_body(R"(<Property id="S" type="String" value="ignored">kept</Property>)");
    EXPECT_TRUE(single_diagnostic(string.diagnostics, severity::warning, errc::invalid_property, "/xisf/Property[1]"));
    EXPECT_EQ(string.properties.standalone.at("S").value.get<std::string>(), "kept");
    // A scalar with a data block ignores the block.
    const unit block =
        open_body(R"(<Property id="P" type="Int32" value="1" location="inline:base64">AAAAAA==</Property>)");
    EXPECT_TRUE(single_diagnostic(block.diagnostics, severity::warning, errc::invalid_property, "/xisf/Property[1]"));
    EXPECT_EQ(block.properties.standalone.at("P").value, property_value(1));
}

TEST(conformance_property_element, a_value_that_its_type_cannot_hold_makes_the_property_unavailable)
{
    struct invalid
    {
        std::string_view element{};
        errc code = errc::invalid_property;
    };
    for (const invalid& entry : {
             invalid{.element = R"(<Property id="P" type="Int8" value="128"/>)", .code = errc::value_out_of_range},
             invalid{.element = R"(<Property id="P" type="UInt64" value="-1"/>)", .code = errc::value_out_of_range},
             invalid{.element = R"(<Property id="P" type="Float32" value="1e39"/>)", .code = errc::value_out_of_range},
             invalid{.element = R"(<Property id="P" type="Float128" value="1e4933"/>)",
                     .code = errc::value_out_of_range},
             invalid{.element = R"(<Property id="P" type="Boolean" value="yes"/>)", .code = errc::invalid_boolean},
             invalid{.element = R"(<Property id="P" type="UInt16" value="1.5"/>)", .code = errc::invalid_integer},
             invalid{.element = R"(<Property id="P" type="Float64" value="1."/>)", .code = errc::invalid_float},
             invalid{.element = R"(<Property id="P" type="Complex32" value="1"/>)", .code = errc::invalid_complex},
             invalid{.element = R"(<Property id="P" type="TimePoint" value="yesterday"/>)",
                     .code = errc::invalid_time_point},
         }) {
        const unit opened = open_body(std::string(entry.element) + R"(<Property id="Q" type="Int32" value="2"/>)");
        EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::error, entry.code, "/xisf/Property[1]"))
            << entry.element;
        EXPECT_EQ(opened.diagnostics.front().context.attribute, "value");
        // The other properties stay readable (spec §7).
        EXPECT_EQ(opened.properties.standalone.size(), 1U);
        EXPECT_TRUE(opened.properties.standalone.contains("Q"));
    }
}

TEST(conformance_property_element, float128_and_complex128_scalars_keep_their_text)
{
    const unit opened =
        open_body(R"(<Property id="Q" type="Float128" value=" 1.18973149535723176508575932662800702e4932 "/>)"
                  R"x(<Property id="C" type="Complex128" value="(1e-4950,-2)"/>)x");
    EXPECT_EQ(standalone(opened, "Q").value,
              property_value::from_float128_text("1.18973149535723176508575932662800702e4932"));
    EXPECT_EQ(standalone(opened, "C").value, property_value::from_complex128_text("(1e-4950,-2)"));
}

TEST(conformance_property_element, data_blocks_of_128_bit_elements)
{
    // 1 and 2 as Float128, big-endian, and -1 as Int128.
    const unit opened = open_body(
        R"(<Property id="F" type="F128Vector" length="2" byteOrder="big" location="inline:hex">)"
        "3fff0000000000000000000000000000"
        "40000000000000000000000000000000</Property>"
        R"(<Property id="I" type="I128Vector" length="1" location="inline:hex">ffffffffffffffffffffffffffffffff</Property>)");
    const std::span<const openxisf::float128> quads = standalone(opened, "F").value.elements<openxisf::float128>();
    ASSERT_EQ(quads.size(), 2U);
    EXPECT_EQ(openxisf::to_double(quads[0]), 1.0);
    EXPECT_EQ(openxisf::to_double(quads[1]), 2.0);
    EXPECT_EQ(standalone(opened, "I").value.elements<openxisf::int128>()[0],
              (openxisf::int128{.high = -1, .low = ~std::uint64_t{0}}));
}

// -------------------------------------------------------------------------------------------------------------------
// Identifiers and types

TEST(conformance_property_element, a_property_needs_an_identifier_and_a_type)
{
    struct problem
    {
        std::string_view element{};
        errc code = errc::invalid_property;
        std::string_view attribute{};
    };
    for (const problem& entry : {
             problem{.element = R"(<Property type="Int32" value="1"/>)",
                     .code = errc::invalid_property_id,
                     .attribute = "id"},
             problem{.element = R"(<Property id="" type="Int32" value="1"/>)",
                     .code = errc::invalid_property_id,
                     .attribute = "id"},
             problem{.element = R"(<Property id="P" value="1"/>)", .code = errc::invalid_property, .attribute = "type"},
             problem{.element = R"(<Property id="P" type="Float16" value="1"/>)",
                     .code = errc::unsupported_property_type,
                     .attribute = "type"},
             problem{.element = R"(<Property id="P" type="int32" value="1"/>)",
                     .code = errc::unsupported_property_type,
                     .attribute = "type"},
             problem{.element = R"(<Property id="P" type="Table"/>)",
                     .code = errc::unsupported_property_type,
                     .attribute = "type"},
         }) {
        const unit opened = open_body(entry.element);
        EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::error, entry.code, "/xisf/Property[1]"))
            << entry.element;
        EXPECT_EQ(opened.diagnostics.front().context.attribute, entry.attribute);
        EXPECT_TRUE(opened.properties.standalone.empty());
    }
    // In strict mode, an unknown type is unsupported, and a missing identifier invalid.
    EXPECT_TRUE(throws<openxisf::unsupported_error>(errc::unsupported_property_type, [] {
        (void)open_body(R"(<Property id="P" type="Float16" value="1"/>)", {.strict = true});
    }));
    EXPECT_TRUE(throws<openxisf::invalid_data_error>(
        errc::invalid_property_id, [] { (void)open_body(R"(<Property type="Int32" value="1"/>)", {.strict = true}); }));
}

TEST(conformance_property_element, an_identifier_outside_the_grammar_is_tolerated)
{
    // Spec §8.4.1. The identifier is still unambiguous.
    const unit opened = open_body(R"(<Property id="Température:1a" type="Int32" value="1"/>)");
    EXPECT_TRUE(
        single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_property_id, "/xisf/Property[1]"));
    EXPECT_TRUE(opened.properties.standalone.contains("Température:1a"));
}

TEST(conformance_property_element, identifiers_are_unique_per_object)
{
    // Spec §8.4.1: the first property keeps the identifier, the second is unavailable.
    const unit opened = open_body(openxisf::test::image_xml(
        {}, R"(<Property id="P" type="Int32" value="1"/><Property id="P" type="Int32" value="2"/>)"));
    EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::error, errc::duplicate_property_id,
                                  "/xisf/Image[1]/Property[2]"));
    EXPECT_EQ(properties_of(opened, "/xisf/Image[1]").at("P").value, property_value(1));
    // Different objects may have properties with the same identifier, and so may an object and the unit.
    const unit separate = open_body(openxisf::test::image_xml({}, R"(<Property id="P" type="Int32" value="1"/>)") +
                                    openxisf::test::image_xml({}, R"(<Property id="P" type="Int32" value="2"/>)") +
                                    R"(<Property id="P" type="Int32" value="3"/>)");
    EXPECT_TRUE(no_diagnostics(separate.diagnostics));
    EXPECT_EQ(properties_of(separate, "/xisf/Image[1]").at("P").value, property_value(1));
    EXPECT_EQ(properties_of(separate, "/xisf/Image[2]").at("P").value, property_value(2));
    EXPECT_EQ(separate.properties.standalone.at("P").value, property_value(3));
    // A Reference that brings a second property of the same identifier is the duplicate.
    const unit referenced = open_body(
        openxisf::test::image_xml({}, R"(<Property id="P" type="Int32" value="1"/><Reference ref="other"/>)") +
        R"(<Property uid="other" id="P" type="Int32" value="2"/>)");
    EXPECT_TRUE(single_diagnostic(referenced.diagnostics, severity::error, errc::duplicate_property_id,
                                  "/xisf/Image[1]/Reference[1]"));
    EXPECT_EQ(referenced.diagnostics.front().context.attribute, "ref");
    EXPECT_EQ(properties_of(referenced, "/xisf/Image[1]").at("P").value, property_value(1));
}

TEST(conformance_property_element, standalone_properties_of_one_identifier_are_tolerated)
{
    // Spec §8.4: standalone properties are associated with no object, so their identifiers need not be unique. The
    // reader returns one of each identifier, the first, with a warning, and each image gets the property that its
    // Reference names.
    const std::string body = R"(<Property uid="red" id="Instrument:Filter:Name" type="String">R</Property>)"
                             R"(<Property uid="green" id="Instrument:Filter:Name" type="String">G</Property>)" +
                             openxisf::test::image_xml({}, R"(<Reference ref="red"/>)") +
                             openxisf::test::image_xml({}, R"(<Reference ref="green"/>)");
    const unit opened = open_body(body, {.strict = true});
    EXPECT_TRUE(
        single_diagnostic(opened.diagnostics, severity::warning, errc::duplicate_property_id, "/xisf/Property[2]"));
    EXPECT_EQ(opened.diagnostics.front().context.attribute, "id");
    ASSERT_EQ(opened.properties.standalone.size(), 1U);
    EXPECT_EQ(opened.properties.standalone.at("Instrument:Filter:Name").value, property_value("R"));
    EXPECT_EQ(properties_of(opened, "/xisf/Image[1]").at("Instrument:Filter:Name").value, property_value("R"));
    EXPECT_EQ(properties_of(opened, "/xisf/Image[2]").at("Instrument:Filter:Name").value, property_value("G"));
}

TEST(conformance_property_element, a_property_associated_twice_with_one_object_is_the_same_property)
{
    // Contained and named, named and contained, or named twice: the object has the property once, and the Reference
    // that associates it again is a warning that says so.
    struct association
    {
        std::string body{};
        std::string reference{};
    };
    const std::string property = R"(<Property uid="p" id="A" type="Int32" value="1"/>)";
    for (const association& entry : {
             association{.body = openxisf::test::image_xml({}, property + R"(<Reference ref="p"/>)"),
                         .reference = "/xisf/Image[1]/Reference[1]"},
             association{.body = openxisf::test::image_xml({}, R"(<Reference ref="p"/>)" + property),
                         .reference = "/xisf/Image[1]/Reference[1]"},
             association{.body =
                             property + openxisf::test::image_xml({}, R"(<Reference ref="p"/><Reference ref="p"/>)"),
                         .reference = "/xisf/Image[1]/Reference[2]"},
         }) {
        const unit opened = open_body(entry.body, {.strict = true});
        ASSERT_TRUE(single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_reference, entry.reference))
            << entry.body;
        EXPECT_EQ(opened.diagnostics.front().context.attribute, "ref");
        EXPECT_NE(opened.diagnostics.front().message.find("same property"), std::string::npos)
            << opened.diagnostics.front().message;
        const property_list& image = properties_of(opened, "/xisf/Image[1]");
        ASSERT_EQ(image.size(), 1U) << entry.body;
        EXPECT_EQ(image.at("A").value, property_value(1));
    }
    // The metadata, too.
    const unit metadata = open_body(property + R"(<Metadata><Reference ref="p"/><Reference ref="p"/></Metadata>)");
    EXPECT_EQ(std::ranges::count(metadata.diagnostics, errc::invalid_reference, &openxisf::diagnostic::code), 1)
        << openxisf::test::describe(metadata.diagnostics);
}

TEST(conformance_property_element, values_of_the_image_namespace_that_the_specification_forbids_are_tolerated)
{
    // Spec §11.5.3.5: a frame number from one, group and subgroup identifiers of [_a-zA-Z0-9][_a-zA-Z0-9-.]*, and a
    // subgroup only with a group. Other values are kept, with a warning.
    const unit valid =
        open_body(openxisf::test::image_xml({}, R"(<Property id="Image:FrameNumber" type="UInt32" value="1"/>)"
                                                R"(<Property id="Image:GroupId" type="String">M42_2026-10.L</Property>)"
                                                R"(<Property id="Image:SubgroupId" type="String">9</Property>)"));
    EXPECT_TRUE(no_diagnostics(valid.diagnostics));
    struct problem
    {
        std::string content{};
        std::string element{};
        std::string_view attribute{};
    };
    const std::string group = R"(<Property id="Image:GroupId" type="String">L</Property>)";
    for (const problem& entry : {
             problem{.content = R"(<Property id="Image:FrameNumber" type="UInt32" value="0"/>)",
                     .element = "/xisf/Image[1]/Property[1]",
                     .attribute = "value"},
             problem{.content = R"(<Property id="Image:GroupId" type="String">M42 L</Property>)",
                     .element = "/xisf/Image[1]/Property[1]",
                     .attribute = "value"},
             problem{.content = R"(<Property id="Image:GroupId" type="String">.L</Property>)",
                     .element = "/xisf/Image[1]/Property[1]",
                     .attribute = "value"},
             problem{.content = R"(<Property id="Image:GroupId" type="String"></Property>)",
                     .element = "/xisf/Image[1]/Property[1]",
                     .attribute = "value"},
             problem{.content = group + R"(<Property id="Image:SubgroupId" type="String">a/b</Property>)",
                     .element = "/xisf/Image[1]/Property[2]",
                     .attribute = "value"},
             problem{.content = R"(<Property id="Image:SubgroupId" type="String">flat</Property>)",
                     .element = "/xisf/Image[1]/Property[1]",
                     .attribute = "id"},
             problem{.content = openxisf::test::thumbnail_xml(
                         {}, R"(<Property id="Image:FrameNumber" type="UInt32" value="0"/>)"),
                     .element = "/xisf/Image[1]/Thumbnail[1]/Property[1]",
                     .attribute = "value"},
         }) {
        const unit opened = open_body(openxisf::test::image_xml({}, entry.content), {.strict = true});
        ASSERT_TRUE(single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_property, entry.element))
            << entry.content;
        EXPECT_EQ(opened.diagnostics.front().context.attribute, entry.attribute) << entry.content;
    }
    const unit kept =
        open_body(openxisf::test::image_xml({}, R"(<Property id="Image:FrameNumber" type="UInt32" value="0"/>)"));
    EXPECT_EQ(properties_of(kept, "/xisf/Image[1]").at("Image:FrameNumber").value, property_value(std::uint32_t{0}));
}

TEST(conformance_property_element, a_reserved_identifier_with_another_type_is_tolerated)
{
    // Spec §11.5.3: Instrument:ExposureTime is a Float32.
    const unit opened = open_body(R"(<Property id="Instrument:ExposureTime" type="Float64" value="300"/>)");
    EXPECT_TRUE(
        single_diagnostic(opened.diagnostics, severity::warning, errc::reserved_property_type, "/xisf/Property[1]"));
    EXPECT_EQ(opened.properties.standalone.at("Instrument:ExposureTime").value, property_value(300.0));
}

TEST(conformance_property_element, a_reserved_time_point_written_as_a_string_is_read_as_the_time_point_it_holds)
{
    // Spec §11.5.3: Observation:Time:Start is a TimePoint. A String that holds one, the form in which PixInsight writes
    // XISF:CreationTime, is read as the TimePoint, with an info diagnostic; a String that holds none is kept, with a
    // warning.
    const unit held =
        open_body(R"(<Property id="Observation:Time:Start" type="String"> 2026-01-02T03:04:05.5Z </Property>)");
    EXPECT_TRUE(single_diagnostic(held.diagnostics, severity::info, errc::reserved_property_type, "/xisf/Property[1]"));
    EXPECT_EQ(held.diagnostics.front().context.attribute, "type");
    EXPECT_EQ(held.properties.standalone.at("Observation:Time:Start").value,
              property_value(date_time{
                  .year = 2026, .month = 1, .day = 2, .hour = 3, .minute = 4, .second = 5, .nanosecond = 500'000'000}));
    // As a TimePoint, it has no format specifier (spec §8.4.3.1): the String's is dropped, with a warning.
    const unit formatted = open_body(
        R"(<Property id="Observation:Time:Start" type="String" format="width:30">2026-01-02T03:04:05Z</Property>)");
    ASSERT_EQ(formatted.diagnostics.size(), 2U) << openxisf::test::describe(formatted.diagnostics);
    EXPECT_EQ(formatted.diagnostics[0].code, errc::reserved_property_type);
    EXPECT_TRUE(single_diagnostic(std::span(formatted.diagnostics).subspan(1), severity::warning,
                                  errc::invalid_format_specifier, "/xisf/Property[1]"));
    const property& start = formatted.properties.standalone.at("Observation:Time:Start");
    EXPECT_EQ(start.value.type(), property_type::time_point);
    EXPECT_FALSE(start.format.has_value());
    const unit other = open_body(R"(<Property id="Observation:Time:Start" type="String">dawn</Property>)");
    EXPECT_TRUE(
        single_diagnostic(other.diagnostics, severity::warning, errc::reserved_property_type, "/xisf/Property[1]"));
    EXPECT_EQ(other.properties.standalone.at("Observation:Time:Start").value, property_value("dawn"));
    // A String property of another reserved identifier, and the properties of an astrometric solution, whose types are
    // checked as a set, are not converted.
    const unit application = open_body(R"(<Property id="XISF:CreatorApplication" type="String">2026</Property>)");
    EXPECT_TRUE(no_diagnostics(application.diagnostics));
    const unit solution = open_body(openxisf::test::image_xml(
        {}, R"(<Property id="AstrometricSolution:CreationTime" type="String">2026-01-02T03:04:05Z</Property>)"));
    EXPECT_TRUE(single_diagnostic(solution.diagnostics, severity::warning, errc::reserved_property_type,
                                  "/xisf/Image[1]/Property[1]"));
    EXPECT_EQ(properties_of(solution, "/xisf/Image[1]").at("AstrometricSolution:CreationTime").value.type(),
              property_type::string);
}

TEST(conformance_property_element, an_astrometric_solution_of_another_revision_has_types_of_its_own)
{
    // Spec §11.5.3.7.6: a major revision of the namespace may change anything; the types of spec §11.5.3.7 are those of
    // revision 1.
    const std::string reference =
        R"(<Property id="AstrometricSolution:ReferenceCelestialCoordinates" type="String">83.8 -5.4</Property>)";
    const auto version = [](std::string_view text) {
        return R"(<Property id="AstrometricSolution:Version" type="String">)" + std::string(text) + "</Property>";
    };
    const unit foreign = open_body(openxisf::test::image_xml({}, version("2.0") + reference));
    EXPECT_TRUE(no_diagnostics(foreign.diagnostics));
    const unit current = open_body(openxisf::test::image_xml({}, version("1.0") + reference));
    EXPECT_TRUE(single_diagnostic(current.diagnostics, severity::warning, errc::reserved_property_type,
                                  "/xisf/Image[1]/Property[2]"));
    // A Property element that two images name is checked once, and a solution without a version is of revision 1.
    const unit shared = open_body(openxisf::test::image_xml({}, R"(<Reference ref="center"/>)") +
                                  openxisf::test::image_xml({}, R"(<Reference ref="center"/>)") +
                                  R"(<Property uid="center" id="AstrometricSolution:ReferenceImageCoordinates" )"
                                  R"(type="Float64" value="1"/>)");
    EXPECT_TRUE(
        single_diagnostic(shared.diagnostics, severity::warning, errc::reserved_property_type, "/xisf/Property[1]"));
    // A property of the namespace in the Metadata element, where it does not belong, has its type checked too.
    const unit misplaced = open_internal(
        monolithic_file(header_xml({}, R"(<Property id="AstrometricSolution:Version" type="Float64" value="1"/>)")));
    ASSERT_EQ(misplaced.diagnostics.size(), 2U);
    EXPECT_EQ(misplaced.diagnostics[0].code, errc::invalid_metadata);
    EXPECT_EQ(misplaced.diagnostics[1].code, errc::reserved_property_type);
    EXPECT_EQ(misplaced.diagnostics[1].context.element, "/xisf/Metadata[1]/Property[3]");
}

// -------------------------------------------------------------------------------------------------------------------
// Placement (spec §11.1)

TEST(conformance_property_element, properties_belong_to_the_element_that_contains_them)
{
    const unit opened =
        open_body(openxisf::test::image_xml({}, R"(<Property id="Image:P" type="Int32" value="1"/>)" +
                                                    openxisf::test::thumbnail_xml(
                                                        {}, R"(<Property id="Thumbnail:P" type="Int32" value="2"/>)")) +
                  R"(<Property id="Standalone:P" type="Int32" value="3"/>)");
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    const property_list& image = properties_of(opened, "/xisf/Image[1]");
    ASSERT_EQ(image.size(), 1U);
    EXPECT_TRUE(image.contains("Image:P"));
    const property_list& thumbnail = properties_of(opened, "/xisf/Image[1]/Thumbnail[1]");
    ASSERT_EQ(thumbnail.size(), 1U);
    EXPECT_TRUE(thumbnail.contains("Thumbnail:P"));
    ASSERT_EQ(opened.properties.standalone.size(), 1U);
    EXPECT_TRUE(opened.properties.standalone.contains("Standalone:P"));
    EXPECT_FALSE(opened.properties.metadata.contains("Standalone:P"));
}

TEST(conformance_property_element, a_reference_associates_a_property_with_more_objects)
{
    // Spec §11.1 and §11.13: a standalone property, shared by two images and the metadata, stays standalone.
    const unit opened = open_body(R"(<Property uid="shared" id="XISF:Title" type="String">M 31</Property>)" +
                                  openxisf::test::image_xml({}, R"(<Reference ref="shared"/>)") +
                                  openxisf::test::image_xml({}, R"(<Reference ref="shared"/>)"));
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    EXPECT_EQ(properties_of(opened, "/xisf/Image[1]").at("XISF:Title").value.get<std::string>(), "M 31");
    EXPECT_EQ(properties_of(opened, "/xisf/Image[2]").at("XISF:Title").value.get<std::string>(), "M 31");
    EXPECT_EQ(opened.properties.standalone.at("XISF:Title").value.get<std::string>(), "M 31");
    // A reference can point forward and backward, and to a property inside another image.
    const unit inside =
        open_body(openxisf::test::image_xml({}, R"(<Reference ref="later"/>)") +
                  openxisf::test::image_xml({}, R"(<Property uid="later" id="P" type="Int32" value="7"/>)"));
    EXPECT_TRUE(no_diagnostics(inside.diagnostics));
    EXPECT_EQ(properties_of(inside, "/xisf/Image[1]").at("P").value, property_value(7));
    EXPECT_EQ(properties_of(inside, "/xisf/Image[2]").at("P").value, property_value(7));
}

// -------------------------------------------------------------------------------------------------------------------
// Format specifiers and comments (spec §11.1.2)

TEST(conformance_property_element, format_specifiers_and_comments_are_kept)
{
    const unit opened = open_body(
        R"(<Property id="Magnitude" type="Float32" value="1.25" format="width:6;precision:2;float:fixed;sign:force" )"
        R"(comment="Visual magnitude"/>)");
    const property& magnitude = standalone(opened, "Magnitude");
    EXPECT_EQ(magnitude.format, (property_format{.width = 6,
                                                 .sign = openxisf::format_sign::force,
                                                 .precision = 2,
                                                 .notation = openxisf::format_notation::fixed}));
    EXPECT_EQ(magnitude.comment, "Visual magnitude");
    EXPECT_FALSE(standalone(open_body(R"(<Property id="P" type="Int32" value="1"/>)"), "P").format.has_value());
}

TEST(conformance_property_element, a_malformed_format_specifier_is_dropped)
{
    for (const std::string_view element :
         {R"(<Property id="P" type="Int32" value="1" format="width:x"/>)",
          R"(<Property id="P" type="TimePoint" value="2026-01-01" format="width:8"/>)"}) {
        const unit opened = open_body(element);
        EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_format_specifier,
                                      "/xisf/Property[1]"))
            << element;
        EXPECT_FALSE(opened.properties.standalone.at("P").format.has_value());
    }
}

// -------------------------------------------------------------------------------------------------------------------
// Values in data blocks, loaded when the unit opens

TEST(conformance_property_element, a_property_with_an_unavailable_block_is_unavailable)
{
    // The block has the only diagnostic.
    const unit opened =
        open_body(R"(<Property id="P" type="ByteArray" length="3" location="inline:base64">A!ID</Property>)");
    EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::error, errc::invalid_base64, "/xisf/Property[1]"));
    EXPECT_TRUE(opened.properties.standalone.empty());
}

TEST(conformance_property_element, a_property_whose_block_does_not_decompress_is_unavailable)
{
    // Equal sizes would mean a block stored as it is. A vector and a String, whose blocks are read apart.
    for (const std::string_view type : {R"(type="ByteArray" length="4")", R"(type="String")"}) {
        const std::string body =
            R"(<Property id="P" )" + std::string(type) + R"( location="attachment:{0}" compression="zlib:4"/>)";
        const unit opened = open_with_blocks(body, {bytes("abc")});
        EXPECT_TRUE(
            single_diagnostic(opened.diagnostics, severity::error, errc::corrupt_compressed_data, "/xisf/Property[1]"))
            << type;
        EXPECT_EQ(opened.diagnostics.front().context.offset, 4096U) << type;
        EXPECT_TRUE(opened.properties.standalone.empty()) << type;
        EXPECT_TRUE(throws<openxisf::integrity_error>(errc::corrupt_compressed_data, [&body] {
            (void)open_with_blocks(body, {bytes("abc")}, {.strict = true});
        })) << type;
    }
}

TEST(conformance_property_element, the_data_loaded_at_open_is_limited)
{
    // Two blocks of 16 bytes against a limit of 24: the first loads, the second does not.
    const std::string body = R"(<Property id="A" type="ByteArray" length="16" location="attachment:{0}"/>)"
                             R"(<Property id="B" type="ByteArray" length="16" location="attachment:{0}"/>)";
    const std::vector<std::byte> block = openxisf::test::pattern(16);
    const unit opened = open_with_blocks(body, {block}, {.limits = {.max_ancillary_data = 24}});
    EXPECT_TRUE(
        single_diagnostic(opened.diagnostics, severity::error, errc::ancillary_data_too_large, "/xisf/Property[2]"));
    EXPECT_TRUE(opened.properties.standalone.contains("A"));
    EXPECT_FALSE(opened.properties.standalone.contains("B"));
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::ancillary_data_too_large, [&] {
        (void)open_with_blocks(body, {block}, {.strict = true, .limits = {.max_ancillary_data = 24}});
    }));
    EXPECT_TRUE(no_diagnostics(open_with_blocks(body, {block}, {.limits = {.max_ancillary_data = 32}}).diagnostics));
    EXPECT_TRUE(no_diagnostics(open_with_blocks(body, {block}, {.limits = {.max_ancillary_data = 0}}).diagnostics));

    // A compressed block counts with its uncompressed size, before it is decompressed.
    const std::vector<std::byte> zeros(1000);
    const openxisf::detail::compressed_block compressed =
        openxisf::detail::compress_block(zeros, {.codec = openxisf::detail::compression_codec::zlib});
    const unit large = open_with_blocks(
        R"(<Property id="Z" type="ByteArray" length="1000" location="attachment:{0}" compression="zlib:1000"/>)",
        {compressed.data}, {.limits = {.max_ancillary_data = 999}});
    EXPECT_TRUE(
        single_diagnostic(large.diagnostics, severity::error, errc::ancillary_data_too_large, "/xisf/Property[1]"));

    // A block that fails to load holds nothing, so it leaves the limit to the blocks after it.
    const unit failed = open_with_blocks(R"(<Property id="A" type="ByteArray" length="16" location="attachment:{0}" )"
                                         R"(checksum="sha1:0000000000000000000000000000000000000000"/>)"
                                         R"(<Property id="B" type="ByteArray" length="16" location="attachment:{0}"/>)",
                                         {block}, {.limits = {.max_ancillary_data = 24}});
    EXPECT_TRUE(single_diagnostic(failed.diagnostics, severity::error, errc::checksum_mismatch, "/xisf/Property[1]"));
    EXPECT_FALSE(failed.properties.standalone.contains("A"));
    EXPECT_TRUE(failed.properties.standalone.contains("B"));

    // So does a block that loads, but whose value is refused: a String that is not UTF-8.
    const std::vector<std::byte> not_utf8(16, std::byte{0xFF});
    const unit refused =
        open_with_blocks(R"(<Property id="S" type="String" location="attachment:{0}"/>)"
                         R"(<Property id="B" type="ByteArray" length="16" location="attachment:{1}"/>)",
                         {not_utf8, block}, {.limits = {.max_ancillary_data = 24}});
    EXPECT_TRUE(single_diagnostic(refused.diagnostics, severity::error, errc::invalid_utf8, "/xisf/Property[1]"));
    EXPECT_FALSE(refused.properties.standalone.contains("S"));
    EXPECT_TRUE(refused.properties.standalone.contains("B"));
}

TEST(conformance_property_element, the_copies_that_references_make_count_against_the_limit_of_loaded_data)
{
    // A standalone String of 100 characters named by the References of two images: the root element and the first image
    // take copies of the property and its 109 bytes each (with the identifier), and the second image takes the property
    // itself. The copies cannot multiply the memory that a unit takes beyond the limit.
    constexpr std::uint64_t copy = sizeof(property) + 109;
    const std::string body = R"(<Property id="Test:Text" uid="text" type="String">)" + std::string(100, 'a') +
                             "</Property>" + openxisf::test::image_xml({}, R"(<Reference ref="text"/>)") +
                             openxisf::test::image_xml({}, R"(<Reference ref="text"/>)");
    EXPECT_TRUE(no_diagnostics(open_body(body, {.limits = {.max_ancillary_data = 2 * copy}}).diagnostics));
    const unit limited = open_body(body, {.limits = {.max_ancillary_data = (2 * copy) - 1}});
    EXPECT_TRUE(single_diagnostic(limited.diagnostics, severity::error, errc::ancillary_data_too_large,
                                  "/xisf/Image[1]/Reference[1]"));
    EXPECT_TRUE(limited.properties.standalone.contains("Test:Text"));
    EXPECT_FALSE(openxisf::test::properties_of(limited, "/xisf/Image[1]").contains("Test:Text"));
    EXPECT_TRUE(openxisf::test::properties_of(limited, "/xisf/Image[2]").contains("Test:Text"));
}

TEST(conformance_property_element, the_reader_returns_the_standalone_properties)
{
    const openxisf::reader file =
        openxisf::test::open_header(header_xml(R"(<Property id="P" type="UInt8" value="7"/>)"));
    ASSERT_EQ(file.properties().size(), 1U);
    EXPECT_EQ(file.properties().at("P").value, property_value(std::uint8_t{7}));
}

} // namespace
