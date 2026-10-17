// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "algorithms/celestial_sphere.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <optional>

namespace openxisf::detail {

namespace {

constexpr double radians_per_degree = std::numbers::pi / 180.0;
constexpr double r = degrees_per_radian;

// A candidate latitude of the native pole that rounding takes beyond ±90 by up to this much is a pole.
constexpr double range_tolerance = 1e-10;
// cos δ below this is a celestial pole, where the rotation takes the special forms of equations [32] and [36]: the
// cosine of a latitude within about 6e-14 degrees of ±90, which only rounding produces.
constexpr double pole_tolerance = 1e-15;
// The ratios of equation [31] and of the inverse zenithal projections tolerate this much beyond 1, which rounding
// produces at the boundary of their domains.
constexpr double ratio_tolerance = 1e-12;
// The reference implementation of the specification rejects points whose angular distance from the reference point
// exceeds 89.999 degrees in the Gnomonic projection (Annex A.3), in either direction here.
constexpr double gnomonic_limit = 89.999;

double square(double x) noexcept
{
    return x * x;
}

// The quadrant of a multiple of 90 degrees: 0 for 0, 1 for 90, 2 for 180 and 3 for 270, modulo 360.
std::size_t quadrant(double angle) noexcept
{
    double turns = std::fmod(angle / 90.0, 4.0);
    if (turns < 0.0) {
        turns += 4.0;
    }
    return static_cast<std::size_t>(turns);
}

bool multiple_of_90(double angle) noexcept
{
    return std::fmod(angle, 90.0) == 0.0;
}

// The latitude whose sine is s, of a point whose longitude has the arguments x and y (equations [34] and [35]): arcsin
// loses precision as s approaches ±1, so near the poles the latitude comes from the arccos of the modulus of (x, y),
// which is its cosine (Annex A.2.2).
double latitude(double s, double x, double y) noexcept
{
    if (std::abs(s) < 0.99) {
        return std::asin(s) * r;
    }
    return std::copysign(std::acos(std::min(std::hypot(x, y), 1.0)) * r, s);
}

// αp reduced to the sign of α0: to [0, 360] when α0 ≥ 0, and to [-360, 0] otherwise (Annex A.2.1).
double reduce_to_sign(double alpha_p, double alpha_0) noexcept
{
    const double reduced = std::fmod(alpha_p, 360.0);
    if (alpha_0 >= 0.0) {
        return reduced < 0.0 ? reduced + 360.0 : reduced;
    }
    return reduced > 0.0 ? reduced - 360.0 : reduced;
}

// The celestial latitude of the native pole (Annex A.2.1, equations [30] and [31]), or empty when the parameters define
// none.
std::optional<double> native_pole_latitude(double delta_0, native_point reference_native,
                                           native_point pole_native) noexcept
{
    const double theta_0 = reference_native.theta;
    const double dphi = pole_native.phi - reference_native.phi;
    const double z = std::sqrt(square(cos_degrees(theta_0) * cos_degrees(dphi)) + square(sin_degrees(theta_0)));
    if (z == 0.0) {
        // θ0 = 0 and |φp - φ0| = 90: valid on the celestial equator only.
        if (delta_0 != 0.0) {
            return std::nullopt;
        }
        return std::clamp(pole_native.theta, -90.0, 90.0);
    }
    const double sin_0 = sin_degrees(delta_0);
    const double ratio = sin_0 / z;
    if (!(std::abs(ratio) <= 1.0 + ratio_tolerance)) {
        return std::nullopt;
    }
    const double base = arg(cos_degrees(theta_0) * cos_degrees(dphi), sin_degrees(theta_0));
    // arccos(sin δ0 / z) in its arctangent form, atan2(√(z² - sin² δ0), sin δ0), with z² - sin² δ0 written as
    // cos² δ0 - (cos θ0 sin Δφ)², whose terms keep their precision: near a celestial pole the ratio approaches 1, where
    // the arccosine lost about 3e-15/ε degrees ε radians from the pole.
    const double cos_0 = cos_degrees(delta_0);
    const double c = std::abs(cos_degrees(theta_0) * sin_degrees(dphi));
    const double spread = std::atan2(std::sqrt(std::max((cos_0 - c) * (cos_0 + c), 0.0)), sin_0) * r;
    const double first = reduce_longitude(base + spread);
    const double second = reduce_longitude(base - spread);
    const bool first_valid = std::abs(first) <= 90.0 + range_tolerance;
    const bool second_valid = std::abs(second) <= 90.0 + range_tolerance;
    double chosen = first;
    if (first_valid && second_valid) {
        // The one closest to θp, and the second on a tie, as WCSLIB's celset() chooses.
        if (std::abs(second - pole_native.theta) <= std::abs(first - pole_native.theta)) {
            chosen = second;
        }
    } else if (second_valid) {
        chosen = second;
    } else if (!first_valid) {
        return std::nullopt;
    }
    return std::clamp(chosen, -90.0, 90.0);
}

// Equation [37], and the longitude of equation [38], which every zenithal projection shares.
plane_point zenithal(native_point point, double r_theta) noexcept
{
    return {.u = r_theta * sin_degrees(point.phi), .v = -r_theta * cos_degrees(point.phi)};
}

native_point zenithal(plane_point point, double theta) noexcept
{
    return {.phi = arg(-point.v, point.u), .theta = theta};
}

} // namespace

double sin_degrees(double angle) noexcept
{
    if (multiple_of_90(angle)) {
        constexpr std::array<double, 4> values{0.0, 1.0, 0.0, -1.0};
        return values[quadrant(angle)];
    }
    return std::sin(angle * radians_per_degree);
}

double cos_degrees(double angle) noexcept
{
    if (multiple_of_90(angle)) {
        constexpr std::array<double, 4> values{1.0, 0.0, -1.0, 0.0};
        return values[quadrant(angle)];
    }
    return std::cos(angle * radians_per_degree);
}

double arg(double x, double y) noexcept
{
    const double angle = std::atan2(y, x) * r;
    return angle <= -180.0 ? angle + 360.0 : angle;
}

double reduce_longitude(double angle) noexcept
{
    const double reduced = std::fmod(angle, 360.0);
    if (reduced <= -180.0) {
        return reduced + 360.0;
    }
    if (reduced > 180.0) {
        return reduced - 360.0;
    }
    return reduced;
}

double reduce_right_ascension(double angle) noexcept
{
    double reduced = std::fmod(angle, 360.0);
    if (reduced < 0.0) {
        reduced += 360.0;
    }
    // A tiny negative value becomes 360 when 360 is added to it.
    return reduced >= 360.0 ? reduced - 360.0 : reduced;
}

bool is_zenithal(projection_system system) noexcept
{
    return system == projection_system::gnomonic || system == projection_system::stereographic ||
           system == projection_system::zenithal_equal_area || system == projection_system::orthographic;
}

native_point default_reference_native(projection_system system) noexcept
{
    return {.phi = 0.0, .theta = is_zenithal(system) ? 90.0 : 0.0};
}

native_point default_pole_native(double reference_dec, native_point reference_native) noexcept
{
    const double phi = reference_dec >= reference_native.theta ? reference_native.phi : reference_native.phi + 180.0;
    return {.phi = reduce_longitude(phi), .theta = 90.0};
}

spherical_rotation::spherical_rotation(double alpha_p, double delta_p, double phi_p) noexcept
    : alpha_p_(alpha_p), delta_p_(delta_p), phi_p_(phi_p), polar_(std::abs(cos_degrees(delta_p)) < pole_tolerance)
{}

std::optional<spherical_rotation> spherical_rotation::make(celestial_point reference, native_point reference_native,
                                                           native_point pole_native) noexcept
{
    const double alpha_0 = reference.ra;
    const double delta_0 = reference.dec;
    const double phi_0 = reference_native.phi;
    const double theta_0 = reference_native.theta;
    const double phi_p = pole_native.phi;
    if (!std::isfinite(alpha_0) || !std::isfinite(delta_0) || !std::isfinite(phi_0) || !std::isfinite(theta_0) ||
        !std::isfinite(phi_p) || !std::isfinite(pole_native.theta) || std::abs(delta_0) > 90.0 ||
        std::abs(theta_0) > 90.0) {
        return std::nullopt;
    }
    // The reference point is the native pole (equation [29]).
    if (theta_0 == 90.0) {
        return spherical_rotation(alpha_0, delta_0, phi_p);
    }
    const std::optional<double> latitude_p = native_pole_latitude(delta_0, reference_native, pole_native);
    if (!latitude_p) {
        return std::nullopt;
    }
    const double delta_p = *latitude_p;
    const double cos_p = cos_degrees(delta_p);
    const double cos_0 = cos_degrees(delta_0);
    double alpha_p = 0.0;
    if (std::abs(cos_p * cos_0) < pole_tolerance) {
        // Equation [32].
        if (std::abs(cos_0) < pole_tolerance) {
            alpha_p = alpha_0;
        } else if (delta_p > 0.0) {
            alpha_p = alpha_0 + phi_p - phi_0 - 180.0;
        } else {
            alpha_p = alpha_0 - phi_p + phi_0;
        }
    } else {
        // Equation [33].
        alpha_p =
            alpha_0 - arg((sin_degrees(theta_0) - (sin_degrees(delta_p) * sin_degrees(delta_0))) / (cos_p * cos_0),
                          sin_degrees(phi_p - phi_0) * cos_degrees(theta_0) / cos_0);
    }
    return spherical_rotation(reduce_to_sign(alpha_p, alpha_0), delta_p, phi_p);
}

celestial_point spherical_rotation::to_celestial(native_point point) const noexcept
{
    if (polar_) {
        // Equation [36].
        if (delta_p_ > 0.0) {
            return {.ra = reduce_right_ascension(point.phi + alpha_p_ - phi_p_ + 180.0), .dec = point.theta};
        }
        return {.ra = reduce_right_ascension(alpha_p_ + phi_p_ - point.phi), .dec = -point.theta};
    }
    const double dphi = point.phi - phi_p_;
    const double sin_theta = sin_degrees(point.theta);
    const double cos_theta = cos_degrees(point.theta);
    const double sin_p = sin_degrees(delta_p_);
    const double cos_p = cos_degrees(delta_p_);
    const double x = (sin_theta * cos_p) - (cos_theta * sin_p * cos_degrees(dphi));
    const double y = -cos_theta * sin_degrees(dphi);
    const double s = (sin_theta * sin_p) + (cos_theta * cos_p * cos_degrees(dphi));
    return {.ra = reduce_right_ascension(alpha_p_ + arg(x, y)), .dec = latitude(s, x, y)};
}

native_point spherical_rotation::to_native(celestial_point point) const noexcept
{
    if (polar_) {
        // The inverse of equation [36].
        if (delta_p_ > 0.0) {
            return {.phi = reduce_longitude(point.ra - alpha_p_ + phi_p_ - 180.0), .theta = point.dec};
        }
        return {.phi = reduce_longitude(alpha_p_ + phi_p_ - point.ra), .theta = -point.dec};
    }
    const double dalpha = point.ra - alpha_p_;
    const double sin_delta = sin_degrees(point.dec);
    const double cos_delta = cos_degrees(point.dec);
    const double sin_p = sin_degrees(delta_p_);
    const double cos_p = cos_degrees(delta_p_);
    const double x = (sin_delta * cos_p) - (cos_delta * sin_p * cos_degrees(dalpha));
    const double y = -cos_delta * sin_degrees(dalpha);
    const double s = (sin_delta * sin_p) + (cos_delta * cos_p * cos_degrees(dalpha));
    return {.phi = reduce_longitude(phi_p_ + arg(x, y)), .theta = latitude(s, x, y)};
}

std::optional<plane_point> project(projection_system system, native_point point) noexcept
{
    const double theta = point.theta;
    switch (system) {
    case projection_system::gnomonic:
        if (!(90.0 - theta <= gnomonic_limit)) {
            return std::nullopt;
        }
        return zenithal(point, r * cos_degrees(theta) / sin_degrees(theta));
    case projection_system::stereographic: {
        if (!(theta > -90.0)) {
            return std::nullopt;
        }
        const double half = (90.0 - theta) / 2.0;
        return zenithal(point, 2.0 * r * sin_degrees(half) / cos_degrees(half));
    }
    case projection_system::zenithal_equal_area:
        return zenithal(point, 2.0 * r * sin_degrees((90.0 - theta) / 2.0));
    case projection_system::orthographic:
        if (!(theta >= 0.0)) {
            return std::nullopt;
        }
        return zenithal(point, r * cos_degrees(theta));
    case projection_system::plate_carree:
        return plane_point{.u = point.phi, .v = theta};
    case projection_system::mercator: {
        if (!(std::abs(theta) < 90.0)) {
            return std::nullopt;
        }
        const double half = (90.0 + theta) / 2.0;
        return plane_point{.u = point.phi, .v = r * std::log(sin_degrees(half) / cos_degrees(half))};
    }
    case projection_system::hammer_aitoff: {
        const double cos_theta = cos_degrees(theta);
        const double gamma = r * std::sqrt(2.0 / (1.0 + (cos_theta * cos_degrees(point.phi / 2.0))));
        return plane_point{.u = 2.0 * gamma * cos_theta * sin_degrees(point.phi / 2.0),
                           .v = gamma * sin_degrees(theta)};
    }
    }
    return std::nullopt;
}

std::optional<native_point> deproject(projection_system system, plane_point point) noexcept
{
    const double r_theta = std::hypot(point.u, point.v);
    switch (system) {
    case projection_system::gnomonic: {
        // The limit of project(): a point of the plane farther out stands for no point that it projects.
        const double theta = std::atan2(r, r_theta) * r;
        if (!(90.0 - theta <= gnomonic_limit)) {
            return std::nullopt;
        }
        return zenithal(point, theta);
    }
    case projection_system::stereographic:
        return zenithal(point, 90.0 - (2.0 * std::atan(r_theta / (2.0 * r)) * r));
    case projection_system::zenithal_equal_area: {
        const double ratio = r_theta / (2.0 * r);
        if (!(ratio <= 1.0 + ratio_tolerance)) {
            return std::nullopt;
        }
        return zenithal(point, 90.0 - (2.0 * std::asin(std::min(ratio, 1.0)) * r));
    }
    case projection_system::orthographic: {
        const double ratio = r_theta / r;
        if (!(ratio <= 1.0 + ratio_tolerance)) {
            return std::nullopt;
        }
        return zenithal(point, std::acos(std::min(ratio, 1.0)) * r);
    }
    case projection_system::plate_carree:
        if (!(std::abs(point.v) <= 90.0)) {
            return std::nullopt;
        }
        return native_point{.phi = point.u, .theta = point.v};
    case projection_system::mercator:
        return native_point{.phi = point.u, .theta = (2.0 * std::atan(std::exp(point.v / r)) * r) - 90.0};
    case projection_system::hammer_aitoff: {
        const double zz = 1.0 - square(point.u / (4.0 * r)) - square(point.v / (2.0 * r));
        if (!(zz >= 0.5 - ratio_tolerance)) {
            return std::nullopt;
        }
        const double clamped = std::max(zz, 0.5);
        const double z = std::sqrt(clamped);
        return native_point{.phi = 2.0 * arg((2.0 * clamped) - 1.0, z * point.u / (2.0 * r)),
                            .theta = std::asin(std::clamp(point.v * z / r, -1.0, 1.0)) * r};
    }
    }
    return std::nullopt;
}

} // namespace openxisf::detail
