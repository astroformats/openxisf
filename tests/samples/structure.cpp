// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Every sample written by PixInsight opens as written: the elements of its header, and its data blocks. The catalogue
// lists every sample.

#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>
#include <openxisf/types.h>

#include "container/data_block.h"
#include "container/file_layout.h"
#include "core/diagnostic_log.h"
#include "io/thread_safe_source.h"
#include "model/header.h"
#include "model/outline.h"
#include "model/unit.h"
#include "samples/sample_catalog.h"
#include "support/diagnostics.h"
#include "support/opened_unit.h"
#include "support/temp_directory.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using openxisf::test::sample;

class samples_structure : public testing::TestWithParam<sample>
{};

TEST_P(samples_structure, opens_without_diagnostics_but_for_the_creation_time_of_older_versions)
{
    // The XISF module of PixInsight 1.9.4 and 1.9.5 writes XISF:CreationTime as a String, and the reader reads the
    // TimePoint that it holds, with an info; that of 1.9.6 writes the TimePoint of spec §11.4.1.
    const openxisf::reader file(openxisf::test::sample_path(GetParam()));

    EXPECT_EQ(file.storage(), openxisf::unit_storage::monolithic);
    EXPECT_EQ(file.signature(), openxisf::signature_status::none);
    if (GetParam().creation_time_as_string) {
        EXPECT_TRUE(openxisf::test::single_diagnostic(file.diagnostics(), openxisf::severity::info,
                                                      openxisf::errc::reserved_property_type,
                                                      openxisf::test::creation_time_path));
    } else {
        EXPECT_TRUE(openxisf::test::no_diagnostics(file.diagnostics()));
    }
    EXPECT_EQ(file.metadata().at("XISF:CreationTime").value.type(), openxisf::property_type::time_point);
}

TEST_P(samples_structure, has_the_metadata_of_pixinsight)
{
    const openxisf::reader file(openxisf::test::sample_path(GetParam()));
    const openxisf::property_list& metadata = file.metadata();
    EXPECT_EQ(metadata.at("XISF:CreatorApplication").value, openxisf::property_value(GetParam().application));
    EXPECT_EQ(metadata.at("XISF:CreatorModule").value, openxisf::property_value(GetParam().module));
    EXPECT_EQ(metadata.at("XISF:CreatorOS").value, openxisf::property_value("Windows"));
    // The creation time is of the day the samples were written.
    EXPECT_EQ(metadata.at("XISF:CreationTime").value.get<openxisf::date_time>().year, 2026);
}

TEST_P(samples_structure, has_the_elements_that_pixinsight_wrote)
{
    const openxisf::detail::thread_safe_source source(
        std::make_unique<openxisf::file_source>(openxisf::test::sample_path(GetParam())));
    openxisf::detail::diagnostic_log log(true);
    const openxisf::detail::unit_header found = openxisf::detail::read_unit_header(source, {}, log);
    const openxisf::detail::parsed_header header = openxisf::detail::parse_header(found.text, found.offset, {}, log);
    const openxisf::detail::unit_outline outline = openxisf::detail::build_outline(header.root, {}, log);

    std::vector<std::string> paths;
    paths.reserve(outline.elements.size());
    for (std::size_t i = 0; i < outline.elements.size(); ++i) {
        paths.push_back(outline.path(i));
    }
    EXPECT_EQ(paths, GetParam().elements);
}

TEST_P(samples_structure, every_data_block_reads_passes_its_checksum_and_decompresses)
{
    const openxisf::detail::unit opened(
        std::make_unique<openxisf::file_source>(openxisf::test::sample_path(GetParam())), {.strict = true});

    EXPECT_EQ(openxisf::test::block_paths(opened), GetParam().blocks);
    for (const openxisf::detail::data_block& block : opened.blocks) {
        const openxisf::detail::block_descriptor& descriptor = openxisf::test::descriptor_at(opened, block.path);
        std::vector<std::byte> data;
        EXPECT_NO_THROW(data = openxisf::detail::read_block(opened.source, block, opened.limits)) << block.path;
        if (descriptor.compression) {
            EXPECT_EQ(data.size(), descriptor.compression->uncompressed_size) << block.path;
        }
    }
}

TEST_P(samples_structure, every_image_has_the_thumbnail_icc_profile_and_resolution_that_pixinsight_wrote)
{
    const openxisf::detail::unit opened(
        std::make_unique<openxisf::file_source>(openxisf::test::sample_path(GetParam())), {.strict = true});
    const std::vector<std::string>& blocks = GetParam().blocks;
    const auto has_block = [&blocks](const std::string& path) {
        return std::ranges::find(blocks, path) != blocks.end();
    };
    for (std::size_t i = 0; i < opened.images.infos.size(); ++i) {
        const openxisf::image_info& info = opened.images.infos[i];
        const std::string& path = opened.blocks[opened.images.blocks[i]].path;
        // PixInsight gives every image a resolution, 72 pixels per inch unless the image window has another.
        EXPECT_TRUE(info.resolution.has_value()) << path;

        EXPECT_EQ(info.thumbnail.has_value(), has_block(path + "/Thumbnail[1]")) << path;
        if (info.thumbnail) {
            const openxisf::thumbnail& small = *info.thumbnail;
            EXPECT_EQ(small.sample_format, openxisf::sample_format::uint8) << path;
            EXPECT_EQ(small.geometry.dimensions.size(), 2U) << path;
            EXPECT_EQ(small.pixels.size(), small.geometry.sample_count()) << path;
        }

        // An ICC profile starts with its size, and has the signature acsp at byte 36 (ICC.1, section 7.2).
        EXPECT_EQ(info.icc_profile.empty(), !has_block(path + "/ICCProfile[1]")) << path;
        if (info.icc_profile.size() >= 40) {
            const std::vector<std::byte>& profile = info.icc_profile;
            const auto byte_at = [&profile](std::size_t at) { return std::to_integer<std::uint32_t>(profile[at]); };
            EXPECT_EQ((byte_at(0) << 24U) | (byte_at(1) << 16U) | (byte_at(2) << 8U) | byte_at(3), profile.size());
            std::string signature;
            for (std::size_t j = 36; j < 40; ++j) {
                signature += static_cast<char>(profile[j]);
            }
            EXPECT_EQ(signature, "acsp") << path;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(pixinsight, samples_structure, testing::ValuesIn(openxisf::test::all_samples()),
                         [](const testing::TestParamInfo<sample>& parameter) {
                             return std::string(parameter.param.id);
                         });

TEST(samples_catalog, lists_every_unit_of_the_sample_directory)
{
    // The tests over all_samples() cover every unit of tests/data/pixinsight, and each one once.
    std::vector<std::string> files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(openxisf::test::path_of(OPENXISF_TEST_DATA_DIR) / "pixinsight")) {
        if (entry.path().extension() == ".xisf") {
            files.push_back(openxisf::test::utf8(entry.path().filename()));
        }
    }
    std::vector<std::string> listed;
    for (const sample& unit : openxisf::test::all_samples()) {
        listed.emplace_back(unit.file);
    }
    std::ranges::sort(files);
    std::ranges::sort(listed);
    EXPECT_EQ(listed, files);
}

} // namespace
