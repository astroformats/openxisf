// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/error.h>
#include <openxisf/image.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// The attributes of the elements that describe images (spec §11.6 to §11.11), read from their text, and the rules that
// relate them. Parsers throw invalid_data_error, whose message has no context; the caller adds it. Names are
// case-sensitive unless the specification says otherwise.

namespace openxisf::detail {

/// True when name is a FITS keyword name (FITS 4.0 §4.1.2.1) without padding: at most eight characters, each an
/// upper-case letter, a digit, a hyphen or an underscore. The empty name is the blank keyword of FITS, which the XML
/// schema of XISF does not allow; the callers check it apart.
[[nodiscard]] bool is_fits_keyword_name(std::string_view name) noexcept;

/// True for the keywords that have no value: COMMENT, HISTORY and the blank keyword (FITS 4.0 §4.4.2.4).
[[nodiscard]] bool is_commentary_keyword(std::string_view name) noexcept;

/// True when text is printable ASCII, from the space to the tilde, as the value and the comment of a FITS keyword are
/// (FITS 4.0 §4.1.1).
[[nodiscard]] bool is_fits_keyword_text(std::string_view text) noexcept;

/// True when profile starts with the header of an ICC profile: 128 bytes, with the signature `acsp` at byte 36
/// (ICC.1:2022 §7.2). An empty profile is no profile, and has none.
[[nodiscard]] bool has_icc_profile_header(std::span<const std::byte> profile) noexcept;

/// The gamma of a gamma attribute (spec §11.8.1): a finite floating point value above zero, or nothing for the sRGB
/// function, whose name is sRGB in any case. Throws with errc::invalid_rgb_working_space.
[[nodiscard]] std::optional<double> parse_gamma(std::string_view text);

/// The three values of an attribute of the form a:b:c, each a finite floating point value of spec §8.3.3. Throws with
/// code.
[[nodiscard]] std::array<double, 3> parse_triplet(std::string_view text, errc code);

/// The four values of an attribute of the form a:b:c:d, like parse_triplet().
[[nodiscard]] std::array<double, 4> parse_quadruplet(std::string_view text, errc code);

/// How far stored luminance coefficients may be from those that luminance_coefficients() derives: values written with
/// four decimals agree, and the coefficients of another reference white, such as those of sRGB for D65, do not.
inline constexpr double luminance_tolerance = 1e-4;

/// Checks the rules of spec §8.5.4.1 for an RGB working space read from its attributes: chromaticities and luminance
/// coefficients in [0, 1], and chromaticities that define a working space. Returns the luminance coefficients that the
/// chromaticities give, which the stored ones may differ from. Throws with errc::invalid_rgb_working_space.
[[nodiscard]] std::array<double, 3> check_rgb_working_space(const rgb_working_space& space);

/// True when the luminance coefficients that check_rgb_working_space() derives are in [0, 1], as those that an encoder
/// computes and writes must be (spec §8.5.4.1, §11.8.1). A white just outside the triangle of the primaries gives one
/// below 0, which a stored 0 may still agree with within luminance_tolerance.
[[nodiscard]] bool derived_luminance_in_range(const std::array<double, 3>& derived) noexcept;

/// Checks the constraints of spec §8.5.6 on the parameters of a display function, for each component: m, s and h in
/// [0, 1], s at most h, l at most 0 and r at least 1. Throws with errc::invalid_display_function.
void check_display_function(const display_function& function);

/// The value of a width or height attribute of a ColorFilterArray element: an unsigned integer above zero. Throws with
/// errc::invalid_color_filter_array.
[[nodiscard]] std::uint64_t parse_cfa_size(std::string_view text);

/// Checks a colour filter array (spec §11.10.1): a pattern of the characters of Table 18 whose length is width ×
/// height. Throws with errc::invalid_color_filter_array.
void check_color_filter_array(const color_filter_array& filter);

/// The value of a horizontal or vertical attribute of a Resolution element: a finite floating point value above zero.
/// Throws with errc::invalid_resolution.
[[nodiscard]] double parse_resolution_value(std::string_view text);

/// The unit of a unit attribute of a Resolution element: inch or cm. Throws with errc::invalid_resolution.
[[nodiscard]] resolution_unit parse_resolution_unit(std::string_view text);

} // namespace openxisf::detail
