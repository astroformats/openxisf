// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Astrometric solutions (spec §11.5.3.7): the layers read from properties, when each is available, the versioning
// rules, and the evaluation of the layers, against WCSLIB for the projections and the spherical rotation.

#include <openxisf/astrometry.h>
#include <openxisf/error.h>
#include <openxisf/property.h>

#include "support/astrometry_reference.h"
#include "support/opened_unit.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using openxisf::astrometric_layer;
using openxisf::astrometric_solution;
using openxisf::astrometric_status;
using openxisf::celestial_point;
using openxisf::errc;
using openxisf::image_point;
using openxisf::projection_system;
using openxisf::property_list;
using openxisf::property_value;
using openxisf::usage_error;
using openxisf::test::separation;
using openxisf::test::throws;
using openxisf::test::value_of;

// The identifiers of the properties of the namespace, and of the distortion models of each direction.
std::string solution(std::string_view name)
{
    return std::string("AstrometricSolution:").append(name);
}

std::string forward(std::string_view name)
{
    return solution("DistortionModel:ImageToProjection:").append(name);
}

std::string backward(std::string_view name)
{
    return solution("DistortionModel:ProjectionToImage:").append(name);
}

property_value matrix(std::uint64_t rows, std::uint64_t columns, std::vector<double> elements)
{
    return property_value::matrix(rows, columns, std::move(elements));
}

// A Global term of a thin plate spline of order 2 that is the constant (x, y): one node with a zero coefficient, and a
// polynomial part of the constant alone.
void add_global_terms(property_list& properties, const std::string& prefix, double x, double y)
{
    properties.set(prefix + "BasisFunction", "ThinPlateSpline");
    properties.set(prefix + "Order", std::int32_t{2});
    properties.set(prefix + "Terms", "Global");
    properties.set(prefix + "Global:X:Normalization", std::vector<double>{0.0, 0.0, 1.0});
    properties.set(prefix + "Global:X:Nodes", matrix(1, 2, {0.0, 0.0}));
    properties.set(prefix + "Global:X:Coefficients", std::vector<double>{0.0, x, 0.0, 0.0});
    properties.set(prefix + "Global:Y:Coefficients", std::vector<double>{0.0, y, 0.0, 0.0});
}

// A solution of the three layers: a Gnomonic projection of 0.01 degrees per pixel about (83.8, -5.4) at (200, 150),
// projective transformations equal to its linear transformation and to the inverse of it, and distortion models that
// add constants: (0.001, -0.002) degrees to projection plane coordinates, and (0.5, -0.25) pixels to image
// coordinates.
property_list complete_solution()
{
    property_list properties;
    properties.set(solution("Version"), "1.0");
    properties.set(solution("ProjectionSystem"), "Gnomonic");
    properties.set(solution("ReferenceCelestialCoordinates"), std::vector<double>{83.8, -5.4});
    properties.set(solution("ReferenceImageCoordinates"), std::vector<double>{200.0, 150.0});
    properties.set(solution("LinearTransformationMatrix"), matrix(2, 2, {-0.01, 0.0, 0.0, -0.01}));
    properties.set(solution("ProjectiveTransformation:ImageToProjection"),
                   matrix(3, 3, {-0.01, 0.0, 2.0, 0.0, -0.01, 1.5, 0.0, 0.0, 1.0}));
    properties.set(solution("ProjectiveTransformation:ProjectionToImage"),
                   matrix(3, 3, {-100.0, 0.0, 200.0, 0.0, -100.0, 150.0, 0.0, 0.0, 1.0}));
    add_global_terms(properties, forward(""), 0.001, -0.002);
    add_global_terms(properties, backward(""), 0.5, -0.25);
    return properties;
}

// The distortion model of the image-to-projection direction with a Local term over the whole image and a Fallback term,
// both constant.
property_list local_solution()
{
    property_list properties = complete_solution();
    for (const char* id :
         {"Terms", "Global:X:Normalization", "Global:X:Nodes", "Global:X:Coefficients", "Global:Y:Coefficients"}) {
        properties.erase(forward(id));
    }
    properties.set(forward("Terms"), "Local\nFallback");
    properties.set(forward("Local:Center"), matrix(1, 2, {200.0, 150.0}));
    properties.set(forward("Local:Radius"), std::vector<double>{500.0});
    properties.set(forward("Local:X:Normalization"), matrix(1, 3, {200.0, 150.0, 0.01}));
    properties.set(forward("Local:X:NodeOffsets"), std::vector<std::int32_t>{0, 1});
    properties.set(forward("Local:X:Nodes"), matrix(1, 2, {0.0, 0.0}));
    properties.set(forward("Local:X:Coefficients"), std::vector<double>{0.0, 0.001, 0.0, 0.0});
    properties.set(forward("Local:Y:Coefficients"), std::vector<double>{0.0, -0.002, 0.0, 0.0});
    properties.set(forward("Fallback:Threshold"), 0.5);
    properties.set(forward("Fallback:X:Normalization"), std::vector<double>{0.0, 0.0, 1.0});
    properties.set(forward("Fallback:X:Nodes"), matrix(1, 2, {0.0, 0.0}));
    properties.set(forward("Fallback:X:Coefficients"), std::vector<double>{0.0, 0.003, 0.0, 0.0});
    properties.set(forward("Fallback:Y:Coefficients"), std::vector<double>{0.0, 0.004, 0.0, 0.0});
    return properties;
}

void erase_with_prefix(property_list& properties, const std::string& prefix)
{
    std::vector<std::string> ids;
    for (const openxisf::property& item : properties) {
        if (item.id.starts_with(prefix)) {
            ids.push_back(item.id);
        }
    }
    for (const std::string& id : ids) {
        properties.erase(id);
    }
}

