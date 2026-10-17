// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The writer (spec §9.2, §9.5, §10, §11): units written with every option read back as their model, and what their
// files and headers hold. The model has a property of every type (§8.1, §8.4.4) with the special values of the text
// forms (§8.3) and format specifiers (§8.4.3), tables (§8.4.4.7), and images of several dimensions, sample formats,
// colour spaces, storage models and ranges (§8.5.1 to §8.5.5, §11.5.1) with every ancillary element (§11.6 to
// §11.12). The files have little-endian structural integers (§8.2) and the layout of §9.1 and §9.2, with attached
// and inline blocks (§10.1), checksums (§10.5), and each codec (§10.6.3 to §10.6.10), with byte shuffling (§10.6.2)
// and subblocks. The properties of an astrometric solution are kept as they are (§11.5.3.7). The round trips cover
// distributed units too; what is particular to them is in conformance/distributed.cpp.

#include <openxisf/color.h>
#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>
#include <openxisf/version.h>
#include <openxisf/writer.h>

#include "container/block_attributes.h"
#include "core/data_encoding.h"
#include "core/uuid.h"
#include "crypto/hash.h"
#include "model/ancillary_attributes.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/faulty_io.h"
#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"
#include "support/temp_directory.h"
#include "support/throws.h"
#include "support/written_unit.h"
#include "xml/xml_document.h"

#include <gtest/gtest.h>
#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::date_time;
using openxisf::errc;
using openxisf::image_info;
using openxisf::property;
using openxisf::property_type;
using openxisf::property_value;
using openxisf::table;
using openxisf::write_options;
using openxisf::test::parsed_header;
using openxisf::test::sink_kind;
using openxisf::test::written;

constexpr date_time creation_time{.year = 2026, .month = 10, .day = 4, .hour = 12, .minute = 34, .second = 56};

write_options basic_options()
{
    return {.creator_application = "OpenXISF tests 1.0", .creation_time = creation_time};
}

// size bytes of pseudo-random noise, which no codec compresses.
std::vector<std::byte> noise(std::size_t size)
{
    std::vector<std::byte> data(size);
    std::uint32_t state = 7;
    for (std::byte& value : data) {
        state = (state * 1'664'525U) + 1'013'904'223U;
        value = static_cast<std::byte>(state >> 24U);
    }
    return data;
}

// The bytes of an ICC profile header, with the embedded flag already set, as the writer writes it.
std::vector<std::byte> icc_profile()
{
    std::vector<std::byte> profile = openxisf::test::pattern(200);
    profile[0] = std::byte{0};
    profile[1] = std::byte{0};
    profile[2] = std::byte{0};
    profile[3] = std::byte{200};
    const std::vector<std::byte> signature = openxisf::test::bytes("acsp");
    std::ranges::copy(signature, profile.begin() + 36);
    profile[47] |= std::byte{0x01};
    return profile;
}

// A unit with an object of each kind, in the forms in which a reader returns them, so that a written unit reads back
// equal to it.
struct unit_model
{
    openxisf::property_list metadata{};
    openxisf::property_list properties{};
    std::vector<table> tables{};
    std::vector<image_info> images{};
    std::vector<std::vector<std::byte>> pixels{};

    void add(image_info image, std::vector<std::byte> data)
    {
        images.push_back(std::move(image));
        pixels.push_back(std::move(data));
    }

    [[nodiscard]] openxisf::writer writer(const write_options& options) const
    {
        openxisf::writer output(options);
        output.metadata() = metadata;
        output.properties() = properties;
        output.tables() = tables;
        for (std::size_t i = 0; i < images.size(); ++i) {
            (void)output.add_image(images[i], pixels[i]);
        }
        return output;
    }
};

