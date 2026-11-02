// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/openxisf.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The I/O classes across the boundary of a shared library: a source of the library read through its interface, and an
// exception of the library caught by its type.
bool reads_through_the_library()
{
    const std::array<std::byte, 3> data{std::byte{1}, std::byte{2}, std::byte{3}};
    const openxisf::memory_source source{std::span<const std::byte>(data)};
    const openxisf::input_source& any = source;
    std::array<std::byte, 2> destination{};
    any.read(1, destination);
    if (destination != std::array<std::byte, 2>{std::byte{2}, std::byte{3}}) {
        return false;
    }
    try {
        any.read(2, destination);
    } catch (const openxisf::io_error& failure) {
        return failure.code() == openxisf::errc::end_of_data;
    }
    return false;
}

std::unique_ptr<openxisf::input_source> unit_of(std::string_view header)
{
    std::vector<std::byte> unit;
    for (const char c : std::string_view("XISF0100")) {
        unit.push_back(static_cast<std::byte>(c));
    }
    for (int shift = 0; shift < 64; shift += 8) {
        // The header length, then the reserved field. Both are little-endian.
        unit.push_back(static_cast<std::byte>(shift < 32 ? (header.size() >> shift) & 0xFFU : 0U));
    }
    for (const char c : header) {
        unit.push_back(static_cast<std::byte>(c));
    }
    return std::make_unique<openxisf::memory_source>(std::move(unit));
}

// The reader across the boundary: a signed unit opened with its diagnostics and its signature, and a unit refused with
// an exception caught by its type.
bool opens_units_through_the_library()
{
    constexpr std::string_view root = R"(<xisf version="1.0" xmlns="http://www.pixinsight.com/xisf">)"
                                      R"(<Metadata/><Unknown/></xisf>)";
    constexpr std::string_view signature = R"(<Signature xmlns="http://www.w3.org/2000/09/xmldsig#"/>)";
    const openxisf::reader file(
        unit_of(R"(<?xml version="1.0" encoding="UTF-8"?>)" + std::string(root) + std::string(signature)));
    const auto unknown = [](const openxisf::diagnostic& entry) {
        return entry.code == openxisf::errc::unknown_element && entry.context.element == "/xisf/Unknown[1]";
    };
    if (file.storage() != openxisf::unit_storage::monolithic || !std::ranges::any_of(file.diagnostics(), unknown) ||
        file.signature() != openxisf::signature_status::not_verified || file.signature_xml() != signature ||
        file.signed_xml() != root) {
        return false;
    }
    try {
        const openxisf::reader refused(unit_of(R"(<?xml version="1.0" encoding="UTF-8"?><xisf version="1.0">)"));
    } catch (const openxisf::invalid_data_error& failure) {
        return failure.code() == openxisf::errc::invalid_header_length;
    }
    return false;
}

// The properties across the boundary: values held in the types of the library, and its checked accessors.
bool reads_properties_through_the_library()
{
    const openxisf::reader file(unit_of(
        R"(<?xml version="1.0" encoding="UTF-8"?><xisf version="1.0" xmlns="http://www.pixinsight.com/xisf"><Metadata>)"
        R"(<Property id="XISF:CreationTime" type="TimePoint" value="2026-10-03T12:00:00Z"/>)"
        R"(<Property id="XISF:CreatorApplication" type="String">consumer 1.0</Property></Metadata>)"
        R"(<Property id="Gains" type="F64Vector" length="2" location="inline:hex">)"
        R"(000000000000f03f0000000000000040</Property></xisf>)"));
    if (!file.diagnostics().empty() || file.metadata().size() != 2 ||
        file.metadata().at("XISF:CreatorApplication").value.get<std::string>() != "consumer 1.0" ||
        file.metadata().at("XISF:CreationTime").value.get<openxisf::date_time>().hour != 12) {
        return false;
    }
    const openxisf::property_value& gains = file.properties().at("Gains").value;
    if (gains.type() != openxisf::property_type::f64_vector || gains.length() != 2 ||
        gains.elements<double>()[1] != 2.0 || openxisf::property_type_name(gains.type()) != "F64Vector") {
        return false;
    }
    try {
        (void)gains.get<float>();
    } catch (const openxisf::usage_error& failure) {
        return failure.code() == openxisf::errc::invalid_argument;
    }
    return false;
}

