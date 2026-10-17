// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// Distributed units (spec §9.1.2, §9.3, §9.4, §10.2, §10.3): header files whose data blocks are in data blocks files or
// in other files, which a resolver opens, and the rules of block indexes as a unit uses them; and the distributed
// units that the writer writes. The walk over a block index is tested in unit/blocks_file.cpp, the confinement of
// paths in unit/file_resolver.cpp, and the round trips of distributed units with every option of the writer in
// conformance/writer.cpp.

#include <openxisf/error.h>
#include <openxisf/image.h>
#include <openxisf/io.h>
#include <openxisf/reader.h>
#include <openxisf/writer.h>

#include "codec/compressed_block.h"
#include "container/block_attributes.h"
#include "core/data_encoding.h"
#include "crypto/hash.h"
#include "model/unit.h"
#include "support/bytes.h"
#include "support/diagnostics.h"
#include "support/faulty_io.h"
#include "support/files.h"
#include "support/fixture_builder.h"
#include "support/opened_unit.h"
#include "support/temp_directory.h"
#include "support/throws.h"
#include "support/virtual_source.h"
#include "support/written_unit.h"
#include "xml/xml_document.h"

#include <gtest/gtest.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::external_reference;
using openxisf::location_form;
using openxisf::read_options;
using openxisf::reader;
using openxisf::severity;
using openxisf::detail::unit;
using openxisf::test::block_at;
using openxisf::test::block_data;
using openxisf::test::blocks_file;
using openxisf::test::blocks_file_of;
using openxisf::test::bytes;
using openxisf::test::header_xml;
using openxisf::test::index_entry;
using openxisf::test::index_node;
using openxisf::test::memory_resolver;
using openxisf::test::no_diagnostics;
using openxisf::test::single_diagnostic;
using openxisf::test::throws;

using file_map = std::map<std::string, std::vector<std::byte>, std::less<>>;

// A ByteArray property of length bytes in the external block at location.
std::string bytes_property(std::string_view id, std::size_t length, std::string_view location,
                           std::string_view attributes = {})
{
    return R"(<Property id=")" + std::string(id) + R"(" type="ByteArray" length=")" + std::to_string(length) +
           R"(" location=")" + std::string(location) + "\" " + std::string(attributes) + "/>";
}

// A gray 8-bit image of one row of width pixels in the external block at location.
std::string row_image(std::size_t width, std::string_view location, std::string_view attributes = {})
{
    return R"(<Image geometry=")" + std::to_string(width) +
           R"(:1:1" sampleFormat="UInt8" colorSpace="Gray" location=")" + std::string(location) + "\" " +
           std::string(attributes) + "/>";
}

// The header file with body, opened from memory with a resolver of files.
unit open_header_file(std::string_view body, file_map files, read_options options = {})
{
    options.resolver = memory_resolver(std::move(files));
    return openxisf::test::open_internal(bytes(header_xml(body)), std::move(options));
}

reader open_header_reader(std::string_view body, file_map files, read_options options = {})
{
    options.resolver = memory_resolver(std::move(files));
    return openxisf::test::open_unit(bytes(header_xml(body)), std::move(options));
}

// The block at path is unavailable because of the only diagnostic of the unit, an error with code.
testing::AssertionResult unavailable(const unit& opened, std::string_view path, errc code)
{
    testing::AssertionResult found = single_diagnostic(opened.diagnostics, severity::error, code, path);
    if (!found) {
        return found;
    }
    const openxisf::detail::data_block& block = block_at(opened, path);
    if (block.descriptor || !block.problem || block.problem->code != code) {
        return testing::AssertionFailure() << "the block at " << path << " is not unavailable with that code";
    }
    return testing::AssertionSuccess();
}

std::vector<std::byte> abc()
{
    return bytes("abc");
}

// ---------------------------------------------------------------------------------------------------------------------
// Where external blocks are (spec §9.4, §10.3)

TEST(conformance_distributed, a_block_in_a_data_blocks_file)
{
    const reader file =
        open_header_reader(bytes_property("Test:Bytes", 3, "path(@header_dir/unit.xisb):0x4d373e33756e480f") +
                               row_image(5, "path(@header_dir/unit.xisb):12"),
                           {{"unit.xisb", blocks_file_of({abc(), bytes("12345")}, {0x4d373e33756e480f, 12})}});
    EXPECT_EQ(file.storage(), openxisf::unit_storage::distributed);
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    const openxisf::property* found = file.properties().find("Test:Bytes");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(openxisf::test::text(std::as_bytes(found->value.elements<std::uint8_t>())), "abc");
    EXPECT_EQ(file.read_pixels(0), bytes("12345"));
}

TEST(conformance_distributed, a_block_that_is_a_whole_file)
{
    // Spec §10.3: without an index-id, the block is the whole file, whatever it holds.
    const unit opened = open_header_file(row_image(3, "path(@header_dir/data(1).bin)") +
                                             row_image(32, "url(https://example.com/a.xisb)"),
                                         {{"data(1).bin", abc()}, {"https://example.com/a.xisb", blocks_file({{}})}});
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    EXPECT_EQ(block_data(opened, "/xisf/Image[1]"), abc());
    EXPECT_EQ(block_data(opened, "/xisf/Image[2]"), blocks_file({{}}));
}

TEST(conformance_distributed, the_resolver_gets_the_form_and_the_location_of_each_file_once)
{
    auto references = std::make_shared<std::vector<external_reference>>();
    const read_options options{.resolver = [references](const external_reference& reference) {
        references->push_back(reference);
        return std::make_unique<openxisf::memory_source>(blocks_file_of({abc(), abc()}, {1, 2}));
    }};
    const unit opened = openxisf::test::open_internal(
        bytes(header_xml(row_image(3, "path(@header_dir/a/b.xisb):1") + row_image(3, "url(ftp://x/(1).xisb):2") +
                         row_image(3, "path(@header_dir/a/b.xisb):2") + row_image(3, "path(/data/b.xisb):1") +
                         row_image(3, "path(/data/b.xisb):0x2"))),
        options);
    EXPECT_TRUE(no_diagnostics(opened.diagnostics));
    ASSERT_EQ(references->size(), 3U);
    EXPECT_EQ((*references)[0].form, location_form::relative_path);
    EXPECT_EQ((*references)[0].location, "a/b.xisb");
    EXPECT_EQ((*references)[1].form, location_form::url);
    EXPECT_EQ((*references)[1].location, "ftp://x/(1).xisb");
    EXPECT_EQ((*references)[2].form, location_form::absolute_path);
    EXPECT_EQ((*references)[2].location, "/data/b.xisb");
    // The blocks of one file share its source.
    EXPECT_EQ(openxisf::test::descriptor_at(opened, "/xisf/Image[1]").external,
              openxisf::test::descriptor_at(opened, "/xisf/Image[3]").external);
}

