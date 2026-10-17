// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §8.4.4.7, §11.2 and §11.3: Structure, Field, Table, Row and Cell elements, on constructed units.

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>

#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::property_type;
using openxisf::property_value;
using openxisf::reader;
using openxisf::severity;
using openxisf::table;
using openxisf::table_field;
using openxisf::test::header_xml;
using openxisf::test::image_xml;
using openxisf::test::no_diagnostics;
using openxisf::test::single_diagnostic;

reader open_body(std::string_view body, openxisf::read_options options = {})
{
    return openxisf::test::open_header(header_xml(body), std::move(options));
}

reader open_attached(std::string_view body, const std::vector<std::vector<std::byte>>& blocks,
                     openxisf::read_options options = {})
{
    return openxisf::test::open_unit(openxisf::test::file_with_attachments(header_xml(body), blocks),
                                     std::move(options));
}

// The Structure element of the example of spec §11.2.1.
constexpr std::string_view messier_structure =
    R"(<Structure uid="MessierCatalogStructure">)"
    R"(<Field id="number" type="UInt8" header="Messier Number"/>)"
    R"(<Field id="ngc_ic" type="String" header="NGC/IC"/>)"
    R"(<Field id="commonName" type="String" header="Common Name"/>)"
    R"(<Field id="objectType" type="String" header="Object Type"/>)"
    R"(<Field id="distance" type="Float32" header="Distance" format="float:fixed;precision:2;unit:kly"/>)"
    R"(<Field id="constellation" type="String" header="Constellation"/>)"
    R"(<Field id="magnitude" type="Float32" header="Apparent Magnitude" format="float:fixed;precision:1"/>)"
    R"(</Structure>)";

// The Table element of the example of spec §11.3.3, with the rows that it shows: M1, M2, M3 and M31.
constexpr std::string_view messier_table =
    R"(<Table id="MessierCatalog" caption="The Messier Catalog" rows="4" columns="7">)"
    R"(<Reference ref="MessierCatalogStructure"/>)"
    R"(<Row><Cell value="1"/><Cell>NGC 1952</Cell><Cell>Crab Nebula</Cell><Cell>Supernova remnant</Cell>)"
    R"(<Cell value="6.5"/><Cell>Taurus</Cell><Cell value="8.4"/></Row>)"
    R"(<Row><Cell value="2"/><Cell>NGC 7089</Cell><Cell/><Cell>Globular cluster</Cell><Cell value="33"/>)"
    R"(<Cell>Aquarius</Cell><Cell value="6.3"/></Row>)"
    R"(<Row><Cell value="3"/><Cell>NGC 5272</Cell><Cell/><Cell>Globular cluster</Cell><Cell value="33.9"/>)"
    R"(<Cell>Canes Venatici</Cell><Cell value="6.2"/></Row>)"
    R"(<Row><Cell value="31"/><Cell>NGC 224</Cell><Cell>Andromeda Galaxy</Cell><Cell>Spiral galaxy</Cell>)"
    R"(<Cell value="2540"/><Cell>Andromeda</Cell><Cell value="3.4"/></Row>)"
    R"(</Table>)";

std::vector<property_value> messier_row(std::uint8_t number, std::string_view ngc, std::string_view name,
                                        std::string_view type, float distance, std::string_view constellation,
                                        float magnitude)
{
    return {property_value(number),   property_value(ngc),           property_value(name),     property_value(type),
            property_value(distance), property_value(constellation), property_value(magnitude)};
}

// A table of one String field with the given content after its structure.
std::string string_table(std::string_view rows, std::string_view attributes = {})
{
    return R"(<Table id="T" )" + std::string(attributes) + R"(><Structure><Field id="f" type="String"/></Structure>)" +
           std::string(rows) + "</Table>";
}

// The only diagnostic is an error with code about the element at path, and the root element has no table.
testing::AssertionResult unavailable(const reader& file, errc code, std::string_view path)
{
    testing::AssertionResult found = single_diagnostic(file.diagnostics(), severity::error, code, path);
    if (found && !file.tables().empty()) {
        return testing::AssertionFailure() << "the table is listed";
    }
    return found;
}

// -----------------------------------------------------------------------------------------------------------------
// The examples of the specification