// The images across the boundary: the description of an image, its pixels read in another storage model, a typed read,
// whose template calls into the library, and a typed read of the wrong type refused with a usage_error.
bool reads_pixels_through_the_library()
{
    const openxisf::reader file(unit_of(
        R"(<?xml version="1.0" encoding="UTF-8"?><xisf version="1.0" xmlns="http://www.pixinsight.com/xisf"><Metadata>)"
        R"(<Property id="XISF:CreationTime" type="TimePoint" value="2026-10-03T12:00:00Z"/>)"
        R"(<Property id="XISF:CreatorApplication" type="String">consumer 1.0</Property></Metadata>)"
        R"(<Image id="rgb" geometry="2:1:3" sampleFormat="UInt16" colorSpace="RGB" location="embedded">)"
        R"(<Data encoding="hex">010002000300040005000600</Data></Image></xisf>)"));
    if (!file.diagnostics().empty() || file.images().size() != 1 || file.image(0).id != "rgb" ||
        file.image(0).color_space != openxisf::color_space::rgb || file.image(0).data_size() != 12) {
        return false;
    }
    const std::vector<std::uint16_t> normal =
        file.read_pixels<std::uint16_t>(0, {.storage = openxisf::pixel_storage::normal});
    if (normal != std::vector<std::uint16_t>{1, 3, 5, 2, 4, 6} || file.read_pixels(0).size() != 12) {
        return false;
    }
    try {
        (void)file.read_pixels<float>(0);
    } catch (const openxisf::usage_error& failure) {
        return failure.code() == openxisf::errc::invalid_argument;
    }
    return false;
}

// The regions of interest across the boundary: a region whose samples an exported member counts, read as values of
// its sample format by a template that calls into the library, and with a pitch into memory of the application.
bool reads_regions_through_the_library()
{
    const openxisf::reader file(unit_of(
        R"(<?xml version="1.0" encoding="UTF-8"?><xisf version="1.0" xmlns="http://www.pixinsight.com/xisf"><Metadata>)"
        R"(<Property id="XISF:CreationTime" type="TimePoint" value="2026-10-03T12:00:00Z"/>)"
        R"(<Property id="XISF:CreatorApplication" type="String">consumer 1.0</Property></Metadata>)"
        R"(<Image geometry="3:2:1" sampleFormat="UInt16" location="embedded">)"
        R"(<Data encoding="hex">010002000300040005000600</Data></Image></xisf>)"));
    const openxisf::pixel_region region{.origin = {1, 0}, .size = {2, 2}};
    if (!file.diagnostics().empty() || region.sample_count(file.image(0).geometry) != 4 ||
        file.read_pixels<std::uint16_t>(0, {.region = region}) != std::vector<std::uint16_t>{2, 3, 5, 6}) {
        return false;
    }
    std::vector<std::uint16_t> tile(5, 0xFFFF);
    file.read_pixels(0, std::span(tile), {.region = region, .pitch = 3});
    return tile == std::vector<std::uint16_t>{2, 3, 0xFFFF, 5, 6};
}

// A device of the application over its own buffer, whose virtual functions the library calls across the boundary.
class buffer_device final : public openxisf::random_access_device
{
public:
    explicit buffer_device(std::vector<std::byte>& bytes) : bytes_(bytes) {}

    std::uint64_t size() const override
    {
        return bytes_.size();
    }

    void read(std::uint64_t offset, std::span<std::byte> destination) const override
    {
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), destination.size(), destination.begin());
    }

    void write(std::uint64_t offset, std::span<const std::byte> data) override
    {
        std::ranges::copy(data, bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        ++writes_;
    }

    int writes() const noexcept
    {
        return writes_;
    }

private:
    std::vector<std::byte>& bytes_;
    int writes_ = 0;
};

