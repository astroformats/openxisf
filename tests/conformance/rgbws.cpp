// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Spec §8.5.4.1 and §11.8: RGBWorkingSpace elements, on constructed units.

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/reader.h>

#include "support/diagnostics.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"
#include "support/throws.h"

#include <gtest/gtest.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

using openxisf::errc;
using openxisf::reader;
using openxisf::rgb_working_space;
using openxisf::severity;
using openxisf::test::header_xml;
using openxisf::test::image_xml;
using openxisf::test::no_diagnostics;
using openxisf::test::single_diagnostic;

// The chromaticities and luminance coefficients of the examples of spec §11.8.2.
constexpr std::string_view adobe = R"(x="0.648431:0.230154:0.155886" y="0.330856:0.701572:0.066044" )"
                                   R"(Y="0.311114:0.625662:0.063224")";
constexpr std::string_view srgb = R"(x="0.648431:0.321152:0.155886" y="0.330856:0.597871:0.066044" )"
                                  R"(Y="0.222491:0.716888:0.060621")";

reader open_space(std::string_view attributes, openxisf::read_options options = {})
{
    return openxisf::test::open_header(header_xml(image_xml({}, "<RGBWorkingSpace " + std::string(attributes) + "/>")),
                                       std::move(options));
}

// The only diagnostic is an error with code about the RGBWorkingSpace element, and the image has no working space.
testing::AssertionResult unavailable(const reader& file, errc code = errc::invalid_rgb_working_space)
{
    testing::AssertionResult found =
        single_diagnostic(file.diagnostics(), severity::error, code, "/xisf/Image[1]/RGBWorkingSpace[1]");
    if (found && file.image(0).rgb_working_space) {
        return testing::AssertionFailure() << "the image has a working space";
    }
    return found;
}

TEST(conformance_rgbws, the_examples_of_the_specification_are_read)
{
    const reader adobe_file = open_space(std::string(adobe) + R"x( gamma="2.2" name="Adobe RGB (1998)")x");
    EXPECT_TRUE(no_diagnostics(adobe_file.diagnostics()));
    EXPECT_EQ(adobe_file.image(0).rgb_working_space, (rgb_working_space{.gamma = 2.2,
                                                                        .x = {0.648431, 0.230154, 0.155886},
                                                                        .y = {0.330856, 0.701572, 0.066044},
                                                                        .luminance = {0.311114, 0.625662, 0.063224},
                                                                        .name = "Adobe RGB (1998)"}));

    const reader srgb_file = open_space(std::string(srgb) + R"( gamma="sRGB" name="sRGB IEC61966-2.1")");
    EXPECT_TRUE(no_diagnostics(srgb_file.diagnostics()));
    EXPECT_EQ(srgb_file.image(0).rgb_working_space, (rgb_working_space{.name = "sRGB IEC61966-2.1"}));

    const reader linear = open_space(std::string(srgb) + R"( gamma="1" name="Linear sRGB")");
    EXPECT_TRUE(no_diagnostics(linear.diagnostics()));
    EXPECT_EQ(linear.image(0).rgb_working_space, (rgb_working_space{.gamma = 1.0, .name = "Linear sRGB"}));
}

TEST(conformance_rgbws, an_image_without_a_working_space_has_srgb)
{
    // Spec §8.5.4.1: sRGB relative to D50 is the default, and is what a default-constructed working space holds.
    const reader file = openxisf::test::open_header(header_xml(image_xml()));
    EXPECT_FALSE(file.image(0).rgb_working_space.has_value());
    const rgb_working_space standard;
    EXPECT_EQ(standard.gamma, std::nullopt);
    EXPECT_EQ(standard.x, (std::array<double, 3>{0.648431, 0.321152, 0.155886}));
    EXPECT_EQ(standard.y, (std::array<double, 3>{0.330856, 0.597871, 0.066044}));
    EXPECT_EQ(standard.luminance, (std::array<double, 3>{0.222491, 0.716888, 0.060621}));
}

TEST(conformance_rgbws, the_srgb_gamma_is_named_in_any_case)
{
    for (const std::string_view gamma : {"sRGB", "srgb", "SRGB"}) {
        const reader file = open_space(std::string(srgb) + R"( gamma=")" + std::string(gamma) + '"');
        EXPECT_TRUE(no_diagnostics(file.diagnostics())) << gamma;
        EXPECT_EQ(openxisf::test::value_of(file.image(0).rgb_working_space).gamma, std::nullopt) << gamma;
    }
}