TEST(conformance_table, the_messier_table_of_the_specification_is_read)
{
    const reader file = open_body(std::string(messier_structure) + std::string(messier_table));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.tables().size(), 1U);
    const table& messier = file.tables()[0];
    EXPECT_EQ(messier.id, "MessierCatalog");
    EXPECT_EQ(messier.caption, "The Messier Catalog");
    EXPECT_EQ(messier.comment, "");
    ASSERT_EQ(messier.fields.size(), 7U);
    EXPECT_EQ(messier.fields[0],
              (table_field{.id = "number", .type = property_type::uint8, .header = "Messier Number"}));
    EXPECT_EQ(messier.fields[4].type, property_type::float32);
    EXPECT_EQ(messier.fields[4].format,
              (openxisf::property_format{.precision = 2, .notation = openxisf::format_notation::fixed, .unit = "kly"}));
    EXPECT_EQ(messier.fields[6].header, "Apparent Magnitude");

    const std::vector<std::vector<property_value>> rows{
        messier_row(1, "NGC 1952", "Crab Nebula", "Supernova remnant", 6.5F, "Taurus", 8.4F),
        messier_row(2, "NGC 7089", "", "Globular cluster", 33.0F, "Aquarius", 6.3F),
        messier_row(3, "NGC 5272", "", "Globular cluster", 33.9F, "Canes Venatici", 6.2F),
        messier_row(31, "NGC 224", "Andromeda Galaxy", "Spiral galaxy", 2540.0F, "Andromeda", 3.4F)};
    EXPECT_EQ(messier.rows, rows);
}

TEST(conformance_table, cells_in_data_blocks_are_read_like_property_values)
{
    // The second example of spec §11.3.3, smaller: ByteArray cells in attached blocks, compressed and with checksums
    // in the specification, here plain, and a String cell in an inline block.
    const reader file = open_attached(
        R"(<Table id="obsvar_2016" caption="Observations of Selected Variable Stars in 2016"><Structure>)"
        R"(<Field id="starName" type="String" header="Star"/><Field id="numObs" type="UInt32"/>)"
        R"(<Field id="data" type="ByteArray"/></Structure>)"
        R"(<Row><Cell>SS Cyg</Cell><Cell value="132729"/><Cell length="4" location="attachment:{0}"/></Row>)"
        R"(<Row><Cell location="inline:base64">UiBDckI=</Cell><Cell value="58135"/>)"
        R"(<Cell length="3" location="attachment:{1}"/></Row></Table>)",
        {openxisf::test::bytes("abcd"), openxisf::test::bytes("xyz")});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.tables().size(), 1U);
    const table& observations = file.tables()[0];
    EXPECT_EQ(observations.fields[2].type, property_type::ui8_vector);
    ASSERT_EQ(observations.rows.size(), 2U);
    EXPECT_EQ(observations.rows[0][0], property_value("SS Cyg"));
    EXPECT_EQ(observations.rows[0][1], property_value(std::uint32_t{132729}));
    EXPECT_EQ(observations.rows[0][2], property_value(std::vector<std::uint8_t>{'a', 'b', 'c', 'd'}));
    EXPECT_EQ(observations.rows[1][0], property_value("R CrB"));
    EXPECT_EQ(observations.rows[1][2], property_value(std::vector<std::uint8_t>{'x', 'y', 'z'}));
}

TEST(conformance_table, an_empty_table_has_no_rows)
{
    const reader file = open_body(string_table({}, R"(rows="0")"));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.tables().size(), 1U);
    EXPECT_TRUE(file.tables()[0].rows.empty());
    EXPECT_EQ(file.tables()[0].fields.size(), 1U);
}

// -----------------------------------------------------------------------------------------------------------------
// Placement and identifiers (spec §11.3)