// The editor across the boundary: a reserved image with a checksum, filled through a device of the application and
// through one of the library, committed, and verified by a reader.
bool edits_units_through_the_library()
{
    openxisf::image_info info;
    info.geometry = {.dimensions = {3, 2}, .channels = 1};
    openxisf::writer output({.creator_application = "consumer 1.0", .checksum = openxisf::checksum_algorithm::sha1});
    output.add_reserved_image(info);
    openxisf::memory_sink sink;
    output.save(sink);
    std::vector<std::byte> unit = sink.release();

    const std::vector<std::uint16_t> row{7, 8, 9};
    auto device = std::make_unique<buffer_device>(unit);
    const buffer_device& counted = *device;
    {
        openxisf::editor edit(std::move(device));
        edit.write_pixels(0, std::span(row), {.region = openxisf::pixel_region{.origin = {0, 1}, .size = {3, 1}}});
        edit.commit();
        if (counted.writes() != 2) {
            return false;
        }
    }
    {
        openxisf::editor edit(std::make_unique<openxisf::memory_device>(unit));
        edit.write_pixels(0, std::span(row), {.region = openxisf::pixel_region{.origin = {0, 0}, .size = {3, 1}}});
        edit.commit();
    }
    const openxisf::reader file(std::make_unique<openxisf::memory_source>(std::move(unit)), {.strict = true});
    return file.read_pixels<std::uint16_t>(0) == std::vector<std::uint16_t>{7, 8, 9, 7, 8, 9};
}

// The elements that describe images across the boundary: keywords, a thumbnail and a table in the types of the
// library, a header-only open, and the loader, which changes the reader through an exported member.
bool reads_descriptions_through_the_library()
{
    const std::string_view header =
        R"(<?xml version="1.0" encoding="UTF-8"?><xisf version="1.0" xmlns="http://www.pixinsight.com/xisf"><Metadata>)"
        R"(<Property id="XISF:CreationTime" type="TimePoint" value="2026-10-03T12:00:00Z"/>)"
        R"(<Property id="XISF:CreatorApplication" type="String">consumer 1.0</Property></Metadata>)"
        R"(<Image geometry="1:1:1" sampleFormat="UInt8" location="embedded"><Data encoding="hex">07</Data>)"
        R"(<FITSKeyword name="EXPTIME" value="300" comment="Exposure time in seconds"/>)"
        R"(<Resolution horizontal="120" vertical="120" unit="cm"/>)"
        R"(<Thumbnail geometry="1:1:1" sampleFormat="UInt8" colorSpace="Gray" location="embedded">)"
        R"(<Data encoding="hex">2a</Data></Thumbnail>)"
        R"(<Table id="Stars"><Structure><Field id="name" type="String"/></Structure>)"
        R"(<Row><Cell>Vega</Cell></Row></Table></Image></xisf>)";
    openxisf::reader file(unit_of(header), {.header_only = true});
    if (!file.diagnostics().empty() || file.ancillary_data_loaded() || file.images().size() != 1) {
        return false;
    }
    // Loading invalidates what the reader returned, so the description is not kept beyond this check.
    if (const openxisf::image_info& described = file.image(0);
        !described.thumbnail || !described.thumbnail->pixels.empty()) {
        return false;
    }
    file.load_ancillary_data();
    const openxisf::image_info& info = file.image(0);
    return file.ancillary_data_loaded() && info.fits_keywords.size() == 1 && info.fits_keywords[0].value == "300" &&
           info.resolution && info.resolution->unit == openxisf::resolution_unit::centimeter && info.thumbnail &&
           info.thumbnail->pixels == std::vector<std::byte>{std::byte{0x2a}} && info.tables.size() == 1 &&
           info.tables[0].rows.at(0).at(0).get<std::string>() == "Vega";
}

// The writer across the boundary: a unit written into a sink of the library, a typed image, which calls an exported
// member from a template, how the reader finds its block stored, and a validation error caught by its type.
bool writes_through_the_library()
{
    openxisf::writer output({.creator_application = "consumer 1.0",
                             .codec = openxisf::codec::zstd,
                             .byte_shuffle = true,
                             .checksum = openxisf::checksum_algorithm::sha1});
    openxisf::image_info info;
    info.geometry = {.dimensions = {3, 2}, .channels = 1};
    info.properties.set("Instrument:ExposureTime", 300.0F);
    const std::vector<std::uint16_t> samples{1, 2, 3, 4, 5, 6};
    (void)output.add_image(info, std::span<const std::uint16_t>(samples));
    openxisf::memory_sink sink;
    output.save(sink);
    const openxisf::reader file(std::make_unique<openxisf::memory_source>(sink.release()), {.strict = true});
    if (file.read_pixels<std::uint16_t>(0) != samples || file.image(0).properties != info.properties) {
        return false;
    }
    // Twelve bytes that Zstandard does not make smaller are stored as they are, with their checksum.
    const openxisf::block_info block = file.block(0);
    if (block.location != openxisf::block_location::attachment || block.codec.has_value() ||
        block.checksum != openxisf::checksum_algorithm::sha1 || block.uncompressed_size != 12) {
        return false;
    }
    try {
        openxisf::writer invalid({.creator_application = "no version"});
        invalid.save(sink);
    } catch (const openxisf::validation_error& failure) {
        return failure.code() == openxisf::errc::invalid_metadata;
    }
    return false;
}

