// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "core/diagnostic_log.h"

#include <openxisf/error.h>

#include "support/throws.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::severity;
using openxisf::detail::diagnostic_log;
using openxisf::detail::throw_unit_error;
using openxisf::test::throws;

TEST(diagnostic_log, records_every_diagnostic_in_order)
{
    diagnostic_log log(false);
    log.info(errc::unknown_element, "an info", {.element = "/xisf/x:Extension[1]"});
    log.warning(errc::invalid_uid, "a warning");
    log.error(errc::duplicate_uid, "an error", {.attribute = "uid"});

    const std::vector<openxisf::diagnostic> diagnostics = log.release();
    ASSERT_EQ(diagnostics.size(), 3U);
    EXPECT_EQ(diagnostics[0].severity, severity::info);
    EXPECT_EQ(diagnostics[0].code, errc::unknown_element);
    EXPECT_EQ(diagnostics[0].message, "an info");
    EXPECT_EQ(diagnostics[0].context.element, "/xisf/x:Extension[1]");
    EXPECT_EQ(diagnostics[1].severity, severity::warning);
    EXPECT_EQ(diagnostics[2].severity, severity::error);
    EXPECT_EQ(diagnostics[2].context.attribute, "uid");
    EXPECT_TRUE(log.entries().empty());
}

// A header can hold a million problems, each of which would cost a diagnostic.
TEST(diagnostic_log, lists_ten_thousand_diagnostics_the_last_of_which_counts_those_that_follow)
{
    constexpr std::size_t listed = 10'000;
    diagnostic_log log(false);
    for (std::size_t i = 1; i < listed; ++i) {
        log.info(errc::unknown_element, "an info", {.element = "/xisf/x:Extension[" + std::to_string(i) + "]"});
    }
    log.warning(errc::invalid_uid, "the last diagnostic", {.element = "/xisf/Image[1]", .attribute = "uid"});
    ASSERT_EQ(log.entries().size(), listed);
    EXPECT_EQ(log.entries().back().message, "the last diagnostic");

    // The last one stands for itself and those after it: their number, the severity of the most serious, and the code
    // of the first of those.
    log.info(errc::unknown_element, "an info");
    log.error(errc::duplicate_uid, "an error", {.element = "/xisf/Image[2]"});
    log.warning(errc::invalid_uid, "a warning");
    log.error(errc::dangling_reference, "another error");
    const std::vector<openxisf::diagnostic> diagnostics = log.release();
    ASSERT_EQ(diagnostics.size(), listed);
    EXPECT_EQ(diagnostics[listed - 2].context.element, "/xisf/x:Extension[9999]");
    const openxisf::diagnostic& last = diagnostics.back();
    EXPECT_EQ(last.severity, severity::error);
    EXPECT_EQ(last.code, errc::duplicate_uid);
    EXPECT_EQ(last.message, "5 more problems were found, which are not listed");
    EXPECT_EQ(last.context, openxisf::error_context{});
}

TEST(diagnostic_log, an_error_throws_in_strict_mode)
{
    diagnostic_log log(true);
    log.info(errc::unknown_element, "an info");
    log.warning(errc::invalid_uid, "a warning");
    try {
        log.error(errc::duplicate_uid, "an error", {.element = "/xisf/Image[2]"});
        ADD_FAILURE() << "no exception";
    } catch (const openxisf::invalid_data_error& failure) {
        EXPECT_EQ(failure.code(), errc::duplicate_uid);
        EXPECT_STREQ(failure.what(), "an error (element /xisf/Image[2])");
    }
    EXPECT_EQ(log.entries().size(), 2U);
}

TEST(diagnostic_log, the_exception_of_an_error_says_what_kind_of_failure_it_is)
{
    for (const errc code : {errc::checksum_mismatch, errc::corrupt_compressed_data}) {
        EXPECT_TRUE(throws<openxisf::integrity_error>(code, [code] { throw_unit_error(code, "message", {}); }));
    }
    for (const errc code :
         {errc::unsupported_version, errc::unsupported_location, errc::unsupported_checksum,
          errc::unsupported_compression, errc::unsupported_property_type, errc::unsupported_sample_format,
          errc::unsupported_color_space, errc::codec_failure, errc::location_not_allowed, errc::hash_failure}) {
        EXPECT_TRUE(throws<openxisf::unsupported_error>(code, [code] { throw_unit_error(code, "message", {}); }));
    }
    for (const errc code : {errc::header_too_large, errc::xml_too_deep, errc::too_many_xml_elements,
                            errc::allocation_too_large, errc::zstd_window_too_large, errc::ancillary_data_too_large,
                            errc::too_many_index_nodes, errc::too_many_external_files}) {
        EXPECT_TRUE(throws<openxisf::limit_error>(code, [code] { throw_unit_error(code, "message", {}); }));
    }
    // The external files of a distributed unit that cannot be opened or read.
    for (const errc code : {errc::open_failed, errc::not_a_regular_file, errc::read_failed, errc::end_of_data}) {
        EXPECT_TRUE(throws<openxisf::io_error>(code, [code] { throw_unit_error(code, "message", {}); }));
    }
    for (const errc code : {errc::duplicate_uid, errc::invalid_location, errc::invalid_checksum}) {
        EXPECT_TRUE(throws<openxisf::invalid_data_error>(code, [code] { throw_unit_error(code, "message", {}); }));
    }

    bool thrown = false;
    try {
        throw_unit_error(errc::checksum_mismatch, "message", {.element = "/xisf/Image[1]", .offset = 4096});
    } catch (const openxisf::error& failure) {
        thrown = true;
        EXPECT_STREQ(failure.what(), "message (element /xisf/Image[1], offset 4096)");
    }
    EXPECT_TRUE(thrown);
}

} // namespace