TEST(conformance_table, a_table_belongs_to_the_element_that_contains_it)
{
    // Spec §11.12: a Thumbnail element is an Image element under another name, so it has tables of its own.
    const reader file = open_body(
        image_xml({}, string_table("<Row><Cell>image</Cell></Row>") +
                          openxisf::test::thumbnail_xml({}, string_table("<Row><Cell>thumbnail</Cell></Row>"))) +
        string_table("<Row><Cell>standalone</Cell></Row>"));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.image(0).tables.size(), 1U);
    EXPECT_EQ(file.image(0).tables[0].rows.at(0).at(0), property_value("image"));
    const openxisf::thumbnail& small = file.image(0).thumbnail.value_or(openxisf::thumbnail{});
    ASSERT_EQ(small.tables.size(), 1U);
    EXPECT_EQ(small.tables[0].rows.at(0).at(0), property_value("thumbnail"));
    ASSERT_EQ(file.tables().size(), 1U);
    EXPECT_EQ(file.tables()[0].rows.at(0).at(0), property_value("standalone"));
}

TEST(conformance_table, a_table_has_a_property_identifier)
{
    const reader without = open_body(R"(<Table><Structure><Field id="f" type="String"/></Structure></Table>)");
    EXPECT_TRUE(unavailable(without, errc::invalid_property_id, "/xisf/Table[1]"));
    const reader malformed =
        open_body(R"(<Table id="1st"><Structure><Field id="f" type="String"/></Structure></Table>)");
    EXPECT_TRUE(
        single_diagnostic(malformed.diagnostics(), severity::warning, errc::invalid_property_id, "/xisf/Table[1]"));
    EXPECT_EQ(malformed.tables().size(), 1U);
}

TEST(conformance_table, a_table_identifier_is_unique_among_the_properties_of_its_object)
{
    // Spec §8.4.1: a table is a property; the table is left out.
    const reader file =
        open_body(image_xml({}, R"(<Property id="T" type="Int32" value="1"/>)" + string_table({}) + string_table({})));
    ASSERT_EQ(file.diagnostics().size(), 2U) << openxisf::test::describe(file.diagnostics());
    for (const openxisf::diagnostic& entry : file.diagnostics()) {
        EXPECT_EQ(entry.code, errc::duplicate_property_id);
        EXPECT_EQ(entry.severity, severity::error);
    }
    EXPECT_TRUE(file.image(0).tables.empty());
    EXPECT_TRUE(file.image(0).properties.contains("T"));
}

TEST(conformance_table, standalone_tables_and_properties_of_one_identifier_are_tolerated)
{
    // Spec §8.4: standalone properties, tables among them, are associated with no object, so their identifiers need not
    // be unique. The reader returns the first of each identifier, with a warning about each other, and each image gets
    // the table that its Reference names.
    const reader file = open_body(R"(<Property id="T" type="Int32" value="1"/>)" + string_table({}) + string_table({}),
                                  {.strict = true});
    ASSERT_EQ(file.diagnostics().size(), 2U) << openxisf::test::describe(file.diagnostics());
    for (const openxisf::diagnostic& entry : file.diagnostics()) {
        EXPECT_EQ(entry.code, errc::duplicate_property_id);
        EXPECT_EQ(entry.severity, severity::warning);
    }
    EXPECT_EQ(file.diagnostics()[1].context.element, "/xisf/Table[2]");
    EXPECT_TRUE(file.tables().empty());
    EXPECT_TRUE(file.properties().contains("T"));

    const reader named =
        open_body(string_table("<Row><Cell>red</Cell></Row>", R"(uid="red")") +
                      string_table("<Row><Cell>green</Cell></Row>", R"(uid="green")") +
                      image_xml({}, R"(<Reference ref="red"/>)") + image_xml({}, R"(<Reference ref="green"/>)"),
                  {.strict = true});
    EXPECT_TRUE(
        single_diagnostic(named.diagnostics(), severity::warning, errc::duplicate_property_id, "/xisf/Table[2]"));
    ASSERT_EQ(named.tables().size(), 1U);
    EXPECT_EQ(named.tables()[0].rows.at(0).at(0), property_value("red"));
    ASSERT_EQ(named.image(0).tables.size(), 1U);
    EXPECT_EQ(named.image(0).tables[0].rows.at(0).at(0), property_value("red"));
    ASSERT_EQ(named.image(1).tables.size(), 1U);
    EXPECT_EQ(named.image(1).tables[0].rows.at(0).at(0), property_value("green"));
}