// A solution whose image-to-projection direction has Local terms of a Gaussian of order 2 alone, of radius 50 and one
// node each: their centres (rows of two), normalizations (rows of three), nodes, the coefficients of X and of Y (four
// for each term), and their shape parameters.
property_list gaussian_locals(const std::vector<double>& centers, const std::vector<double>& normalizations,
                              const std::vector<double>& nodes, const std::vector<double>& x,
                              const std::vector<double>& y, const std::vector<double>& shapes)
{
    property_list properties = complete_solution();
    erase_with_prefix(properties, forward(""));
    const std::size_t count = shapes.size();
    std::vector<std::int32_t> offsets(count + 1);
    for (std::size_t i = 0; i < offsets.size(); ++i) {
        offsets[i] = static_cast<std::int32_t>(i);
    }
    properties.set(forward("BasisFunction"), "Gaussian");
    properties.set(forward("Order"), std::int32_t{2});
    properties.set(forward("Terms"), "Local");
    properties.set(forward("Local:Center"), matrix(count, 2, centers));
    properties.set(forward("Local:Radius"), std::vector<double>(count, 50.0));
    properties.set(forward("Local:X:Normalization"), matrix(count, 3, normalizations));
    properties.set(forward("Local:X:NodeOffsets"), offsets);
    properties.set(forward("Local:X:Nodes"), matrix(count, 2, nodes));
    properties.set(forward("Local:X:Coefficients"), x);
    properties.set(forward("Local:X:ShapeParameter"), shapes);
    properties.set(forward("Local:Y:Coefficients"), y);
    return properties;
}

std::array<astrometric_status, 3> statuses(const astrometric_solution& solved)
{
    return {solved.status(astrometric_layer::linear), solved.status(astrometric_layer::projective),
            solved.status(astrometric_layer::distortion)};
}

void expect_same_point(const std::optional<celestial_point>& actual, const std::optional<celestial_point>& expected,
                       double tolerance)
{
    ASSERT_TRUE(actual.has_value());
    ASSERT_TRUE(expected.has_value());
    EXPECT_LT(separation(value_of(actual), value_of(expected)), tolerance);
}

// -----------------------------------------------------------------------------------------------------------------
// The layers and when each is available (spec §11.5.3.7.7)

TEST(astrometry, a_unit_without_a_solution_has_none)
{
    property_list properties;
    properties.set("Observation:Center:RA", 83.8);
    properties.set("PCL:AstrometricSolution:Engine", "DDM");
    const astrometric_solution solved(properties);
    EXPECT_EQ(solved.layer(), std::nullopt);
    for (const astrometric_status status : statuses(solved)) {
        EXPECT_EQ(status, astrometric_status::absent);
    }
    EXPECT_TRUE(solved.problem(astrometric_layer::linear).empty());
    EXPECT_TRUE(solved.version().empty());
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)solved.image_to_celestial({}); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)solved.celestial_to_image({}); }));
}

