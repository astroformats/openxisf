// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/openxisf.h>

#include <gtest/gtest.h>

#include <string>

// The header comes from the build tree and the function from the linked library, so equality shows that
// the two belong together.
TEST(version, library_reports_the_version_of_the_header)
{
    EXPECT_EQ(openxisf::version(), OPENXISF_VERSION_STRING);
}

TEST(version, string_is_made_of_the_numeric_macros)
{
    const std::string expected = std::to_string(OPENXISF_VERSION_MAJOR) + '.' + std::to_string(OPENXISF_VERSION_MINOR) +
                                 '.' + std::to_string(OPENXISF_VERSION_PATCH);
    EXPECT_EQ(openxisf::version(), expected);
}

TEST(version, library_reports_the_numbers_of_the_header)
{
    const openxisf::version_info header{
        .major = OPENXISF_VERSION_MAJOR, .minor = OPENXISF_VERSION_MINOR, .patch = OPENXISF_VERSION_PATCH};
    EXPECT_EQ(openxisf::version_numbers(), header);
}