TEST(conformance_table, a_table_associated_twice_with_one_object_is_the_same_table)
{
    // Contained and named, or named twice: the object has the table once, and the Reference that associates it again
    // is a warning that says so.
    for (const auto& [body, reference] : std::initializer_list<std::pair<std::string, std::string_view>>{
             {image_xml({}, string_table({}, R"(uid="t")") + R"(<Reference ref="t"/>)"), "/xisf/Image[1]/Reference[1]"},
             {string_table({}, R"(uid="t")") + image_xml({}, R"(<Reference ref="t"/><Reference ref="t"/>)"),
              "/xisf/Image[1]/Reference[2]"}}) {
        const reader file = open_body(body, {.strict = true});
        ASSERT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_reference, reference))
            << body;
        EXPECT_NE(file.diagnostics().front().message.find("same table"), std::string::npos)
            << file.diagnostics().front().message;
        EXPECT_EQ(file.image(0).tables.size(), 1U) << body;
    }
}

// -----------------------------------------------------------------------------------------------------------------
// Structures (spec §8.4.4.7, §11.2)

TEST(conformance_table, a_table_has_exactly_one_structure)
{
    const reader none = open_body(R"(<Table id="T"><Row><Cell>x</Cell></Row></Table>)");
    EXPECT_TRUE(unavailable(none, errc::invalid_table, "/xisf/Table[1]"));
    const reader two = open_body(std::string(messier_structure) +
                                 R"(<Table id="T"><Structure><Field id="f" type="String"/></Structure>)"
                                 R"(<Reference ref="MessierCatalogStructure"/></Table>)");
    EXPECT_TRUE(unavailable(two, errc::invalid_table, "/xisf/Table[1]"));
}

TEST(conformance_table, a_referenced_structure_is_a_standalone_structure)
{
    // Spec §11.3: the structure of a table cannot be named by another table.
    const reader inner = open_body(R"(<Table id="A"><Structure uid="inner"><Field id="f" type="String"/></Structure>)"
                                   R"(</Table><Table id="B"><Reference ref="inner"/></Table>)");
    ASSERT_EQ(inner.diagnostics().size(), 1U) << openxisf::test::describe(inner.diagnostics());
    EXPECT_EQ(inner.diagnostics()[0].code, errc::invalid_reference);
    EXPECT_EQ(inner.diagnostics()[0].context.element, "/xisf/Table[2]/Reference[1]");
    ASSERT_EQ(inner.tables().size(), 1U);
    EXPECT_EQ(inner.tables()[0].id, "A");

    const reader property = open_body(R"(<Property uid="p" id="P" type="Int32" value="1"/>)"
                                      R"(<Table id="T"><Reference ref="p"/></Table>)");
    EXPECT_TRUE(single_diagnostic(property.diagnostics(), severity::error, errc::invalid_reference,
                                  "/xisf/Table[1]/Reference[1]"));
    EXPECT_TRUE(property.tables().empty());
}

TEST(conformance_table, a_standalone_structure_is_shared_by_the_tables_that_name_it)
{
    const reader file = open_body(std::string(messier_structure) +
                                  R"(<Table id="A"><Reference ref="MessierCatalogStructure"/></Table>)"
                                  R"(<Table id="B"><Reference ref="MessierCatalogStructure"/></Table>)");
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.tables().size(), 2U);
    EXPECT_EQ(file.tables()[0].fields, file.tables()[1].fields);
    EXPECT_EQ(file.tables()[0].fields.size(), 7U);
}

TEST(conformance_table, a_standalone_structure_needs_a_uid)
{
    const reader file = open_body(R"(<Structure><Field id="f" type="String"/></Structure>)");
    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_table, "/xisf/Structure[1]"));
}

TEST(conformance_table, a_structure_has_at_least_one_field)
{
    const reader file = open_body(R"(<Table id="T"><Structure/></Table>)");
    EXPECT_TRUE(unavailable(file, errc::invalid_table, "/xisf/Table[1]/Structure[1]"));
}

