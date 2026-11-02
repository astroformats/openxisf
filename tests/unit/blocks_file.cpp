// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

// The block index of data blocks files (spec §9.4): the signature, the walk over the nodes and its bounds, and the
// elements of the identifiers looked for.

#include "container/blocks_file.h"

#include <openxisf/error.h>
#include <openxisf/io.h>

#include "support/bytes.h"
#include "support/fixture_builder.h"
#include "support/throws.h"
#include "support/virtual_source.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

using openxisf::errc;
using openxisf::detail::block_index;
using openxisf::detail::index_element;
using openxisf::detail::read_block_index;
using openxisf::detail::thread_safe_source;
using openxisf::test::blocks_file;
using openxisf::test::blocks_file_parts;
using openxisf::test::bytes;
using openxisf::test::index_entry;
using openxisf::test::index_node;
using openxisf::test::throws;

block_index read(std::vector<std::byte> file, const std::unordered_set<std::uint64_t>& wanted,
                 const openxisf::limits& limits = {})
{
    const thread_safe_source source(std::make_unique<openxisf::memory_source>(std::move(file)));
    return read_block_index(source, wanted, limits);
}

testing::AssertionResult refused(errc code, std::vector<std::byte> file, const openxisf::limits& limits = {})
{
    return throws<openxisf::invalid_data_error>(code, [&] { (void)read(std::move(file), {1}, limits); });
}

TEST(blocks_file, finds_the_elements_of_the_identifiers_looked_for)
{
    const block_index index =
        read(blocks_file({{.elements = {{.id = 7, .position = 112, .length = 3},
                                        {.id = 9, .position = 115, .length = 2},
                                        {.id = 11, .position = 117, .length = 5, .uncompressed_length = 40}}}},
                         {}, 122),
             {7, 11, 13});
    // In the order of their identifiers.
    EXPECT_EQ(index.elements,
              (std::vector<index_element>{{.id = 7, .position = 112, .length = 3},
                                          {.id = 11, .position = 117, .length = 5, .uncompressed_length = 40}}));
    EXPECT_EQ(index.find(11), &index.elements[1]);
    EXPECT_EQ(index.find(9), nullptr);
    EXPECT_EQ(index.find(13), nullptr);
    EXPECT_TRUE(index.duplicates.empty());
    EXPECT_EQ(index.nonzero_reserved_fields, 0U);
}

TEST(blocks_file, follows_every_node_of_the_index)
{
    // Three nodes: at the start, at the end of the file, and between them, in that order.
    const block_index index = read(blocks_file({{.position = 16, .elements = {{.id = 1}}, .next = 300},
                                                {.position = 300, .elements = {{.id = 2}, {.id = 3}}, .next = 100},
                                                {.position = 100, .elements = {}, .next = 200},
                                                {.position = 200, .elements = {{.id = 4, .position = 250}}}}),
                                   {1, 2, 3, 4});
    EXPECT_EQ(index.elements.size(), 4U);
    ASSERT_NE(index.find(4), nullptr);
    EXPECT_EQ(index.find(4)->position, 250U);
}

TEST(blocks_file, keeps_free_elements_and_reports_duplicated_identifiers)
{
    const block_index index = read(
        blocks_file({{.elements = {{.id = 5}, {.id = 6, .position = 200, .length = 1}}, .next = 200},
                     {.position = 200, .elements = {{.id = 6, .position = 300, .length = 1}, {.id = 8}, {.id = 8}}}},
                    {}, 400),
        {5, 6});
    ASSERT_NE(index.find(5), nullptr);
    EXPECT_EQ(index.find(5)->position, 0U);
    // The first element stays; an identifier not looked for is not checked.
    ASSERT_NE(index.find(6), nullptr);
    EXPECT_EQ(index.find(6)->position, 200U);
    EXPECT_EQ(index.duplicates, std::vector<std::uint64_t>{6});
    EXPECT_TRUE(index.is_duplicate(6));
    EXPECT_FALSE(index.is_duplicate(5));
}