// A property of every type, and of the special values that serialization must keep.
openxisf::property_list every_property()
{
    using openxisf::complex128;
    using openxisf::float128;
    using openxisf::int128;
    using openxisf::uint128;
    openxisf::property_list list;
    list.set("Test:Boolean", true);
    list.set("Test:Int8", std::int8_t{-128});
    list.set("Test:UInt8", std::uint8_t{255});
    list.set("Test:Int16", std::int16_t{-32768});
    list.set("Test:UInt16", std::uint16_t{65535});
    list.set("Test:Int32", std::int32_t{-2'147'483'647 - 1});
    list.set("Test:UInt32", std::uint32_t{4'294'967'295U});
    list.set("Test:Int64", std::numeric_limits<std::int64_t>::min());
    list.set("Test:UInt64", std::numeric_limits<std::uint64_t>::max());
    list.set("Test:Int128", int128{.high = -2, .low = 12345});
    list.set("Test:UInt128", uint128{.high = 1, .low = std::numeric_limits<std::uint64_t>::max()});
    list.set("Test:Float32", 0.1F);
    list.set("Test:Float64", -std::numeric_limits<double>::infinity());
    list.set("Test:Subnormal", std::numeric_limits<double>::denorm_min());
    list.set("Test:Float128", property_value::from_float128_text("1.000000000000000000000000000000001"));
    list.set("Test:Complex32", std::complex<float>(1.5F, -2.25F));
    list.set("Test:Complex64", std::complex<double>(1e-10, 3.0));
    list.set("Test:Complex128", property_value::from_complex128_text("(1e-4000,-2)"));
    list.set("Test:String", "  \xC3\x91"
                            "and\xC3\xBA \xE2\x80\x94 \xE6\x98\x9F\xE9\x9B\xB2  ");
    list.set("Test:Markup", R"(<a href="x">&amp;</a> ]]> "quoted" 'single')");
    list.set("Test:Lines", "line 1\nline 2\r\nline 3\rtab\there");
    list.set("Test:Control", "bell\x07, escape\x1B, U+FFFF \xEF\xBF\xBF");
    list.set("Test:Empty", "");
    list.set("Test:Spaces", "   ");
    list.set("Test:LongString", std::string(5000, 'x'));
    list.set(
        "Test:TimePoint",
        date_time{
            .year = 2015, .month = 1, .day = 23, .hour = 19, .minute = 52, .second = 31, .nanosecond = 460'000'000});
    list.set("Test:I8Vector", std::vector<std::int8_t>{-128, 0, 127});
    list.set("Test:UI8Vector", std::vector<std::uint8_t>{0, 1, 255});
    list.set("Test:I16Vector", std::vector<std::int16_t>{-32768, -1, 32767});
    list.set("Test:UI16Vector", std::vector<std::uint16_t>{0, 1, 65535});
    list.set("Test:I32Vector", std::vector<std::int32_t>{-2'147'483'647 - 1, -1, 2'147'483'647});
    list.set("Test:UI32Vector", std::vector<std::uint32_t>{0, 1, 4'294'967'295U});
    list.set("Test:I64Vector", std::vector<std::int64_t>{std::numeric_limits<std::int64_t>::min(), -1, 1});
    list.set("Test:UI64Vector", std::vector<std::uint64_t>{0, 1, std::numeric_limits<std::uint64_t>::max()});
    list.set("Test:I128Vector", std::vector<int128>{{.high = -1, .low = 0}, {.high = 1, .low = 2}});
    list.set("Test:UI128Vector", std::vector<uint128>{{.high = 3, .low = 4}});
    list.set("Test:F32Vector", std::vector<float>{-1.5F, 0.0F, 3.25F});
    list.set("Test:F64Vector", std::vector<double>{-1e300, 0.0, 1e-300});
    list.set("Test:F128Vector", std::vector<float128>{{.high = 0x3FFF'0000'0000'0000U, .low = 0}});
    list.set("Test:C32Vector", std::vector<std::complex<float>>{{1.0F, 2.0F}, {-3.0F, 4.0F}});
    list.set("Test:C64Vector", std::vector<std::complex<double>>{{1.0, 2.0}, {-3.0, 4.0}});
    list.set("Test:C128Vector",
             std::vector<complex128>{{.real = {.high = 1, .low = 2}, .imag = {.high = 3, .low = 4}}});
    list.set("Test:EmptyVector", std::vector<double>{});
    std::vector<double> large(1000);
    for (std::size_t i = 0; i < large.size(); ++i) {
        large[i] = static_cast<double>(i) * 0.5;
    }
    list.set("Test:LargeVector", std::move(large));
    list.set("Test:F64Matrix", property_value::matrix<double>(3, 4, {0, 1, 2, 3, 10, 11, 12, 13, 20, 21, 22, 23}));
    list.set("Test:UI16Matrix", property_value::matrix<std::uint16_t>(2, 2, {1, 2, 3, 4}));
    list.set("Test:EmptyMatrix", property_value::matrix<double>(0, 0, {}));
    list.set({.id = "Test:Formatted",
              .value = 1.25F,
              .format = openxisf::property_format{.width = 6,
                                                  .sign = openxisf::format_sign::force,
                                                  .precision = 2,
                                                  .notation = openxisf::format_notation::fixed,
                                                  .unit = "km/s"},
              .comment = "A \"formatted\" value & a comment"});
    list.set("Instrument:ExposureTime", 300.0F);
    list.set("Observation:Object:Name", "M31");
    return list;
}

table messier_table()
{
    table result;
    result.id = "Test:Messier";
    result.caption = "Some Messier objects";
    result.comment = "From spec §11.3";
    result.fields = {
        {.id = "number", .type = property_type::uint8, .header = "Messier Number"},
        {.id = "name", .type = property_type::string, .header = "Common Name"},
        {.id = "distance",
         .type = property_type::float32,
         .header = "Distance",
         .format =
             openxisf::property_format{.precision = 2, .notation = openxisf::format_notation::fixed, .unit = "kly"}},
        {.id = "data", .type = property_type::f64_vector},
        {.id = "seen", .type = property_type::time_point}};
    result.rows.push_back({std::uint8_t{1}, "Crab Nebula", 6.5F, std::vector<double>{1, 2}, date_time{.year = 2026}});
    result.rows.push_back({std::uint8_t{2}, "", 33.0F, std::vector<double>(500, 0.25), date_time{.year = 2025}});
    result.rows.push_back(
        {std::uint8_t{31}, "Andromeda\x01Galaxy", 2540.0F, std::vector<double>{}, date_time{.year = 1}});
    return result;
}

image_info rich_image()
{
    image_info image;
    image.geometry = {.dimensions = {37, 23}, .channels = 3};
    image.sample_format = openxisf::sample_format::float32;
    image.color_space = openxisf::color_space::rgb;
    image.image_type = openxisf::image_type::master_light;
    image.bounds = openxisf::bounds{.lower = 0.0, .upper = 1.0};
    image.offset = 0.25;
    image.orientation = openxisf::orientation::rotate_90_flip;
    image.id = "rich";
    image.uuid = "c5c93b6d-9072-4e85-9548-1a5391377683";
    image.properties = every_property();
    image.tables.push_back(messier_table());
    image.fits_keywords = {{.name = "EXPTIME", .value = "300", .comment = "Exposure time in seconds"},
                           {.name = "OBJECT", .value = "'M31     '", .comment = "Name & \"designation\""},
                           {.name = "HISTORY", .comment = "Calibrated <again>"},
                           {.name = "COMMENT", .comment = "A comment"},
                           {.name = "DATE-OBS", .value = "'2012-03-15T02:55:15'"}};
    image.icc_profile = icc_profile();
    openxisf::rgb_working_space adobe{.gamma = 2.2,
                                      .x = {0.648431, 0.230154, 0.155886},
                                      .y = {0.330856, 0.701572, 0.066044},
                                      .name = "Adobe RGB (1998)"};
    adobe.luminance = openxisf::luminance_coefficients(adobe.x, adobe.y).value_or(adobe.luminance);
    image.rgb_working_space = adobe;
    image.display_function = openxisf::display_function{.midtones = {0.000735, 0.000735, 0.000735, 0.5},
                                                        .shadows = {0.003758, 0.003758, 0.003758, 0.0},
                                                        .name = "AutoStretch"};
    image.resolution =
        openxisf::resolution{.horizontal = 120.0, .vertical = 118.5, .unit = openxisf::resolution_unit::centimeter};
    openxisf::thumbnail small;
    small.geometry = {.dimensions = {8, 5}, .channels = 3};
    small.properties.set("Test:Thumbnail", "small");
    small.fits_keywords = {{.name = "COMMENT", .comment = "Thumbnail"}};
    small.resolution = openxisf::resolution{};
    small.pixels = openxisf::test::pattern(120);
    image.thumbnail = small;
    return image;
}

image_info simple_image(openxisf::sample_format format, std::vector<std::uint64_t> dimensions, std::uint64_t channels,
                        openxisf::color_space space = openxisf::color_space::gray)
{
    image_info image;
    image.geometry = {.dimensions = std::move(dimensions), .channels = channels};
    image.sample_format = format;
    image.color_space = space;
    return image;
}

// The pixel data of the rich image: Float32 samples that repeat every 61 samples, in native byte order, so that the
// bytes written, little-endian on every host, are the same everywhere and compress the same, in subblocks of 1000
// bytes too. A pattern of bytes is swapped on a big-endian host, where its period grows fourfold, beyond such a
// subblock, and LZ4 and Zstandard then make no subblock smaller.
std::vector<std::byte> rich_pixels(const image_info& image)
{
    std::vector<float> samples(image.geometry.sample_count());
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(i % 61) / 61.0F;
    }
    const std::span<const std::byte> bytes = std::as_bytes(std::span<const float>(samples));
    return {bytes.begin(), bytes.end()};
}

unit_model rich_model()
{
    unit_model model;
    model.metadata.set("XISF:Title", "A unit for the tests");
    model.metadata.set("XISF:Authors", "First Author\nSecond Author");
    model.metadata.set("XISF:Description", std::string(4000, 'd'));
    model.properties.set("Test:Standalone", std::int32_t{-7});
    std::vector<float> matrix(2500);
    for (std::size_t i = 0; i < matrix.size(); ++i) {
        matrix[i] = static_cast<float>(i);
    }
    model.properties.set("Test:Matrix", property_value::matrix<float>(50, 50, std::move(matrix)));
    model.tables.push_back(messier_table());

    image_info rich = rich_image();
    model.add(rich, rich_pixels(rich));

    image_info mosaic = simple_image(openxisf::sample_format::uint16, {64, 48}, 1);
    mosaic.id = "mosaic";
    mosaic.image_type = openxisf::image_type::light;
    mosaic.color_filter_array =
        openxisf::color_filter_array{.pattern = "RGGB", .width = 2, .height = 2, .name = "RGGB Bayer filter"};
    model.add(mosaic, openxisf::test::pattern(mosaic.data_size()));

    image_info random = simple_image(openxisf::sample_format::uint8, {300, 200}, 1);
    random.id = "noise";
    model.add(random, noise(random.data_size()));

    image_info line = simple_image(openxisf::sample_format::uint32, {100}, 1);
    model.add(line, openxisf::test::pattern(line.data_size()));

    image_info cube = simple_image(openxisf::sample_format::uint64, {4, 3, 2}, 2);
    model.add(cube, openxisf::test::pattern(cube.data_size()));

    image_info rgba = simple_image(openxisf::sample_format::float64, {5, 4}, 4, openxisf::color_space::rgb);
    rgba.pixel_storage = openxisf::pixel_storage::normal;
    rgba.bounds = openxisf::bounds{.lower = -1.0, .upper = 1.0};
    model.add(rgba, openxisf::test::pattern(rgba.data_size()));

    image_info lab = simple_image(openxisf::sample_format::complex32, {6, 5}, 3, openxisf::color_space::cie_lab);
    model.add(lab, openxisf::test::pattern(lab.data_size()));

    image_info complex_line = simple_image(openxisf::sample_format::complex64, {7}, 1);
    complex_line.bounds = openxisf::bounds{.lower = 0.0, .upper = 2.0};
    model.add(complex_line, openxisf::test::pattern(complex_line.data_size()));
    return model;
}

// The elements of a header that serialize a data block, in document order.
void collect_block_elements(const pugi::xml_node& node, std::vector<pugi::xml_node>& elements)
{
    for (pugi::xml_node child = node.first_child(); !child.empty(); child = child.next_sibling()) {
        if (child.type() != pugi::node_element) {
            continue;
        }
        if (!child.attribute("location").empty()) {
            elements.push_back(child);
        }
        collect_block_elements(child, elements);
    }
}

std::vector<pugi::xml_node> block_elements(const pugi::xml_document& header)
{
    std::vector<pugi::xml_node> elements;
    collect_block_elements(header, elements);
    return elements;
}

// The Image element with the given id.
pugi::xml_node image_element(const pugi::xml_document& header, std::string_view id)
{
    for (pugi::xml_node image = header.document_element().child("Image"); !image.empty();
         image = image.next_sibling("Image")) {
        if (id == image.attribute("id").value()) {
            return image;
        }
    }
    return {};
}

// The attached blocks of a unit, as their locations give them.
std::vector<openxisf::detail::block_location> attachments(std::span<const std::byte> file)
{
    std::vector<openxisf::detail::block_location> found;
    const auto header = parsed_header(file);
    for (const pugi::xml_node& element : block_elements(*header)) {
        const openxisf::detail::block_location location =
            openxisf::detail::parse_location(element.attribute("location").value());
        if (location.kind == openxisf::detail::location_kind::attachment) {
            found.push_back(location);
        }
    }
    return found;
}

// Checks that every byte after the header is in an attached block or zero (spec §9.2).
testing::AssertionResult unused_space_is_zero(std::span<const std::byte> file)
{
    const std::string header = openxisf::test::header_of(file);
    std::vector<bool> used(file.size(), false);
    std::fill_n(used.begin(), std::min(file.size(), 16 + header.size()), true);
    for (const openxisf::detail::block_location& location : attachments(file)) {
        if (location.position > file.size() || location.size > file.size() - location.position) {
            return testing::AssertionFailure() << "a block goes beyond the end of the file";
        }
        std::fill_n(used.begin() + static_cast<std::ptrdiff_t>(location.position),
                    static_cast<std::ptrdiff_t>(location.size), true);
    }
    for (std::size_t i = 0; i < file.size(); ++i) {
        if (!used[i] && file[i] != std::byte{0}) {
            return testing::AssertionFailure() << "byte " << i << " is unused and not zero";
        }
    }
    return testing::AssertionSuccess();
}

openxisf::reader open_strictly(std::vector<std::byte> file)
{
    return openxisf::test::open_unit(std::move(file), {.strict = true});
}

// Checks that every byte of a data blocks file is in its signature, its block index or a block that the index points
// to, or zero (spec §9.4), reading the file by itself.
testing::AssertionResult blocks_file_unused_space_is_zero(std::span<const std::byte> file)
{
    const auto number = [file](std::size_t offset, std::size_t size) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < size; ++i) {
            value |= std::to_integer<std::uint64_t>(file[offset + i]) << (8U * i);
        }
        return value;
    };
    if (file.size() < 32 || openxisf::test::text(file.first(8)) != "XISB0100" || number(24, 8) != 0) {
        return testing::AssertionFailure() << "not a data blocks file of one index node";
    }
    const std::uint64_t count = number(16, 4);
    const std::uint64_t index_end = 32 + (40 * count);
    if (index_end > file.size()) {
        return testing::AssertionFailure() << "the index goes beyond the end of the file";
    }
    std::vector<bool> used(file.size(), false);
    std::fill_n(used.begin(), index_end, true);
    for (std::uint64_t i = 0; i < count; ++i) {
        const std::uint64_t position = number(32 + (40 * i) + 8, 8);
        const std::uint64_t length = number(32 + (40 * i) + 16, 8);
        if (position < index_end || position > file.size() || length > file.size() - position) {
            return testing::AssertionFailure() << "block " << i << " is not after the index and inside the file";
        }
        std::fill_n(used.begin() + static_cast<std::ptrdiff_t>(position), length, true);
    }
    for (std::size_t i = 0; i < file.size(); ++i) {
        if (!used[i] && file[i] != std::byte{0}) {
            return testing::AssertionFailure() << "byte " << i << " is unused and not zero";
        }
    }
    return testing::AssertionSuccess();
}

// ---------------------------------------------------------------------------------------------------------------------
// Round trips of every option

struct round_trip_case
{
    std::optional<openxisf::codec> codec{};
    bool shuffle = false;
    std::optional<openxisf::checksum_algorithm> checksum{};
    std::uint64_t subblock_size = 0;
    sink_kind sink = sink_kind::rewritable;
    bool distributed = false;
};

std::string case_name(const round_trip_case& value)
{
    std::string name = value.codec ? std::string(openxisf::codec_name(*value.codec)) : "uncompressed";
    if (value.shuffle) {
        name += "_shuffled";
    }
    if (value.subblock_size != 0) {
        name += "_subblocks";
    }
    if (value.checksum) {
        name += "_checksum";
    }
    name += value.sink == sink_kind::rewritable ? "_rewritable" : "_append_only";
    if (value.distributed) {
        name += "_distributed";
    }
    return name;
}

// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const round_trip_case& value, std::ostream* output)
{
    *output << case_name(value);
}

std::vector<round_trip_case> round_trip_cases()
{
    std::vector<round_trip_case> cases;
    const std::array<std::optional<openxisf::codec>, 5> codecs{
        std::nullopt, openxisf::codec::zlib, openxisf::codec::lz4, openxisf::codec::lz4hc, openxisf::codec::zstd};
    for (const std::optional<openxisf::codec>& codec : codecs) {
        for (const bool shuffle : {false, true}) {
            for (const std::uint64_t subblock_size : {std::uint64_t{0}, std::uint64_t{1000}}) {
                if (!codec && (shuffle || subblock_size != 0)) {
                    continue;
                }
                for (const bool checksum : {false, true}) {
                    for (const sink_kind sink : {sink_kind::rewritable, sink_kind::append_only}) {
                        for (const bool distributed : {false, true}) {
                            round_trip_case entry{.codec = codec,
                                                  .shuffle = shuffle,
                                                  .subblock_size = subblock_size,
                                                  .sink = sink,
                                                  .distributed = distributed};
                            if (checksum) {
                                entry.checksum = openxisf::checksum_algorithm::sha256;
                            }
                            cases.push_back(entry);
                        }
                    }
                }
            }
        }
    }
    return cases;
}

// A unit written as a round trip case asks, opened strictly, with its header and the check of its unused space.
struct written_case
{
    openxisf::reader unit;
    std::unique_ptr<pugi::xml_document> header;
    testing::AssertionResult unused_space_is_zero = testing::AssertionSuccess();
};

written_case write_case(const openxisf::writer& output, const round_trip_case& tested)
{
    if (tested.distributed) {
        const openxisf::test::distributed_unit unit = openxisf::test::written_distributed(output, tested.sink);
        return {.unit = openxisf::test::open_distributed(unit, {.strict = true}),
                .header = openxisf::detail::parse_xml(openxisf::test::text(unit.header), 0, {}),
                .unused_space_is_zero = blocks_file_unused_space_is_zero(unit.blocks)};
    }
    const std::vector<std::byte> file = written(output, tested.sink);
    return {
        .unit = open_strictly(file), .header = parsed_header(file), .unused_space_is_zero = unused_space_is_zero(file)};
}

class conformance_writer_round_trip : public testing::TestWithParam<round_trip_case>
{};

TEST_P(conformance_writer_round_trip, reads_back_as_its_model)
{
    const round_trip_case& tested = GetParam();
    write_options options = basic_options();
    options.codec = tested.codec;
    options.byte_shuffle = tested.shuffle;
    options.subblock_size = tested.subblock_size;
    options.checksum = tested.checksum;
    const unit_model model = rich_model();
    const written_case written_unit = write_case(model.writer(options), tested);

    const openxisf::reader& unit = written_unit.unit;
    EXPECT_TRUE(openxisf::test::no_diagnostics(unit.diagnostics()));
    ASSERT_EQ(unit.images().size(), model.images.size());
    for (std::size_t i = 0; i < model.images.size(); ++i) {
        EXPECT_EQ(unit.image(i), model.images[i]) << "image " << i;
        EXPECT_EQ(unit.read_pixels(i), model.pixels[i]) << "image " << i;
    }
    EXPECT_EQ(unit.properties(), model.properties);
    EXPECT_EQ(std::vector<table>(unit.tables().begin(), unit.tables().end()), model.tables);
    for (const property& item : model.metadata) {
        EXPECT_EQ(unit.metadata().at(item.id), item);
    }
    EXPECT_EQ(unit.metadata().at("XISF:CreationTime").value, property_value(creation_time));
    EXPECT_TRUE(written_unit.unused_space_is_zero);

    // Every block has the checksum asked for, and the blocks that compress are compressed; the noise is not.
    const std::unique_ptr<pugi::xml_document>& header = written_unit.header;
    for (const pugi::xml_node& element : block_elements(*header)) {
        // PixInsight reads a subblock that is not smaller than its data as data stored as they are.
        if (const std::string_view subblocks = element.attribute("subblocks").value(); !subblocks.empty()) {
            for (const openxisf::detail::subblock& part : openxisf::detail::parse_subblocks(subblocks)) {
                EXPECT_LE(part.compressed_size, part.uncompressed_size) << element.path();
            }
        }
        EXPECT_EQ(element.attribute("checksum").empty(), !tested.checksum) << element.path();
        if (!tested.checksum) {
            continue;
        }
        EXPECT_TRUE(std::string_view(element.attribute("checksum").value()).starts_with("sha256:"));
    }
    const pugi::xml_node rich = image_element(*header, "rich");
    const pugi::xml_node random = image_element(*header, "noise");
    EXPECT_TRUE(random.attribute("compression").empty());
    EXPECT_EQ(rich.attribute("compression").empty(), !tested.codec);
    if (tested.codec) {
        const std::string expected = std::string(openxisf::codec_name(*tested.codec)) + (tested.shuffle ? "+sh" : "") +
                                     ":" + std::to_string(model.images[0].data_size()) + (tested.shuffle ? ":4" : "");
        EXPECT_EQ(std::string_view(rich.attribute("compression").value()), expected);
        // 37 × 23 × 3 samples of 4 bytes in subblocks of up to 1000 bytes.
        EXPECT_EQ(rich.attribute("subblocks").empty(), tested.subblock_size == 0);
    }
}

INSTANTIATE_TEST_SUITE_P(options, conformance_writer_round_trip, testing::ValuesIn(round_trip_cases()),
                         [](const testing::TestParamInfo<round_trip_case>& parameter) {
                             return case_name(parameter.param);
                         });

TEST(conformance_writer, the_tables_and_properties_of_a_unit_read_are_written_again_as_they_were_read)
{
    // What the reader returns of tables with a structure of their own or a standalone one, of cells and properties in
    // data blocks, and of standalone properties is a model that the writer writes again as it is (spec §11.1 to
    // §11.3). The samples of PixInsight have no Table, Structure or standalone Property element.
    const std::string_view star = R"(<Cell location="inline:base64">UiBDckI=</Cell>)";
    const std::string body =
        R"(<Structure uid="observations"><Field id="star" type="String" header="Star"/>)"
        R"(<Field id="count" type="UInt32"/><Field id="data" type="ByteArray"/>)"
        R"(<Field id="magnitude" type="Float32" format="float:fixed;precision:2"/></Structure>)"
        R"(<Property id="Test:Count" type="UInt32" value="7"/>)"
        R"(<Property id="Test:Bytes" type="ByteArray" length="5" location="attachment:{0}"/>)"
        R"(<Property id="Test:Note" type="String" location="inline:base64">YSBub3Rl</Property>)"
        R"(<Table id="Stars" caption="Observed stars"><Reference ref="observations"/>)"
        R"(<Row><Cell>SS Cyg</Cell><Cell value="132729"/><Cell length="4" location="attachment:{1}"/>)"
        R"(<Cell value="8.25"/></Row><Row>)" +
        std::string(star) +
        R"(<Cell value="58135"/><Cell length="3" location="attachment:{2}"/><Cell value="5.75"/></Row></Table>)"
        R"(<Table id="Settings"><Structure><Field id="name" type="String"/><Field id="value" type="Float64"/>)"
        R"(</Structure><Row><Cell>gain</Cell><Cell value="1.5"/></Row></Table>)" +
        openxisf::test::image_xml({}, R"(<Table id="Frames"><Reference ref="observations"/><Row>)" + std::string(star) +
                                          R"(<Cell value="3"/><Cell length="2" location="inline:hex">0102</Cell>)"
                                          R"(<Cell value="6"/></Row></Table>)"
                                          R"(<Property id="Test:Frame" type="String" location="attachment:{3}"/>)");
    const openxisf::reader original = openxisf::test::open_unit(
        openxisf::test::file_with_attachments(openxisf::test::header_xml(body),
                                              {openxisf::test::bytes("12345"), openxisf::test::bytes("abcd"),
                                               openxisf::test::bytes("xyz"), openxisf::test::bytes("first frame")}),
        {.strict = true});
    ASSERT_EQ(original.properties().size(), 3U);
    ASSERT_EQ(original.tables().size(), 2U);
    ASSERT_EQ(original.images().size(), 1U);
    ASSERT_EQ(original.image(0).tables.size(), 1U);

    openxisf::writer output(basic_options());
    output.properties() = original.properties();
    output.tables().assign(original.tables().begin(), original.tables().end());
    const std::vector<std::byte> pixels = original.read_pixels(0);
    (void)output.add_image(original.image(0), pixels);
    const openxisf::reader unit = open_strictly(written(output));
    EXPECT_EQ(unit.properties(), original.properties());
    EXPECT_TRUE(std::ranges::equal(unit.tables(), original.tables()));
    ASSERT_EQ(unit.images().size(), 1U);
    EXPECT_EQ(unit.image(0), original.image(0));
}

// ---------------------------------------------------------------------------------------------------------------------
// The file and its header

#if defined(_WIN32)
constexpr std::string_view creator_os = "Windows";
#elif defined(__APPLE__)
constexpr std::string_view creator_os = "macOS";
#elif defined(__linux__)
constexpr std::string_view creator_os = "Linux";
#elif defined(__FreeBSD__)
constexpr std::string_view creator_os = "FreeBSD";
#else
constexpr std::string_view creator_os{};
#endif

// A file written byte for byte as the specification and the defaults of the writer give it, from a fixed model and
// creation time.
TEST(conformance_writer, writes_a_unit_byte_for_byte)
{
    image_info image = simple_image(openxisf::sample_format::uint16, {2, 2}, 1);
    image.id = "tiny";
    image.properties.set("Instrument:ExposureTime", 300.0F);
    openxisf::writer output(basic_options());
    const std::array<std::uint16_t, 4> samples{1, 2, 0x1234, 0xFFFF};
    (void)output.add_image(image, std::span<const std::uint16_t>(samples));

    std::string header =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!--\n"
        "Extensible Image Serialization Format - XISF version 1.0\n"
        "Created with OpenXISF " OPENXISF_VERSION_STRING " - https://github.com/openxisf/openxisf\n"
        "-->\n"
        "<xisf version=\"1.0\" xmlns=\"http://www.pixinsight.com/xisf\" "
        "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\" "
        "xsi:schemaLocation=\"http://www.pixinsight.com/xisf http://pixinsight.com/xisf/xisf-1.0.xsd\">\n"
        "   <Metadata>\n"
        "      <Property id=\"XISF:CreationTime\" type=\"TimePoint\" value=\"2026-10-04T12:34:56Z\"/>\n"
        "      <Property id=\"XISF:CreatorApplication\" type=\"String\">OpenXISF tests 1.0</Property>\n"
        "      <Property id=\"XISF:CreatorModule\" type=\"String\">OpenXISF " OPENXISF_VERSION_STRING "</Property>\n";
    if (!creator_os.empty()) {
        header += R"(      <Property id="XISF:CreatorOS" type="String">)" + std::string(creator_os) + "</Property>\n";
    }
    header += "      <Property id=\"XISF:BlockAlignmentSize\" type=\"UInt16\" value=\"4096\"/>\n"
              "      <Property id=\"XISF:MaxInlineBlockSize\" type=\"UInt16\" value=\"3072\"/>\n"
              "   </Metadata>\n"
              "   <Image geometry=\"2:2:1\" sampleFormat=\"UInt16\" colorSpace=\"Gray\" id=\"tiny\" "
              "location=\"attachment:4096:8\">\n"
              "      <Property id=\"Instrument:ExposureTime\" type=\"Float32\" value=\"300\"/>\n"
              "   </Image>\n"
              "</xisf>";

    std::vector<std::byte> expected = openxisf::test::bytes("XISF0100");
    for (std::size_t i = 0; i < 4; ++i) {
        expected.push_back(static_cast<std::byte>((header.size() >> (8 * i)) & 0xFFU));
    }
    expected.resize(16);
    const std::vector<std::byte> text = openxisf::test::bytes(header);
    expected.insert(expected.end(), text.begin(), text.end());
    expected.resize(4096);
    for (const std::uint16_t sample : samples) {
        expected.push_back(static_cast<std::byte>(sample & 0xFFU));
        expected.push_back(static_cast<std::byte>(sample >> 8U));
    }

    EXPECT_EQ(written(output), expected);
    EXPECT_EQ(written(output, sink_kind::append_only), expected);
}

TEST(conformance_writer, a_unit_without_data_blocks_has_its_metadata_alone)
{
    write_options options = basic_options();
    options.checksum = openxisf::checksum_algorithm::sha1;
    options.codec = openxisf::codec::zstd;
    openxisf::writer output(options);
    output.metadata().set("XISF:Title", "Nothing else");
    const std::vector<std::byte> file = written(output);
    // No block has a checksum or is compressed, so the metadata do not say so.
    const openxisf::reader unit = open_strictly(file);
    EXPECT_TRUE(unit.images().empty());
    EXPECT_EQ(unit.metadata().size(), creator_os.empty() ? 6U : 7U);
    EXPECT_FALSE(unit.metadata().contains("XISF:ChecksumAlgorithms"));
    EXPECT_FALSE(unit.metadata().contains("XISF:CompressionCodecs"));
    EXPECT_EQ(file.size(), 16 + openxisf::test::header_of(file).size());
}

TEST(conformance_writer, the_metadata_name_the_creator_and_the_layout)
{
    const openxisf::reader unit = open_strictly(written(rich_model().writer(basic_options())));
    const openxisf::property_list& metadata = unit.metadata();
    EXPECT_EQ(metadata.at("XISF:CreationTime").value, property_value(creation_time));
    EXPECT_EQ(metadata.at("XISF:CreatorApplication").value, property_value("OpenXISF tests 1.0"));
    EXPECT_EQ(metadata.at("XISF:CreatorModule").value,
              property_value(std::string("OpenXISF ") + std::string(openxisf::version())));
    if (!creator_os.empty()) {
        EXPECT_EQ(metadata.at("XISF:CreatorOS").value, property_value(creator_os));
    }
    EXPECT_EQ(metadata.at("XISF:BlockAlignmentSize").value, property_value(std::uint16_t{4096}));
    EXPECT_EQ(metadata.at("XISF:MaxInlineBlockSize").value, property_value(std::uint16_t{3072}));
}

TEST(conformance_writer, the_creation_time_is_the_time_of_the_save_without_a_fixed_one)
{
    write_options options = basic_options();
    options.creation_time.reset();
    const date_time before =
        openxisf::to_date_time(std::chrono::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
    const openxisf::reader unit = open_strictly(written(openxisf::writer(options)));
    const date_time after = openxisf::to_date_time(std::chrono::system_clock::now());
    const date_time created = unit.metadata().at("XISF:CreationTime").value.get<date_time>();
    EXPECT_LE(before, created);
    EXPECT_LE(created, after);
    EXPECT_EQ(created.nanosecond % 1'000'000, 0U);
}

TEST(conformance_writer, the_metadata_that_the_writer_writes_replace_those_of_the_model)
{
    unit_model model;
    model.metadata.set("XISF:CreationTime", "2020-01-01T00:00:00Z");
    model.metadata.set("XISF:CreatorApplication", "Another application 2.0");
    model.metadata.set("XISF:BlockAlignmentSize", std::uint16_t{1});
    model.metadata.set("XISF:OutputHints", "kept");
    const openxisf::reader unit = open_strictly(written(model.writer(basic_options())));
    EXPECT_EQ(unit.metadata().at("XISF:CreationTime").value, property_value(creation_time));
    EXPECT_EQ(unit.metadata().at("XISF:CreatorApplication").value, property_value("OpenXISF tests 1.0"));
    EXPECT_EQ(unit.metadata().at("XISF:BlockAlignmentSize").value, property_value(std::uint16_t{4096}));
    EXPECT_EQ(unit.metadata().at("XISF:OutputHints").value, property_value("kept"));
}

// Without options, nothing that a decoder of the first version of XISF 1.0 could miss.
TEST(conformance_writer, by_default_nothing_is_compressed_checksummed_divided_or_identified)
{
    const std::vector<std::byte> file = written(rich_model().writer(basic_options()));
    const std::string header = openxisf::test::header_of(file);
    for (const std::string_view attribute : {" compression=", " checksum=", " subblocks=", " byteOrder="}) {
        EXPECT_EQ(header.find(attribute), std::string::npos) << attribute;
    }
    // Only the image that has one keeps its UUID.
    const std::size_t uuid = header.find(" uuid=");
    EXPECT_NE(uuid, std::string::npos);
    EXPECT_EQ(uuid, header.rfind(" uuid="));
    for (const std::string_view id : {"XISF:ChecksumAlgorithms", "XISF:CompressionCodecs", "XISF:CompressionLevel"}) {
        EXPECT_EQ(header.find(id), std::string::npos) << id;
    }
}

TEST(conformance_writer, the_preamble_counts_the_header_exactly)
{
    const std::vector<std::byte> file = written(rich_model().writer(basic_options()));
    EXPECT_EQ(openxisf::test::text(std::span(file).first(8)), "XISF0100");
    const std::string header = openxisf::test::header_of(file);
    EXPECT_TRUE(header.ends_with("</xisf>"));
    EXPECT_TRUE(std::ranges::all_of(std::span(file).subspan(12, 4), [](std::byte b) { return b == std::byte{0}; }));
}

TEST(conformance_writer, attached_blocks_start_at_multiples_of_the_alignment)
{
    // An image of nine bytes before the attached blocks of the standalone properties and tables.
    unit_model model = rich_model();
    model.add(simple_image(openxisf::sample_format::uint8, {3, 3}, 1), std::vector<std::byte>(9, std::byte{1}));
    for (const std::uint16_t alignment :
         {std::uint16_t{4096}, std::uint16_t{512}, std::uint16_t{7}, std::uint16_t{2}}) {
        write_options options = basic_options();
        options.block_alignment = alignment;
        const std::vector<std::byte> file = written(model.writer(options));
        for (const openxisf::detail::block_location& location : attachments(file)) {
            EXPECT_EQ(location.position % alignment, 0U) << alignment;
        }
        EXPECT_TRUE(unused_space_is_zero(file));
        EXPECT_EQ(open_strictly(file).metadata().at("XISF:BlockAlignmentSize").value, property_value(alignment));
    }
}

TEST(conformance_writer, without_alignment_the_blocks_follow_each_other)
{
    for (const std::uint16_t alignment : {std::uint16_t{0}, std::uint16_t{1}}) {
        for (const sink_kind sink : {sink_kind::rewritable, sink_kind::append_only}) {
            write_options options = basic_options();
            options.block_alignment = alignment;
            const std::vector<std::byte> file = written(rich_model().writer(options), sink);
            std::uint64_t next = 16 + openxisf::test::header_of(file).size();
            for (const openxisf::detail::block_location& location : attachments(file)) {
                EXPECT_EQ(location.position, next);
                next = location.position + location.size;
            }
            EXPECT_EQ(next, file.size());
            EXPECT_EQ(open_strictly(file).image(0), rich_model().images[0]);
        }
    }
}

TEST(conformance_writer, small_blocks_of_properties_are_inline_and_pixel_data_attached)
{
    // 384 doubles are 3072 bytes, the default maximum.
    for (const std::uint16_t maximum : {std::uint16_t{3072}, std::uint16_t{0}, std::uint16_t{65535}}) {
        write_options options = basic_options();
        options.max_inline_block_size = maximum;
        unit_model model;
        model.properties.set("Test:AtMost", std::vector<double>(384, 1.0));
        model.properties.set("Test:Beyond", std::vector<double>(385, 1.0));
        model.properties.set("Test:Empty", std::vector<double>{});
        image_info image = simple_image(openxisf::sample_format::uint8, {1}, 1);
        image.icc_profile = icc_profile();
        model.add(image, {std::byte{1}});
        const std::vector<std::byte> file = written(model.writer(options));
        const auto header = parsed_header(file);
        const auto location_of = [&header](std::string_view id) {
            for (pugi::xml_node property = header->document_element().child("Property"); !property.empty();
                 property = property.next_sibling("Property")) {
                if (id == property.attribute("id").value()) {
                    return std::string(property.attribute("location").value());
                }
            }
            return std::string();
        };
        EXPECT_EQ(location_of("Test:AtMost") == "inline:base64", maximum >= 3072) << maximum;
        EXPECT_EQ(location_of("Test:Beyond") == "inline:base64", maximum >= 3080) << maximum;
        // An empty value is an empty inline block, whatever the maximum (spec §11.1.8).
        EXPECT_EQ(location_of("Test:Empty"), "inline:base64");
        const pugi::xml_node profile = openxisf::test::element_at(*header, "Image/ICCProfile");
        EXPECT_EQ(std::string_view(profile.attribute("location").value()) == "inline:base64", maximum >= 200);
        // An image of one byte is attached all the same (spec §7.1, §11.5).
        EXPECT_TRUE(std::string_view(openxisf::test::element_at(*header, "Image").attribute("location").value())
                        .starts_with("attachment:"));
        const openxisf::reader unit = open_strictly(file);
        EXPECT_EQ(unit.properties(), model.properties);
        EXPECT_EQ(unit.image(0).icc_profile, image.icc_profile);
        EXPECT_EQ(unit.metadata().at("XISF:MaxInlineBlockSize").value, property_value(maximum));
    }
}

TEST(conformance_writer, both_kinds_of_sink_get_the_same_bytes_without_compression_or_checksums)
{
    const unit_model model = rich_model();
    const openxisf::writer output = model.writer(basic_options());
    EXPECT_EQ(written(output, sink_kind::rewritable), written(output, sink_kind::append_only));
}

TEST(conformance_writer, a_model_is_written_the_same_way_every_time)
{
    write_options options = basic_options();
    options.codec = openxisf::codec::zstd;
    options.byte_shuffle = true;
    options.checksum = openxisf::checksum_algorithm::sha1;
    const unit_model model = rich_model();
    for (const sink_kind sink : {sink_kind::rewritable, sink_kind::append_only}) {
        EXPECT_EQ(written(model.writer(options), sink), written(model.writer(options), sink));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Compression

TEST(conformance_writer, the_compression_metadata_name_the_codecs_used_and_the_level)
{
    struct expectation
    {
        openxisf::codec codec = openxisf::codec::zlib;
        int level = 0;
        std::optional<property_value> reported{};
    };
    for (const expectation& entry :
         {expectation{.codec = openxisf::codec::zlib, .reported = property_value(67)},
          expectation{.codec = openxisf::codec::lz4},
          expectation{.codec = openxisf::codec::lz4hc, .reported = property_value(73)},
          expectation{.codec = openxisf::codec::zstd, .reported = property_value(10)},
          expectation{.codec = openxisf::codec::zstd, .level = 50, .reported = property_value(50)}}) {
        write_options options = basic_options();
        options.codec = entry.codec;
        options.compression_level = entry.level;
        options.byte_shuffle = true;
        unit_model model = rich_model();
        const image_info bytes = simple_image(openxisf::sample_format::uint8, {64, 48}, 1);
        model.add(bytes, std::vector<std::byte>(bytes.data_size()));
        const std::vector<std::byte> file = written(model.writer(options));

        // Each codec of a compressed block once, in the order of their first blocks.
        std::string codecs;
        const auto header = parsed_header(file);
        for (const pugi::xml_node& element : block_elements(*header)) {
            const std::string_view compression = element.attribute("compression").value();
            const std::string name(compression.substr(0, compression.find(':')));
            if (!name.empty() && ("," + codecs + ",").find("," + name + ",") == std::string::npos) {
                codecs += codecs.empty() ? name : "," + name;
            }
        }
        const std::string name(openxisf::codec_name(entry.codec));
        std::string expected = name;
        expected += "+sh,";
        expected += name;
        EXPECT_EQ(codecs, expected);

        const openxisf::reader unit = open_strictly(file);
        EXPECT_EQ(unit.metadata().at("XISF:CompressionCodecs").value, property_value(codecs));
        const openxisf::property* level = unit.metadata().find("XISF:CompressionLevel");
        EXPECT_EQ(level == nullptr ? std::nullopt : std::optional<property_value>(level->value), entry.reported)
            << name;
    }
}

TEST(conformance_writer, nothing_is_compressed_when_nothing_gets_smaller)
{
    write_options options = basic_options();
    options.codec = openxisf::codec::zstd;
    unit_model model;
    const image_info image = simple_image(openxisf::sample_format::uint8, {300, 200}, 1);
    model.add(image, noise(image.data_size()));
    // Inline.
    std::vector<std::uint8_t> values;
    for (const std::byte value : noise(1000)) {
        values.push_back(std::to_integer<std::uint8_t>(value));
    }
    model.properties.set("Test:Noise", values);
    for (const sink_kind sink : {sink_kind::rewritable, sink_kind::append_only}) {
        const std::vector<std::byte> file = written(model.writer(options), sink);
        const std::string header = openxisf::test::header_of(file);
        EXPECT_NE(header.find(R"(location="inline:base64")"), std::string::npos);
        EXPECT_EQ(header.find(" compression="), std::string::npos);
        EXPECT_EQ(header.find("XISF:CompressionCodecs"), std::string::npos);
        EXPECT_EQ(header.find("XISF:CompressionLevel"), std::string::npos);
        EXPECT_EQ(open_strictly(file).read_pixels(0), model.pixels[0]);
        EXPECT_EQ(open_strictly(file).properties(), model.properties);
        EXPECT_TRUE(unused_space_is_zero(file));
    }
}

TEST(conformance_writer, the_room_for_a_header_written_last_holds_every_subblock)
{
    // A sink that can rewrite gets the header last, in the room reserved before the blocks: without alignment, that
    // room alone must hold the subblocks attribute, here of 256 subblocks.
    write_options options = basic_options();
    options.codec = openxisf::codec::zstd;
    options.subblock_size = 256;
    options.block_alignment = 0;
    unit_model model;
    const image_info image = simple_image(openxisf::sample_format::uint16, {64, 512}, 1);
    model.add(image, std::vector<std::byte>(image.data_size()));
    const std::vector<std::byte> file = written(model.writer(options));
    const auto header = parsed_header(file);
    EXPECT_EQ(
        openxisf::detail::parse_subblocks(openxisf::test::element_at(*header, "Image").attribute("subblocks").value())
            .size(),
        256U);
    const std::vector<openxisf::detail::block_location> blocks = attachments(file);
    ASSERT_EQ(blocks.size(), 1U);
    EXPECT_GE(blocks[0].position, 16 + openxisf::test::header_of(file).size());
    EXPECT_EQ(open_strictly(file).read_pixels(0), model.pixels[0]);
}

TEST(conformance_writer, shuffling_takes_the_size_of_the_numbers_of_each_block)
{
    // The size of a sample, or of a part of a complex sample (spec §10.4), and of an element of a vector; blocks of
    // single bytes are not shuffled. Zeros compress with every codec.
    using openxisf::sample_format;
    unit_model model;
    for (const sample_format format :
         {sample_format::uint8, sample_format::uint16, sample_format::uint32, sample_format::uint64,
          sample_format::float32, sample_format::float64, sample_format::complex32, sample_format::complex64}) {
        image_info image = simple_image(format, {64}, 1);
        if (format == sample_format::float32 || format == sample_format::float64) {
            image.bounds = openxisf::bounds{.lower = 0.0, .upper = 1.0};
        }
        model.add(image, std::vector<std::byte>(image.data_size()));
    }
    openxisf::thumbnail small;
    small.geometry = {.dimensions = {16, 16}, .channels = 1};
    small.color_space = openxisf::color_space::gray;
    small.sample_format = sample_format::uint16;
    small.pixels.resize(512);
    model.images[0].thumbnail = small;
    model.properties.set("Test:Vector", std::vector<std::int16_t>(4000));
    model.properties.set("Test:Bytes", std::vector<std::uint8_t>(4000));

    write_options options = basic_options();
    options.codec = openxisf::codec::zlib;
    options.byte_shuffle = true;
    const std::vector<std::byte> file = written(model.writer(options));
    const auto header = parsed_header(file);
    std::vector<std::string> compressions;
    for (const pugi::xml_node& element : block_elements(*header)) {
        compressions.emplace_back(element.attribute("compression").value());
    }
    const std::vector<std::string> expected{"zlib:64",        "zlib+sh:512:2",  "zlib+sh:128:2", "zlib+sh:256:4",
                                            "zlib+sh:512:8",  "zlib+sh:256:4",  "zlib+sh:512:8", "zlib+sh:512:4",
                                            "zlib+sh:1024:8", "zlib+sh:8000:2", "zlib:4000"};
    EXPECT_EQ(compressions, expected);
    const openxisf::reader unit = open_strictly(file);
    for (std::size_t i = 0; i < model.images.size(); ++i) {
        EXPECT_EQ(unit.read_pixels(i), model.pixels[i]);
    }
    EXPECT_EQ(unit.properties(), model.properties);
}

TEST(conformance_writer, subblocks_divide_a_block_into_pieces_of_at_most_their_size)
{
    write_options options = basic_options();
    options.codec = openxisf::codec::lz4;
    options.subblock_size = 4096;
    for (const sink_kind sink : {sink_kind::rewritable, sink_kind::append_only}) {
        const std::vector<std::byte> file = written(rich_model().writer(options), sink);
        const auto header = parsed_header(file);
        // 10,212 bytes: 4,096, 4,096 and 2,020.
        const std::vector<openxisf::detail::subblock> subblocks =
            openxisf::detail::parse_subblocks(image_element(*header, "rich").attribute("subblocks").value());
        ASSERT_EQ(subblocks.size(), 3U);
        EXPECT_EQ(subblocks[0].uncompressed_size, 4096U);
        EXPECT_EQ(subblocks[2].uncompressed_size, 2020U);
        // A block within the size has no subblocks attribute.
        for (pugi::xml_node image = header->document_element().child("Image"); !image.empty();
             image = image.next_sibling("Image")) {
            const std::string_view compression = image.attribute("compression").value();
            if (!compression.empty()) {
                const std::uint64_t size = openxisf::detail::parse_compression(compression).uncompressed_size;
                EXPECT_EQ(image.attribute("subblocks").empty(), size <= 4096) << compression;
            }
        }
        EXPECT_EQ(open_strictly(file).read_pixels(0), rich_model().pixels[0]);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Checksums

TEST(conformance_writer, every_checksum_algorithm_is_written_over_the_stored_bytes)
{
    using openxisf::checksum_algorithm;
    using openxisf::detail::hash_algorithm;
    struct expectation
    {
        checksum_algorithm algorithm = checksum_algorithm::sha1;
        std::string_view name{};
        hash_algorithm hash = hash_algorithm::sha1;
    };
    for (const expectation& entry :
         {expectation{.algorithm = checksum_algorithm::sha1, .name = "sha1", .hash = hash_algorithm::sha1},
          expectation{.algorithm = checksum_algorithm::sha256, .name = "sha256", .hash = hash_algorithm::sha256},
          expectation{.algorithm = checksum_algorithm::sha512, .name = "sha512", .hash = hash_algorithm::sha512},
          expectation{.algorithm = checksum_algorithm::sha3_256, .name = "sha3-256", .hash = hash_algorithm::sha3_256},
          expectation{
              .algorithm = checksum_algorithm::sha3_512, .name = "sha3-512", .hash = hash_algorithm::sha3_512}}) {
        write_options options = basic_options();
        options.codec = openxisf::codec::zstd;
        options.checksum = entry.algorithm;
        const std::vector<std::byte> file = written(rich_model().writer(options));
        const openxisf::reader unit = open_strictly(file);
        EXPECT_EQ(unit.metadata().at("XISF:ChecksumAlgorithms").value, property_value(entry.name));
        EXPECT_EQ(unit.read_pixels(0), rich_model().pixels[0]);

        // The digest of the compressed bytes (spec §10.6.1), computed here independently of the reader.
        const auto header = parsed_header(file);
        const pugi::xml_node rich = image_element(*header, "rich");
        ASSERT_FALSE(rich.attribute("compression").empty());
        const openxisf::detail::block_location location =
            openxisf::detail::parse_location(rich.attribute("location").value());
        const std::span<const std::byte> stored = std::span(file).subspan(location.position, location.size);
        EXPECT_EQ(std::string(rich.attribute("checksum").value()),
                  std::string(entry.name) + ":" +
                      openxisf::detail::encode_hex(openxisf::detail::compute_digest(entry.hash, stored)));

        // A corrupted byte fails the read.
        std::vector<std::byte> corrupted = file;
        corrupted[location.position + 1] ^= std::byte{0x40};
        const openxisf::reader damaged = openxisf::test::open_unit(corrupted);
        EXPECT_TRUE(openxisf::test::throws<openxisf::integrity_error>(errc::checksum_mismatch,
                                                                      [&damaged] { (void)damaged.read_pixels(0); }));
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// What the writer sets or computes itself

TEST(conformance_writer, uuids_are_generated_on_request_and_written_in_lowercase)
{
    unit_model model;
    image_info named = simple_image(openxisf::sample_format::uint8, {1, 1}, 1);
    named.uuid = "C5C93B6D-9072-4E85-9548-1A5391377683";
    openxisf::thumbnail small;
    small.geometry = {.dimensions = {1, 1}, .channels = 3};
    small.uuid = "D5C93B6D-9072-4E85-9548-1A5391377683";
    small.pixels.resize(3);
    named.thumbnail = small;
    model.add(named, {std::byte{0}});
    model.add(simple_image(openxisf::sample_format::uint8, {1}, 1), {std::byte{0}});

    const std::vector<std::byte> file = written(model.writer(basic_options()));
    const auto header = parsed_header(file);
    EXPECT_STREQ(openxisf::test::element_at(*header, "Image").attribute("uuid").value(),
                 "c5c93b6d-9072-4e85-9548-1a5391377683");
    EXPECT_STREQ(openxisf::test::element_at(*header, "Image/Thumbnail").attribute("uuid").value(),
                 "d5c93b6d-9072-4e85-9548-1a5391377683");
    const openxisf::reader plain = open_strictly(file);
    EXPECT_EQ(plain.image(0).uuid, "c5c93b6d-9072-4e85-9548-1a5391377683");
    EXPECT_TRUE(plain.image(1).uuid.empty());

    write_options options = basic_options();
    options.generate_uuids = true;
    const openxisf::writer output = model.writer(options);
    const openxisf::reader first = open_strictly(written(output));
    const openxisf::reader second = open_strictly(written(output));
    EXPECT_EQ(first.image(0).uuid, "c5c93b6d-9072-4e85-9548-1a5391377683");
    EXPECT_TRUE(openxisf::detail::is_version_4_uuid(openxisf::detail::parse_uuid(first.image(1).uuid)));
    EXPECT_NE(first.image(1).uuid, second.image(1).uuid);
}

TEST(conformance_writer, an_icc_profile_is_written_unaltered_but_for_its_embedded_flag)
{
    // ICC.1:2022 §7.2.11: bit 0 of the big-endian flags at bytes 44 to 47 is the least significant bit of byte 47.
    std::vector<std::byte> profile = icc_profile();
    profile[47] = std::byte{0x02};
    profile[44] = std::byte{0x01};
    unit_model model;
    image_info image = simple_image(openxisf::sample_format::uint8, {1}, 1);
    image.icc_profile = profile;
    model.add(image, {std::byte{0}});
    const openxisf::reader unit = open_strictly(written(model.writer(basic_options())));
    std::vector<std::byte> expected = profile;
    expected[47] = std::byte{0x03};
    EXPECT_EQ(unit.image(0).icc_profile, expected);
}

TEST(conformance_writer, the_luminance_of_a_working_space_is_that_of_its_chromaticities)
{
    // Coefficients within the tolerance of the derived ones are replaced with them (spec §11.8.1).
    unit_model model;
    image_info image = simple_image(openxisf::sample_format::uint8, {1}, 3, openxisf::color_space::rgb);
    image.rgb_working_space = openxisf::rgb_working_space{.luminance = {0.22249, 0.71689, 0.06062}};
    model.add(image, std::vector<std::byte>(3));
    const openxisf::reader unit = open_strictly(written(model.writer(basic_options())));
    const openxisf::rgb_working_space& space = openxisf::test::value_of(unit.image(0).rgb_working_space);
    EXPECT_EQ(space.luminance, openxisf::luminance_coefficients(space.x, space.y));
    EXPECT_FALSE(space.gamma.has_value());
}

TEST(conformance_writer, the_default_range_of_an_unsigned_integer_image_is_not_written)
{
    // Spec §11.5.1: bounds equal to the default range [0, 2^k - 1] of a k-bit image should not be specified. A model
    // read from another encoder may hold them; it reads back without them, with the same representable range.
    unit_model model;
    for (const auto& [format, upper] : std::initializer_list<std::pair<openxisf::sample_format, double>>{
             {openxisf::sample_format::uint8, 255.0},
             {openxisf::sample_format::uint16, 65535.0},
             {openxisf::sample_format::uint32, 4294967295.0},
             {openxisf::sample_format::uint64, static_cast<double>(std::numeric_limits<std::uint64_t>::max())}}) {
        image_info image = simple_image(format, {1}, 1);
        image.bounds = openxisf::bounds{.lower = 0.0, .upper = upper};
        model.add(image, std::vector<std::byte>(image.data_size()));
    }
    // Other bounds are written: a narrower range, and the bounds of a floating point image.
    image_info narrower = simple_image(openxisf::sample_format::uint16, {1}, 1);
    narrower.bounds = openxisf::bounds{.lower = 0.0, .upper = 4095.0};
    model.add(narrower, std::vector<std::byte>(2));
    image_info floating = simple_image(openxisf::sample_format::float32, {1}, 1);
    floating.bounds = openxisf::bounds{.lower = 0.0, .upper = 1.0};
    model.add(floating, std::vector<std::byte>(4));

    const std::vector<std::byte> file = written(model.writer(basic_options()));
    const auto header = parsed_header(file);
    std::vector<std::string> written_bounds;
    for (const pugi::xml_node& element : header->document_element().children("Image")) {
        written_bounds.emplace_back(element.attribute("bounds").value());
    }
    EXPECT_EQ(written_bounds, (std::vector<std::string>{"", "", "", "", "0:4095", "0:1"}));
    const openxisf::reader unit = open_strictly(file);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_FALSE(unit.image(i).bounds.has_value()) << i;
        EXPECT_EQ(unit.image(i).representable_range(), model.images[i].bounds) << i;
    }
    EXPECT_EQ(unit.image(4).bounds, narrower.bounds);
}

TEST(conformance_writer, strings_that_xml_cannot_hold_go_in_data_blocks)
{
    const std::vector<std::byte> file = written(rich_model().writer(basic_options()));
    const auto header = parsed_header(file);
    std::size_t checked = 0;
    for (pugi::xml_node property = openxisf::test::element_at(*header, "Image/Property"); !property.empty();
         property = property.next_sibling("Property")) {
        const std::string_view id = property.attribute("id").value();
        const bool in_block = !property.attribute("location").empty();
        if (id == "Test:Control") {
            EXPECT_EQ(std::string_view(property.attribute("location").value()), "inline:base64");
            EXPECT_EQ(openxisf::test::text(openxisf::detail::decode_base64(property.text().get())),
                      "bell\x07, escape\x1B, U+FFFF \xEF\xBF\xBF");
            ++checked;
        } else if (id == "Test:Lines" || id == "Test:LongString" || id == "Test:Markup") {
            EXPECT_FALSE(in_block) << id;
            ++checked;
        }
    }
    EXPECT_EQ(checked, 4U);
}

TEST(conformance_writer, strings_longer_than_1_mib_go_in_data_blocks)
{
    // A reader limits the size of a header, and reads a data block within its limit of loaded data instead.
    unit_model model;
    const std::string at_the_limit(std::size_t{1} << 20, 'a');
    const std::string beyond(at_the_limit + "b");
    model.properties.set("Test:AtTheLimit", at_the_limit);
    model.properties.set("Test:Beyond", beyond);
    const std::vector<std::byte> file = written(model.writer(basic_options()));
    const auto header = parsed_header(file);
    const pugi::xml_node first = openxisf::test::element_at(*header, "Property");
    EXPECT_EQ(std::string_view(first.attribute("id").value()), "Test:AtTheLimit");
    EXPECT_TRUE(first.attribute("location").empty());
    const pugi::xml_node second = first.next_sibling("Property");
    EXPECT_EQ(std::string_view(second.attribute("id").value()), "Test:Beyond");
    EXPECT_TRUE(std::string_view(second.attribute("location").value()).starts_with("attachment:"));
    const openxisf::reader unit = open_strictly(file);
    EXPECT_EQ(unit.properties(), model.properties);
}

// ---------------------------------------------------------------------------------------------------------------------
// The API

TEST(conformance_writer, images_need_pixel_data_of_their_size_and_type)
{
    openxisf::writer output(basic_options());
    const image_info image = simple_image(openxisf::sample_format::uint16, {3, 2}, 1);
    const std::vector<std::uint16_t> samples(6);
    const std::vector<std::uint8_t> bytes(12);
    EXPECT_TRUE(openxisf::test::throws<openxisf::usage_error>(
        errc::invalid_argument, [&] { (void)output.add_image(image, std::as_bytes(std::span(samples)).first(11)); }));
    EXPECT_TRUE(openxisf::test::throws<openxisf::usage_error>(
        errc::invalid_argument, [&] { (void)output.add_image(image, std::span<const std::uint8_t>(bytes)); }));
    EXPECT_TRUE(output.images().empty());
    EXPECT_EQ(output.add_image(image, std::span<const std::uint16_t>(samples)), 0U);
    EXPECT_EQ(output.add_image(image, std::as_bytes(std::span(samples))), 1U);
    ASSERT_EQ(output.images().size(), 2U);
    EXPECT_EQ(output.images()[1], image);
}

TEST(conformance_writer, the_compression_level_is_from_0_to_100)
{
    write_options highest = basic_options();
    highest.compression_level = 100;
    EXPECT_NO_THROW(openxisf::writer output(highest));
    for (const int level : {-1, 101}) {
        write_options options = basic_options();
        options.compression_level = level;
        EXPECT_TRUE(openxisf::test::throws<openxisf::usage_error>(errc::invalid_argument,
                                                                  [&options] { openxisf::writer output(options); }));
    }
}

TEST(conformance_writer, the_codec_and_the_checksum_algorithm_are_among_their_enumerators)
{
    // A value cast from another number would be written as another codec or algorithm.
    for (const openxisf::codec codec : {openxisf::codec::zlib, openxisf::codec::zstd}) {
        write_options options = basic_options();
        options.codec = codec;
        EXPECT_NO_THROW(openxisf::writer output(options));
    }
    for (const openxisf::checksum_algorithm algorithm :
         {openxisf::checksum_algorithm::sha1, openxisf::checksum_algorithm::sha3_512}) {
        write_options options = basic_options();
        options.checksum = algorithm;
        EXPECT_NO_THROW(openxisf::writer output(options));
    }
    for (const int value : {4, 9, 255}) {
        write_options options = basic_options();
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): the value is outside on purpose
        options.codec = static_cast<openxisf::codec>(value);
        EXPECT_TRUE(openxisf::test::throws<openxisf::usage_error>(errc::invalid_argument, [&options] {
            openxisf::writer output(options);
        })) << value;
    }
    for (const int value : {5, 9, 255}) {
        write_options options = basic_options();
        // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): the value is outside on purpose
        options.checksum = static_cast<openxisf::checksum_algorithm>(value);
        EXPECT_TRUE(openxisf::test::throws<openxisf::usage_error>(errc::invalid_argument, [&options] {
            openxisf::writer output(options);
        })) << value;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Saving to a file (spec §9.6)

TEST(conformance_writer, a_file_name_ends_with_xisf)
{
    const openxisf::test::temp_directory directory;
    const unit_model model = rich_model();
    const openxisf::writer output = model.writer(basic_options());
    for (const std::string_view name :
         {"unit.fits", "unit", "unit.xisf.tmp", "unit.xish", ".xis", "unitxisf", "unit.yisf"}) {
        EXPECT_TRUE(openxisf::test::throws<openxisf::usage_error>(errc::invalid_argument, [&] {
            output.save(directory.file(name));
        })) << name;
    }
    EXPECT_EQ(directory.entries(), 0U);
    for (const std::string_view name : {"unit.xisf", "Unit.XISF",
                                        "N\xC3\xA9"
                                        "bula.Xisf",
                                        ".xisf"}) {
        output.save(directory.file(name));
        openxisf::test::record_unit(
            openxisf::test::bytes(openxisf::test::read_file(openxisf::test::path_of(directory.file(name)))));
        const openxisf::reader unit(directory.file(name));
        EXPECT_EQ(unit.image(0), model.images[0]) << name;
    }
}

TEST(conformance_writer, a_file_is_replaced_only_by_a_complete_unit)
{
    const openxisf::test::temp_directory directory;
    const std::string path = directory.file("unit.xisf");
    openxisf::test::write_file(openxisf::test::path_of(path), openxisf::test::bytes("old content"));
    const unit_model model = rich_model();

    // Cancelled halfway: the old file stays, and nothing else is left.
    write_options options = basic_options();
    std::uint64_t calls = 0;
    options.progress = [&calls](std::uint64_t /*done*/, std::uint64_t /*total*/) { return ++calls < 3; };
    EXPECT_THROW(model.writer(options).save(path), openxisf::cancelled_error);
    EXPECT_EQ(openxisf::test::read_file(openxisf::test::path_of(path)), "old content");
    EXPECT_EQ(directory.entries(), 1U);

    // Complete, flushed to the storage device.
    options.progress = {};
    options.flush_to_disk = true;
    model.writer(options).save(path);
    openxisf::test::record_unit(openxisf::test::bytes(openxisf::test::read_file(openxisf::test::path_of(path))));
    EXPECT_EQ(openxisf::reader(path).images().size(), model.images.size());
}

TEST(conformance_writer, a_cancelled_save_leaves_no_file)
{
    const openxisf::test::temp_directory directory;
    write_options options = basic_options();
    options.progress = [](std::uint64_t /*done*/, std::uint64_t /*total*/) { return false; };
    const unit_model model = rich_model();
    EXPECT_TRUE(openxisf::test::throws<openxisf::cancelled_error>(
        errc::cancelled, [&] { model.writer(options).save(directory.file("unit.xisf")); }));
    EXPECT_EQ(directory.entries(), 0U);
}

// ---------------------------------------------------------------------------------------------------------------------
// Progress and failures

TEST(conformance_writer, progress_counts_the_data_of_the_attached_blocks)
{
    const unit_model model = rich_model();
    std::uint64_t attached = 0;
    for (const std::vector<std::byte>& pixels : model.pixels) {
        attached += pixels.size();
    }
    // The thumbnail, the large vector, the standalone matrix, and the cells of the two tables that are attached.
    attached += 120 + 8000 + 10000 + (2 * 4000);

    struct expectation
    {
        bool encoded = false;
        sink_kind sink = sink_kind::rewritable;
        std::uint64_t total = 0;
    };
    for (const expectation& entry :
         {expectation{.total = attached}, expectation{.sink = sink_kind::append_only, .total = attached},
          expectation{.encoded = true, .total = attached},
          expectation{.encoded = true, .sink = sink_kind::append_only, .total = 2 * attached}}) {
        write_options options = basic_options();
        if (entry.encoded) {
            options.codec = openxisf::codec::lz4;
            options.checksum = openxisf::checksum_algorithm::sha1;
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
        options.progress = [&calls](std::uint64_t done, std::uint64_t total) {
            calls.emplace_back(done, total);
            return true;
        };
        (void)written(model.writer(options), entry.sink);
        ASSERT_FALSE(calls.empty());
        EXPECT_EQ(calls.front(), std::make_pair(std::uint64_t{0}, entry.total));
        EXPECT_EQ(calls.back(), std::make_pair(entry.total, entry.total));
        EXPECT_TRUE(std::ranges::is_sorted(calls));
    }
}

// io.h: the exceptions of a progress function pass through unchanged, and a save to a path leaves no file.
TEST(conformance_writer, the_exceptions_of_a_progress_function_pass_through)
{
    const openxisf::test::temp_directory directory;
    std::uint64_t calls = 0;
    write_options options = basic_options();
    // Fails at its second call, once the save has begun.
    options.progress = [&calls](std::uint64_t /*done*/, std::uint64_t /*total*/) {
        if (++calls == 2) {
            throw openxisf::test::injected_fault();
        }
        return true;
    };
    const unit_model model = rich_model();
    const openxisf::writer output = model.writer(options);
    openxisf::memory_sink sink;
    EXPECT_THROW(output.save(sink), openxisf::test::injected_fault);
    calls = 0;
    EXPECT_THROW(output.save(directory.file("unit.xisf")), openxisf::test::injected_fault);
    EXPECT_EQ(directory.entries(), 0U);
    calls = 0;
    EXPECT_THROW(output.save_distributed(directory.file("unit.xish")), openxisf::test::injected_fault);
    EXPECT_EQ(directory.entries(), 0U);
}

TEST(conformance_writer, a_save_cancelled_before_it_starts_writes_nothing)
{
    write_options options = basic_options();
    options.progress = [](std::uint64_t /*done*/, std::uint64_t /*total*/) { return false; };
    openxisf::memory_sink sink;
    EXPECT_THROW(rich_model().writer(options).save(sink), openxisf::cancelled_error);
    EXPECT_EQ(sink.position(), 0U);
}

TEST(conformance_writer, a_sink_that_already_holds_bytes_is_refused)
{
    // Every position in a unit counts from its first byte (spec §9.2), so a second unit after the first would have its
    // blocks misplaced, with a codec or without.
    write_options compressed = basic_options();
    compressed.codec = openxisf::codec::zstd;
    const unit_model model = rich_model();
    for (const write_options& options : {basic_options(), compressed}) {
        const openxisf::writer output = model.writer(options);
        openxisf::memory_sink memory;
        openxisf::callback_sink append_only([](std::span<const std::byte> /*data*/) {});
        for (openxisf::output_sink* sink : std::array<openxisf::output_sink*, 2>{&memory, &append_only}) {
            output.save(*sink);
            const std::uint64_t first = sink->position();
            EXPECT_TRUE(
                openxisf::test::throws<openxisf::usage_error>(errc::invalid_argument, [&] { output.save(*sink); }));
            EXPECT_EQ(sink->position(), first);
        }
    }
}

TEST(conformance_writer, failures_of_the_sink_pass_through)
{
    using openxisf::test::fault;
    write_options options = basic_options();
    options.codec = openxisf::codec::zstd;
    options.checksum = openxisf::checksum_algorithm::sha256;
    const unit_model model = rich_model();
    const openxisf::writer output = model.writer(options);
    for (const bool rewritable : {true, false}) {
        std::size_t calls = 0;
        std::size_t foreign = 0;
        for (std::size_t failing = 1; calls == 0; ++failing) {
            openxisf::memory_sink memory;
            std::vector<std::byte> appended;
            openxisf::callback_sink append_only([&appended](std::span<const std::byte> data) {
                appended.insert(appended.end(), data.begin(), data.end());
            });
            openxisf::output_sink& inner = rewritable ? static_cast<openxisf::output_sink&>(memory) : append_only;
            openxisf::test::faulty_sink sink(inner, failing,
                                             failing % 2 == 0 ? fault::error : fault::foreign_exception);
            try {
                output.save(sink);
                calls = failing - 1;
            } catch (const openxisf::io_error& failure) {
                EXPECT_EQ(failure.code(), errc::write_failed);
            } catch (const openxisf::test::injected_fault&) {
                // Passed through unchanged.
                ++foreign;
            }
        }
        EXPECT_EQ(foreign, (calls + 1) / 2) << rewritable;
        // Every call failed once: the writes and rewrites of the preamble, the header, the padding and the blocks, and
        // the finish.
        EXPECT_GT(calls, 10U) << rewritable;
    }
}

// Spec §11.5.3.7.6 and §11.5.3.7.8: an encoder keeps the properties of a solution as it is given them, those of a
// revision of spec §11.5.3.7 that it does not read included, whatever their types; the types of revision 1 apply to a
// solution of revision 1.
TEST(conformance_writer, the_properties_of_an_astrometric_solution_are_kept_as_they_are)
{
    image_info image = simple_image(openxisf::sample_format::uint16, {2, 2}, 1);
    image.properties.set("AstrometricSolution:Version", "2.0");
    image.properties.set("AstrometricSolution:ReferenceCelestialCoordinates", "83.8 -5.4");
    image.properties.set("AstrometricSolution:Future:Kernel", std::vector<double>{1.0, 2.0});
    const std::array<std::uint16_t, 4> samples{1, 2, 3, 4};
    openxisf::writer output(basic_options());
    (void)output.add_image(image, std::span<const std::uint16_t>(samples));
    const openxisf::reader unit = open_strictly(written(output));
    EXPECT_TRUE(openxisf::test::no_diagnostics(unit.diagnostics()));
    EXPECT_EQ(unit.image(0).properties, image.properties);

    image.properties.set("AstrometricSolution:Version", "1.0");
    openxisf::writer refused(basic_options());
    (void)refused.add_image(image, std::span<const std::uint16_t>(samples));
    EXPECT_TRUE(openxisf::test::throws<openxisf::validation_error>(errc::reserved_property_type,
                                                                   [&] { (void)written(refused); }));
}

} // namespace