TEST(astrometry, each_layer_is_available_when_its_properties_are_complete_and_consistent)
{
    using enum astrometric_status;
    struct rule
    {
        const char* name = "";
        property_list properties{};
        std::array<astrometric_status, 3> expected{};
    };
    const auto changed = [](const auto& change, property_list properties = complete_solution()) {
        change(properties);
        return properties;
    };
    const auto set = [](const std::string& id, const property_value& value,
                        property_list properties = complete_solution()) {
        properties.set(id, value);
        return properties;
    };
    const auto erase = [](const std::string& id, property_list properties = complete_solution()) {
        properties.erase(id);
        return properties;
    };
    const auto set_local = [&](const std::string& id, const property_value& value) {
        return set(id, value, local_solution());
    };
    const auto erase_local = [&](const std::string& id) { return erase(id, local_solution()); };
    const std::array<astrometric_status, 3> all = {available, available, available};
    const std::array<astrometric_status, 3> unsupported = {unsupported_version, unsupported_version,
                                                           unsupported_version};
    const auto first = [](astrometric_status status) {
        return std::array<astrometric_status, 3>{status, lower_layer_unavailable, lower_layer_unavailable};
    };
    const auto second = [](astrometric_status status) {
        return std::array<astrometric_status, 3>{available, status, lower_layer_unavailable};
    };
    const auto third = [](astrometric_status status) {
        return std::array<astrometric_status, 3>{available, available, status};
    };
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const std::vector<rule> rules{
        {.name = "the complete solution", .properties = complete_solution(), .expected = all},
        // Layer 1 and the version (spec §11.5.3.7.2, §11.5.3.7.6).
        {.name = "no version", .properties = erase(solution("Version")), .expected = first(missing_property)},
        {.name = "a newer minor revision", .properties = set(solution("Version"), "1.12"), .expected = all},
        {.name = "major revision 2", .properties = set(solution("Version"), "2.0"), .expected = unsupported},
        {.name = "revision 0.9", .properties = set(solution("Version"), "0.9"), .expected = unsupported},
        {.name = "a version without minor revision",
         .properties = set(solution("Version"), "1"),
         .expected = unsupported},
        {.name = "a version of three parts", .properties = set(solution("Version"), "1.0.1"), .expected = unsupported},
        {.name = "a version with a sign", .properties = set(solution("Version"), "+1.0"), .expected = unsupported},
        {.name = "a version of another type",
         .properties = set(solution("Version"), 1.0),
         .expected = first(inconsistent)},
        {.name = "no projection",
         .properties = erase(solution("ProjectionSystem")),
         .expected = first(missing_property)},
        {.name = "an unknown projection",
         .properties = set(solution("ProjectionSystem"), "Conic"),
         .expected = first(unknown_identifier)},
        {.name = "a projection in lowercase",
         .properties = set(solution("ProjectionSystem"), "gnomonic"),
         .expected = first(unknown_identifier)},
        {.name = "a reference point of three coordinates",
         .properties = set(solution("ReferenceCelestialCoordinates"), std::vector<double>{83.8, -5.4, 0.0}),
         .expected = first(inconsistent)},
        {.name = "a reference point of another type",
         .properties = set(solution("ReferenceCelestialCoordinates"), std::vector<float>{83.8F, -5.4F}),
         .expected = first(inconsistent)},
        {.name = "a declination beyond 90",
         .properties = set(solution("ReferenceCelestialCoordinates"), std::vector<double>{83.8, 90.5}),
         .expected = first(inconsistent)},
        {.name = "no reference image coordinates",
         .properties = erase(solution("ReferenceImageCoordinates")),
         .expected = first(missing_property)},
        {.name = "a linear transformation of 2 x 3",
         .properties = set(solution("LinearTransformationMatrix"), matrix(2, 3, {1, 0, 0, 0, 1, 0})),
         .expected = first(inconsistent)},
        {.name = "a linear transformation of two rows and no columns",
         .properties = set(solution("LinearTransformationMatrix"), matrix(2, 0, {})),
         .expected = first(inconsistent)},
        {.name = "a singular linear transformation",
         .properties = set(solution("LinearTransformationMatrix"), matrix(2, 2, {0.01, 0.02, 0.02, 0.04})),
         .expected = first(inconsistent)},
        // Singular in decimal: in binary, 0.1 x 2.1 - 0.3 x 0.7 leaves a residue of rounding, 2.8e-17.
        {.name = "a linear transformation that is singular but for rounding",
         .properties = set(solution("LinearTransformationMatrix"), matrix(2, 2, {0.1, 0.3, 0.7, 2.1})),
         .expected = first(inconsistent)},
        {.name = "a linear transformation that is not finite",
         .properties = set(solution("LinearTransformationMatrix"), matrix(2, 2, {0.01, 0.0, 0.0, nan})),
         .expected = first(inconsistent)},
        {.name = "native coordinates beyond the pole",
         .properties = set(solution("ReferenceNativeCoordinates"), std::vector<double>{0.0, 95.0}),
         .expected = first(inconsistent)},
        {.name = "native coordinates of one value",
         .properties = set(solution("ReferenceNativeCoordinates"), std::vector<double>{0.0}),
         .expected = first(inconsistent)},
        {.name = "native coordinates that define no rotation",
         .properties = changed([](property_list& p) {
             p.set(solution("ReferenceNativeCoordinates"), std::vector<double>{0.0, 0.0});
             p.set(solution("CelestialPoleNativeCoordinates"), std::vector<double>{90.0, 90.0});
         }),
         .expected = first(inconsistent)},
        {.name = "an unknown reference system",
         .properties = set(solution("CelestialReferenceSystem"), "FK5"),
         .expected = all},
        {.name = "an unknown property of the namespace",
         .properties = set(solution("Comment"), "made by hand"),
         .expected = all},
        // Layer 2 (spec §11.5.3.7.3).
        {.name = "no layer 2 or 3",
         .properties = changed([](property_list& p) {
             erase_with_prefix(p, solution("ProjectiveTransformation:"));
             erase_with_prefix(p, solution("DistortionModel:"));
         }),
         .expected = {available, absent, absent}},
        {.name = "distortion models without projective transformations",
         .properties = changed([](property_list& p) { erase_with_prefix(p, solution("ProjectiveTransformation:")); }),
         .expected = {available, absent, lower_layer_unavailable}},
        {.name = "one projective transformation",
         .properties = erase(solution("ProjectiveTransformation:ProjectionToImage")),
         .expected = second(missing_property)},
        {.name = "a projective transformation of 2 x 3",
         .properties = set(solution("ProjectiveTransformation:ImageToProjection"), matrix(2, 3, {1, 0, 0, 0, 1, 0})),
         .expected = second(inconsistent)},
        {.name = "a projective transformation that is not finite",
         .properties =
             set(solution("ProjectiveTransformation:ImageToProjection"), matrix(3, 3, {1, 0, 0, 0, 1, 0, 0, 0, nan})),
         .expected = second(inconsistent)},
        // Layer 3 (spec §11.5.3.7.4).
        {.name = "no distortion model",
         .properties = changed([](property_list& p) { erase_with_prefix(p, solution("DistortionModel:")); }),
         .expected = {available, available, absent}},
        {.name = "a distortion model in one direction",
         .properties = changed([](property_list& p) { erase_with_prefix(p, backward("")); }),
         .expected = third(missing_property)},
        {.name = "an unknown basis function",
         .properties = set(forward("BasisFunction"), "Wavelet"),
         .expected = third(unknown_identifier)},
        {.name = "an unknown kind of term",
         .properties = set(forward("Terms"), "Global\nPatch"),
         .expected = third(unknown_identifier)},
        {.name = "a kind of term twice",
         .properties = set(forward("Terms"), "Global\nGlobal"),
         .expected = third(inconsistent)},
        {.name = "a kind of term twice, with an empty line between",
         .properties = set(forward("Terms"), "Global\n\nGlobal"),
         .expected = third(inconsistent)},
        {.name = "a Fallback term without Local terms",
         .properties = set(forward("Terms"), "Fallback"),
         .expected = third(inconsistent)},
        {.name = "terms that end with a newline", .properties = set(forward("Terms"), "Global\n"), .expected = all},
        // A newline may be CR LF, as on Windows; any other character belongs to the identifier.
        {.name = "terms on lines that end with CR LF",
         .properties = set(forward("Terms"), "\r\nGlobal\r\n"),
         .expected = all},
        {.name = "a kind of term with a space after it",
         .properties = set(forward("Terms"), "Global "),
         .expected = third(unknown_identifier)},
        {.name = "no terms", .properties = set(forward("Terms"), ""), .expected = all},
        {.name = "no order", .properties = erase(forward("Order")), .expected = third(missing_property)},
        // Coefficients of the right length for the order, so that only the order makes the layer unavailable.
        {.name = "a thin plate spline of order 1",
         .properties = changed([](property_list& p) {
             p.set(forward("Order"), std::int32_t{1});
             p.set(forward("Global:X:Coefficients"), std::vector<double>{0.0, 0.001});
             p.set(forward("Global:Y:Coefficients"), std::vector<double>{0.0, -0.002});
         }),
         .expected = third(inconsistent)},
        {.name = "a variable order spline of order 2",
         .properties = set(forward("BasisFunction"), "VariableOrder"),
         .expected = third(inconsistent)},
        {.name = "an order of another type",
         .properties = set(forward("Order"), std::int16_t{2}),
         .expected = third(inconsistent)},
        {.name = "a thin plate spline without polynomial part",
         .properties = changed([](property_list& p) {
             p.set(forward("Polynomial"), false);
             p.set(forward("Global:X:Coefficients"), std::vector<double>{0.0});
             p.set(forward("Global:Y:Coefficients"), std::vector<double>{0.0});
         }),
         .expected = third(inconsistent)},
        {.name = "a Gaussian without polynomial part",
         .properties = changed([](property_list& p) {
             p.set(forward("BasisFunction"), "Gaussian");
             p.set(forward("Global:X:ShapeParameter"), 2.0);
             p.set(forward("Polynomial"), false);
             p.set(forward("Global:X:Coefficients"), std::vector<double>{0.001});
             p.set(forward("Global:Y:Coefficients"), std::vector<double>{-0.002});
         }),
         .expected = all},
        {.name = "coefficients of the wrong length",
         .properties = set(forward("Global:X:Coefficients"), std::vector<double>{0.0, 1.0, 0.0}),
         .expected = third(inconsistent)},
        {.name = "nodes of three columns",
         .properties = set(forward("Global:X:Nodes"), matrix(1, 3, {0, 0, 0})),
         .expected = third(inconsistent)},
        {.name = "no nodes, in an empty matrix",
         .properties = changed([](property_list& p) {
             p.set(forward("Global:X:Nodes"), matrix(0, 0, {}));
             p.set(forward("Global:X:Coefficients"), std::vector<double>{0.001, 0.0, 0.0});
             p.set(forward("Global:Y:Coefficients"), std::vector<double>{-0.002, 0.0, 0.0});
         }),
         .expected = all},
        {.name = "coefficients that are not finite",
         .properties = set(forward("Global:X:Coefficients"), std::vector<double>{0.0, nan, 0.0, 0.0}),
         .expected = third(inconsistent)},
        {.name = "no normalization",
         .properties = erase(forward("Global:X:Normalization")),
         .expected = third(missing_property)},
        {.name = "no Y coefficients",
         .properties = erase(forward("Global:Y:Coefficients")),
         .expected = third(missing_property)},
        {.name = "Y nodes without their normalization",
         .properties = set(forward("Global:Y:Nodes"), matrix(1, 2, {0, 0})),
         .expected = third(missing_property)},
        {.name = "Y nodes with their normalization",
         .properties = changed([](property_list& p) {
             p.set(forward("Global:Y:Nodes"), matrix(2, 2, {0.0, 0.0, 0.5, 0.5}));
             p.set(forward("Global:Y:Normalization"), std::vector<double>{0.0, 0.0, 1.0});
             p.set(forward("Global:Y:Coefficients"), std::vector<double>{0.0, 0.0, -0.002, 0.0, 0.0});
         }),
         .expected = all},
        {.name = "a Gaussian without shape parameter",
         .properties = set(forward("BasisFunction"), "Gaussian"),
         .expected = third(missing_property)},
        {.name = "a shape parameter of zero",
         .properties = changed([](property_list& p) {
             p.set(forward("BasisFunction"), "Gaussian");
             p.set(forward("Global:X:ShapeParameter"), 0.0);
         }),
         .expected = third(inconsistent)},
        {.name = "Y nodes of a Gaussian without their shape parameter",
         .properties = changed([](property_list& p) {
             p.set(forward("BasisFunction"), "Gaussian");
             p.set(forward("Global:X:ShapeParameter"), 2.0);
             p.set(forward("Global:Y:Nodes"), matrix(2, 2, {0.0, 0.0, 0.5, 0.5}));
             p.set(forward("Global:Y:Normalization"), std::vector<double>{0.0, 0.0, 1.0});
             p.set(forward("Global:Y:Coefficients"), std::vector<double>{0.0, 0.0, -0.002, 0.0, 0.0});
         }),
         .expected = third(missing_property)},
        {.name = "a Gaussian with its shape parameter",
         .properties = changed([](property_list& p) {
             p.set(forward("BasisFunction"), "Gaussian");
             p.set(forward("Global:X:ShapeParameter"), 2.0);
         }),
         .expected = all},
        {.name = "a shape parameter that the basis function does not have",
         .properties = set(forward("Global:X:ShapeParameter"), 2.0),
         .expected = all},
        {.name = "an unknown property of a distortion model",
         .properties = set(forward("Global:Z:Coefficients"), 1.0),
         .expected = all},
        // Local and Fallback terms.
        {.name = "local and fallback terms", .properties = local_solution(), .expected = all},
        {.name = "node offsets that do not start at 0",
         .properties = set_local(forward("Local:X:NodeOffsets"), std::vector<std::int32_t>{1, 1}),
         .expected = third(inconsistent)},
        {.name = "decreasing node offsets",
         .properties = set_local(forward("Local:X:NodeOffsets"), std::vector<std::int32_t>{0, -1}),
         .expected = third(inconsistent)},
        {.name = "node offsets of the wrong length",
         .properties = set_local(forward("Local:X:NodeOffsets"), std::vector<std::int32_t>{0}),
         .expected = third(inconsistent)},
        {.name = "node offsets beyond the nodes",
         .properties = set_local(forward("Local:X:NodeOffsets"), std::vector<std::int32_t>{0, 2}),
         .expected = third(inconsistent)},
        // With coefficients for the nodes that the offsets name, and Y nodes of their own that agree with them.
        {.name = "node offsets beyond the X nodes",
         .properties =
             changed(
                 [](property_list& p) {
                     p.set(forward("Local:X:NodeOffsets"), std::vector<std::int32_t>{0, 2});
                     p.set(forward("Local:X:Coefficients"), std::vector<double>{0.0, 0.0, 0.001, 0.0, 0.0});
                     p.set(forward("Local:Y:Nodes"), matrix(2, 2, {0.0, 0.0, 0.5, 0.5}));
                     p.set(forward("Local:Y:NodeOffsets"), std::vector<std::int32_t>{0, 2});
                     p.set(forward("Local:Y:Normalization"), matrix(1, 3, {200.0, 150.0, 0.01}));
                     p.set(forward("Local:Y:Coefficients"), std::vector<double>{0.0, 0.0, -0.002, 0.0, 0.0});
                 },
                 local_solution()),
         .expected = third(inconsistent)},
        {.name = "Y node offsets beyond the nodes of X that they take",
         .properties =
             changed(
                 [](property_list& p) {
                     p.set(forward("Local:Y:NodeOffsets"), std::vector<std::int32_t>{0, 2});
                     p.set(forward("Local:Y:Coefficients"), std::vector<double>{0.0, 0.0, -0.002, 0.0, 0.0});
                 },
                 local_solution()),
         .expected = third(inconsistent)},
        {.name = "a radius of zero",
         .properties = set_local(forward("Local:Radius"), std::vector<double>{0.0}),
         .expected = third(inconsistent)},
        {.name = "normalizations of no columns",
         .properties = set_local(forward("Local:X:Normalization"), matrix(1, 0, {})),
         .expected = third(inconsistent)},
        {.name = "no fallback threshold",
         .properties = erase_local(forward("Fallback:Threshold")),
         .expected = third(missing_property)},
        {.name = "a fallback threshold of zero",
         .properties = set_local(forward("Fallback:Threshold"), 0.0),
         .expected = third(inconsistent)},
        {.name = "local Y nodes without their offsets",
         .properties =
             changed(
                 [](property_list& p) {
                     p.set(forward("Local:Y:Nodes"), matrix(1, 2, {0.0, 0.0}));
                     p.set(forward("Local:Y:Normalization"), matrix(1, 3, {200.0, 150.0, 0.01}));
                 },
                 local_solution()),
         .expected = third(missing_property)},
    };
    for (const rule& r : rules) {
        SCOPED_TRACE(r.name);
        const astrometric_solution solved(r.properties);
        EXPECT_EQ(statuses(solved), r.expected);
        for (std::size_t i = 0; i < 3; ++i) {
            const auto layer = static_cast<astrometric_layer>(i + 1);
            const bool explained = r.expected[i] != available && r.expected[i] != absent;
            EXPECT_EQ(solved.problem(layer).empty(), !explained) << solved.problem(layer);
        }
    }
}

