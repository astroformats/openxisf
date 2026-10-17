// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/export.h>
#include <openxisf/property.h>
#include <openxisf/types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// @file
/// Images and what describes them (spec §8.5, §11.5 to §11.12).

namespace openxisf {

/// A FITS header keyword of an image (spec §11.6), as the FITS standard defines it: a name of one to eight characters
/// without padding, and the text of its value and comment, in printable ASCII. HISTORY and COMMENT keywords have an
/// empty value. The reader keeps a keyword whose name is empty, the blank keyword of FITS, with a warning, and so it
/// keeps a value or comment with other characters, such as the degree sign; the writer refuses both, since XISF
/// requires a name and FITS printable ASCII.
struct fits_keyword
{
    std::string name{};
    /// The value as written in a FITS header, such as 'M31' with its quotes, or 300. The specification asks encoders
    /// not to pad it with spaces, as the fixed-length records of FITS do (spec §11.6.1); the writer writes it as given.
    std::string value{};
    std::string comment{};

    friend bool operator==(const fits_keyword&, const fits_keyword&) = default;
};

/// An RGB working space (spec §8.5.4.1, §11.8): the colorimetry of RGB samples, relative to the D50 reference white. A
/// default-constructed one is sRGB, the working space of images that do not specify one.
///
/// The luminance coefficients are not free: they follow from the chromaticities of the primaries and D50. Their order,
/// in each array, is red, green and blue.
struct rgb_working_space
{
    /// The exponent that linearizes the RGB components, above zero; empty for the sRGB function, which has none.
    std::optional<double> gamma{};
    /// The x chromaticity coordinates of the primaries.
    std::array<double, 3> x{0.648431, 0.321152, 0.155886};
    /// The y chromaticity coordinates of the primaries.
    std::array<double, 3> y{0.330856, 0.597871, 0.066044};
    /// The luminance coefficients of the primaries, Y in the specification. The writer writes those that the
    /// chromaticities give.
    std::array<double, 3> luminance{0.222491, 0.716888, 0.060621};
    /// A name for the working space, such as "Adobe RGB (1998)"; empty when there is none.
    std::string name{};

    friend bool operator==(const rgb_working_space&, const rgb_working_space&) = default;
};

/// A display function (spec §8.5.6, §11.9): how to stretch the samples of an image to show it. Each array has one
/// parameter for each component: red or gray, green, blue, and the lightness of colour images, in that order. A
/// default-constructed one is the identity, the display function of images that do not specify one.
struct display_function
{
    /// The midtones balance, in [0, 1]; 0.5 is linear.
    std::array<double, 4> midtones{0.5, 0.5, 0.5, 0.5};
    /// The shadows clipping point, in [0, 1].
    std::array<double, 4> shadows{0.0, 0.0, 0.0, 0.0};
    /// The highlights clipping point, in [0, 1] and not below the shadows clipping point.
    std::array<double, 4> highlights{1.0, 1.0, 1.0, 1.0};
    /// The shadows dynamic range expansion, at most 0.
    std::array<double, 4> shadows_expansion{0.0, 0.0, 0.0, 0.0};
    /// The highlights dynamic range expansion, at least 1.
    std::array<double, 4> highlights_expansion{1.0, 1.0, 1.0, 1.0};
    /// A name for the display function; empty when there is none.
    std::string name{};

    friend bool operator==(const display_function&, const display_function&) = default;
};

/// A colour filter array (spec §11.10), such as a Bayer filter, with which a two-dimensional image is mosaiced.
struct color_filter_array
{
    /// The elements of the matrix, row by row from the top, left to right within a row, as they lie on the image: 0
    /// (none or undefined), R, G, B, W (white or panchromatic), C, M or Y. Its length is width * height.
    std::string pattern{};
    /// The size of the matrix, in pixels, above zero.
    std::uint64_t width = 0;
    std::uint64_t height = 0;
    /// What the filter is, such as "RGGB Bayer filter"; empty when there is none.
    std::string name{};

    friend bool operator==(const color_filter_array&, const color_filter_array&) = default;
};

/// The unit of length of a resolution.
enum class resolution_unit : std::uint8_t
{
    inch,
    centimeter,
};

/// The value of a unit attribute: "inch" or "cm".
[[nodiscard]] constexpr std::string_view resolution_unit_name(resolution_unit unit) noexcept
{
    switch (unit) {
    case resolution_unit::inch:
        return "inch";
    case resolution_unit::centimeter:
        return "cm";
    }
    return {};
}

/// The resolution of an image on a display medium (spec §11.11): pixels per unit of length along each axis. A
/// default-constructed one is 72 pixels per inch, the resolution of images that do not specify one.
struct resolution
{
    /// Pixels per unit along the X axis, above zero.
    double horizontal = 72.0;
    /// Pixels per unit along the Y axis, above zero.
    double vertical = 72.0;
    resolution_unit unit = resolution_unit::inch;

