// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/limits.h>

#include <gtest/gtest.h>

#include <cstdint>

namespace {

constexpr std::uint64_t mib = std::uint64_t{1} << 20;
constexpr std::uint64_t gib = std::uint64_t{1} << 30;

// The defaults are documented, and applications rely on them.
TEST(limits, defaults)
{
    const openxisf::limits defaults;

    EXPECT_EQ(defaults.max_header_size, 64 * mib);
    EXPECT_EQ(defaults.max_xml_depth, 64U);
    EXPECT_EQ(defaults.max_xml_elements, 1'000'000U);
    EXPECT_EQ(defaults.max_allocation, 16 * gib);
    EXPECT_EQ(defaults.max_ancillary_data, 256 * mib);
    EXPECT_EQ(defaults.max_index_nodes, 65'536U);
    EXPECT_EQ(defaults.max_external_files, 128U);
    EXPECT_EQ(defaults.max_zstd_window, 128 * mib);
    EXPECT_EQ(defaults.max_diagnostics, 10'000U);
    EXPECT_EQ(defaults.max_index_elements, 16'777'216U);
    EXPECT_EQ(defaults.max_model_objects, 1'000'000U);
    EXPECT_EQ(defaults.max_zstd_expansion, 32768U);
}

} // namespace