TEST(conformance_distributed, the_index_of_a_file_is_read_with_the_unit)
{
    // header_only leaves the data unloaded, but locates the blocks, so that their sizes are checked.
    const unit opened = open_header_file(row_image(4, "path(@header_dir/unit.xisb):1"),
                                         {{"unit.xisb", blocks_file_of({abc()}, {1})}}, {.header_only = true});
    EXPECT_TRUE(
        single_diagnostic(opened.diagnostics, severity::error, errc::pixel_data_size_mismatch, "/xisf/Image[1]"));
}

TEST(conformance_distributed, ancillary_data_load_from_the_files_again)
{
    auto opened = std::make_shared<std::vector<std::string>>();
    reader file = openxisf::test::open_unit(
        bytes(header_xml(bytes_property("Test:Bytes", 3, "path(@header_dir/unit.xisb):1"))),
        {.header_only = true, .resolver = memory_resolver({{"unit.xisb", blocks_file_of({abc()}, {1})}}, opened)});
    EXPECT_EQ(file.properties().find("Test:Bytes"), nullptr);
    file.load_ancillary_data();
    EXPECT_NE(file.properties().find("Test:Bytes"), nullptr);
    EXPECT_EQ(*opened, (std::vector<std::string>{"unit.xisb", "unit.xisb"}));
}

TEST(conformance_distributed, an_external_block_is_verified_and_decompressed_when_it_is_read)
{
    const std::vector<std::byte> data = openxisf::test::pattern(1000);
    const openxisf::detail::compressed_block compressed =
        openxisf::detail::compress_block(data, {.codec = openxisf::detail::compression_codec::zlib});
    const std::string digest = openxisf::detail::encode_hex(
        openxisf::detail::compute_digest(openxisf::detail::hash_algorithm::sha1, compressed.data));
    const std::string compression = R"(compression="zlib:1000")";
    const reader file = open_header_reader(
        row_image(1000, "path(@header_dir/unit.xisb):1", compression + R"( checksum="sha1:)" + digest + "\"") +
            row_image(1000, "path(@header_dir/unit.xisb):1",
                      compression + R"( checksum="sha1:)" + std::string(digest.size(), '0') + "\""),
        {{"unit.xisb",
          blocks_file(
              {{.elements =
                    {{.id = 1, .position = 100, .length = compressed.data.size(), .uncompressed_length = 1000}}}},
              {{.position = 100, .data = compressed.data}})}});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.read_pixels(0), data);
    EXPECT_TRUE(throws<openxisf::integrity_error>(errc::checksum_mismatch, [&file] { (void)file.read_pixels(1); }));
}

// ---------------------------------------------------------------------------------------------------------------------
// Files that cannot be opened

TEST(conformance_distributed, a_file_that_the_resolver_does_not_open_makes_its_blocks_unavailable)
{
    const unit opened = open_header_file(row_image(3, "path(@header_dir/missing.xisb):1"), {});
    EXPECT_TRUE(unavailable(opened, "/xisf/Image[1]", errc::unsupported_location));
}

TEST(conformance_distributed, the_io_and_unsupported_errors_of_a_resolver_make_blocks_unavailable)
{
    const auto open_with = [](openxisf::external_resolver resolver) {
        return openxisf::test::open_internal(
            bytes(header_xml(row_image(3, "path(@header_dir/a.xisb):1") + row_image(3, "path(@header_dir/a.xisb):2"))),
            {.resolver = std::move(resolver)});
    };
    const unit failed = open_with([](const external_reference&) -> std::unique_ptr<openxisf::input_source> {
        throw openxisf::io_error(errc::open_failed, "cannot open it");
    });
    ASSERT_EQ(failed.diagnostics.size(), 2U);
    for (const openxisf::diagnostic& entry : failed.diagnostics) {
        EXPECT_EQ(entry.code, errc::open_failed);
    }
    const unit refused = open_with([](const external_reference&) -> std::unique_ptr<openxisf::input_source> {
        throw openxisf::unsupported_error(errc::location_not_allowed, "not here");
    });
    ASSERT_EQ(refused.diagnostics.size(), 2U);
    EXPECT_EQ(refused.diagnostics[1].code, errc::location_not_allowed);
    // The source fails as soon as it is used.
    const unit broken = open_with([](const external_reference&) {
        return std::make_unique<openxisf::callback_source>(100, [](std::uint64_t, std::span<std::byte>) {
            throw openxisf::io_error(errc::read_failed, "unreadable");
        });
    });
    ASSERT_EQ(broken.diagnostics.size(), 2U);
    EXPECT_EQ(broken.diagnostics[0].code, errc::read_failed);
}

TEST(conformance_distributed, other_exceptions_of_a_resolver_pass_through)
{
    const read_options options{.resolver = [](const external_reference&) -> std::unique_ptr<openxisf::input_source> {
        throw openxisf::test::injected_fault();
    }};
    EXPECT_THROW((void)openxisf::test::open_unit(bytes(header_xml(row_image(3, "path(@header_dir/a.xisb)"))), options),
                 openxisf::test::injected_fault);
}

TEST(conformance_distributed, a_unit_opened_from_a_source_has_no_resolver_by_default)
{
    const unit opened = openxisf::test::open_internal(bytes(header_xml(row_image(3, "path(@header_dir/a.xisb)"))));
    EXPECT_TRUE(unavailable(opened, "/xisf/Image[1]", errc::unsupported_location));
}

TEST(conformance_distributed, strict_reading_fails_with_the_error_of_the_file)
{
    const auto open_strictly = [](openxisf::external_resolver resolver) {
        (void)openxisf::test::open_unit(bytes(header_xml(row_image(3, "path(@header_dir/a.xisb)"))),
                                        {.strict = true, .resolver = std::move(resolver)});
    };
    EXPECT_TRUE(throws<openxisf::io_error>(errc::open_failed, [&] {
        open_strictly([](const external_reference&) -> std::unique_ptr<openxisf::input_source> {
            throw openxisf::io_error(errc::open_failed, "cannot open it");
        });
    }));
    EXPECT_TRUE(throws<openxisf::unsupported_error>(errc::unsupported_location, [&] { open_strictly({}); }));
}

