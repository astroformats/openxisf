// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/property_catalog.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openxisf::detail {

namespace {

struct reserved_property
{
    std::string_view id{};
    property_type type{};
};

using enum property_type;

// Sorted by identifier, byte by byte, for a binary search.
constexpr std::array<reserved_property, 100> reserved_properties{{
    {.id = "AstrometricSolution:Catalog", .type = string},
    {.id = "AstrometricSolution:CelestialPoleNativeCoordinates", .type = f64_vector},
    {.id = "AstrometricSolution:CelestialReferenceSystem", .type = string},
    {.id = "AstrometricSolution:ControlPoints:Celestial", .type = f64_matrix},
    {.id = "AstrometricSolution:ControlPoints:Image", .type = f64_matrix},
    {.id = "AstrometricSolution:ControlPoints:Rejected", .type = i32_vector},
    {.id = "AstrometricSolution:CreationTime", .type = time_point},
    {.id = "AstrometricSolution:CreatorApplication", .type = string},
    {.id = "AstrometricSolution:CreatorModule", .type = string},
    {.id = "AstrometricSolution:CreatorOS", .type = string},
    {.id = "AstrometricSolution:LinearTransformationMatrix", .type = f64_matrix},
    {.id = "AstrometricSolution:ProjectionSystem", .type = string},
    {.id = "AstrometricSolution:ProjectiveTransformation:ImageToProjection", .type = f64_matrix},
    {.id = "AstrometricSolution:ProjectiveTransformation:ProjectionToImage", .type = f64_matrix},
    {.id = "AstrometricSolution:ReferenceCelestialCoordinates", .type = f64_vector},
    {.id = "AstrometricSolution:ReferenceImageCoordinates", .type = f64_vector},
    {.id = "AstrometricSolution:ReferenceNativeCoordinates", .type = f64_vector},
    {.id = "AstrometricSolution:Version", .type = string},
    {.id = "AstrometricSolution:Weights", .type = f64_vector},
    {.id = "Image:FrameNumber", .type = uint32},
    {.id = "Image:GroupId", .type = string},
    {.id = "Image:SubgroupId", .type = string},
    {.id = "Instrument:Camera:Gain", .type = float32},
    {.id = "Instrument:Camera:ISOSpeed", .type = int32},
    {.id = "Instrument:Camera:Name", .type = string},
    {.id = "Instrument:Camera:ReadoutNoise", .type = float32},
    {.id = "Instrument:Camera:Rotation", .type = float32},
    {.id = "Instrument:Camera:XBinning", .type = int32},
    {.id = "Instrument:Camera:YBinning", .type = int32},
    {.id = "Instrument:ExposureTime", .type = float32},
    {.id = "Instrument:Filter:Name", .type = string},
    {.id = "Instrument:Focuser:Position", .type = float32},
    {.id = "Instrument:Sensor:TargetTemperature", .type = float32},
    {.id = "Instrument:Sensor:Temperature", .type = float32},
    {.id = "Instrument:Sensor:XPixelSize", .type = float32},
    {.id = "Instrument:Sensor:YPixelSize", .type = float32},
    {.id = "Instrument:Telescope:Aperture", .type = float32},
    {.id = "Instrument:Telescope:CollectingArea", .type = float32},
    {.id = "Instrument:Telescope:FocalLength", .type = float32},
    {.id = "Instrument:Telescope:Name", .type = string},
    {.id = "Observation:BibliographicReferences", .type = string},
    {.id = "Observation:CelestialReferenceSystem", .type = string},
    {.id = "Observation:Center:Dec", .type = float64},
    {.id = "Observation:Center:RA", .type = float64},
    {.id = "Observation:Center:X", .type = float64},
    {.id = "Observation:Center:Y", .type = float64},
    {.id = "Observation:Description", .type = string},
    {.id = "Observation:Equinox", .type = float64},
    {.id = "Observation:GeodeticReferenceSystem", .type = string},
    {.id = "Observation:Location:Elevation", .type = float64},
    {.id = "Observation:Location:Latitude", .type = float64},
    {.id = "Observation:Location:Longitude", .type = float64},
    {.id = "Observation:Location:Name", .type = string},
    {.id = "Observation:Meteorology:AmbientTemperature", .type = float32},
    {.id = "Observation:Meteorology:AtmosphericPressure", .type = float32},
    {.id = "Observation:Meteorology:RelativeHumidity", .type = float32},
    {.id = "Observation:Meteorology:WindDirection", .type = float32},
    {.id = "Observation:Meteorology:WindGust", .type = float32},
    {.id = "Observation:Meteorology:WindSpeed", .type = float32},
    {.id = "Observation:Object:Dec", .type = float64},
    {.id = "Observation:Object:Name", .type = string},
    {.id = "Observation:Object:RA", .type = float64},
    {.id = "Observation:RelatedResources", .type = string},
    {.id = "Observation:Time:End", .type = time_point},
    {.id = "Observation:Time:Start", .type = time_point},
    {.id = "Observation:Title", .type = string},
    {.id = "Observer:EmailAddress", .type = string},
    {.id = "Observer:Name", .type = string},
    {.id = "Observer:PostalAddress", .type = string},
    {.id = "Observer:Website", .type = string},
    {.id = "Organization:EmailAddress", .type = string},
    {.id = "Organization:Name", .type = string},
    {.id = "Organization:PostalAddress", .type = string},
    {.id = "Organization:Website", .type = string},
    {.id = "Processing:Description", .type = string},
    {.id = "Processing:History", .type = string},
    {.id = "XISF:Abstract", .type = string},
    {.id = "XISF:AccessRights", .type = string},
    {.id = "XISF:Authors", .type = string},
    {.id = "XISF:BibliographicReferences", .type = string},
    {.id = "XISF:BlockAlignmentSize", .type = uint16},
    {.id = "XISF:BriefDescription", .type = string},
    {.id = "XISF:ChecksumAlgorithms", .type = string},
    {.id = "XISF:CompressionCodecs", .type = string},
    {.id = "XISF:CompressionLevel", .type = int32},
    {.id = "XISF:Contributors", .type = string},
    {.id = "XISF:Copyright", .type = string},
    {.id = "XISF:CreationTime", .type = time_point},
    {.id = "XISF:CreatorApplication", .type = string},
    {.id = "XISF:CreatorModule", .type = string},
    {.id = "XISF:CreatorOS", .type = string},
    {.id = "XISF:Description", .type = string},
    {.id = "XISF:Keywords", .type = string},
    {.id = "XISF:Languages", .type = string},
    {.id = "XISF:License", .type = string},
    {.id = "XISF:MaxInlineBlockSize", .type = uint16},
    {.id = "XISF:OriginalCreationTime", .type = time_point},
    {.id = "XISF:OutputHints", .type = string},
    {.id = "XISF:RelatedResources", .type = string},
    {.id = "XISF:Title", .type = string},
}};

// The properties of a distortion model, after the prefix of its direction (spec §11.5.3.7.4.4). Sorted like the table
// above.
constexpr std::array<reserved_property, 33> distortion_model_properties{{
    {.id = "BasisFunction", .type = string},
    {.id = "Fallback:Threshold", .type = float64},
    {.id = "Fallback:X:Coefficients", .type = f64_vector},
    {.id = "Fallback:X:Nodes", .type = f64_matrix},
    {.id = "Fallback:X:Normalization", .type = f64_vector},
    {.id = "Fallback:X:ShapeParameter", .type = float64},
    {.id = "Fallback:Y:Coefficients", .type = f64_vector},
    {.id = "Fallback:Y:Nodes", .type = f64_matrix},
    {.id = "Fallback:Y:Normalization", .type = f64_vector},
    {.id = "Fallback:Y:ShapeParameter", .type = float64},
    {.id = "Global:X:Coefficients", .type = f64_vector},
    {.id = "Global:X:Nodes", .type = f64_matrix},
    {.id = "Global:X:Normalization", .type = f64_vector},
    {.id = "Global:X:ShapeParameter", .type = float64},
    {.id = "Global:Y:Coefficients", .type = f64_vector},
    {.id = "Global:Y:Nodes", .type = f64_matrix},
    {.id = "Global:Y:Normalization", .type = f64_vector},
    {.id = "Global:Y:ShapeParameter", .type = float64},
    {.id = "Local:Center", .type = f64_matrix},
    {.id = "Local:Radius", .type = f64_vector},
    {.id = "Local:X:Coefficients", .type = f64_vector},
    {.id = "Local:X:NodeOffsets", .type = i32_vector},
    {.id = "Local:X:Nodes", .type = f64_matrix},
    {.id = "Local:X:Normalization", .type = f64_matrix},
    {.id = "Local:X:ShapeParameter", .type = f64_vector},
    {.id = "Local:Y:Coefficients", .type = f64_vector},
    {.id = "Local:Y:NodeOffsets", .type = i32_vector},
    {.id = "Local:Y:Nodes", .type = f64_matrix},
    {.id = "Local:Y:Normalization", .type = f64_matrix},
    {.id = "Local:Y:ShapeParameter", .type = f64_vector},
    {.id = "Order", .type = int32},
    {.id = "Polynomial", .type = boolean},
    {.id = "Terms", .type = string},
}};

static_assert(std::ranges::is_sorted(reserved_properties, std::less<>{}, &reserved_property::id));
static_assert(std::ranges::adjacent_find(reserved_properties, std::ranges::equal_to{}, &reserved_property::id) ==
              reserved_properties.end());
static_assert(std::ranges::is_sorted(distortion_model_properties, std::less<>{}, &reserved_property::id));
static_assert(std::ranges::adjacent_find(distortion_model_properties, std::ranges::equal_to{},
                                         &reserved_property::id) == distortion_model_properties.end());

// The prefixes of the two directions of a distortion model.
constexpr std::array<std::string_view, 2> distortion_model_prefixes{
    "AstrometricSolution:DistortionModel:ImageToProjection:",
    "AstrometricSolution:DistortionModel:ProjectionToImage:",
};

// An index rather than an iterator: the iterators of std::array are pointers in some standard libraries only, which
// readability-qualified-auto treats differently.
template <std::size_t N>
std::optional<property_type> find_in(const std::array<reserved_property, N>& table, std::string_view id) noexcept
{
    const std::ptrdiff_t index =
        std::ranges::lower_bound(table, id, std::less<>{}, &reserved_property::id) - table.begin();
    const auto position = static_cast<std::size_t>(index);
    if (position < table.size() && table[position].id == id) {
        return table[position].type;
    }
    return std::nullopt;
}

} // namespace