TEST(astrometry, the_highest_available_layer_is_used)
{
    const astrometric_solution complete(complete_solution());
    EXPECT_EQ(complete.layer(), astrometric_layer::distortion);
    property_list properties = complete_solution();
    properties.erase(backward("Order"));
    EXPECT_EQ(astrometric_solution(properties).layer(), astrometric_layer::projective);
    properties.erase(solution("ProjectiveTransformation:ImageToProjection"));
    const astrometric_solution linear(properties);
    EXPECT_EQ(linear.layer(), astrometric_layer::linear);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { (void)linear.image_to_celestial({}, astrometric_layer::projective); }));
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument,
                                    [&] { (void)linear.celestial_to_image({}, astrometric_layer::distortion); }));
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): the value is outside on purpose
    const auto no_layer = static_cast<astrometric_layer>(0);
    EXPECT_TRUE(throws<usage_error>(errc::invalid_argument, [&] { (void)linear.image_to_celestial({}, no_layer); }));
}

TEST(astrometry, a_solution_of_another_major_revision_is_not_read)
{
    property_list properties = complete_solution();
    properties.set(solution("Version"), "2.0");
    // A revision 2 may change anything, the type of a property included.
    properties.set(solution("ReferenceCelestialCoordinates"), "83.8 -5.4");
    const astrometric_solution solved(properties);
    EXPECT_EQ(solved.version(), "2.0");
    EXPECT_EQ(solved.layer(), std::nullopt);
    EXPECT_EQ(solved.projection(), std::nullopt);
    EXPECT_EQ(solved.status(astrometric_layer::distortion), astrometric_status::unsupported_version);
    EXPECT_FALSE(solved.problem(astrometric_layer::linear).empty());
}

