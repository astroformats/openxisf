// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/image.h>
#include <openxisf/limits.h>

#include "container/data_block.h"
#include "core/diagnostic_log.h"
#include "io/thread_safe_source.h"
#include "model/ancillary_budget.h"
#include "model/outline.h"

#include <cstddef>
#include <unordered_map>
#include <vector>

// The elements that describe images (spec §11.6 to §11.11): every FITSKeyword, ICCProfile, RGBWorkingSpace,
// DisplayFunction, ColorFilterArray and Resolution element of a header, read once. Tables and thumbnails are read by
// model/tables and model/images.

namespace openxisf::detail {

/// The value of each element that could be read, by the index of its element in the outline.
struct ancillary_elements
{
    std::unordered_map<std::size_t, fits_keyword> keywords{};
    /// The bytes of each ICC profile; empty when the data blocks are not loaded.
    std::unordered_map<std::size_t, std::vector<std::byte>> icc_profiles{};
    std::unordered_map<std::size_t, rgb_working_space> working_spaces{};
    std::unordered_map<std::size_t, display_function> display_functions{};
    std::unordered_map<std::size_t, color_filter_array> filters{};
    std::unordered_map<std::size_t, resolution> resolutions{};
};

/// Reads every such element of outline. The data block of an ICC profile is loaded from source within budget, unless
/// load_blocks is false. Every problem is recorded in log, and an element that cannot be read is unavailable:
/// - a FITSKeyword element without a name; an ICCProfile element without a data block, or whose block cannot be loaded
///   (load_block()); an RGBWorkingSpace, DisplayFunction, ColorFilterArray or Resolution element without one of its
///   mandatory attributes, with a value that cannot be read, or whose values break the rules of its section of the
///   specification are errors;
/// - a keyword name outside the grammar of FITS, a missing value or comment attribute of a keyword, a value or comment
///   that is not printable ASCII, a value of a COMMENT or HISTORY keyword, a profile that does not start with the
///   header of an ICC profile, luminance coefficients that differ from those that the chromaticities give by more than
///   luminance_tolerance, and chromaticities that give luminance coefficients outside [0, 1] are warnings, and the
///   element is read as written; a keyword name padded with trailing spaces is a warning too, and is read without
///   them.
[[nodiscard]] ancillary_elements read_ancillary(const unit_outline& outline, const std::vector<data_block>& blocks,
                                                const thread_safe_source& source, const limits& limits,
                                                ancillary_budget& budget, bool load_blocks, diagnostic_log& log);

} // namespace openxisf::detail