// Success when function throws io_error with code and the system error.
testing::AssertionResult throws_io_error(errc code, std::error_code system, const std::function<void()>& function)
{
    try {
        function();
    } catch (const openxisf::io_error& failure) {
        if (failure.code() == code && failure.system_code() == system) {
            return testing::AssertionSuccess();
        }
        return testing::AssertionFailure()
               << "threw code " << static_cast<int>(failure.code()) << " with the system error "
               << failure.system_code().value() << ": " << failure.what();
    } catch (const std::exception& other) {
        return testing::AssertionFailure() << "threw another exception: " << other.what();
    }
    return testing::AssertionFailure() << "threw nothing";
}

TEST(conformance_distributed, the_errors_of_a_resolver_keep_their_class_and_system_error)
{
    // A source that cannot seek is an io_error whatever its code: a strict open throws it, and so does a read of the
    // image in its file, with the error of the system, whether the resolver or the source of the data blocks file
    // fails. An unsupported_error keeps its class with a code of invalid data too.
    const std::error_code denied = std::make_error_code(std::errc::permission_denied);
    const std::string header = header_xml(row_image(3, "path(@header_dir/a.xisb):1"));
    const auto open = [&header](openxisf::external_resolver resolver, bool strict) {
        return openxisf::test::open_unit(bytes(header), {.strict = strict, .resolver = std::move(resolver)});
    };
    const std::vector<openxisf::external_resolver> pipes{
        [denied](const external_reference&) -> std::unique_ptr<openxisf::input_source> {
            throw openxisf::io_error(errc::not_seekable, "a pipe", denied);
        },
        [denied](const external_reference&) -> std::unique_ptr<openxisf::input_source> {
            return std::make_unique<openxisf::callback_source>(100, [denied](std::uint64_t, std::span<std::byte>) {
                throw openxisf::io_error(errc::not_seekable, "a pipe", denied);
            });
        }};
    for (const openxisf::external_resolver& resolver : pipes) {
        const reader file = open(resolver, false);
        EXPECT_TRUE(single_diagnostic(file.diagnostics(), severity::error, errc::not_seekable, "/xisf/Image[1]"));
        EXPECT_TRUE(throws_io_error(errc::not_seekable, denied, [&file] { (void)file.read_pixels(0); }));
        EXPECT_TRUE(throws_io_error(errc::not_seekable, denied, [&] { (void)open(resolver, true); }));
    }
    const openxisf::external_resolver refusing =
        [](const external_reference&) -> std::unique_ptr<openxisf::input_source> {
        throw openxisf::unsupported_error(errc::invalid_location, "not here");
    };
    const reader refused = open(refusing, false);
    EXPECT_TRUE(
        throws<openxisf::unsupported_error>(errc::invalid_location, [&refused] { (void)refused.read_pixels(0); }));
    EXPECT_TRUE(throws<openxisf::unsupported_error>(errc::invalid_location, [&] { (void)open(refusing, true); }));
}

TEST(conformance_distributed, an_io_error_of_a_file_while_ancillary_data_load_makes_their_blocks_unavailable)
{
    // The index of the data blocks file is read, then every read of a block fails: the pixels of a thumbnail, an ICC
    // profile and the value of a property, which are loaded with the unit, are unavailable, each with an error of the
    // code of the source, as when the index cannot be read. A strict open throws the io_error, with its system error,
    // and so does a load of the ancillary data that a header-only open left. An io_error of the unit's own source
    // fails the open (reader.a_failure_of_the_source_while_loading_the_ancillary_data_leaves_the_reader_unchanged).
    const std::error_code denied = std::make_error_code(std::errc::permission_denied);
    const auto blocks = std::make_shared<const std::vector<std::byte>>(
        blocks_file_of({bytes("1234"), bytes("abcd"), bytes("efgh"), bytes("ijkl")}, {1, 2, 3, 4}));
    // The four blocks of 4 bytes end the file.
    const std::uint64_t first_block = blocks->size() - 16;
    const openxisf::external_resolver resolver = [blocks, first_block, denied](const external_reference&) {
        return std::make_unique<openxisf::callback_source>(
            blocks->size(), [blocks, first_block, denied](std::uint64_t offset, std::span<std::byte> destination) {
                if (offset >= first_block) {
                    throw openxisf::io_error(errc::read_failed, "the network went away", denied);
                }
                std::ranges::copy(std::span(*blocks).subspan(offset, destination.size()), destination.begin());
            });
    };
    const std::string header = header_xml(
        R"(<Image geometry="4:1:1" sampleFormat="UInt8" colorSpace="Gray" location="path(@header_dir/a.xisb):1">)"
        R"(<Thumbnail geometry="2:2:1" sampleFormat="UInt8" colorSpace="Gray" location="path(@header_dir/a.xisb):2"/>)"
        R"(<ICCProfile location="path(@header_dir/a.xisb):3"/></Image>)" +
        bytes_property("Test:Bytes", 4, "path(@header_dir/a.xisb):4"));
    const auto open = [&](bool strict, bool header_only) {
        return openxisf::test::open_unit(bytes(header),
                                         {.strict = strict, .header_only = header_only, .resolver = resolver});
    };
    const auto expect_unavailable = [](const reader& file) {
        std::vector<std::string> paths;
        for (const openxisf::diagnostic& entry : file.diagnostics()) {
            EXPECT_EQ(entry.severity, severity::error) << entry.context.element;
            EXPECT_EQ(entry.code, errc::read_failed) << entry.context.element;
            paths.push_back(entry.context.element);
        }
        std::ranges::sort(paths);
        EXPECT_EQ(paths, (std::vector<std::string>{"/xisf/Image[1]/ICCProfile[1]", "/xisf/Image[1]/Thumbnail[1]",
                                                   "/xisf/Property[1]"}));
        EXPECT_FALSE(file.image(0).thumbnail.has_value());
        EXPECT_TRUE(file.image(0).icc_profile.empty());
        EXPECT_EQ(file.properties().find("Test:Bytes"), nullptr);
    };

    expect_unavailable(open(false, false));
    reader deferred = open(false, true);
    EXPECT_TRUE(no_diagnostics(deferred.diagnostics()));
    deferred.load_ancillary_data();
    expect_unavailable(deferred);

    EXPECT_TRUE(throws_io_error(errc::read_failed, denied, [&] { (void)open(true, false); }));
    reader strict = open(true, true);
    EXPECT_TRUE(throws_io_error(errc::read_failed, denied, [&strict] { strict.load_ancillary_data(); }));
    EXPECT_FALSE(strict.ancillary_data_loaded());
}