std::optional<property_type> reserved_property_type(std::string_view id) noexcept
{
    for (const std::string_view prefix : distortion_model_prefixes) {
        if (id.starts_with(prefix)) {
            return find_in(distortion_model_properties, id.substr(prefix.size()));
        }
    }
    return find_in(reserved_properties, id);
}

std::vector<std::pair<std::string, property_type>> reserved_property_types()
{
    std::vector<std::pair<std::string, property_type>> all;
    all.reserve(reserved_properties.size() + (distortion_model_prefixes.size() * distortion_model_properties.size()));
    for (const reserved_property& entry : reserved_properties) {
        all.emplace_back(entry.id, entry.type);
    }
    for (const std::string_view prefix : distortion_model_prefixes) {
        for (const reserved_property& entry : distortion_model_properties) {
            all.emplace_back(std::string(prefix) + std::string(entry.id), entry.type);
        }
    }
    return all;
}

std::optional<std::string> forbidden_image_value(const property& item)
{
    if (item.id == "Image:FrameNumber" && item.value.type() == property_type::uint32 &&
        item.value.get<std::uint32_t>() == 0) {
        return "the frame number is 0, and frame numbers start at one (spec §11.5.3.5)";
    }
    if ((item.id == "Image:GroupId" || item.id == "Image:SubgroupId") && item.value.type() == property_type::string) {
        const auto& text = item.value.get<std::string>();
        const auto is_first = [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        };
        const auto is_next = [&is_first](char c) { return is_first(c) || c == '-' || c == '.'; };
        if (text.empty() || !is_first(text.front()) ||
            !std::ranges::all_of(std::string_view(text).substr(1), is_next)) {
            return "the identifier of the " + std::string(item.id == "Image:GroupId" ? "group" : "subgroup") +
                   " is not of the form [_a-zA-Z0-9][_a-zA-Z0-9-.]* (spec §11.5.3.5)";
        }
    }
    return std::nullopt;
}