// A distributed unit across the boundary: written into two sinks, read with a resolver of the application, and the file
// resolver of the library, which refuses a path outside its directory.
bool reads_distributed_units_through_the_library()
{
    openxisf::writer output({.creator_application = "consumer 1.0"});
    openxisf::image_info info;
    info.geometry = {.dimensions = {2, 2}, .channels = 1};
    const std::vector<std::uint16_t> samples{1, 2, 3, 4};
    (void)output.add_image(info, std::span<const std::uint16_t>(samples));
    openxisf::memory_sink header;
    auto blocks = std::make_shared<openxisf::memory_sink>();
    output.save_distributed(header, *blocks, "unit.xisb");
    const openxisf::reader file(std::make_unique<openxisf::memory_source>(header.release()),
                                {.strict = true, .resolver = [blocks](const openxisf::external_reference& reference) {
                                     return reference.location == "unit.xisb"
                                                ? std::make_unique<openxisf::memory_source>(blocks->data())
                                                : nullptr;
                                 }});
    if (file.storage() != openxisf::unit_storage::distributed || file.read_pixels<std::uint16_t>(0) != samples) {
        return false;
    }
    // Refused before the file system is asked about it: absolute paths are not allowed by default.
    try {
        (void)openxisf::file_resolver(".")({.form = openxisf::location_form::absolute_path, .location = "/unit.xisb"});
    } catch (const openxisf::unsupported_error& failure) {
        return failure.code() == openxisf::errc::location_not_allowed;
    }
    return false;
}

// The algorithms of the specification across the boundary: an exported class with an inline member, the conversion of
// pixel data, display functions, orientation and format rendering, and a usage_error caught by its type.
bool runs_the_algorithms_through_the_library()
{
    const openxisf::color_converter srgb;
    const openxisf::color_components white = srgb.rgb_to_lab({1.0, 1.0, 1.0});
    if (std::abs(white[0] - 1.0) > 1e-12 || srgb.rgb_to_xyz_matrix()[1][1] <= 0.0) {
        return false;
    }
    openxisf::image_info lab;
    lab.geometry = {.dimensions = {1, 1}, .channels = 3};
    lab.color_space = openxisf::color_space::cie_lab;
    std::vector<std::uint16_t> samples{65535, 32768, 32768};
    // The lightest colour, a and b within 1/65535 of the achromatic axis: white.
    openxisf::convert_lab_to_rgb(std::as_writable_bytes(std::span(samples)), lab);
    if (!std::ranges::all_of(samples, [](std::uint16_t sample) { return sample > 65500; })) {
        return false;
    }
    openxisf::image_info rgb = lab;
    rgb.color_space = openxisf::color_space::rgb;
    const openxisf::display_function stretch =
        openxisf::adaptive_display_function(std::as_bytes(std::span(samples)), rgb);
    if (openxisf::apply_display_function(stretch, 0, 0.5) != openxisf::midtones_transfer(0.5, stretch.midtones[0])) {
        return false;
    }
    const std::vector<std::byte> turned =
        openxisf::orient_pixels(std::as_bytes(std::span(samples)), {.dimensions = {3, 1}, .channels = 1}, 2,
                                openxisf::pixel_storage::planar, openxisf::orientation::rotate_90);
    if (turned.size() != 6 ||
        openxisf::format_value(std::uint16_t{255}, {.base = openxisf::format_base::hexadecimal}) != "ff") {
        return false;
    }
    try {
        openxisf::rgb_working_space flat;
        flat.y = {0.0, 0.5, 0.5};
        (void)openxisf::color_converter(flat);
    } catch (const openxisf::usage_error& failure) {
        return failure.code() == openxisf::errc::invalid_rgb_working_space;
    }
    return false;
}