TEST(conformance_distributed, the_number_of_external_files_is_limited)
{
    const std::string body = row_image(3, "path(@header_dir/a.bin)") + row_image(3, "path(@header_dir/b.bin)") +
                             row_image(3, "path(@header_dir/a.bin)") + row_image(3, "path(@header_dir/c.bin)");
    const file_map files{{"a.bin", abc()}, {"b.bin", abc()}, {"c.bin", abc()}};
    const unit opened = open_header_file(body, files, {.limits = {.max_external_files = 2}});
    EXPECT_TRUE(unavailable(opened, "/xisf/Image[4]", errc::too_many_external_files));
    EXPECT_TRUE(no_diagnostics(open_header_file(body, files, {.limits = {.max_external_files = 3}}).diagnostics));
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::too_many_external_files, [&] {
        (void)open_header_file(body, files, {.strict = true, .limits = {.max_external_files = 1}});
    }));
}

// ---------------------------------------------------------------------------------------------------------------------
// Blocks in data blocks files (spec §9.4)

TEST(conformance_distributed, an_index_id_that_no_element_has)
{
    const unit opened =
        open_header_file(row_image(3, "path(@header_dir/unit.xisb):2"), {{"unit.xisb", blocks_file_of({abc()}, {1})}});
    EXPECT_TRUE(unavailable(opened, "/xisf/Image[1]", errc::index_id_not_found));
}

TEST(conformance_distributed, a_free_element_points_to_no_block)
{
    const unit opened = open_header_file(
        row_image(3, "path(@header_dir/unit.xisb):7") + row_image(3, "path(@header_dir/unit.xisb):8"),
        {{"unit.xisb", blocks_file({{.elements = {{.id = 7}, {.id = 8, .position = 200, .length = 3}}}},
                                   {{.position = 200, .data = abc()}})}});
    ASSERT_EQ(opened.diagnostics.size(), 1U);
    EXPECT_EQ(opened.diagnostics[0].code, errc::index_id_not_found);
    EXPECT_EQ(block_data(opened, "/xisf/Image[2]"), abc());
}

TEST(conformance_distributed, a_duplicated_identifier_is_ambiguous)
{
    const unit opened = open_header_file(
        row_image(3, "path(@header_dir/unit.xisb):1") + row_image(3, "path(@header_dir/unit.xisb):2"),
        {{"unit.xisb",
          blocks_file({{.elements = {{.id = 1, .position = 200, .length = 3}, {.id = 2, .position = 200, .length = 3}},
                        .next = 300},
                       {.position = 300, .elements = {{.id = 1, .position = 203, .length = 3}}}},
                      {{.position = 200, .data = bytes("abcdef")}})}});
    ASSERT_EQ(opened.diagnostics.size(), 1U);
    EXPECT_EQ(opened.diagnostics[0].code, errc::duplicate_index_id);
    EXPECT_EQ(block_data(opened, "/xisf/Image[2]"), abc());
}

TEST(conformance_distributed, an_element_points_inside_the_file_after_its_signature)
{
    const auto open_at = [](std::uint64_t position, std::uint64_t length) {
        return open_header_file(
            row_image(length, "path(@header_dir/unit.xisb):1"),
            {{"unit.xisb", blocks_file({{.elements = {{.id = 1, .position = position, .length = length}}}}, {}, 100)}});
    };
    EXPECT_TRUE(unavailable(open_at(15, 3), "/xisf/Image[1]", errc::block_out_of_bounds));
    EXPECT_TRUE(unavailable(open_at(98, 3), "/xisf/Image[1]", errc::block_out_of_bounds));
    EXPECT_TRUE(unavailable(open_at(101, 1), "/xisf/Image[1]", errc::block_out_of_bounds));
    EXPECT_TRUE(no_diagnostics(open_at(97, 3).diagnostics));
    EXPECT_TRUE(no_diagnostics(open_at(16, 3).diagnostics));
}

TEST(conformance_distributed, a_data_blocks_file_beyond_4_gib)
{
    // A data blocks file of 5 GiB whose bytes are computed from their offset, but for its index, which is beyond 4 GiB.
    // A block of 5 MiB ends where the file does, and is read in pieces as its progress is reported; another one
    // starts a byte later. A block of almost 5 GiB fits from byte 64, which is never read, and one a byte longer
    // does not.
    constexpr std::uint64_t size = std::uint64_t{5} << 30;
    constexpr std::uint64_t node = (std::uint64_t{1} << 32) + 64;
    constexpr std::size_t width = std::size_t{5} << 20;
    constexpr std::uint64_t position = size - width;
    const std::vector<openxisf::test::placed_bytes> parts =
        openxisf::test::blocks_file_parts({{.next = node},
                                           {.position = node,
                                            .elements = {{.id = 1, .position = position, .length = width},
                                                         {.id = 2, .position = position + 1, .length = width},
                                                         {.id = 3, .position = 64, .length = size - 64},
                                                         {.id = 4, .position = 64, .length = size - 63}}}});
    const std::string body =
        row_image(width, "path(@header_dir/unit.xisb):1") + row_image(width, "path(@header_dir/unit.xisb):2") +
        row_image(size - 64, "path(@header_dir/unit.xisb):3") + row_image(size - 63, "path(@header_dir/unit.xisb):4");
    const reader file(
        std::make_unique<openxisf::memory_source>(bytes(header_xml(body))),
        {.resolver = [&parts](const external_reference&) { return openxisf::test::virtual_source(size, parts); }});
    const std::span<const openxisf::diagnostic> diagnostics = file.diagnostics();
    ASSERT_EQ(diagnostics.size(), 2U) << openxisf::test::describe(diagnostics);
    EXPECT_EQ(diagnostics[0].code, errc::block_out_of_bounds);
    EXPECT_EQ(diagnostics[0].context.element, "/xisf/Image[2]");
    EXPECT_EQ(diagnostics[1].code, errc::block_out_of_bounds);
    EXPECT_EQ(diagnostics[1].context.element, "/xisf/Image[4]");
    EXPECT_EQ(file.image(2).data_size(), size - 64);

    std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
    const openxisf::pixel_read_options options{.progress = [&calls](std::uint64_t done, std::uint64_t total) {
        calls.emplace_back(done, total);
        return true;
    }};
    EXPECT_EQ(file.read_pixels(0, options), openxisf::test::virtual_bytes(position, width));
    EXPECT_EQ(calls, (std::vector<std::pair<std::uint64_t, std::uint64_t>>{
                         {0, width}, {std::uint64_t{4} << 20, width}, {width, width}}));
}