TEST(blocks_file, counts_the_reserved_fields_that_are_not_zero)
{
    const block_index index =
        read(blocks_file({{.elements = {{.id = 1, .reserved = 1}, {.id = 2, .reserved = 2}}, .reserved = 3}}, {}, 0, 4),
             {1});
    EXPECT_EQ(index.nonzero_reserved_fields, 4U);
}

TEST(blocks_file, reads_nodes_of_many_elements)
{
    // More elements than are read at once.
    index_node node;
    for (std::uint64_t id = 1; id <= 2500; ++id) {
        node.elements.push_back({.id = id, .position = id});
    }
    const block_index index = read(blocks_file({node}), {1, 1024, 1025, 2048, 2049, 2500});
    EXPECT_EQ(index.elements.size(), 6U);
    ASSERT_NE(index.find(2049), nullptr);
    EXPECT_EQ(index.find(2049)->position, 2049U);
}

TEST(blocks_file, requires_the_signature)
{
    std::vector<std::byte> file = blocks_file({{}});
    file[3] = std::byte{'F'};
    EXPECT_TRUE(refused(errc::invalid_blocks_file, file));
    EXPECT_TRUE(refused(errc::invalid_blocks_file, bytes("XISB010")));
    // The signature alone has no block index.
    EXPECT_TRUE(refused(errc::invalid_blocks_file, bytes("XISB0100")));
    EXPECT_TRUE(refused(errc::invalid_blocks_file, blocks_file({}, {}, 16)));
}

TEST(blocks_file, refuses_a_node_outside_the_file)
{
    // A node that starts in the file header, beyond the end, or ends beyond it.
    EXPECT_TRUE(refused(errc::invalid_blocks_file, blocks_file({{.next = 8}}, {}, 100)));
    EXPECT_TRUE(refused(errc::invalid_blocks_file, blocks_file({{.next = 101}}, {}, 100)));
    EXPECT_TRUE(refused(errc::invalid_blocks_file, blocks_file({{.next = 90}}, {}, 100)));
    EXPECT_TRUE(refused(errc::invalid_blocks_file, blocks_file({{.next = UINT64_MAX}}, {}, 100)));
    // Elements that go past the end, by one byte or by the largest count.
    std::vector<std::byte> file = blocks_file({{.elements = {{}}}});
    ASSERT_EQ(file.size(), 72U);
    EXPECT_NO_THROW((void)read(file, {1}));
    file.pop_back();
    EXPECT_TRUE(refused(errc::invalid_blocks_file, file));
    EXPECT_TRUE(refused(errc::invalid_blocks_file, blocks_file({{.length = UINT32_MAX}}, {}, 100)));
}

TEST(blocks_file, walks_nodes_and_elements_beyond_4_gib)
{
    // A file of 5 GiB in which only the index takes memory: the first node leads to one beyond 4 GiB, whose element
    // points beyond 4 GiB too.
    constexpr std::uint64_t size = std::uint64_t{5} << 30;
    constexpr std::uint64_t node = (std::uint64_t{1} << 32) + 64;
    const auto read_virtual = [](const std::vector<index_node>& nodes) {
        const thread_safe_source source(openxisf::test::virtual_source(size, blocks_file_parts(nodes)));
        return read_block_index(source, {1, 2}, {});
    };
    const block_index index =
        read_virtual({{.elements = {{.id = 1, .position = 100, .length = 3}}, .next = node},
                      {.position = node, .elements = {{.id = 2, .position = node + 56, .length = size - node - 56}}}});
    EXPECT_EQ(index.elements,
              (std::vector<index_element>{{.id = 1, .position = 100, .length = 3},
                                          {.id = 2, .position = node + 56, .length = size - node - 56}}));

    // A node whose element ends one byte beyond the file, and one beyond the file.
    EXPECT_TRUE(throws<openxisf::invalid_data_error>(errc::invalid_blocks_file, [&] {
        (void)read_virtual({{.next = size - 55}, {.position = size - 55, .elements = {{.id = 1}}}});
    }));
    EXPECT_TRUE(throws<openxisf::invalid_data_error>(errc::invalid_blocks_file,
                                                     [&] { (void)read_virtual({{.next = size + 16}}); }));
}