TEST(conformance_table, a_field_has_an_identifier_and_a_type)
{
    EXPECT_TRUE(unavailable(open_body(R"(<Table id="T"><Structure><Field type="String"/></Structure></Table>)"),
                            errc::invalid_property_id, "/xisf/Table[1]/Structure[1]/Field[1]"));
    EXPECT_TRUE(unavailable(open_body(R"(<Table id="T"><Structure><Field id="" type="String"/></Structure></Table>)"),
                            errc::invalid_property_id, "/xisf/Table[1]/Structure[1]/Field[1]"));
    EXPECT_TRUE(unavailable(open_body(R"(<Table id="T"><Structure><Field id="f"/></Structure></Table>)"),
                            errc::invalid_table, "/xisf/Table[1]/Structure[1]/Field[1]"));
    // A field is not of table type (spec §8.4.4.7), and no property type has that name.
    EXPECT_TRUE(unavailable(open_body(R"(<Table id="T"><Structure><Field id="f" type="Table"/></Structure></Table>)"),
                            errc::unsupported_property_type, "/xisf/Table[1]/Structure[1]/Field[1]"));
}

TEST(conformance_table, a_field_identifier_outside_the_grammar_is_tolerated)
{
    // Spec §11.2.1 gives fields property identifiers; another one is a warning, as for a property.
    const reader file = open_body(R"(<Table id="T"><Structure><Field id="a field" type="String"/></Structure>)"
                                  R"(<Row><Cell>x</Cell></Row></Table>)");
    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_property_id,
                                  "/xisf/Table[1]/Structure[1]/Field[1]"));
    ASSERT_EQ(file.tables().size(), 1U);
    EXPECT_EQ(file.tables()[0].fields.at(0).id, "a field");
}

TEST(conformance_table, the_field_identifiers_of_a_structure_are_unique)
{
    const reader file = open_body(R"(<Table id="T"><Structure><Field id="f" type="String"/>)"
                                  R"(<Field id="f" type="Int32"/></Structure></Table>)");
    EXPECT_TRUE(unavailable(file, errc::invalid_table, "/xisf/Table[1]/Structure[1]/Field[2]"));
}

TEST(conformance_table, a_malformed_field_format_is_ignored)
{
    const reader file = open_body(R"(<Table id="T"><Structure><Field id="f" type="Float32" format="width:x"/>)"
                                  R"(<Field id="t" type="TimePoint" format="width:3"/></Structure></Table>)");
    ASSERT_EQ(file.diagnostics().size(), 2U) << openxisf::test::describe(file.diagnostics());
    EXPECT_EQ(file.diagnostics()[0].code, errc::invalid_format_specifier);
    EXPECT_EQ(file.diagnostics()[1].code, errc::invalid_format_specifier);
    ASSERT_EQ(file.tables().size(), 1U);
    EXPECT_FALSE(file.tables()[0].fields[0].format.has_value());
    EXPECT_FALSE(file.tables()[0].fields[1].format.has_value());
}

// -----------------------------------------------------------------------------------------------------------------
// Rows and cells (spec §11.3.3)

TEST(conformance_table, each_row_has_one_cell_for_each_field)
{
    EXPECT_TRUE(unavailable(open_body(string_table("<Row><Cell>a</Cell><Cell>b</Cell></Row>")), errc::invalid_table,
                            "/xisf/Table[1]/Row[1]"));
    EXPECT_TRUE(unavailable(open_body(string_table("<Row/>")), errc::invalid_table, "/xisf/Table[1]/Row[1]"));
}

TEST(conformance_table, a_cell_holds_a_value_of_the_type_of_its_field)
{
    const reader file = open_body(R"(<Table id="T"><Structure><Field id="n" type="UInt8"/></Structure>)"
                                  R"(<Row><Cell value="256"/></Row></Table>)");
    EXPECT_TRUE(unavailable(file, errc::value_out_of_range, "/xisf/Table[1]/Row[1]/Cell[1]"));
    const reader missing = open_body(R"(<Table id="T"><Structure><Field id="n" type="UInt8"/></Structure>)"
                                     R"(<Row><Cell/></Row></Table>)");
    EXPECT_TRUE(unavailable(missing, errc::invalid_property, "/xisf/Table[1]/Row[1]/Cell[1]"));
}