TEST(conformance_distributed, a_malformed_index_fails_the_blocks_that_need_it)
{
    std::vector<std::byte> file = blocks_file_of({abc()}, {1});
    file[0] = std::byte{'Y'};
    const unit opened = open_header_file(row_image(3, "path(@header_dir/unit.xisb):1") +
                                             row_image(file.size(), "path(@header_dir/unit.xisb)"),
                                         {{"unit.xisb", file}});
    EXPECT_TRUE(unavailable(opened, "/xisf/Image[1]", errc::invalid_blocks_file));
    EXPECT_EQ(block_data(opened, "/xisf/Image[2]"), file);
    // A limit of the index fails the open in strict mode.
    const std::vector<std::byte> two_nodes =
        blocks_file({{.elements = {{.id = 1, .position = 200, .length = 3}}, .next = 100}, {.position = 100}},
                    {{.position = 200, .data = abc()}});
    EXPECT_TRUE(no_diagnostics(
        open_header_file(row_image(3, "path(@header_dir/unit.xisb):1"), {{"unit.xisb", two_nodes}}).diagnostics));
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::too_many_index_nodes, [&two_nodes] {
        (void)open_header_file(row_image(3, "path(@header_dir/unit.xisb):1"), {{"unit.xisb", two_nodes}},
                               {.strict = true, .limits = {.max_index_nodes = 1}});
    }));
}

TEST(conformance_distributed, the_uncompressed_length_is_that_of_a_compressed_block)
{
    const std::vector<std::byte> data = openxisf::test::pattern(1000);
    const openxisf::detail::compressed_block compressed =
        openxisf::detail::compress_block(data, {.codec = openxisf::detail::compression_codec::zlib});
    const auto open_with = [&](std::uint64_t uncompressed_length, std::string_view attributes) {
        return open_header_file(
            row_image(1000, "path(@header_dir/unit.xisb):1", attributes),
            {{"unit.xisb", blocks_file({{.elements = {{.id = 1,
                                                       .position = 100,
                                                       .length = compressed.data.size(),
                                                       .uncompressed_length = uncompressed_length}}}},
                                       {{.position = 100, .data = compressed.data}})}});
    };
    const std::string zlib = R"(compression="zlib:1000")";
    EXPECT_TRUE(no_diagnostics(open_with(1000, zlib).diagnostics));
    EXPECT_TRUE(unavailable(open_with(999, zlib), "/xisf/Image[1]", errc::invalid_index_element));
    // An uncompressed block has none; this block of compressed bytes is not the size of the image either.
    EXPECT_EQ(open_with(1000, "").diagnostics.front().code, errc::invalid_index_element);
    // A compressed block whose element has none is read as the header says.
    const unit unnamed = open_with(0, zlib);
    EXPECT_TRUE(
        single_diagnostic(unnamed.diagnostics, severity::warning, errc::invalid_index_element, "/xisf/Image[1]"));
    EXPECT_EQ(block_data(unnamed, "/xisf/Image[1]"), data);
}

TEST(conformance_distributed, the_subblocks_of_an_external_block_add_up_to_its_size)
{
    const unit opened = open_header_file(
        bytes_property("Test:Bytes", 6, "path(@header_dir/a.bin)", R"(compression="zlib:6" subblocks="2,3:2,3")"),
        {{"a.bin", abc()}});
    EXPECT_TRUE(unavailable(opened, "/xisf/Property[1]", errc::invalid_subblocks));
    EXPECT_EQ(opened.diagnostics[0].context.attribute, "subblocks");
}

TEST(conformance_distributed, an_empty_file_is_an_empty_block)
{
    // Only an inline block can be empty (spec §10).
    const unit opened =
        open_header_file(bytes_property("Test:Bytes", 0, "path(@header_dir/empty.bin)"), {{"empty.bin", bytes("")}});
    EXPECT_TRUE(single_diagnostic(opened.diagnostics, severity::warning, errc::invalid_location, "/xisf/Property[1]"));
}

TEST(conformance_distributed, reserved_fields_that_are_not_zero_are_a_warning_for_each_file)
{
    const unit opened = open_header_file(
        row_image(3, "path(@header_dir/unit.xisb):1") + row_image(3, "path(@header_dir/unit.xisb):2"),
        {{"unit.xisb", blocks_file({{.elements = {{.id = 1, .position = 200, .length = 3, .reserved = 5},
                                                  {.id = 2, .position = 200, .length = 3}},
                                     .reserved = 1}},
                                   {{.position = 200, .data = abc()}}, 0, 2)}});
    EXPECT_TRUE(
        single_diagnostic(opened.diagnostics, severity::warning, errc::reserved_field_not_zero, "/xisf/Image[1]"));
    EXPECT_EQ(block_data(opened, "/xisf/Image[2]"), abc());
}

// ---------------------------------------------------------------------------------------------------------------------
// Units opened from a path

TEST(conformance_distributed, a_unit_opened_from_a_path_finds_its_files_in_its_directory)
{
    const openxisf::test::temp_directory directory;
    std::filesystem::create_directory(directory.path() / "unit");
    openxisf::test::write_file(directory.path() / "unit" / "unit.xisb", blocks_file_of({abc()}, {1}));
    openxisf::test::write_file(directory.path() / "secret.bin", abc());
    const std::string header =
        header_xml(row_image(3, "path(@header_dir/unit.xisb):1") + row_image(3, "path(@header_dir/../secret.bin)") +
                   row_image(3, "path(/secret.bin)"));
    openxisf::test::write_file(directory.path() / "unit" / "unit.xish", bytes(header));

    const reader file(directory.file("unit/unit.xish"));
    ASSERT_EQ(file.diagnostics().size(), 2U);
    EXPECT_EQ(file.read_pixels(0), abc());
    EXPECT_EQ(file.diagnostics()[0].code, errc::location_not_allowed);
    EXPECT_EQ(file.diagnostics()[1].code, errc::location_not_allowed);
    EXPECT_TRUE(
        throws<openxisf::unsupported_error>(errc::location_not_allowed, [&file] { (void)file.read_pixels(1); }));

    // A resolver of the options replaces the default one.
    const reader replaced(directory.file("unit/unit.xish"),
                          {.resolver = memory_resolver({{"unit.xisb", blocks_file_of({bytes("xyz")}, {1})}})});
    EXPECT_EQ(replaced.read_pixels(0), bytes("xyz"));
}