TEST(astrometry, the_first_layer_applies_the_defaults_of_the_specification)
{
    const astrometric_solution solved(complete_solution());
    EXPECT_EQ(solved.version(), "1.0");
    const openxisf::astrometric_projection& projection = value_of(solved.projection());
    EXPECT_EQ(projection.projection_system, projection_system::gnomonic);
    EXPECT_EQ(projection.reference_celestial, (celestial_point{.ra = 83.8, .dec = -5.4}));
    EXPECT_EQ(projection.reference_image, (image_point{.x = 200.0, .y = 150.0}));
    EXPECT_EQ(projection.linear_transformation, (std::array<double, 4>{-0.01, 0.0, 0.0, -0.01}));
    // A zenithal projection has its reference point at the native pole, and the celestial pole at the native longitude
    // 180 when the reference point is below the native latitude of 90 (Annex A.2).
    EXPECT_EQ(projection.reference_native, (std::array<double, 2>{0.0, 90.0}));
    EXPECT_EQ(projection.celestial_pole_native, (std::array<double, 2>{180.0, 90.0}));
    EXPECT_EQ(projection.celestial_reference_system, "ICRS");
    EXPECT_EQ(value_of(solved.projective()).projection_to_image,
              (std::array<double, 9>{-100.0, 0.0, 200.0, 0.0, -100.0, 150.0, 0.0, 0.0, 1.0}));

    property_list cylindrical = complete_solution();
    cylindrical.set(solution("ProjectionSystem"), "PlateCarree");
    cylindrical.set(solution("ReferenceCelestialCoordinates"), std::vector<double>{10.0, -20.0});
    cylindrical.set(solution("CelestialReferenceSystem"), "GCRS");
    const astrometric_solution plate(cylindrical);
    const openxisf::astrometric_projection& carree = value_of(plate.projection());
    EXPECT_EQ(carree.reference_native, (std::array<double, 2>{0.0, 0.0}));
    EXPECT_EQ(carree.celestial_pole_native, (std::array<double, 2>{180.0, 90.0}));
    EXPECT_EQ(carree.celestial_reference_system, "GCRS");
    // A reference system that is not a String is unknown, and the layer stays available.
    cylindrical.set(solution("CelestialReferenceSystem"), std::int32_t{2000});
    EXPECT_EQ(value_of(astrometric_solution(cylindrical).projection()).celestial_reference_system, "");
}