    friend bool operator==(const resolution&, const resolution&) = default;
};

/// A thumbnail (spec §11.12): a small representation of an image, with its pixel data. It is a two-dimensional Gray or
/// RGB image of UInt8 or UInt16 samples, with at most one alpha channel, and its representable range is that of its
/// sample format. Its width and height should not be greater than 1024 pixels, which the writer does not check. A
/// Thumbnail element is an Image element under another name, without bounds, colour filter array or thumbnail, so the
/// members are those of image_info that it can have, but with other defaults: UInt8 samples and the RGB colour space,
/// where an image_info has UInt16 and Gray, the colour space that the specification gives an element without one.
struct thumbnail
{
    openxisf::geometry geometry{};
    openxisf::sample_format sample_format = openxisf::sample_format::uint8;
    openxisf::color_space color_space = openxisf::color_space::rgb;
    openxisf::pixel_storage pixel_storage = openxisf::pixel_storage::planar;
    std::optional<openxisf::image_type> image_type{};
    double offset = 0.0;
    openxisf::orientation orientation = openxisf::orientation::none;
    std::string id{};
    std::string uuid{};
    property_list properties{};
    std::vector<table> tables{};
    std::vector<fits_keyword> fits_keywords{};
    std::vector<std::byte> icc_profile{};
    std::optional<openxisf::rgb_working_space> rgb_working_space{};
    std::optional<openxisf::display_function> display_function{};
    std::optional<openxisf::resolution> resolution{};
    /// The pixel data: the samples in native byte order, in the storage model of pixel_storage. Empty when the unit was
    /// opened with read_options::header_only and its ancillary data have not been loaded.
    std::vector<std::byte> pixels{};

    friend bool operator==(const thumbnail&, const thumbnail&) = default;
};

/// An image (spec §8.5): the attributes of its Image element (spec §11.5), its properties and tables, and the elements
/// that describe it (spec §11.6 to §11.12). Its pixel data are read separately, with reader::read_pixels().
///
/// A plain value: the members follow the attributes and elements of the specification, and those that the
/// specification makes optional have its defaults or are empty.
struct image_info
{
    openxisf::geometry geometry{};
    openxisf::sample_format sample_format = openxisf::sample_format::uint16;
    openxisf::color_space color_space = openxisf::color_space::gray;
    openxisf::pixel_storage pixel_storage = openxisf::pixel_storage::planar;
    /// What the image is; empty when the unit does not say.
    std::optional<openxisf::image_type> image_type{};
    /// The representable range as written (spec §11.5.1): required for floating point images, optional for complex
    /// images, whose range is undefined without it, and empty when an integer image uses its default.
    /// representable_range() applies the default.
    std::optional<openxisf::bounds> bounds{};
    /// A pedestal added to every sample, at least 0 (spec §11.5.2).
    double offset = 0.0;
    openxisf::orientation orientation = openxisf::orientation::none;
    /// An identifier of the image, unique in its unit: [_a-zA-Z][_a-zA-Z0-9]*. Empty when there is none.
    std::string id{};
    /// A version 4 UUID in canonical form, in lowercase. Empty when there is none.
    std::string uuid{};
    /// The properties of the image (spec §11.1).
    property_list properties{};
    /// The table properties of the image (spec §11.3).
    std::vector<table> tables{};
    /// The FITS header keywords of the image, in order (spec §11.6).
    std::vector<fits_keyword> fits_keywords{};
    /// The ICC profile (spec §11.7), its bytes as the unit holds them; empty when there is none, or when the unit was
    /// opened with read_options::header_only and its ancillary data have not been loaded. The reader keeps a profile
    /// that does not start with the 128-byte header of an ICC profile, with its signature `acsp`, with a warning; the
    /// writer refuses it. The writer sets the embedded-profile flag of a profile that it writes (bit 0 of byte 47),
    /// and changes nothing else.
    std::vector<std::byte> icc_profile{};
    /// The RGB working space; empty when the unit does not specify one, and sRGB applies.
    std::optional<openxisf::rgb_working_space> rgb_working_space{};
    /// The display function; empty when the unit does not specify one, and the identity applies.
    std::optional<openxisf::display_function> display_function{};
    /// The colour filter array with which the image is mosaiced; empty when it is not.
    std::optional<openxisf::color_filter_array> color_filter_array{};
    /// The resolution; empty when the unit does not specify one, and 72 pixels per inch applies.
    std::optional<openxisf::resolution> resolution{};
    /// The thumbnail; empty when there is none.
    std::optional<openxisf::thumbnail> thumbnail{};

    /// The size of the pixel data, in bytes: geometry.sample_count() * sample_size(sample_format).
    /// @throws usage_error (errc::arithmetic_overflow) when it does not fit in 64 bits.
    [[nodiscard]] OPENXISF_API std::uint64_t data_size() const;

    /// The representable range (spec §8.5.5): bounds when there are, and otherwise [0, 2^k - 1] for an image of k-bit
    /// unsigned integers. Empty for a floating point image without bounds, which violates the specification, and for a
    /// complex image without bounds, whose range is undefined.
    [[nodiscard]] OPENXISF_API std::optional<openxisf::bounds> representable_range() const noexcept;

    friend bool operator==(const image_info&, const image_info&) = default;
};

} // namespace openxisf