// ---------------------------------------------------------------------------------------------------------------------
// Writing distributed units (spec §9.1.2, §9.4, §9.6)

constexpr openxisf::date_time creation_time{.year = 2026, .month = 10, .day = 4, .hour = 12};

openxisf::write_options basic_options()
{
    return {.creator_application = "OpenXISF tests 1.0", .creation_time = creation_time};
}

// A model with two blocks for the data blocks file, the pixels of a 4 × 3 UInt16 image and a vector of 1000 values,
// and a property in the header. It borrows its pixels.
struct small_model
{
    openxisf::image_info image{};
    std::vector<std::uint16_t> pixels{};

    small_model() : pixels(12)
    {
        image.geometry = openxisf::geometry{.dimensions = {4, 3}, .channels = 1};
        image.sample_format = openxisf::sample_format::uint16;
        image.color_space = openxisf::color_space::gray;
        for (std::size_t i = 0; i < pixels.size(); ++i) {
            pixels[i] = static_cast<std::uint16_t>(1000 * i);
        }
    }

    [[nodiscard]] openxisf::writer writer(openxisf::write_options options = basic_options()) const
    {
        openxisf::writer output(std::move(options));
        output.metadata().set("XISF:Title", "A distributed unit");
        (void)output.add_image(image, std::span<const std::uint16_t>(pixels));
        output.properties().set("Test:Vector", openxisf::property_value(std::vector<double>(1000, 0.5)));
        output.properties().set("Test:Small", std::int32_t{-7});
        return output;
    }
};

// The elements of the single index node of a data blocks file, read by the test itself.
std::vector<index_entry> index_of(std::span<const std::byte> file)
{
    const auto number = [file](std::size_t offset, std::size_t size) {
        std::uint64_t value = 0;
        for (std::size_t i = 0; i < size; ++i) {
            value |= std::to_integer<std::uint64_t>(file[offset + i]) << (8U * i);
        }
        return value;
    };
    std::vector<index_entry> elements;
    if (file.size() < 32 || openxisf::test::text(file.first(8)) != "XISB0100") {
        ADD_FAILURE() << "not a data blocks file";
        return elements;
    }
    EXPECT_EQ(number(8, 8), 0U);
    EXPECT_EQ(number(20, 4), 0U);
    EXPECT_EQ(number(24, 8), 0U) << "a second node";
    const std::uint64_t count = number(16, 4);
    for (std::size_t i = 0; i < count && 32 + (40 * (i + 1)) <= file.size(); ++i) {
        const std::size_t at = 32 + (40 * i);
        elements.push_back({.id = number(at, 8),
                            .position = number(at + 8, 8),
                            .length = number(at + 16, 8),
                            .uncompressed_length = number(at + 24, 8),
                            .reserved = number(at + 32, 8)});
    }
    EXPECT_EQ(elements.size(), count);
    return elements;
}

void collect_locations(const pugi::xml_node& node, std::vector<std::string>& locations)
{
    for (pugi::xml_node child = node.first_child(); !child.empty(); child = child.next_sibling()) {
        if (!child.attribute("location").empty()) {
            locations.emplace_back(child.attribute("location").value());
        }
        collect_locations(child, locations);
    }
}

// The locations of the elements of a header file that serialize a data block, in document order.
std::vector<std::string> locations_of(std::span<const std::byte> header)
{
    std::vector<std::string> locations;
    collect_locations(*openxisf::detail::parse_xml(openxisf::test::text(header), 0, {}), locations);
    return locations;
}

std::string location_with(std::uint64_t id)
{
    return "path(@header_dir/unit.xisb):" + openxisf::detail::format_index_id(id);
}

TEST(conformance_distributed, a_writer_writes_a_header_file_and_a_data_blocks_file)
{
    const small_model model;
    const openxisf::test::distributed_unit unit = openxisf::test::written_distributed(model.writer());

    // The header file is the header alone (spec §9.3), and has no alignment for attached blocks.
    EXPECT_TRUE(openxisf::test::text(unit.header).starts_with(R"(<?xml version="1.0" encoding="UTF-8"?>)"));
    EXPECT_EQ(openxisf::test::text(unit.header).find("XISF:BlockAlignmentSize"), std::string::npos);
    EXPECT_NE(openxisf::test::text(unit.header).find("XISF:MaxInlineBlockSize"), std::string::npos);

    // One index element for each block, at its place, in the order of the header.
    const std::vector<index_entry> elements = index_of(unit.blocks);
    ASSERT_EQ(elements.size(), 2U);
    EXPECT_EQ(locations_of(unit.header),
              (std::vector<std::string>{location_with(elements[0].id), location_with(elements[1].id)}));
    EXPECT_NE(elements[0].id, elements[1].id);
    EXPECT_EQ(elements[0].position, 4096U);
    EXPECT_EQ(elements[0].length, 24U);
    EXPECT_EQ(elements[1].position, 8192U);
    EXPECT_EQ(elements[1].length, 8000U);
    EXPECT_EQ(unit.blocks.size(), 8192U + 8000U);
    for (const index_entry& element : elements) {
        EXPECT_EQ(element.uncompressed_length, 0U);
        EXPECT_EQ(element.reserved, 0U);
    }

    const reader file = openxisf::test::open_distributed(unit, {.strict = true});
    EXPECT_TRUE(no_diagnostics(file.diagnostics()));
    EXPECT_EQ(file.image(0), model.image);
    EXPECT_EQ(file.read_pixels<std::uint16_t>(0), model.pixels);
    EXPECT_EQ(file.properties(), model.writer().properties());
}

TEST(conformance_distributed, the_identifiers_of_the_blocks_are_random)
{
    const small_model model;
    const openxisf::writer output = model.writer();
    const std::vector<index_entry> first = index_of(openxisf::test::written_distributed(output).blocks);
    const std::vector<index_entry> second = index_of(openxisf::test::written_distributed(output).blocks);
    ASSERT_EQ(first.size(), 2U);
    ASSERT_EQ(second.size(), 2U);
    EXPECT_NE(first[0].id, second[0].id);
    EXPECT_NE(first[1].id, second[1].id);
}