TEST(astrometry, the_projection_systems_have_the_identifiers_of_the_specification)
{
    using openxisf::projection_system_name;
    EXPECT_EQ(projection_system_name(projection_system::gnomonic), "Gnomonic");
    EXPECT_EQ(projection_system_name(projection_system::stereographic), "Stereographic");
    EXPECT_EQ(projection_system_name(projection_system::zenithal_equal_area), "ZenithalEqualArea");
    EXPECT_EQ(projection_system_name(projection_system::orthographic), "Orthographic");
    EXPECT_EQ(projection_system_name(projection_system::plate_carree), "PlateCarree");
    EXPECT_EQ(projection_system_name(projection_system::mercator), "Mercator");
    EXPECT_EQ(projection_system_name(projection_system::hammer_aitoff), "HammerAitoff");
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange): the value is outside on purpose
    EXPECT_EQ(projection_system_name(static_cast<projection_system>(7)), "");
}

// -----------------------------------------------------------------------------------------------------------------
// Evaluation through each layer

TEST(astrometry, the_layers_add_up_the_projective_transformation_and_the_distortion_model)
{
    const astrometric_solution solved(complete_solution());
    for (const image_point point : {image_point{.x = 200.0, .y = 150.0}, image_point{.x = 12.5, .y = 290.25},
                                    image_point{.x = 400.0, .y = 0.0}}) {
        SCOPED_TRACE(point.x);
        // The projective transformations equal the linear one.
        expect_same_point(solved.image_to_celestial(point, astrometric_layer::projective),
                          solved.image_to_celestial(point, astrometric_layer::linear), 1e-13);
        // The residual (0.001, -0.002) of the projection plane is (-0.1, 0.2) pixels through the inverse of the
        // matrix.
        const image_point shifted{.x = point.x - 0.1, .y = point.y + 0.2};
        expect_same_point(solved.image_to_celestial(point, astrometric_layer::distortion),
                          solved.image_to_celestial(shifted, astrometric_layer::linear), 1e-13);
        EXPECT_EQ(solved.image_to_celestial(point), solved.image_to_celestial(point, astrometric_layer::distortion));

        const celestial_point sky = value_of(solved.image_to_celestial(point, astrometric_layer::linear));
        const image_point linear = value_of(solved.celestial_to_image(sky, astrometric_layer::linear));
        EXPECT_NEAR(linear.x, point.x, 1e-9);
        EXPECT_NEAR(linear.y, point.y, 1e-9);
        const image_point projective = value_of(solved.celestial_to_image(sky, astrometric_layer::projective));
        EXPECT_NEAR(projective.x, point.x, 1e-9);
        EXPECT_NEAR(projective.y, point.y, 1e-9);
        const image_point distorted = value_of(solved.celestial_to_image(sky));
        EXPECT_NEAR(distorted.x, point.x + 0.5, 1e-9);
        EXPECT_NEAR(distorted.y, point.y - 0.25, 1e-9);
    }
}

TEST(astrometry, local_and_fallback_terms_weigh_their_residuals)
{
    const astrometric_solution solved(local_solution());
    ASSERT_EQ(solved.layer(), astrometric_layer::distortion);
    // At the centre of the Local term its weight 1 is above the threshold, so the Fallback term has no weight; far
    // beyond the disc, the Fallback term alone, (0.003, 0.004), is -0.3 and -0.4 pixels through the matrix.
    const image_point centre{.x = 200.0, .y = 150.0};
    expect_same_point(solved.image_to_celestial(centre),
                      solved.image_to_celestial({.x = 199.9, .y = 150.2}, astrometric_layer::linear), 1e-13);
    const image_point far{.x = 5000.0, .y = 150.0};
    expect_same_point(solved.image_to_celestial(far),
                      solved.image_to_celestial({.x = 4999.7, .y = 149.6}, astrometric_layer::linear), 1e-13);
}

TEST(astrometry, each_local_term_has_the_spline_at_its_offsets)
{
    // Term i of N packed Local terms has the nodes [Oi, Oi+1) and the coefficients [Oi + iQ, Oi+1 + (i + 1)Q) (spec
    // §11.5.3.7.4.4). Each of two terms of a Gaussian has a node, coefficients and a shape parameter of its own: at a
    // point that only one of them covers, the pair gives what that term alone gives.
    const astrometric_solution pair(gaussian_locals(
        {100.0, 100.0, 300.0, 200.0}, {100.0, 100.0, 0.01, 300.0, 200.0, 0.01}, {0.0, 0.0, 0.1, 0.1},
        {0.001, 0.0, 0.0, 0.0, 0.002, 0.0005, 0.0, 0.0}, {0.001, 0.0, 0.0, 0.0, 0.003, 0.0, 0.0005, 0.0}, {1.0, 30.0}));
    ASSERT_EQ(pair.layer(), astrometric_layer::distortion) << pair.problem(astrometric_layer::distortion);
    const astrometric_solution first(gaussian_locals({100.0, 100.0}, {100.0, 100.0, 0.01}, {0.0, 0.0},
                                                     {0.001, 0.0, 0.0, 0.0}, {0.001, 0.0, 0.0, 0.0}, {1.0}));
    const astrometric_solution second(gaussian_locals({300.0, 200.0}, {300.0, 200.0, 0.01}, {0.1, 0.1},
                                                      {0.002, 0.0005, 0.0, 0.0}, {0.003, 0.0, 0.0005, 0.0}, {30.0}));
    for (const image_point point : {image_point{.x = 105.0, .y = 98.0}, image_point{.x = 120.0, .y = 110.0}}) {
        expect_same_point(pair.image_to_celestial(point), first.image_to_celestial(point), 1e-13);
    }
    for (const image_point point : {image_point{.x = 310.0, .y = 205.0}, image_point{.x = 290.0, .y = 180.0}}) {
        expect_same_point(pair.image_to_celestial(point), second.image_to_celestial(point), 1e-13);
    }
}

