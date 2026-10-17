// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/export.h>
#include <openxisf/property.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

/// @file
/// Astrometric solutions (spec §11.5.3.7): the transformation between the image coordinates of an image and celestial
/// coordinates that the properties of the AstrometricSolution namespace describe, and its evaluation.

namespace openxisf {

/// A point in image coordinates, in pixels (spec §11.5.3.7.1): x grows with the column index and y with the row index,
/// and the centre of the pixel at column i and row j is at (i + 0.5, j + 0.5).
struct image_point
{
    double x = 0.0;
    double y = 0.0;

    friend bool operator==(const image_point&, const image_point&) = default;
};

/// Celestial coordinates, in degrees: the right ascension, in [0, 360), and the declination, in [-90, 90], referred to
/// the celestial reference system of a solution.
struct celestial_point
{
    double ra = 0.0;
    double dec = 0.0;

    friend bool operator==(const celestial_point&, const celestial_point&) = default;
};

/// The projection systems of astrometric solutions (spec §11.5.3.7.2.2, Annex A). The cylindrical ones, plate_carree
/// and mercator, take a point of the plane more than 180 degrees from the reference point to its native longitude
/// modulo 360, as PixInsight does: the points of an image that spans more than 360 degrees have the same celestial
/// coordinates 360 degrees apart, which go back to the one within 180 degrees of the reference point.
enum class projection_system : std::uint8_t
{
    gnomonic,
    stereographic,
    zenithal_equal_area,
    orthographic,
    plate_carree,
    mercator,
    hammer_aitoff,
};

/// The identifier of a projection system in the specification, such as "Gnomonic".
[[nodiscard]] OPENXISF_API std::string_view projection_system_name(projection_system system) noexcept;

/// The layers of an astrometric solution that transform coordinates, each the fallback of the next (spec §11.5.3.7).
enum class astrometric_layer : std::uint8_t
{
    linear = 1,     ///< The projection, with a linear transformation of image coordinates (layer 1).
    projective = 2, ///< Projective transformations of image coordinates (layer 2).
    distortion = 3, ///< Distortion models added to the projective transformations (layer 3).
};

/// Whether a layer of an astrometric solution can be evaluated, and why not (spec §11.5.3.7.7).
enum class astrometric_status : std::uint8_t
{
    available,               ///< The layer can be evaluated.
    absent,                  ///< None of the properties of the layer is present.
    unsupported_version,     ///< The solution is not of revision 1.x of spec §11.5.3.7, so no part of it is read.
    missing_property,        ///< A property that the layer requires is missing.
    inconsistent,            ///< A property has another type, dimensions that disagree, or a value out of its range.
    unknown_identifier,      ///< A projection system, basis function or kind of term that OpenXISF does not know.
    lower_layer_unavailable, ///< A layer below it is not available.
};

/// The first layer of an astrometric solution (spec §11.5.3.7.2), with the defaults of the specification applied.
struct astrometric_projection
{
    openxisf::projection_system projection_system = openxisf::projection_system::gnomonic;
    /// The celestial coordinates of the projection reference point.
    celestial_point reference_celestial{};
    /// The image coordinates that correspond to the origin of the projection plane.
    image_point reference_image{};
    /// The matrix of the linear transformation from image coordinates to projection plane coordinates, in degrees per
    /// pixel, row by row.
    std::array<double, 4> linear_transformation{};
    /// The native longitude and latitude of the projection reference point, in degrees.
    std::array<double, 2> reference_native{};
    /// The native longitude and latitude of the celestial pole, in degrees. Without the property, Annex A.2.1 gives
    /// the longitude 0 to a Gnomonic solution whose reference point is the north celestial pole, while PixInsight
    /// evaluates Gnomonic solutions with the direct formulas of Annex A.3, which take 180: the two then differ by a
    /// half turn about the pole. An application that needs PixInsight's result for such a solution sets
    /// AstrometricSolution:CelestialPoleNativeCoordinates to (180, 90).
    std::array<double, 2> celestial_pole_native{};
    /// The identifier of the celestial reference system, ICRS when the solution does not specify one, or empty when
    /// its property is not a String.
    std::string celestial_reference_system{};

    friend bool operator==(const astrometric_projection&, const astrometric_projection&) = default;
};

/// The second layer of an astrometric solution (spec §11.5.3.7.3): a 3 x 3 matrix in each direction, row by row,
/// acting on homogeneous coordinates.
struct projective_transformations
{
    std::array<double, 9> image_to_projection{};
    std::array<double, 9> projection_to_image{};