TEST(blocks_file, refuses_an_index_that_comes_back_to_a_node)
{
    EXPECT_TRUE(refused(errc::invalid_blocks_file, blocks_file({{.next = 16}})));
    EXPECT_TRUE(refused(errc::invalid_blocks_file, blocks_file({{.next = 100}, {.position = 100, .next = 16}})));
    // Found as a cycle, before the bytes of the nodes outgrow a large file or their number the limit.
    EXPECT_TRUE(refused(errc::invalid_blocks_file,
                        blocks_file({{.next = 100}, {.position = 100, .next = 16}}, {}, 100'000),
                        {.max_index_nodes = 3}));
}

TEST(blocks_file, refuses_nodes_that_read_the_same_bytes_again)
{
    // Each node starts among the elements of the one before; together they would read the same bytes many times.
    std::vector<index_node> nodes;
    for (std::uint64_t position = 16; position < 100; position += 16) {
        nodes.push_back({.position = position, .next = position + 16, .length = 2});
    }
    nodes.back().next = 0;
    EXPECT_TRUE(refused(errc::invalid_blocks_file, blocks_file(nodes, {}, 200)));
    nodes.resize(1);
    nodes.back().next = 0;
    EXPECT_NO_THROW((void)read(blocks_file(nodes, {}, 200), {1}));

    // Two nodes of 96 bytes fit a file of 208 bytes after its first 16, and not one byte smaller.
    const std::vector<index_node> apart{{.next = 112, .length = 2}, {.position = 112, .length = 2}};
    EXPECT_NO_THROW((void)read(blocks_file(apart, {}, 208), {1}));
    const std::vector<index_node> overlapping{{.next = 32, .length = 2}, {.position = 32, .length = 2}};
    EXPECT_TRUE(refused(errc::invalid_blocks_file, blocks_file(overlapping, {}, 207)));
}

TEST(blocks_file, limits_the_number_of_nodes)
{
    const std::vector<index_node> nodes{{.next = 100}, {.position = 100, .next = 200}, {.position = 200}};
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::too_many_index_nodes,
                                              [&] { (void)read(blocks_file(nodes), {1}, {.max_index_nodes = 2}); }));
    EXPECT_NO_THROW((void)read(blocks_file(nodes), {1}, {.max_index_nodes = 3}));
    EXPECT_NO_THROW((void)read(blocks_file(nodes), {1}, {.max_index_nodes = 0}));
}

TEST(blocks_file, limits_the_number_of_elements_of_all_nodes)
{
    // Five elements, in a node of two and a node of three.
    const std::vector<index_node> nodes{{.elements = {{.id = 1}, {.id = 2}}, .next = 200},
                                        {.position = 200, .elements = {{.id = 3}, {.id = 4}, {.id = 5}}}};
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::too_many_index_elements,
                                              [&] { (void)read(blocks_file(nodes), {1}, {.max_index_elements = 4}); }));
    EXPECT_EQ(read(blocks_file(nodes), {1, 5}, {.max_index_elements = 5}).elements.size(), 2U);
    EXPECT_EQ(read(blocks_file(nodes), {1, 5}, {.max_index_elements = 0}).elements.size(), 2U);
}

TEST(blocks_file, refuses_a_node_of_too_many_elements_before_it_reads_them)
{
    // A file of 160 GiB whose node declares 2^32 - 1 elements, which would take minutes to read. The default limit
    // refuses the node once its header is read.
    constexpr std::uint64_t size = openxisf::detail::blocks_file_header_size +
                                   openxisf::detail::index_node_header_size +
                                   (std::uint64_t{UINT32_MAX} * openxisf::detail::index_element_size);
    const auto reads = std::make_shared<std::vector<std::uint64_t>>();
    const thread_safe_source source(
        openxisf::test::virtual_source(size, blocks_file_parts({{.length = UINT32_MAX}}), reads));
    EXPECT_TRUE(throws<openxisf::limit_error>(errc::too_many_index_elements,
                                              [&source] { (void)read_block_index(source, {1}, {}); }));
    EXPECT_EQ(*reads, (std::vector<std::uint64_t>{0, openxisf::detail::blocks_file_header_size}));
}

} // namespace