TEST(astrometry, a_y_component_takes_from_x_only_what_it_does_not_specify)
{
    // Each pair of solutions differs only in that the second gives the Y component, in full, the properties that the
    // first takes from X (spec §11.5.3.7.4.4): they give the same coordinates.
    const auto expect_same_solutions = [](const property_list& taken, const property_list& given) {
        const astrometric_solution first(taken);
        const astrometric_solution second(given);
        ASSERT_EQ(first.layer(), astrometric_layer::distortion) << first.problem(astrometric_layer::distortion);
        ASSERT_EQ(second.layer(), astrometric_layer::distortion) << second.problem(astrometric_layer::distortion);
        for (const image_point point : {image_point{.x = 200.0, .y = 150.0}, image_point{.x = 215.0, .y = 160.0},
                                        image_point{.x = 480.0, .y = 20.0}}) {
            SCOPED_TRACE(point.x);
            expect_same_point(first.image_to_celestial(point), second.image_to_celestial(point), 1e-13);
        }
    };

    // A Global term whose Y component has a normalization of its own, and the nodes of X.
    property_list global = complete_solution();
    global.set(forward("Global:X:Normalization"), std::vector<double>{200.0, 150.0, 0.01});
    global.set(forward("Global:X:Coefficients"), std::vector<double>{0.0005, 0.001, 0.002, -0.001});
    global.set(forward("Global:Y:Coefficients"), std::vector<double>{-0.0005, -0.002, 0.001, 0.003});
    global.set(forward("Global:Y:Normalization"), std::vector<double>{100.0, 50.0, 0.02});
    property_list global_given = global;
    global_given.set(forward("Global:Y:Nodes"), matrix(1, 2, {0.0, 0.0}));
    expect_same_solutions(global, global_given);

    // A Local term whose Y component has a normalization of its own, and the nodes and offsets of X.
    property_list local = local_solution();
    local.set(forward("Local:X:Coefficients"), std::vector<double>{0.0005, 0.001, 0.002, -0.001});
    local.set(forward("Local:Y:Coefficients"), std::vector<double>{-0.0005, -0.002, 0.001, 0.003});
    local.set(forward("Local:Y:Normalization"), matrix(1, 3, {100.0, 50.0, 0.02}));
    property_list local_given = local;
    local_given.set(forward("Local:Y:Nodes"), matrix(1, 2, {0.0, 0.0}));
    local_given.set(forward("Local:Y:NodeOffsets"), std::vector<std::int32_t>{0, 1});
    expect_same_solutions(local, local_given);

    // Local terms of a Gaussian whose Y component has a shape parameter of its own.
    property_list shaped = local_solution();
    shaped.set(forward("BasisFunction"), "Gaussian");
    shaped.set(forward("Local:X:ShapeParameter"), std::vector<double>{2.0});
    shaped.set(forward("Local:Y:ShapeParameter"), std::vector<double>{5.0});
    shaped.set(forward("Fallback:X:ShapeParameter"), 2.0);
    shaped.set(forward("Local:X:Coefficients"), std::vector<double>{0.001, 0.0, 0.0, 0.0});
    shaped.set(forward("Local:Y:Coefficients"), std::vector<double>{0.002, 0.0, 0.0, 0.0});
    property_list shaped_given = shaped;
    shaped_given.set(forward("Local:Y:Nodes"), matrix(1, 2, {0.0, 0.0}));
    shaped_given.set(forward("Local:Y:NodeOffsets"), std::vector<std::int32_t>{0, 1});
    shaped_given.set(forward("Local:Y:Normalization"), matrix(1, 3, {200.0, 150.0, 0.01}));
    expect_same_solutions(shaped, shaped_given);
}

TEST(astrometry, points_without_coordinates_give_none)
{
    property_list properties = complete_solution();
    // w' = 0.01 x - 2 vanishes on the column x = 200.
    properties.set(solution("ProjectiveTransformation:ImageToProjection"),
                   matrix(3, 3, {-0.01, 0.0, 2.0, 0.0, -0.01, 1.5, 0.01, 0.0, -2.0}));
    const astrometric_solution solved(properties);
    EXPECT_EQ(solved.image_to_celestial({.x = 200.0, .y = 10.0}, astrometric_layer::projective), std::nullopt);
    // Elsewhere (u, v) = (u'/w', v'/w'): at (250, 10), (-0.5, 1.4) / 0.5, the image point (300, -130) of layer 1.
    expect_same_point(solved.image_to_celestial({.x = 250.0, .y = 10.0}, astrometric_layer::projective),
                      solved.image_to_celestial({.x = 300.0, .y = -130.0}, astrometric_layer::linear), 1e-13);
    // Points that are not finite, declinations beyond ±90, and the far side of a Gnomonic projection.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    EXPECT_EQ(solved.image_to_celestial({.x = nan, .y = 0.0}), std::nullopt);
    EXPECT_EQ(solved.image_to_celestial({.x = 0.0, .y = infinity}, astrometric_layer::linear), std::nullopt);
    EXPECT_EQ(solved.celestial_to_image({.ra = nan, .dec = 0.0}), std::nullopt);
    EXPECT_EQ(solved.celestial_to_image({.ra = 10.0, .dec = 90.5}), std::nullopt);
    EXPECT_EQ(solved.celestial_to_image({.ra = 263.8, .dec = 5.4}), std::nullopt);
    // The south celestial pole is 84.6 degrees from the reference point.
    EXPECT_TRUE(solved.celestial_to_image({.ra = 0.0, .dec = -90.0}, astrometric_layer::linear));
    // A declination beyond 90 is no point, also where the rotation would take it next to the reference point.
    property_list polar = complete_solution();
    polar.set(solution("ReferenceCelestialCoordinates"), std::vector<double>{0.0, 89.5});
    const astrometric_solution near_pole(polar);
    EXPECT_TRUE(near_pole.celestial_to_image({.ra = 0.0, .dec = 89.5}));
    EXPECT_EQ(near_pole.celestial_to_image({.ra = 180.0, .dec = 90.5}), std::nullopt);
}