    friend bool operator==(const projective_transformations&, const projective_transformations&) = default;
};

#if defined(_MSC_VER)
// astrometric_solution holds a standard-library member, which cl reports for an exported class. A C++ interface
// requires the same standard library on both sides of the DLL boundary anyway.
#pragma warning(push)
#pragma warning(disable : 4251)
#endif

/// An astrometric solution (spec §11.5.3.7), read from the properties of the AstrometricSolution namespace of an
/// image.
///
/// Reading a solution never fails. Each layer is checked: one whose properties are missing, have other types or
/// dimensions that disagree, or that names an identifier OpenXISF does not know, is not available, and neither is any
/// layer above it. A solution of a major revision other than 1 is not read at all. Properties of the namespace that the
/// solution does not use are ignored, and nothing outside the namespace is read. The evaluation uses the highest layer
/// available, in double precision, and the inverse that each layer stores, never a numerical inversion.
///
/// A solution is immutable: any number of threads may use it at once. Copies share its data. A moved-from solution
/// can only be destroyed or assigned to.
///
/// A solution describes the image it was computed for. The writer keeps the properties of a solution as it is given
/// them, so an application that crops, resamples or otherwise changes the geometry of an image removes them
/// (remove_astrometric_solution()), unless it transforms the solution exactly (spec §11.5.3.7.8).
class OPENXISF_API astrometric_solution
{
public:
    /// The solution that properties describe, such as image_info::properties.
    explicit astrometric_solution(const property_list& properties);

    /// The highest layer available; empty when the first layer is not, and the solution cannot be evaluated.
    [[nodiscard]] std::optional<astrometric_layer> layer() const noexcept;

    /// Whether a layer is available.
    [[nodiscard]] astrometric_status status(astrometric_layer layer) const noexcept;

    /// What makes a layer unavailable, for a person to read, such as "AstrometricSolution:LinearTransformationMatrix
    /// is missing"; empty when the layer is available or absent.
    [[nodiscard]] const std::string& problem(astrometric_layer layer) const noexcept;

    /// The value of AstrometricSolution:Version, such as "1.0"; empty when there is none, or when it is not a String,
    /// which makes the first layer inconsistent.
    [[nodiscard]] const std::string& version() const noexcept;

    /// The first layer; empty when it is not available.
    [[nodiscard]] const std::optional<astrometric_projection>& projection() const noexcept;

    /// The second layer; empty when it is not available.
    [[nodiscard]] const std::optional<projective_transformations>& projective() const noexcept;

    /// The celestial coordinates of a point of the image, through the highest layer available. Empty when the point
    /// has none: beyond the domain of the projection, where a projective transformation is undefined, or when the
    /// point is not finite.
    /// @throws usage_error when no layer is available.
    [[nodiscard]] std::optional<celestial_point> image_to_celestial(image_point point) const;

    /// The celestial coordinates of a point of the image through a given layer, which must be available.
    /// @throws usage_error when the layer is not available.
    [[nodiscard]] std::optional<celestial_point> image_to_celestial(image_point point, astrometric_layer layer) const;

    /// The image coordinates of a point of the sky, through the highest layer available. Empty when the point has none:
    /// beyond the domain of the projection, where a projective transformation is undefined, or when the point is not
    /// finite or its declination is beyond +/-90. The point may lie outside the image.
    /// @throws usage_error when no layer is available.
    [[nodiscard]] std::optional<image_point> celestial_to_image(celestial_point point) const;

    /// The image coordinates of a point of the sky through a given layer, which must be available.
    /// @throws usage_error when the layer is not available.
    [[nodiscard]] std::optional<image_point> celestial_to_image(celestial_point point, astrometric_layer layer) const;

private:
    struct model;

    [[nodiscard]] astrometric_layer checked_layer(std::optional<astrometric_layer> layer) const;

    std::shared_ptr<const model> model_;
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

/// Removes every property of the AstrometricSolution namespace from properties, as an encoder must when an image
/// undergoes a geometric transformation that its solution does not follow (spec §11.5.3.7.8). Properties of other
/// namespaces are kept, in their order, those of applications included. Returns the number removed. It takes time
/// linear in the number of properties, and leaves them unchanged when it throws.
OPENXISF_API std::size_t remove_astrometric_solution(property_list& properties);

} // namespace openxisf
