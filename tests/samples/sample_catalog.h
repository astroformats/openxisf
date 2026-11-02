// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/error.h>

#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openxisf::test {

/// A unit written by PixInsight, in tests/data/pixinsight, and what is known about it independently of any decoder.
struct sample
{
    /// The name of the sample in its group: A1, A2, ...
    std::string_view id{};
    std::string_view file{};
    /// What the unit holds.
    std::string_view content{};
    /// The PixInsight version that wrote it.
    std::string_view producer{};
    /// How it was made.
    std::string_view procedure{};
    /// Values set by hand, if any.
    std::string_view set_by_hand{};
    /// The paths of the XISF elements in its header, in document order.
    std::vector<std::string> elements{};
    /// The paths of the elements that serialize a data block, in document order.
    std::vector<std::string> blocks{};
    /// The value of XISF:CreatorApplication.
    std::string_view application = "PixInsight 1.9.4";
    /// The value of XISF:CreatorModule.
    std::string_view module = "XISF module version 1.1.3";
    /// Whether PixInsight wrote XISF:CreationTime as a String, as the XISF module of 1.9.4 and 1.9.5 does, rather
    /// than as the TimePoint of spec §11.4.1, which that of 1.9.6 writes.
    bool creation_time_as_string = true;
};

/// Every sample.
[[nodiscard]] const std::vector<sample>& all_samples();

/// The large samples, which are not versioned (tests/samples/large.cpp).
[[nodiscard]] const std::vector<sample>& large_samples();

/// The samples whose id starts with group.
[[nodiscard]] std::vector<sample> samples_of_group(char group);

/// The sample with the given id. Throws std::logic_error when there is none.
[[nodiscard]] const sample& sample_by_id(std::string_view id);

/// The path of the XISF:CreationTime property in every sample, which PixInsight before 1.9.6 writes as a String rather
/// than as the TimePoint of spec §11.4.1, so that each such sample has an info diagnostic about it.
inline constexpr std::string_view creation_time_path = "/xisf/Metadata[1]/Property[1]";

/// The diagnostics of a sample, without the info that every sample has about the type of its XISF:CreationTime.
[[nodiscard]] std::vector<diagnostic> unexpected_diagnostics(std::span<const diagnostic> diagnostics);

/// The UTF-8 path of the file of a sample.
[[nodiscard]] std::string sample_path(const sample& unit);

/// GoogleTest prints the parameter in messages, and CTest names the test after it: the id. The name is fixed by
/// GoogleTest.
// NOLINTNEXTLINE(readability-identifier-naming)
void PrintTo(const sample& unit, std::ostream* output);

} // namespace openxisf::test