TEST(astrometry, linear_solutions_agree_with_wcslib)
{
    // tests/data/astrometry/wcslib.csv, written by wcslib.py with WCSLIB, the implementation of the WCS formulation
    // that the specification follows: every projection, native coordinates other than the defaults, and parameters that
    // define no rotation.
    const openxisf::test::reference_data data = openxisf::test::read_reference_data("astrometry/wcslib.csv");
    std::map<std::string, std::pair<astrometric_solution, double>> solutions;
    std::size_t invalid = 0;
    for (const openxisf::test::reference_solution& reference : data.solutions) {
        SCOPED_TRACE(reference.name);
        const astrometric_solution solved(reference.properties);
        if (!reference.valid) {
            EXPECT_EQ(solved.status(astrometric_layer::linear), astrometric_status::inconsistent);
            ++invalid;
            continue;
        }
        EXPECT_EQ(solved.layer(), astrometric_layer::linear) << solved.problem(astrometric_layer::linear);
        solutions.emplace(reference.name, std::pair{solved, reference.scale});
    }
    EXPECT_GT(invalid, 10U);
    std::size_t compared = 0;
    for (const openxisf::test::reference_point& point : data.points) {
        SCOPED_TRACE(point.name + " " + point.direction + " " + std::to_string(point.input[0]) + " " +
                     std::to_string(point.input[1]));
        const auto& [solved, scale] = solutions.at(point.name);
        if (point.direction == "image") {
            const std::optional<celestial_point> sky =
                solved.image_to_celestial({.x = point.input[0], .y = point.input[1]});
            ASSERT_EQ(sky.has_value(), point.output.has_value());
            if (sky && point.output) {
                const double pixels = separation(*sky, {.ra = (*point.output)[0], .dec = (*point.output)[1]}) / scale;
                EXPECT_LT(pixels, 1e-9);
                ++compared;
            }
        } else {
            const std::optional<image_point> image =
                solved.celestial_to_image({.ra = point.input[0], .dec = point.input[1]});
            ASSERT_EQ(image.has_value(), point.output.has_value());
            if (image && point.output) {
                EXPECT_LT(std::hypot(image->x - (*point.output)[0], image->y - (*point.output)[1]), 1e-9);
                ++compared;
            }
        }
    }
    EXPECT_GT(compared, 600U);
}

TEST(astrometry, the_cylindrical_projections_take_longitudes_modulo_360_as_pixinsight_does)
{
    // PixInsight 1.9.5 with a linear solution of one degree per pixel about (0, 0) at (200, 150): the image point
    // (10, 150), 190 degrees from the reference point, is at right ascension 190, which goes back to (370, 150). WCSLIB
    // gives no coordinates more than 180 degrees from the reference point.
    for (const char* system : {"PlateCarree", "Mercator"}) {
        SCOPED_TRACE(system);
        property_list properties;
        properties.set(solution("Version"), "1.0");
        properties.set(solution("ProjectionSystem"), system);
        properties.set(solution("ReferenceCelestialCoordinates"), std::vector<double>{0.0, 0.0});
        properties.set(solution("ReferenceImageCoordinates"), std::vector<double>{200.0, 150.0});
        properties.set(solution("LinearTransformationMatrix"), matrix(2, 2, {-1.0, 0.0, 0.0, -1.0}));
        const astrometric_solution solved(properties);
        const celestial_point sky = value_of(solved.image_to_celestial({.x = 10.0, .y = 150.0}));
        EXPECT_NEAR(sky.ra, 190.0, 1e-12);
        EXPECT_NEAR(sky.dec, 0.0, 1e-12);
        const image_point back = value_of(solved.celestial_to_image(sky));
        EXPECT_NEAR(back.x, 370.0, 1e-10);
        EXPECT_NEAR(back.y, 150.0, 1e-10);
    }
}

// -----------------------------------------------------------------------------------------------------------------
// Removing a solution (spec §11.5.3.7.8)

TEST(astrometry, removing_a_solution_keeps_the_properties_of_other_namespaces)
{
    property_list properties = local_solution();
    properties.set("Observation:Center:RA", 83.8);
    properties.set("PCL:AstrometricSolution:Grid:Fingerprint", "5072aecd492e232e");
    const std::size_t count = properties.size() - 2;
    EXPECT_EQ(openxisf::remove_astrometric_solution(properties), count);
    EXPECT_EQ(properties.size(), 2U);
    EXPECT_TRUE(properties.contains("PCL:AstrometricSolution:Grid:Fingerprint"));
    EXPECT_EQ(astrometric_solution(properties).status(astrometric_layer::linear), astrometric_status::absent);
    EXPECT_EQ(openxisf::remove_astrometric_solution(properties), 0U);
}

// A unit decides how many properties its solution has, and every application that changes the geometry of an image
// removes them. Removing them one at a time would scan the list for each one.
TEST(astrometry, removing_a_solution_takes_one_pass_over_the_properties)
{
    constexpr std::size_t count = 1'500'000;
    std::vector<openxisf::property> items;
    items.reserve(count + 2);
    items.push_back({.id = "Observation:Center:RA", .value = 83.8});
    for (std::size_t i = 0; i < count; ++i) {
        items.push_back({.id = "AstrometricSolution:Test:" + std::to_string(i), .value = std::int8_t{1}});
    }
    items.push_back({.id = "Observation:Center:Dec", .value = -5.4});
    property_list properties(std::move(items));

    EXPECT_EQ(openxisf::remove_astrometric_solution(properties), count);
    ASSERT_EQ(properties.size(), 2U);
    EXPECT_EQ(properties.begin()->id, "Observation:Center:RA");
    EXPECT_EQ(std::next(properties.begin())->id, "Observation:Center:Dec");
}

} // namespace
