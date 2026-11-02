// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/image.h>
#include <openxisf/property.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace openxisf::detail {

/// The model of a unit that a writer holds: what a save writes.
struct unit_contents
{
    property_list metadata{};
    property_list properties{};
    std::vector<table> tables{};
    std::vector<image_info> images{};
    /// The pixel data of each image, borrowed from the caller; empty for a reserved image, whose data are zeros.
    std::vector<std::optional<std::span<const std::byte>>> pixels{};
};

/// The metadata properties that the writer writes itself, from its options and from what it writes (spec §11.4).
inline constexpr std::array<std::string_view, 9> generated_metadata{
    "XISF:CreationTime",       "XISF:CreatorApplication", "XISF:CreatorModule",
    "XISF:CreatorOS",          "XISF:BlockAlignmentSize", "XISF:MaxInlineBlockSize",
    "XISF:ChecksumAlgorithms", "XISF:CompressionCodecs",  "XISF:CompressionLevel",
};

/// True for the identifiers of generated_metadata, which the metadata of a unit_contents cannot replace.
[[nodiscard]] inline bool is_generated_metadata(std::string_view id) noexcept
{
    return std::ranges::find(generated_metadata, id) != generated_metadata.end();
}

} // namespace openxisf::detail