TEST(conformance_distributed, the_blocks_follow_the_index_at_multiples_of_the_alignment)
{
    openxisf::write_options options = basic_options();
    options.block_alignment = 0;
    const std::vector<index_entry> packed =
        index_of(openxisf::test::written_distributed(small_model().writer(options)).blocks);
    ASSERT_EQ(packed.size(), 2U);
    EXPECT_EQ(packed[0].position, 32U + 80U);
    EXPECT_EQ(packed[1].position, 32U + 80U + 24U);
    options.block_alignment = 100;
    const std::vector<index_entry> aligned =
        index_of(openxisf::test::written_distributed(small_model().writer(options)).blocks);
    ASSERT_EQ(aligned.size(), 2U);
    EXPECT_EQ(aligned[0].position, 200U);
    EXPECT_EQ(aligned[1].position, 300U);

    // The same when the blocks are hashed as they are written, and the index written last.
    options.checksum = openxisf::checksum_algorithm::sha1;
    for (const std::uint16_t alignment : {std::uint16_t{0}, std::uint16_t{100}}) {
        options.block_alignment = alignment;
        const openxisf::test::distributed_unit unit =
            openxisf::test::written_distributed(small_model().writer(options));
        const std::vector<index_entry> streamed = index_of(unit.blocks);
        ASSERT_EQ(streamed.size(), 2U);
        EXPECT_EQ(streamed[0].position, alignment == 0 ? 112U : 200U);
        EXPECT_EQ(streamed[1].position, alignment == 0 ? 136U : 300U);
        EXPECT_TRUE(no_diagnostics(openxisf::test::open_distributed(unit, {.strict = true}).diagnostics()));
    }
}

TEST(conformance_distributed, an_index_element_gives_the_uncompressed_length_of_a_compressed_block)
{
    openxisf::write_options options = basic_options();
    options.codec = openxisf::codec::zstd;
    options.byte_shuffle = true;
    for (const openxisf::test::sink_kind sink :
         {openxisf::test::sink_kind::rewritable, openxisf::test::sink_kind::append_only}) {
        const openxisf::test::distributed_unit unit =
            openxisf::test::written_distributed(small_model().writer(options), sink);
        const std::vector<index_entry> elements = index_of(unit.blocks);
        ASSERT_EQ(elements.size(), 2U);
        // The constant vector compresses; the 24 bytes of the image do not, and are stored as they are.
        EXPECT_EQ(elements[0].uncompressed_length, 0U);
        EXPECT_EQ(elements[0].length, 24U);
        EXPECT_EQ(elements[1].uncompressed_length, 8000U);
        EXPECT_LT(elements[1].length, 8000U);
        const reader file = openxisf::test::open_distributed(unit, {.strict = true});
        EXPECT_EQ(file.properties(), small_model().writer().properties());
    }
}

TEST(conformance_distributed, a_unit_without_blocks_has_a_data_blocks_file_with_an_empty_index)
{
    const openxisf::writer output(basic_options());
    const openxisf::test::distributed_unit unit = openxisf::test::written_distributed(output);
    std::vector<std::byte> expected = bytes("XISB0100");
    expected.resize(32);
    EXPECT_EQ(unit.blocks, expected);
    EXPECT_TRUE(locations_of(unit.header).empty());
    EXPECT_TRUE(no_diagnostics(openxisf::test::open_distributed(unit, {.strict = true}).diagnostics()));
}

TEST(conformance_distributed, the_data_blocks_file_is_finished_before_the_header_file)
{
    const small_model model;
    const openxisf::writer output = model.writer();
    std::vector<std::string> finished;
    const auto ignore = [](std::span<const std::byte>) {};
    openxisf::callback_sink header(ignore, {}, [&finished] { finished.emplace_back("header"); });
    openxisf::callback_sink blocks(ignore, {}, [&finished] { finished.emplace_back("blocks"); });
    output.save_distributed(header, blocks, "unit.xisb");
    EXPECT_EQ(finished, (std::vector<std::string>{"blocks", "header"}));

    // Both files are complete before either is finished, so a failure there finishes neither.
    finished.clear();
    std::uint64_t header_bytes = 0;
    openxisf::callback_sink counted([&header_bytes](std::span<const std::byte> data) { header_bytes += data.size(); },
                                    {}, [&finished] { finished.emplace_back("header"); });
    openxisf::callback_sink failing(ignore, {}, [] { throw openxisf::io_error(errc::write_failed, "full"); });
    EXPECT_THROW(output.save_distributed(counted, failing, "unit.xisb"), openxisf::io_error);
    EXPECT_GT(header_bytes, 0U);
    EXPECT_TRUE(finished.empty());
}

TEST(conformance_distributed, progress_counts_the_data_of_the_data_blocks_file)
{
    // The pixels of the image and the vector: each block counts when it is written, and once more when its checksum or
    // compressed size must be known before the index, which a sink of blocks that cannot rewrite requires.
    constexpr std::uint64_t blocks = 24 + 8000;
    struct expectation
    {
        bool encoded = false;
        openxisf::test::sink_kind sink = openxisf::test::sink_kind::rewritable;
        std::uint64_t total = 0;
    };
    const small_model model;
    for (const expectation& entry :
         {expectation{.total = blocks}, expectation{.sink = openxisf::test::sink_kind::append_only, .total = blocks},
          expectation{.encoded = true, .total = blocks},
          expectation{.encoded = true, .sink = openxisf::test::sink_kind::append_only, .total = 2 * blocks}}) {
        openxisf::write_options options = basic_options();
        if (entry.encoded) {
            options.codec = openxisf::codec::lz4;
            options.checksum = openxisf::checksum_algorithm::sha1;
        }
        std::vector<std::pair<std::uint64_t, std::uint64_t>> calls;
        options.progress = [&calls](std::uint64_t done, std::uint64_t total) {
            calls.emplace_back(done, total);
            return true;
        };
        (void)openxisf::test::written_distributed(model.writer(options), entry.sink);
        ASSERT_FALSE(calls.empty());
        EXPECT_EQ(calls.front(), std::make_pair(std::uint64_t{0}, entry.total));
        EXPECT_EQ(calls.back(), std::make_pair(entry.total, entry.total));
        EXPECT_TRUE(std::ranges::is_sorted(calls));
    }
}