TEST(conformance_rgbws, a_working_space_without_one_of_its_attributes_is_unavailable)
{
    EXPECT_TRUE(unavailable(open_space(srgb)));
    EXPECT_TRUE(
        unavailable(open_space(R"(gamma="2.2" y="0.330856:0.597871:0.066044" Y="0.222491:0.716888:0.060621")")));
    EXPECT_TRUE(
        unavailable(open_space(R"(gamma="2.2" x="0.648431:0.321152:0.155886" Y="0.222491:0.716888:0.060621")")));
    EXPECT_TRUE(
        unavailable(open_space(R"(gamma="2.2" x="0.648431:0.321152:0.155886" y="0.330856:0.597871:0.066044")")));
    // Attribute names are case-sensitive: y is not Y.
    EXPECT_TRUE(unavailable(open_space(R"(gamma="2.2" x="0.6:0.3:0.15" y="0.33:0.6:0.07" YY="0.2:0.7:0.1")")));
}

TEST(conformance_rgbws, a_working_space_whose_values_cannot_be_read_is_unavailable)
{
    // The gammas and lists of values refused are in unit/ancillary_attributes.cpp.
    EXPECT_TRUE(unavailable(open_space(R"(gamma="2.2" x="0.648431:0.321152" y="0.330856:0.597871:0.066044" )"
                                       R"(Y="0.222491:0.716888:0.060621")")));
}

TEST(conformance_rgbws, a_working_space_has_its_values_in_the_unit_range)
{
    // Spec §8.5.4.1: chromaticity coordinates and luminance coefficients are normalized to [0, 1].
    EXPECT_TRUE(unavailable(open_space(R"(gamma="2.2" x="1.648431:0.321152:0.155886" y="0.330856:0.597871:0.066044" )"
                                       R"(Y="0.222491:0.716888:0.060621")")));
}

TEST(conformance_rgbws, chromaticities_that_define_no_working_space_make_it_unavailable)
{
    // Spec §8.5.4.1: a singular system, here three equal primaries, defines no RGB working space.
    EXPECT_TRUE(unavailable(open_space(R"(gamma="2.2" x="0.3:0.3:0.3" y="0.3:0.3:0.3" Y="0.3:0.3:0.4")")));
}

TEST(conformance_rgbws, luminance_coefficients_that_the_chromaticities_do_not_give_are_kept_with_a_warning)
{
    // Spec §11.8.1: decoders may verify them. The coefficients of sRGB relative to D65 are not those of D50.
    const reader file = open_space(R"(gamma="sRGB" x="0.648431:0.321152:0.155886" y="0.330856:0.597871:0.066044" )"
                                   R"(Y="0.2126:0.7152:0.0722")");
    EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_rgb_working_space,
                                  "/xisf/Image[1]/RGBWorkingSpace[1]"));
    EXPECT_EQ(file.diagnostics()[0].context.attribute, "Y");
    EXPECT_EQ(openxisf::test::value_of(file.image(0).rgb_working_space).luminance,
              (std::array<double, 3>{0.2126, 0.7152, 0.0722}));

    // Written with four decimals, they agree within the tolerance.
    const reader rounded = open_space(R"(gamma="sRGB" x="0.6484:0.3212:0.1559" y="0.3309:0.5979:0.0660" )"
                                      R"(Y="0.2225:0.7169:0.0606")");
    EXPECT_TRUE(no_diagnostics(rounded.diagnostics()));
}

TEST(conformance_rgbws, chromaticities_that_give_luminance_coefficients_outside_the_unit_range_are_kept_with_a_warning)
{
    // Spec §8.5.4.1: a white just outside the triangle of the primaries gives a coefficient below 0, which the writer
    // refuses to write, though the coefficients given agree with it within the tolerance.
    const reader file =
        open_space(R"(gamma="2.2" x="0.64:0.30:0.0" y="0.33:0.60:0.39198" Y="0.4972:0:0.5029")", {.strict = true});
    ASSERT_TRUE(single_diagnostic(file.diagnostics(), severity::warning, errc::invalid_rgb_working_space,
                                  "/xisf/Image[1]/RGBWorkingSpace[1]"));
    EXPECT_EQ(file.diagnostics()[0].context.attribute, "Y");
    EXPECT_EQ(openxisf::test::value_of(file.image(0).rgb_working_space).luminance,
              (std::array<double, 3>{0.4972, 0.0, 0.5029}));
}

TEST(conformance_rgbws, strict_reading_refuses_an_invalid_working_space)
{
    EXPECT_TRUE(openxisf::test::throws<openxisf::invalid_data_error>(errc::invalid_rgb_working_space,
                                                                     [] { (void)open_space(srgb, {.strict = true}); }));
}

} // namespace