TEST(conformance_table, a_string_cell_keeps_the_white_space_beside_markup)
{
    // Spec §11.3.3 serializes a cell as a property value (spec §11.1.6): all its white space is significant, beside a
    // comment, a processing instruction or a CDATA section too.
    const reader file = open_body(string_table("<Row><Cell> <!-- c -->a</Cell></Row><Row><Cell>a<?pi?> </Cell></Row>"
                                               "<Row><Cell> <![CDATA[<b>]]> </Cell></Row>"));
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    ASSERT_EQ(file.tables().size(), 1U);
    const std::vector<std::vector<property_value>> rows{{" a"}, {"a "}, {" <b> "}};
    EXPECT_EQ(file.tables()[0].rows, rows);
}

TEST(conformance_table, a_cell_has_no_id_type_or_format)
{
    const reader file = open_body(string_table(R"(<Row><Cell id="x" type="Int32" format="width:3">text</Cell></Row>)"));
    ASSERT_EQ(file.diagnostics().size(), 3U) << openxisf::test::describe(file.diagnostics());
    for (const openxisf::diagnostic& entry : file.diagnostics()) {
        EXPECT_EQ(entry.code, errc::invalid_table);
        EXPECT_EQ(entry.severity, severity::warning);
    }
    ASSERT_EQ(file.tables().size(), 1U);
    EXPECT_EQ(file.tables()[0].rows.at(0).at(0), property_value("text"));
}

TEST(conformance_table, the_rows_and_columns_attributes_agree_with_the_table)
{
    EXPECT_TRUE(no_diagnostics(open_body(string_table("<Row><Cell/></Row>", R"(rows="1" columns="1")")).diagnostics()));
    EXPECT_TRUE(unavailable(open_body(string_table("<Row><Cell/></Row>", R"(rows="2")")), errc::invalid_table,
                            "/xisf/Table[1]"));
    EXPECT_TRUE(unavailable(open_body(string_table("<Row><Cell/></Row>", R"(rows="0")")), errc::invalid_table,
                            "/xisf/Table[1]"));
    EXPECT_TRUE(unavailable(open_body(string_table("<Row><Cell/></Row>", R"(columns="2")")), errc::invalid_table,
                            "/xisf/Table[1]"));
    EXPECT_TRUE(unavailable(open_body(string_table("<Row><Cell/></Row>", R"(rows="one")")), errc::invalid_integer,
                            "/xisf/Table[1]"));
}

TEST(conformance_table, a_table_with_a_cell_beyond_the_limit_of_loaded_data_is_unavailable)
{
    const std::string body = R"(<Table id="T"><Structure><Field id="v" type="UI8Vector"/></Structure>)"
                             R"(<Row><Cell length="10" location="attachment:{0}"/></Row></Table>)";
    const std::vector<std::byte> data = openxisf::test::pattern(10);
    EXPECT_TRUE(no_diagnostics(open_attached(body, {data}, {.limits = {.max_ancillary_data = 10}}).diagnostics()));
    EXPECT_TRUE(unavailable(open_attached(body, {data}, {.limits = {.max_ancillary_data = 9}}),
                            errc::ancillary_data_too_large, "/xisf/Table[1]/Row[1]/Cell[1]"));
}

TEST(conformance_table, the_copies_of_a_shared_structure_count_against_the_limit)
{
    // The fields of a standalone structure are copied into each table that names it, so that a large structure named by
    // many tables cannot exhaust the memory.
    const std::string body = std::string(messier_structure) +
                             R"(<Table id="A"><Reference ref="MessierCatalogStructure"/></Table>)"
                             R"(<Table id="B"><Reference ref="MessierCatalogStructure"/></Table>)";
    const reader limited = open_body(body, {.limits = {.max_ancillary_data = 1}});
    ASSERT_EQ(limited.diagnostics().size(), 2U) << openxisf::test::describe(limited.diagnostics());
    EXPECT_EQ(limited.diagnostics()[0].code, errc::ancillary_data_too_large);
    EXPECT_TRUE(limited.tables().empty());
}

TEST(conformance_table, strict_reading_refuses_an_invalid_table)
{
    EXPECT_TRUE(openxisf::test::throws<openxisf::invalid_data_error>(
        errc::invalid_table, [] { (void)open_body(R"(<Table id="T"/>)", {.strict = true}); }));
}

} // namespace
