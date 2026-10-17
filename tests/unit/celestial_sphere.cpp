// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Annex A: the spherical rotation between native and celestial coordinates, and the projections of astrometric
// solutions (spec §11.5.3.7.2.2).

#include "algorithms/celestial_sphere.h"

#include <openxisf/astrometry.h>

#include "support/opened_unit.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>

namespace {

namespace detail = openxisf::detail;
using detail::native_point;
using detail::plane_point;
using detail::spherical_rotation;
using openxisf::celestial_point;
using openxisf::projection_system;
using openxisf::test::value_of;

constexpr double r = 180.0 / std::numbers::pi;

const std::array<projection_system, 7> every_projection{
    projection_system::gnomonic,     projection_system::stereographic, projection_system::zenithal_equal_area,
    projection_system::orthographic, projection_system::plate_carree,  projection_system::mercator,
    projection_system::hammer_aitoff};

// The angular distance between two points of the sphere, by the haversine formula, which keeps its precision for small
// distances.
double distance(celestial_point a, celestial_point b)
{
    const double rad = std::numbers::pi / 180.0;
    const double h =
        std::pow(std::sin((b.dec - a.dec) * rad / 2.0), 2.0) +
        (std::cos(a.dec * rad) * std::cos(b.dec * rad) * std::pow(std::sin((b.ra - a.ra) * rad / 2.0), 2.0));
    return 2.0 * std::asin(std::sqrt(h)) * r;
}

spherical_rotation rotation(celestial_point reference, native_point reference_native, native_point pole_native)
{
    return value_of(spherical_rotation::make(reference, reference_native, pole_native));
}

void expect_same_point(celestial_point actual, celestial_point expected, double tolerance)
{
    EXPECT_LT(distance(actual, expected), tolerance)
        << actual.ra << " " << actual.dec << " for " << expected.ra << " " << expected.dec;
}

// -----------------------------------------------------------------------------------------------------------------
// Angles (Annex A.1)

TEST(celestial_sphere, sines_and_cosines_are_exact_at_multiples_of_90_degrees)
{
    struct expectation
    {
        double angle = 0.0;
        double sin = 0.0;
        double cos = 0.0;
    };
    const std::array<expectation, 10> table{{{.angle = 0.0, .sin = 0.0, .cos = 1.0},
                                             {.angle = 90.0, .sin = 1.0, .cos = 0.0},
                                             {.angle = 180.0, .sin = 0.0, .cos = -1.0},
                                             {.angle = 270.0, .sin = -1.0, .cos = 0.0},
                                             {.angle = 360.0, .sin = 0.0, .cos = 1.0},
                                             {.angle = 450.0, .sin = 1.0, .cos = 0.0},
                                             {.angle = -90.0, .sin = -1.0, .cos = 0.0},
                                             {.angle = -180.0, .sin = 0.0, .cos = -1.0},
                                             {.angle = -270.0, .sin = 1.0, .cos = 0.0},
                                             {.angle = 720.0, .sin = 0.0, .cos = 1.0}}};
    for (const expectation& row : table) {
        EXPECT_EQ(detail::sin_degrees(row.angle), row.sin) << row.angle;
        EXPECT_EQ(detail::cos_degrees(row.angle), row.cos) << row.angle;
    }
    EXPECT_DOUBLE_EQ(detail::sin_degrees(30.0), 0.5);
    EXPECT_DOUBLE_EQ(detail::cos_degrees(60.0), 0.5);
    EXPECT_DOUBLE_EQ(detail::sin_degrees(-45.0), -std::sqrt(0.5));
    EXPECT_TRUE(std::isnan(detail::sin_degrees(std::numeric_limits<double>::infinity())));
}

TEST(celestial_sphere, the_arctangent_of_two_arguments_is_in_the_range_above_minus_180_up_to_180)
{
    EXPECT_EQ(detail::arg(1.0, 0.0), 0.0);
    EXPECT_DOUBLE_EQ(detail::arg(0.0, 2.0), 90.0);
    EXPECT_DOUBLE_EQ(detail::arg(1.0, -1.0), -45.0);
    EXPECT_DOUBLE_EQ(detail::arg(-1.0, 0.0), 180.0);
    // atan2 gives -π for a negative zero, which Annex A.1 takes to 180.
    EXPECT_DOUBLE_EQ(detail::arg(-1.0, -0.0), 180.0);
    EXPECT_DOUBLE_EQ(detail::arg(-1.0, -1.0), -135.0);
}

TEST(celestial_sphere, longitudes_and_right_ascensions_are_reduced_to_their_ranges)
{
    EXPECT_EQ(detail::reduce_longitude(180.0), 180.0);
    EXPECT_EQ(detail::reduce_longitude(-180.0), 180.0);
    EXPECT_EQ(detail::reduce_longitude(540.0), 180.0);
    EXPECT_EQ(detail::reduce_longitude(190.0), -170.0);
    EXPECT_EQ(detail::reduce_longitude(-190.0), 170.0);
    EXPECT_EQ(detail::reduce_longitude(-45.0), -45.0);
    EXPECT_EQ(detail::reduce_right_ascension(360.0), 0.0);
    EXPECT_EQ(detail::reduce_right_ascension(-30.0), 330.0);
    EXPECT_EQ(detail::reduce_right_ascension(725.0), 5.0);
    // Adding 360 to a tiny negative value rounds to 360, which is not a right ascension.
    EXPECT_EQ(detail::reduce_right_ascension(-1e-17), 0.0);
}

// -----------------------------------------------------------------------------------------------------------------
// Default native coordinates (spec §11.5.3.7.2, Annex A.2)

TEST(celestial_sphere, zenithal_projections_put_the_reference_point_at_the_native_pole_by_default)
{
    for (const projection_system system : every_projection) {
        const native_point reference = detail::default_reference_native(system);
        EXPECT_EQ(reference.phi, 0.0);
        EXPECT_EQ(reference.theta, detail::is_zenithal(system) ? 90.0 : 0.0) << static_cast<int>(system);
    }
    EXPECT_TRUE(detail::is_zenithal(projection_system::orthographic));
    EXPECT_FALSE(detail::is_zenithal(projection_system::hammer_aitoff));
}

TEST(celestial_sphere, the_default_longitude_of_the_celestial_pole_depends_on_the_reference_declination)
{
    // φ0 when δ0 ≥ θ0, φ0 + 180 otherwise, reduced to (-180, 180].
    EXPECT_EQ(detail::default_pole_native(30.0, {.phi = 0.0, .theta = 90.0}).phi, 180.0);
    EXPECT_EQ(detail::default_pole_native(90.0, {.phi = 0.0, .theta = 90.0}).phi, 0.0);
    EXPECT_EQ(detail::default_pole_native(10.0, {.phi = 0.0, .theta = 0.0}).phi, 0.0);
    EXPECT_EQ(detail::default_pole_native(-10.0, {.phi = 0.0, .theta = 0.0}).phi, 180.0);
    EXPECT_EQ(detail::default_pole_native(-10.0, {.phi = 170.0, .theta = 0.0}).phi, -10.0);
    EXPECT_EQ(detail::default_pole_native(-10.0, {.phi = 170.0, .theta = 0.0}).theta, 90.0);
}

// -----------------------------------------------------------------------------------------------------------------
// The spherical rotation (Annex A.2)

TEST(celestial_sphere, the_reference_point_is_the_native_pole_when_its_native_latitude_is_90)
{
    // Equation [29].
    const spherical_rotation turn =
        rotation({.ra = 83.8, .dec = -5.4}, {.phi = 0.0, .theta = 90.0}, {.phi = 180.0, .theta = 90.0});
    EXPECT_EQ(turn.native_pole(), (celestial_point{.ra = 83.8, .dec = -5.4}));
    expect_same_point(turn.to_celestial({.phi = 123.0, .theta = 90.0}), {.ra = 83.8, .dec = -5.4}, 1e-12);
}

TEST(celestial_sphere, the_rotation_takes_the_reference_point_and_the_celestial_pole_to_their_native_coordinates)
{
    // The definitions of ReferenceNativeCoordinates and CelestialPoleNativeCoordinates, for valid parameters of every
    // kind: the default ones of zenithal and cylindrical projections, and others.
    struct parameters
    {
        celestial_point reference{};
        native_point reference_native{};
        native_point pole_native{};
    };
    const std::array<parameters, 8> cases{{
        {.reference = {.ra = 40.0, .dec = 30.0},
         .reference_native = {.phi = 0.0, .theta = 90.0},
         .pole_native = {.phi = 180.0, .theta = 90.0}},
        {.reference = {.ra = 300.0, .dec = -20.0},
         .reference_native = {.phi = 0.0, .theta = 90.0},
         .pole_native = {.phi = -135.0, .theta = 90.0}},
        {.reference = {.ra = 100.0, .dec = 30.0},
         .reference_native = {.phi = 0.0, .theta = 0.0},
         .pole_native = {.phi = 0.0, .theta = 90.0}},
        {.reference = {.ra = 100.0, .dec = -30.0},
         .reference_native = {.phi = 0.0, .theta = 0.0},
         .pole_native = {.phi = 180.0, .theta = 90.0}},
        {.reference = {.ra = 330.0, .dec = -10.0},
         .reference_native = {.phi = 20.0, .theta = -45.0},
         .pole_native = {.phi = -30.0, .theta = 40.0}},
        {.reference = {.ra = 210.0, .dec = 15.0},
         .reference_native = {.phi = 0.0, .theta = 70.0},
         .pole_native = {.phi = 180.0, .theta = 90.0}},
        {.reference = {.ra = 60.0, .dec = -40.0},
         .reference_native = {.phi = 30.0, .theta = 60.0},
         .pole_native = {.phi = 150.0, .theta = 10.0}},
        {.reference = {.ra = 75.0, .dec = 0.0},
         .reference_native = {.phi = 0.0, .theta = 0.0},
         .pole_native = {.phi = 90.0, .theta = 45.0}},
    }};
    for (const parameters& c : cases) {
        SCOPED_TRACE(c.reference.ra);
        const spherical_rotation turn = rotation(c.reference, c.reference_native, c.pole_native);
        const native_point reference = turn.to_native(c.reference);
        EXPECT_NEAR(reference.theta, c.reference_native.theta, 1e-10);
        // The longitude of a native pole is any.
        if (std::abs(c.reference_native.theta) < 90.0) {
            EXPECT_NEAR(reference.phi, c.reference_native.phi, 1e-10);
        }
        // The celestial pole has the native longitude φp, unless the native pole is a celestial pole.
        if (std::abs(turn.native_pole().dec) < 90.0) {
            EXPECT_NEAR(turn.to_native({.ra = 0.0, .dec = 90.0}).phi, c.pole_native.phi, 1e-9);
        }
        // And the rotation goes both ways.
        for (const celestial_point point :
             {celestial_point{.ra = 10.0, .dec = 20.0}, celestial_point{.ra = 250.0, .dec = -75.0},
              celestial_point{.ra = 359.5, .dec = 89.0}}) {
            expect_same_point(turn.to_celestial(turn.to_native(point)), point, 1e-12);
        }
    }
}

TEST(celestial_sphere, the_native_pole_is_the_candidate_closest_to_its_native_latitude)
{
    // A cylindrical projection with the reference point at native (0, 0) has candidates ±(90 - δ0) (equation [31]).
    const spherical_rotation north =
        rotation({.ra = 50.0, .dec = 20.0}, {.phi = 0.0, .theta = 0.0}, {.phi = 0.0, .theta = 90.0});
    EXPECT_NEAR(north.native_pole().dec, 70.0, 1e-12);
    const spherical_rotation south =
        rotation({.ra = 50.0, .dec = 20.0}, {.phi = 0.0, .theta = 0.0}, {.phi = 0.0, .theta = -90.0});
    EXPECT_NEAR(south.native_pole().dec, -70.0, 1e-12);
    // When both are as close, the second, u - v, as WCSLIB's celset() takes it: astropy 8.0.1 gives -70 for LATPOLE 0.
    const spherical_rotation tie =
        rotation({.ra = 50.0, .dec = 20.0}, {.phi = 0.0, .theta = 0.0}, {.phi = 0.0, .theta = 0.0});
    EXPECT_NEAR(tie.native_pole().dec, -70.0, 1e-12);
    const spherical_rotation above =
        rotation({.ra = 50.0, .dec = 20.0}, {.phi = 0.0, .theta = 0.0}, {.phi = 0.0, .theta = 1e-9});
    EXPECT_NEAR(above.native_pole().dec, 70.0, 1e-12);
}

TEST(celestial_sphere, the_native_pole_keeps_its_precision_near_a_celestial_pole)
{
    // A cylindrical projection whose reference point is 10^-k degrees from a celestial pole, north or south, at native
    // (0, 0), has its native pole at 90 - |δ0| (equation [31]), which the double δ0 gives exactly. The arccosine of
    // sin δ0, near 1, lost about 3e-15/ε degrees ε radians from the pole, 1.5e-7 degrees for k = 6, and from k = 7 on,
    // where sin δ0 rounds to 1, the whole distance; the arctangent form keeps within 3e-14.
    for (int k = 1; k <= 9; ++k) {
        for (const double sign : {1.0, -1.0}) {
            const double delta_0 = sign * (90.0 - std::pow(10.0, -k));
            SCOPED_TRACE(delta_0);
            const native_point reference_native{.phi = 0.0, .theta = 0.0};
            const spherical_rotation turn = rotation({.ra = 30.0, .dec = delta_0}, reference_native,
                                                     detail::default_pole_native(delta_0, reference_native));
            EXPECT_NEAR(turn.native_pole().dec, 90.0 - std::abs(delta_0), 1e-12);
            const native_point reference = turn.to_native({.ra = 30.0, .dec = delta_0});
            EXPECT_NEAR(reference.theta, 0.0, 1e-12);
            EXPECT_NEAR(reference.phi, 0.0, 1e-12);
        }
    }
}

TEST(celestial_sphere, parameters_without_a_native_pole_define_no_rotation)
{
    // |sin δ0| > z: θ0 = 0 and φp - φ0 = 60 give z = 1/2.
    EXPECT_FALSE(
        spherical_rotation::make({.ra = 0.0, .dec = 40.0}, {.phi = 0.0, .theta = 0.0}, {.phi = 60.0, .theta = 90.0}));
    // Also when |sin δ0| / z exceeds 1 by far less, but by more than rounding does.
    const double beyond = std::asin(1.0001 * std::cos(89.0 / r)) * r;
    EXPECT_FALSE(
        spherical_rotation::make({.ra = 0.0, .dec = beyond}, {.phi = 0.0, .theta = 0.0}, {.phi = 89.0, .theta = 90.0}));
    // Both candidates beyond ±90.
    EXPECT_FALSE(
        spherical_rotation::make({.ra = 0.0, .dec = 20.0}, {.phi = 0.0, .theta = 10.0}, {.phi = 180.0, .theta = 90.0}));
    // z = 0, which is valid on the celestial equator only.
    EXPECT_FALSE(
        spherical_rotation::make({.ra = 75.0, .dec = 30.0}, {.phi = 0.0, .theta = 0.0}, {.phi = 90.0, .theta = 45.0}));
    const spherical_rotation equator =
        rotation({.ra = 75.0, .dec = 0.0}, {.phi = 0.0, .theta = 0.0}, {.phi = 90.0, .theta = 45.0});
    EXPECT_EQ(equator.native_pole().dec, 45.0);
    // Latitudes beyond ±90 and values that are not finite.
    EXPECT_FALSE(
        spherical_rotation::make({.ra = 0.0, .dec = 90.5}, {.phi = 0.0, .theta = 90.0}, {.phi = 180.0, .theta = 90.0}));
    EXPECT_FALSE(spherical_rotation::make({.ra = 0.0, .dec = 10.0}, {.phi = 0.0, .theta = -91.0},
                                          {.phi = 180.0, .theta = 90.0}));
    EXPECT_FALSE(spherical_rotation::make({.ra = std::numeric_limits<double>::quiet_NaN(), .dec = 10.0},
                                          {.phi = 0.0, .theta = 90.0}, {.phi = 180.0, .theta = 90.0}));
}

TEST(celestial_sphere, a_native_pole_at_a_celestial_pole_changes_only_the_origin_of_longitude)
{
    // Equations [32] and [36]: the reference point of a cylindrical projection on the celestial equator, with the
    // native pole at the north celestial pole, and a zenithal projection about the south celestial pole.
    const spherical_rotation north =
        rotation({.ra = 75.0, .dec = 0.0}, {.phi = 0.0, .theta = 0.0}, {.phi = 0.0, .theta = 90.0});
    EXPECT_EQ(north.native_pole().dec, 90.0);
    EXPECT_EQ(north.native_pole().ra, 255.0); // α0 + φp - φ0 - 180, reduced to [0, 360]
    const celestial_point point = north.to_celestial({.phi = 30.0, .theta = 20.0});
    EXPECT_DOUBLE_EQ(point.ra, 105.0);
    EXPECT_DOUBLE_EQ(point.dec, 20.0);
    const spherical_rotation south =
        rotation({.ra = 120.0, .dec = -90.0}, {.phi = 0.0, .theta = 90.0}, {.phi = 0.0, .theta = 90.0});
    const native_point native = south.to_native({.ra = 150.0, .dec = -60.0});
    EXPECT_DOUBLE_EQ(native.phi, -30.0); // αp + φp - α
    EXPECT_DOUBLE_EQ(native.theta, 60.0);
    const celestial_point back = south.to_celestial(native);
    EXPECT_DOUBLE_EQ(back.ra, 150.0);
    EXPECT_DOUBLE_EQ(back.dec, -60.0);

    // The celestial pole at the native longitude 40, which moves the origin of native longitudes.
    const spherical_rotation turned =
        rotation({.ra = 75.0, .dec = 0.0}, {.phi = 0.0, .theta = 0.0}, {.phi = 40.0, .theta = 90.0});
    EXPECT_EQ(turned.native_pole().ra, 295.0);
    const celestial_point moved = turned.to_celestial({.phi = 30.0, .theta = 20.0});
    EXPECT_DOUBLE_EQ(moved.ra, 105.0); // φ + αp - φp + 180, reduced to [0, 360)
    EXPECT_DOUBLE_EQ(moved.dec, 20.0);
    const native_point again = turned.to_native(moved);
    EXPECT_DOUBLE_EQ(again.phi, 30.0);
    EXPECT_DOUBLE_EQ(again.theta, 20.0);

    // The native pole at the south celestial pole with the reference point on the equator, the last case of [32].
    const spherical_rotation down =
        rotation({.ra = 75.0, .dec = 0.0}, {.phi = 0.0, .theta = 0.0}, {.phi = 120.0, .theta = -90.0});
    EXPECT_EQ(down.native_pole().dec, -90.0);
    EXPECT_EQ(down.native_pole().ra, 315.0); // α0 - φp + φ0, reduced to [0, 360]
    const celestial_point below = down.to_celestial({.phi = 30.0, .theta = 20.0});
    EXPECT_DOUBLE_EQ(below.ra, 45.0); // αp + φp - φ
    EXPECT_DOUBLE_EQ(below.dec, -20.0);
    EXPECT_DOUBLE_EQ(down.to_native(below).phi, 30.0);
}

TEST(celestial_sphere, the_right_ascension_of_the_native_pole_has_the_sign_of_that_of_the_reference_point)
{
    // Annex A.2.1 reduces αp to [0, 360] when α0 ≥ 0 and to [-360, 0] otherwise: the same rotation, about α0 = -10 and
    // about α0 = 350, has its native pole 360 degrees apart.
    const native_point reference_native{.phi = 0.0, .theta = 0.0};
    const native_point pole_native{.phi = -60.0, .theta = 90.0};
    const spherical_rotation west = rotation({.ra = -10.0, .dec = 10.0}, reference_native, pole_native);
    const spherical_rotation east = rotation({.ra = 350.0, .dec = 10.0}, reference_native, pole_native);
    EXPECT_GE(west.native_pole().ra, -360.0);
    EXPECT_LE(west.native_pole().ra, 0.0);
    EXPECT_GE(east.native_pole().ra, 0.0);
    EXPECT_LE(east.native_pole().ra, 360.0);
    EXPECT_NEAR(east.native_pole().ra - west.native_pole().ra, 360.0, 1e-9);
    expect_same_point(west.to_celestial({.phi = 30.0, .theta = 20.0}), east.to_celestial({.phi = 30.0, .theta = 20.0}),
                      1e-12);
}

TEST(celestial_sphere, latitudes_keep_their_precision_near_the_poles)
{
    // Points 1e-3 to 1e-7 degrees from the reference point of a zenithal projection are as far from the native pole.
    // The sines of their latitudes are within 2e-10 of 1, where arcsin loses from 3e-10 to 1e-7 degrees.
    const celestial_point reference{.ra = 10.0, .dec = 35.0};
    const spherical_rotation turn = rotation(reference, {.phi = 0.0, .theta = 90.0}, {.phi = 180.0, .theta = 90.0});
    for (const celestial_point point :
         {celestial_point{.ra = 10.0, .dec = 35.001}, celestial_point{.ra = 10.00001, .dec = 34.99999},
          celestial_point{.ra = 10.0, .dec = 35.0000001}, celestial_point{.ra = 10.00000012, .dec = 34.99999995}}) {
        const double expected = 90.0 - distance(point, reference);
        EXPECT_NEAR(turn.to_native(point).theta, expected, 5e-14) << point.ra << " " << point.dec;
    }
    // And near the celestial pole: with the native pole at declination 89.9 and φp = 180, the native point (180, θ)
    // lies between the native pole and the celestial pole, θ - 89.9 degrees from the latter.
    const spherical_rotation polar =
        rotation({.ra = 0.0, .dec = 89.9}, {.phi = 0.0, .theta = 90.0}, {.phi = 180.0, .theta = 90.0});
    const celestial_point near_pole = polar.to_celestial({.phi = 180.0, .theta = 89.9 + 1e-9});
    EXPECT_NEAR(90.0 - near_pole.dec, 1e-9, 1e-13);
}

// -----------------------------------------------------------------------------------------------------------------
// The projections (Annex A.3 to A.5)

TEST(celestial_sphere, the_projections_give_the_values_of_annex_a)
{
    struct expectation
    {
        projection_system system{};
        native_point point{};
        plane_point plane{};
    };
    const double sqrt2 = std::numbers::sqrt2;
    const std::array<expectation, 12> cases{{
        // Rθ = R / tan θ, at θ = 45; u = Rθ sin φ, v = -Rθ cos φ.
        {.system = projection_system::gnomonic, .point = {.phi = 90.0, .theta = 45.0}, .plane = {.u = r, .v = 0.0}},
        {.system = projection_system::gnomonic, .point = {.phi = 180.0, .theta = 45.0}, .plane = {.u = 0.0, .v = r}},
        // Rθ = 2R tan((90 - θ)/2), 2R at the native equator.
        {.system = projection_system::stereographic,
         .point = {.phi = 0.0, .theta = 0.0},
         .plane = {.u = 0.0, .v = -2.0 * r}},
        // Rθ = 2R sin((90 - θ)/2): √2 R at the equator and 2R at the opposite pole.
        {.system = projection_system::zenithal_equal_area,
         .point = {.phi = 90.0, .theta = 0.0},
         .plane = {.u = sqrt2 * r, .v = 0.0}},
        {.system = projection_system::zenithal_equal_area,
         .point = {.phi = -90.0, .theta = -90.0},
         .plane = {.u = -2.0 * r, .v = 0.0}},
        // Rθ = R cos θ.
        {.system = projection_system::orthographic,
         .point = {.phi = 90.0, .theta = 60.0},
         .plane = {.u = r / 2.0, .v = 0.0}},
        {.system = projection_system::orthographic,
         .point = {.phi = 0.0, .theta = 90.0},
         .plane = {.u = 0.0, .v = 0.0}},
        // The identity.
        {.system = projection_system::plate_carree,
         .point = {.phi = -120.0, .theta = 33.5},
         .plane = {.u = -120.0, .v = 33.5}},
        // v = R ln tan((90 + θ)/2): ln tan(67.5°) = asinh(1).
        {.system = projection_system::mercator,
         .point = {.phi = 10.0, .theta = 45.0},
         .plane = {.u = 10.0, .v = r * std::asinh(1.0)}},
        {.system = projection_system::mercator, .point = {.phi = 10.0, .theta = 0.0}, .plane = {.u = 10.0, .v = 0.0}},
        // γ = R √(2 / (1 + cos θ cos(φ/2))): 2√2 R across the equator, and √2 R at the pole.
        {.system = projection_system::hammer_aitoff,
         .point = {.phi = 180.0, .theta = 0.0},
         .plane = {.u = 2.0 * sqrt2 * r, .v = 0.0}},
        {.system = projection_system::hammer_aitoff,
         .point = {.phi = 0.0, .theta = 90.0},
         .plane = {.u = 0.0, .v = sqrt2 * r}},
    }};
    for (const expectation& c : cases) {
        SCOPED_TRACE(static_cast<int>(c.system));
        SCOPED_TRACE(c.point.theta);
        const plane_point plane = value_of(detail::project(c.system, c.point));
        EXPECT_NEAR(plane.u, c.plane.u, 1e-12);
        EXPECT_NEAR(plane.v, c.plane.v, 1e-12);
        const native_point back = value_of(detail::deproject(c.system, plane));
        EXPECT_NEAR(back.theta, c.point.theta, 1e-12);
        if (std::abs(c.point.theta) < 90.0) {
            EXPECT_NEAR(back.phi, c.point.phi, 1e-12);
        }
    }
}

TEST(celestial_sphere, the_projections_go_both_ways_across_their_domains)
{
    for (const projection_system system : every_projection) {
        for (int i = 0; i <= 14; ++i) {
            const double theta = -85.0 + 12.5 * static_cast<double>(i);
            for (int j = 0; j <= 10; ++j) {
                const double phi = -175.0 + 35.0 * static_cast<double>(j);
                const std::optional<plane_point> plane = detail::project(system, {.phi = phi, .theta = theta});
                if (!plane) {
                    continue;
                }
                const native_point back = value_of(detail::deproject(system, *plane));
                EXPECT_NEAR(back.theta, theta, 1e-11) << static_cast<int>(system) << " " << phi << " " << theta;
                // The longitude of a native pole is any.
                if (theta < 90.0) {
                    EXPECT_NEAR(detail::reduce_longitude(back.phi - phi), 0.0, 1e-10)
                        << static_cast<int>(system) << " " << phi << " " << theta;
                }
            }
        }
    }
}

TEST(celestial_sphere, points_beyond_the_domain_of_a_projection_have_no_coordinates)
{
    using detail::deproject;
    using detail::project;
    // The reference implementation rejects Gnomonic points farther than 89.999 degrees from the reference point.
    EXPECT_TRUE(project(projection_system::gnomonic, {.phi = 0.0, .theta = 0.001}));
    EXPECT_FALSE(project(projection_system::gnomonic, {.phi = 0.0, .theta = 0.0009}));
    EXPECT_FALSE(project(projection_system::gnomonic, {.phi = 0.0, .theta = -10.0}));
    EXPECT_FALSE(project(projection_system::stereographic, {.phi = 0.0, .theta = -90.0}));
    EXPECT_TRUE(project(projection_system::stereographic, {.phi = 0.0, .theta = -89.0}));
    EXPECT_TRUE(project(projection_system::zenithal_equal_area, {.phi = 0.0, .theta = -90.0}));
    EXPECT_FALSE(project(projection_system::orthographic, {.phi = 0.0, .theta = -0.5}));
    EXPECT_TRUE(project(projection_system::orthographic, {.phi = 30.0, .theta = 0.0}));
    EXPECT_FALSE(project(projection_system::mercator, {.phi = 0.0, .theta = 90.0}));
    EXPECT_FALSE(project(projection_system::mercator, {.phi = 0.0, .theta = -90.0}));
    EXPECT_FALSE(project(projection_system::gnomonic, {.phi = 0.0, .theta = std::numeric_limits<double>::quiet_NaN()}));
    // Beyond the boundary of the plane.
    EXPECT_FALSE(deproject(projection_system::zenithal_equal_area, {.u = 0.0, .v = 2.0 * r * 1.001}));
    EXPECT_FALSE(deproject(projection_system::orthographic, {.u = r * 1.001, .v = 0.0}));
    EXPECT_FALSE(deproject(projection_system::plate_carree, {.u = 0.0, .v = 90.5}));
    // Beyond 180 degrees from the reference point, the cylindrical projections give the native longitude that the
    // rotation reduces modulo 360, as PixInsight 1.9.5 does; WCSLIB gives no coordinates there.
    EXPECT_EQ(value_of(deproject(projection_system::plate_carree, {.u = 200.0, .v = 90.0})).phi, 200.0);
    EXPECT_EQ(value_of(deproject(projection_system::mercator, {.u = -190.0, .v = 0.0})).phi, -190.0);
    EXPECT_FALSE(deproject(projection_system::hammer_aitoff, {.u = 2.0 * std::numbers::sqrt2 * r * 1.001, .v = 0.0}));
    EXPECT_TRUE(deproject(projection_system::hammer_aitoff, {.u = 2.0 * std::numbers::sqrt2 * r * 0.999, .v = 0.0}));
    // The Stereographic and Mercator projections cover the whole plane, and the Gnomonic one all of it within the limit
    // of the reference implementation, which is 3.28e6 degrees from the reference point on the plane.
    EXPECT_TRUE(deproject(projection_system::gnomonic, {.u = 1e6, .v = -1e6}));
    EXPECT_TRUE(deproject(projection_system::gnomonic, {.u = 0.0, .v = 3.2e6}));
    EXPECT_FALSE(deproject(projection_system::gnomonic, {.u = 0.0, .v = 3.3e6}));
    EXPECT_TRUE(deproject(projection_system::stereographic, {.u = 1e6, .v = -1e6}));
    EXPECT_TRUE(deproject(projection_system::mercator, {.u = 100.0, .v = 1e4}));
}

} // namespace