TEST(conformance_distributed, the_path_of_a_data_blocks_file_is_relative_and_ends_with_xisb)
{
    const small_model model;
    const openxisf::writer output = model.writer();
    for (const std::string_view path :
         {"", "unit.xisf", "unit", "/data/unit.xisb", "../unit.xisb", "./unit.xisb", "blocks//unit.xisb",
          "blocks/./unit.xisb", "blocks/", R"(blocks\unit.xisb)", "unit\n.xisb", "unit\x7F.xisb",
          // U+FFFE and U+FFFF, which XML cannot hold.
          "unit\xEF\xBF\xBE.xisb", "unit\xEF\xBF\xBF.xisb"}) {
        openxisf::memory_sink header;
        openxisf::memory_sink blocks;
        EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument, [&] {
            output.save_distributed(header, blocks, path);
        })) << path;
        EXPECT_EQ(header.position() + blocks.position(), 0U);
    }
    openxisf::memory_sink header;
    openxisf::memory_sink blocks;
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_utf8,
                                              [&] { output.save_distributed(header, blocks, "\xFF.xisb"); }));
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument,
                                              [&] { output.save_distributed(header, header, "unit.xisb"); }));
    // Each sink receives one file, from its start.
    openxisf::memory_sink used;
    used.write(openxisf::test::bytes("x"));
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument,
                                              [&] { output.save_distributed(used, blocks, "unit.xisb"); }));
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument,
                                              [&] { output.save_distributed(header, used, "unit.xisb"); }));
    EXPECT_EQ(header.position() + blocks.position(), 0U);
    EXPECT_EQ(used.position(), 1U);

    for (const std::string_view path : {"blocks/unit.xisb", "UNIT.XISB", "a unit (1).xisb",
                                        "\xC3\x91"
                                        "and\xC3\xBA.xisb"}) {
        openxisf::memory_sink header_file;
        openxisf::memory_sink blocks_file;
        output.save_distributed(header_file, blocks_file, path);
        const openxisf::test::distributed_unit unit{.header = header_file.release(), .blocks = blocks_file.release()};
        openxisf::test::record_distributed_unit(unit, path);
        const std::vector<std::string> locations = locations_of(unit.header);
        ASSERT_EQ(locations.size(), 2U);
        EXPECT_TRUE(locations[0].starts_with("path(@header_dir/" + std::string(path) + "):0x")) << locations[0];
    }
}

TEST(conformance_distributed, a_writer_saves_a_header_file_and_its_data_blocks_file)
{
    const openxisf::test::temp_directory directory;
    const small_model model;
    const openxisf::writer output = model.writer();
    const std::string name = "Nebulosa del \xC3\x91"
                             "and\xC3\xBA";
    output.save_distributed(directory.file(name + ".xish"));
    EXPECT_EQ(directory.entries(), 2U);
    EXPECT_TRUE(std::filesystem::exists(openxisf::test::path_of(directory.file(name + ".xisb"))));
    const std::string header = openxisf::test::read_file(openxisf::test::path_of(directory.file(name + ".xish")));
    EXPECT_NE(header.find("path(@header_dir/" + name + ".xisb):0x"), std::string::npos);
    openxisf::test::record_distributed_unit(
        {.header = bytes(header),
         .blocks = bytes(openxisf::test::read_file(openxisf::test::path_of(directory.file(name + ".xisb"))))},
        name + ".xisb");

    // Saved again, both files are replaced, and their blocks found.
    output.save_distributed(directory.file(name + ".xish"));
    EXPECT_EQ(directory.entries(), 2U);
    const reader file(directory.file(name + ".xish"), {.strict = true});
    EXPECT_EQ(file.read_pixels<std::uint16_t>(0), model.pixels);
    EXPECT_EQ(file.properties(), output.properties());
}

TEST(conformance_distributed, the_path_of_a_header_file_ends_with_xish)
{
    const openxisf::test::temp_directory directory;
    const small_model model;
    const openxisf::writer output = model.writer();
    for (const std::string_view name : {"unit.xisf", "unit.xisb", "unit", "unit.xish.tmp", ".xis"}) {
        EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument, [&] {
            output.save_distributed(directory.file(name));
        })) << name;
    }
    EXPECT_TRUE(throws<openxisf::usage_error>(errc::invalid_argument,
                                              [&] { output.save_distributed(directory.file("unit\x01.xish")); }));
    EXPECT_EQ(directory.entries(), 0U);
    output.save_distributed(directory.file("UNIT.XISH"));
    EXPECT_TRUE(std::filesystem::exists(openxisf::test::path_of(directory.file("UNIT.xisb"))));
}

TEST(conformance_distributed, a_cancelled_or_invalid_save_leaves_the_files_as_they_were)
{
    const openxisf::test::temp_directory directory;
    const std::string path = directory.file("unit.xish");
    const std::filesystem::path blocks_path = openxisf::test::path_of(directory.file("unit.xisb"));
    const small_model model;
    openxisf::write_options options = basic_options();
    options.codec = openxisf::codec::zstd;

    // The calls of the progress function in a save, elsewhere.
    std::uint64_t calls = 0;
    options.progress = [&calls](std::uint64_t /*done*/, std::uint64_t /*total*/) {
        ++calls;
        return true;
    };
    {
        const openxisf::test::temp_directory elsewhere;
        model.writer(options).save_distributed(elsewhere.file("unit.xish"));
    }
    ASSERT_GT(calls, 2U);
    const auto cancelled_at = [&options](std::uint64_t stop) {
        options.progress = [stop, count = std::uint64_t{0}](std::uint64_t /*done*/, std::uint64_t /*total*/) mutable {
            return ++count < stop;
        };
        return options;
    };

    // Cancelled at any call, or invalid, a first save leaves neither file.
    for (std::uint64_t stop = 1; stop <= calls; ++stop) {
        EXPECT_THROW(model.writer(cancelled_at(stop)).save_distributed(path), openxisf::cancelled_error) << stop;
        EXPECT_EQ(directory.entries(), 0U) << stop;
    }
    EXPECT_THROW(model.writer({}).save_distributed(path), openxisf::validation_error);
    EXPECT_EQ(directory.entries(), 0U);

    // A save over existing files leaves both as they were.
    model.writer().save_distributed(path);
    const std::string header = openxisf::test::read_file(openxisf::test::path_of(path));
    const std::string blocks = openxisf::test::read_file(blocks_path);
    for (std::uint64_t stop = 1; stop <= calls; ++stop) {
        EXPECT_THROW(model.writer(cancelled_at(stop)).save_distributed(path), openxisf::cancelled_error) << stop;
        EXPECT_EQ(openxisf::test::read_file(openxisf::test::path_of(path)), header) << stop;
        EXPECT_EQ(openxisf::test::read_file(blocks_path), blocks) << stop;
        EXPECT_EQ(directory.entries(), 2U) << stop;
    }
}

} // namespace