// An astrometric solution across the boundary: a class whose copies share their data, the evaluation in both
// directions, the removal of a solution, and a usage_error caught by its type.
bool evaluates_astrometric_solutions_through_the_library()
{
    openxisf::property_list properties;
    properties.set("AstrometricSolution:Version", "1.0");
    properties.set("AstrometricSolution:ProjectionSystem", "Gnomonic");
    properties.set("AstrometricSolution:ReferenceCelestialCoordinates", std::vector<double>{83.8, -5.4});
    properties.set("AstrometricSolution:ReferenceImageCoordinates", std::vector<double>{200.0, 150.0});
    properties.set("AstrometricSolution:LinearTransformationMatrix",
                   openxisf::property_value::matrix(2, 2, std::vector<double>{-0.01, 0.0, 0.0, -0.01}));
    const openxisf::astrometric_solution solution(properties);
    openxisf::astrometric_solution copy{openxisf::property_list{}};
    copy = solution;
    const std::optional<openxisf::celestial_point> centre = copy.image_to_celestial({.x = 200.0, .y = 150.0});
    if (copy.layer() != openxisf::astrometric_layer::linear || !centre || std::abs(centre->ra - 83.8) > 1e-12 ||
        std::abs(centre->dec + 5.4) > 1e-12) {
        return false;
    }
    const std::optional<openxisf::image_point> back = solution.celestial_to_image(*centre);
    if (!back || std::abs(back->x - 200.0) > 1e-9 || openxisf::remove_astrometric_solution(properties) != 5) {
        return false;
    }
    try {
        (void)solution.image_to_celestial({}, openxisf::astrometric_layer::distortion);
    } catch (const openxisf::usage_error& failure) {
        return failure.code() == openxisf::errc::invalid_argument;
    }
    return false;
}

} // namespace

// Built against an installed package. It fails when the header, the library and the package version file
// do not agree.
int main()
{
    try {
        const std::string_view linked = openxisf::version();
        std::cout << "openxisf " << linked << '\n';

        if (linked != OPENXISF_VERSION_STRING) {
            std::cerr << "the library reports " << linked << ", the header says " << OPENXISF_VERSION_STRING << '\n';
            return 1;
        }
        if (linked != EXPECTED_VERSION) {
            std::cerr << "the library reports " << linked << ", find_package found " << EXPECTED_VERSION << '\n';
            return 1;
        }
        if (!reads_through_the_library()) {
            std::cerr << "a memory source of the library does not read as expected\n";
            return 1;
        }
        if (!opens_units_through_the_library()) {
            std::cerr << "the reader of the library does not open units as expected\n";
            return 1;
        }
        if (!reads_pixels_through_the_library()) {
            std::cerr << "the images of the library do not read as expected\n";
            return 1;
        }
        if (!reads_regions_through_the_library()) {
            std::cerr << "the regions of images of the library do not read as expected\n";
            return 1;
        }
        if (!reads_properties_through_the_library()) {
            std::cerr << "the properties of the library do not read as expected\n";
            return 1;
        }
        if (!reads_descriptions_through_the_library()) {
            std::cerr << "the elements that describe images do not read as expected\n";
            return 1;
        }
        if (!writes_through_the_library()) {
            std::cerr << "the writer of the library does not write units as expected\n";
            return 1;
        }
        if (!reads_distributed_units_through_the_library()) {
            std::cerr << "the distributed units of the library do not read as expected\n";
            return 1;
        }
        if (!edits_units_through_the_library()) {
            std::cerr << "the editor of the library does not write units as expected\n";
            return 1;
        }
        if (!runs_the_algorithms_through_the_library()) {
            std::cerr << "the algorithms of the library do not work as expected\n";
            return 1;
        }
        if (!evaluates_astrometric_solutions_through_the_library()) {
            std::cerr << "the astrometric solutions of the library do not work as expected\n";
            return 1;
        }
        return 0;
    } catch (...) {
        // A failing stream must not turn into a crash that hides which check was running.
        return 2;
    }
}
