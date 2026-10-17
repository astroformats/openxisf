// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/export.h>

#include <complex>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

/// @file
/// The types that describe images: sample formats, colour spaces, storage models, image types, orientations, geometry
/// and bounds (spec §8.5, §11.5).

namespace openxisf {

/// The data type of the pixel samples of an image (spec §11.5.1, Table 11).
enum class sample_format : std::uint8_t
{
    uint8,
    uint16,
    uint32,
    uint64,
    float32,   ///< IEEE 754 binary32.
    float64,   ///< IEEE 754 binary64.
    complex32, ///< A complex number whose parts are IEEE 754 binary32 values, the real part first.
    complex64, ///< A complex number whose parts are IEEE 754 binary64 values, the real part first.
};

/// The name of a sample format in the specification, such as "UInt16".
[[nodiscard]] constexpr std::string_view sample_format_name(sample_format format) noexcept
{
    switch (format) {
    case sample_format::uint8:
        return "UInt8";
    case sample_format::uint16:
        return "UInt16";
    case sample_format::uint32:
        return "UInt32";
    case sample_format::uint64:
        return "UInt64";
    case sample_format::float32:
        return "Float32";
    case sample_format::float64:
        return "Float64";
    case sample_format::complex32:
        return "Complex32";
    case sample_format::complex64:
        return "Complex64";
    }
    return {};
}

/// The size of a sample, in bytes.
[[nodiscard]] constexpr std::size_t sample_size(sample_format format) noexcept
{
    switch (format) {
    case sample_format::uint8:
        return 1;
    case sample_format::uint16:
        return 2;
    case sample_format::uint32:
    case sample_format::float32:
        return 4;
    case sample_format::uint64:
    case sample_format::float64:
    case sample_format::complex32:
        return 8;
    case sample_format::complex64:
        return 16;
    }
    return 0;
}

/// The C++ types of pixel samples, one for each sample format.
template <typename T>
concept pixel_sample =
    std::same_as<T, std::uint8_t> || std::same_as<T, std::uint16_t> || std::same_as<T, std::uint32_t> ||
    std::same_as<T, std::uint64_t> || std::same_as<T, float> || std::same_as<T, double> ||
    std::same_as<T, std::complex<float>> || std::same_as<T, std::complex<double>>;

/// The sample format whose samples T holds: sample_format::float32 for float, sample_format::complex64 for
/// std::complex<double>.
template <pixel_sample T> [[nodiscard]] constexpr sample_format sample_format_of() noexcept
{
    if constexpr (std::same_as<T, std::uint8_t>) {
        return sample_format::uint8;
    } else if constexpr (std::same_as<T, std::uint16_t>) {
        return sample_format::uint16;
    } else if constexpr (std::same_as<T, std::uint32_t>) {
        return sample_format::uint32;
    } else if constexpr (std::same_as<T, std::uint64_t>) {
        return sample_format::uint64;
    } else if constexpr (std::same_as<T, float>) {
        return sample_format::float32;
    } else if constexpr (std::same_as<T, double>) {
        return sample_format::float64;
    } else if constexpr (std::same_as<T, std::complex<float>>) {
        return sample_format::complex32;
    } else {
        return sample_format::complex64;
    }
}

/// The colour space of the pixel samples of an image (spec §8.5.4, §11.5.2, Table 14).
enum class color_space : std::uint8_t
{
    gray,    ///< Grayscale: one nominal channel.
    rgb,     ///< RGB: three nominal channels, red, green and blue.
    cie_lab, ///< CIE L*a*b*, normalized as Annex B describes: three nominal channels.
};

/// The name of a colour space in the specification: "Gray", "RGB" or "CIELab".
[[nodiscard]] constexpr std::string_view color_space_name(color_space space) noexcept
{
    switch (space) {
    case color_space::gray:
        return "Gray";
    case color_space::rgb:
        return "RGB";
    case color_space::cie_lab:
        return "CIELab";
    }
    return {};
}

/// The number of nominal channels of a colour space: the channels that define its colour model (spec §8.5.1). The
/// channels of an image beyond them are alpha channels.
[[nodiscard]] constexpr std::uint64_t nominal_channels(color_space space) noexcept
{
    return space == color_space::gray ? 1 : 3;
}

/// How the samples of an image are ordered (spec §8.5.3, Table 13). In both models the first pixel coordinate varies
/// fastest.
enum class pixel_storage : std::uint8_t
{
    planar, ///< Channel by channel: all the samples of channel 0, then those of channel 1, and so on.
    normal, ///< Pixel by pixel: the samples of each pixel together, in channel order.
};

/// The name of a storage model in the specification: "Planar" or "Normal".
[[nodiscard]] constexpr std::string_view pixel_storage_name(pixel_storage storage) noexcept
{
    switch (storage) {
    case pixel_storage::planar:
        return "Planar";
    case pixel_storage::normal:
        return "Normal";
    }
    return {};
}

/// What an image is, for applications that process astronomical images (spec §11.5.1, Table 12).
enum class image_type : std::uint8_t
{
    bias,
    dark,
    flat,
    light,
    master_bias,
    master_dark,
    master_flat,
    master_light,
    defect_map,
    rejection_map_high,
    rejection_map_low,
    binary_rejection_map_high,
    binary_rejection_map_low,
    slope_map,
    weight_map,
};

/// The name of an image type in the specification, such as "MasterDark".
[[nodiscard]] constexpr std::string_view image_type_name(image_type type) noexcept
{
    switch (type) {
    case image_type::bias:
        return "Bias";
    case image_type::dark:
        return "Dark";
    case image_type::flat:
        return "Flat";
    case image_type::light:
        return "Light";
    case image_type::master_bias:
        return "MasterBias";
    case image_type::master_dark:
        return "MasterDark";
    case image_type::master_flat:
        return "MasterFlat";
    case image_type::master_light:
        return "MasterLight";
    case image_type::defect_map:
        return "DefectMap";
    case image_type::rejection_map_high:
        return "RejectionMapHigh";
    case image_type::rejection_map_low:
        return "RejectionMapLow";
    case image_type::binary_rejection_map_high:
        return "BinaryRejectionMapHigh";
    case image_type::binary_rejection_map_low:
        return "BinaryRejectionMapLow";
    case image_type::slope_map:
        return "SlopeMap";
    case image_type::weight_map:
        return "WeightMap";
    }
    return {};
}

/// How to turn an image to show it in its intended orientation (spec §11.5.2). It is meant for showing images only:
/// processing that depends on the physical disposition of the pixels, such as calibration, must ignore it.
enum class orientation : std::uint8_t
{
    none,                 ///< Keep the orientation as stored ("0").
    flip,                 ///< Flip the image horizontally ("flip").
    rotate_90,            ///< Rotate it by 90 degrees counter-clockwise ("90").
    rotate_90_flip,       ///< Rotate it by 90 degrees counter-clockwise, then flip it horizontally ("90;flip").
    rotate_minus_90,      ///< Rotate it by 90 degrees clockwise ("-90").
    rotate_minus_90_flip, ///< Rotate it by 90 degrees clockwise, then flip it horizontally ("-90;flip").
    rotate_180,           ///< Rotate it by 180 degrees ("180").
    rotate_180_flip,      ///< Rotate it by 180 degrees, then flip it horizontally: a vertical flip ("180;flip").
};

/// The value of an orientation attribute, such as "90;flip".
[[nodiscard]] constexpr std::string_view orientation_name(orientation turn) noexcept
{
    switch (turn) {
    case orientation::none:
        return "0";
    case orientation::flip:
        return "flip";
    case orientation::rotate_90:
        return "90";
    case orientation::rotate_90_flip:
        return "90;flip";
    case orientation::rotate_minus_90:
        return "-90";
    case orientation::rotate_minus_90_flip:
        return "-90;flip";
    case orientation::rotate_180:
        return "180";
    case orientation::rotate_180_flip:
        return "180;flip";
    }
    return {};
}

/// The size of an image (spec §8.5.1, §11.5.1): its length along each axis, in pixels, and its number of channels. For
/// a two-dimensional image the dimensions are its width and height.
struct geometry
{
    /// The lengths of the axes, the first one varying fastest in storage. An image has at least one, and none is zero.
    std::vector<std::uint64_t> dimensions{};
    /// The number of channels, nominal and alpha. An image has at least one.
    std::uint64_t channels = 1;

    /// The number of pixels: the product of the dimensions, and 1 without dimensions, as for a default-constructed
    /// geometry, which describes no image.
    /// @throws usage_error (errc::arithmetic_overflow) when it does not fit in 64 bits.
    [[nodiscard]] OPENXISF_API std::uint64_t pixel_count() const;

    /// The number of samples: pixel_count() * channels.
    /// @throws usage_error (errc::arithmetic_overflow) when it does not fit in 64 bits.
    [[nodiscard]] OPENXISF_API std::uint64_t sample_count() const;

    friend bool operator==(const geometry&, const geometry&) = default;
};

/// The representable range of an image (spec §8.5.5): the sample values shown as black and as white.
struct bounds
{
    double lower = 0.0; ///< The black point.
    double upper = 1.0; ///< The white point.

    friend bool operator==(const bounds&, const bounds&) = default;
};

} // namespace openxisf
