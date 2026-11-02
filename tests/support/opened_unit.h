// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/io.h>
#include <openxisf/property.h>
#include <openxisf/reader.h>

#include "container/data_block.h"
#include "model/unit.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Units opened with the internal reader, whose data blocks the public API does not expose yet.

namespace openxisf::test {

/// Opens the unit in memory.
[[nodiscard]] inline detail::unit open_internal(std::vector<std::byte> unit, read_options options = {})
{
    return {std::make_unique<memory_source>(std::move(unit)), std::move(options)};
}

/// A source of the bytes of a unit that copies them at each read, as a file source does, and gives none in memory
/// (input_source::bytes()), so that the library reads it as it reads a file. Any number of threads can read it at once.
[[nodiscard]] inline std::unique_ptr<input_source> file_like_source(std::vector<std::byte> unit)
{
    auto held = std::make_shared<const std::vector<std::byte>>(std::move(unit));
    const std::uint64_t size = held->size();
    return std::make_unique<callback_source>(
        size,
        [held = std::move(held)](std::uint64_t offset, std::span<std::byte> destination) {
            std::ranges::copy(std::span(*held).subspan(offset, destination.size()), destination.begin());
        },
        callback_source_options{.concurrent_reads = true});
}

/// Opens the unit through file_like_source().
[[nodiscard]] inline detail::unit open_file_like(std::vector<std::byte> unit, read_options options = {})
{
    return {file_like_source(std::move(unit)), std::move(options)};
}

/// Opens the unit in memory (open_internal()) or through file_like_source() (open_file_like()). A unit can be neither
/// copied nor moved, and the MSVC of Visual Studio 2022 does not elide the copy that a conditional expression of the
/// two calls makes (C2280), so the choice is made here.
[[nodiscard]] inline detail::unit open_in_memory_or_file_like(bool in_memory, std::vector<std::byte> unit,
                                                              read_options options = {})
{
    if (in_memory) {
        return open_internal(std::move(unit), std::move(options));
    }
    return open_file_like(std::move(unit), std::move(options));
}

/// The block of the element at path. Fails the test when there is none.
[[nodiscard]] inline const detail::data_block& block_at(const detail::unit& opened, std::string_view path)
{
    const auto found = std::ranges::find(opened.blocks, path, &detail::data_block::path);
    if (found == opened.blocks.end()) {
        ADD_FAILURE() << "no block at " << path;
        static const detail::data_block none{};
        return none;
    }
    return *found;
}

/// The descriptor of the block of the element at path. Fails the test when the block is missing or unavailable.
[[nodiscard]] inline const detail::block_descriptor& descriptor_at(const detail::unit& opened, std::string_view path)
{
    const detail::data_block& block = block_at(opened, path);
    if (!block.descriptor) {
        ADD_FAILURE() << "the block at " << path << " is unavailable";
        static const detail::block_descriptor none{};
        return none;
    }
    return *block.descriptor;
}

/// The value of an optional. Fails the test when it is empty.
template <typename T> [[nodiscard]] const T& value_of(const std::optional<T>& value)
{
    if (!value) {
        ADD_FAILURE() << "no value";
        static const T none{};
        return none;
    }
    return *value;
}

/// The properties of the Image element at path, or of its thumbnail when path names its Thumbnail element. Fails the
/// test when there is no such image or thumbnail.
[[nodiscard]] inline const property_list& properties_of(const detail::unit& opened, std::string_view path)
{
    constexpr std::string_view thumbnail_step = "/Thumbnail[1]";
    const bool of_thumbnail = path.ends_with(thumbnail_step);
    const std::string_view image_path = of_thumbnail ? path.substr(0, path.size() - thumbnail_step.size()) : path;
    // The block of the element of an image tells which one it is.
    for (std::size_t i = 0; i < opened.images.infos.size(); ++i) {
        const image_info& info = opened.images.infos[i];
        if (opened.blocks[opened.images.blocks[i]].path != image_path) {
            continue;
        }
        if (!of_thumbnail) {
            return info.properties;
        }
        if (info.thumbnail) {
            return info.thumbnail->properties;
        }
        break;
    }
    ADD_FAILURE() << "no image or thumbnail at " << path;
    static const property_list none{};
    return none;
}

/// The paths of the elements of the blocks of a unit, available or not, in order.
[[nodiscard]] inline std::vector<std::string> block_paths(const detail::unit& opened)
{
    std::vector<std::string> paths;
    paths.reserve(opened.blocks.size());
    for (const detail::data_block& block : opened.blocks) {
        paths.push_back(block.path);
    }
    return paths;
}

/// The stored bytes of the block of the element at path.
[[nodiscard]] inline std::vector<std::byte> stored_block(const detail::unit& opened, std::string_view path)
{
    return detail::read_stored_block(opened.source, block_at(opened, path), opened.limits);
}

/// The data of the block of the element at path, decompressed.
[[nodiscard]] inline std::vector<std::byte> block_data(const detail::unit& opened, std::string_view path)
{
    return detail::read_block(opened.source, block_at(opened, path), opened.limits);
}

} // namespace openxisf::test
