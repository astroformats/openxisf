// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/types.h>

#include "codec/compressed_block.h"
#include "container/block_attributes.h"
#include "core/data_encoding.h"
#include "crypto/hash.h"
#include "support/fixture_builder.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

// Units of one UInt16 image whose data block takes one of the forms of a block, for the tests of the reads and the
// writes of regions of interest.

namespace openxisf::test {

/// How a test stores the pixel data of an image of UInt16 samples.
struct block_form
{
    pixel_storage storage = pixel_storage::planar;
    /// Compressed with zlib, in subblocks of at most subblock_size bytes unless it is 0, and byte-shuffled by sample.
    bool compressed = false;
    std::uint64_t subblock_size = 0;
    bool shuffled = false;
    bool checksum = false;
    bool big_endian = false;
    bool embedded = false;
};

[[nodiscard]] inline std::string describe(const block_form& form)
{
    return std::string(pixel_storage_name(form.storage)) + (form.compressed ? " zlib" : "") +
           (form.subblock_size != 0 ? " in subblocks of " + std::to_string(form.subblock_size) : "") +
           (form.shuffled ? " shuffled" : "") + (form.checksum ? " checksummed" : "") +
           (form.big_endian ? " big-endian" : "") + (form.embedded ? " embedded" : " attached");
}

[[nodiscard]] inline std::string describe(const pixel_region& region)
{
    std::string text = "region";
    for (std::size_t a = 0; a < region.origin.size(); ++a) {
        text += " " + std::to_string(region.origin[a]) + "+" + std::to_string(region.size[a]);
    }
    return text + " channels " + std::to_string(region.first_channel) + "+" + std::to_string(region.channel_count);
}

/// The pixel data of a UInt16 image of geometry image in the model storage, in native byte order: sample c of pixel p
/// is first + 1000c + p, wrapped to 16 bits, so that a sample in a wrong place shows.
[[nodiscard]] inline std::vector<std::byte> native_samples(const geometry& image, pixel_storage storage,
                                                           std::uint16_t first = 0)
{
    const std::uint64_t pixels = image.pixel_count();
    std::vector<std::uint16_t> values(pixels * image.channels);
    for (std::uint64_t c = 0; c < image.channels; ++c) {
        for (std::uint64_t p = 0; p < pixels; ++p) {
            values[storage == pixel_storage::planar ? (c * pixels) + p : (p * image.channels) + c] =
                static_cast<std::uint16_t>(first + (1000 * c) + p);
        }
    }
    std::vector<std::byte> bytes(values.size() * 2);
    std::memcpy(bytes.data(), values.data(), bytes.size());
    return bytes;
}

/// The UInt16 samples of native in little-endian byte order, or in big-endian byte order.
[[nodiscard]] inline std::vector<std::byte> in_byte_order(std::span<const std::byte> native, bool big_endian)
{
    std::vector<std::byte> bytes(native.size());
    for (std::size_t i = 0; i + 1 < native.size(); i += 2) {
        std::uint16_t value = 0;
        std::memcpy(&value, native.data() + i, 2);
        const auto low = static_cast<std::byte>(value & 0xFFU);
        const auto high = static_cast<std::byte>(value >> 8U);
        bytes[i] = big_endian ? high : low;
        bytes[i + 1] = big_endian ? low : high;
    }
    return bytes;
}

/// The attributes of the Image element of a UInt16 image of geometry image in the model storage, but for its location.
[[nodiscard]] inline std::string image_attributes(const geometry& image, pixel_storage storage)
{
    std::string text = R"(geometry=")";
    for (const std::uint64_t length : image.dimensions) {
        text += std::to_string(length) + ":";
    }
    return text + std::to_string(image.channels) + R"(" sampleFormat="UInt16" pixelStorage=")" +
           std::string(pixel_storage_name(storage)) + "\"";
}

/// A monolithic unit of one UInt16 image of geometry image, whose data block takes form and holds native_samples().
[[nodiscard]] inline std::vector<std::byte> unit_of(const geometry& image, const block_form& form)
{
    // The attributes of the block, which an embedded block has on its Data element.
    std::string attributes;
    if (form.big_endian) {
        attributes += R"( byteOrder="big")";
    }
    std::vector<std::byte> stored = in_byte_order(native_samples(image, form.storage), form.big_endian);
    if (form.compressed) {
        detail::compressed_block block = detail::compress_block(stored, {.codec = detail::compression_codec::zlib,
                                                                         .item_size = form.shuffled ? 2U : 0U,
                                                                         .max_subblock_size = form.subblock_size});
        stored = std::move(block.data);
        attributes += R"( compression=")" + detail::format_compression(block.compression) + "\"";
        if (!block.compression.subblocks.empty()) {
            attributes += R"( subblocks=")" + detail::format_subblocks(block.compression.subblocks) + "\"";
        }
    }
    if (form.checksum) {
        const detail::block_checksum checksum{.algorithm = detail::hash_algorithm::sha1,
                                              .digest = detail::compute_digest(detail::hash_algorithm::sha1, stored)};
        attributes += R"( checksum=")" + detail::format_checksum(checksum) + "\"";
    }
    const std::string element = "<Image " + image_attributes(image, form.storage);
    if (form.embedded) {
        return monolithic_file(header_xml(element + R"( location="embedded"><Data encoding="hex")" + attributes + ">" +
                                          detail::encode_hex(stored) + "</Data></Image>"));
    }
    return file_with_attachments(header_xml(element + attributes + R"( location="attachment:{0}"/>)"), {stored});
}

/// The samples of a row of the region in the model storage: those of every channel in the normal model, of one in the
/// planar model.
[[nodiscard]] inline std::uint64_t row_of(const pixel_region& region, pixel_storage storage)
{
    return region.size.front() * (storage == pixel_storage::normal ? region.channel_count : 1);
}

} // namespace openxisf::test