bool is_metadata_id(std::string_view id) noexcept
{
    return id.starts_with("XISF:");
}

bool is_astrometric_solution_id(std::string_view id) noexcept
{
    return id.starts_with("AstrometricSolution:");
}

std::optional<std::array<std::uint32_t, 2>> parse_astrometric_version(std::string_view text) noexcept
{
    const std::size_t dot = text.find('.');
    if (dot == std::string_view::npos) {
        return std::nullopt;
    }
    std::array<std::uint32_t, 2> revision{};
    const std::array<std::string_view, 2> parts{text.substr(0, dot), text.substr(dot + 1)};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (parts[i].empty()) {
            return std::nullopt;
        }
        for (const char c : parts[i]) {
            if (c < '0' || c > '9') {
                return std::nullopt;
            }
            const auto digit = static_cast<std::uint32_t>(c - '0');
            if (revision[i] > (std::numeric_limits<std::uint32_t>::max() - digit) / 10) {
                return std::nullopt;
            }
            revision[i] = revision[i] * 10 + digit;
        }
    }
    return revision;
}

bool is_foreign_astrometric_version(const property_value& version) noexcept
{
    if (version.type() != property_type::string) {
        return false;
    }
    const std::optional<std::array<std::uint32_t, 2>> revision = parse_astrometric_version(version.get<std::string>());
    return !revision || (*revision)[0] != 1;
}

bool has_foreign_astrometric_solution(const property_list& properties) noexcept
{
    const property* version = properties.find("AstrometricSolution:Version");
    return version != nullptr && is_foreign_astrometric_version(version->value);
}

} // namespace openxisf::detail
