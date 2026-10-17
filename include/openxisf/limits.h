// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/export.h>

#include <cstdint>

/// @file
/// Safety limits for reading untrusted units.

namespace openxisf {

/// Safety limits for reading units from untrusted sources. Zero means no limit.
///
/// A header beyond max_header_size, max_xml_depth or max_xml_elements fails the open with limit_error. What an open
/// loads beyond the other limits is left unavailable, with an error diagnostic, as other problems confined to one
/// object are: ancillary data, and the external files of a distributed unit and their block indexes. limit_error is
/// then thrown by a strict open (read_options::strict), and by a later read of what was left unavailable, such as
/// reader::read_pixels() of an image whose data block was not located. A read of pixel data that needs an allocation
/// beyond max_allocation, or a Zstandard window beyond max_zstd_window, throws limit_error; a read into memory of the
/// caller that keeps the storage model of the image allocates only the stored bytes of compressed data, and for
/// byte-shuffled data the buffer that they are unshuffled from (reader::read_pixels()).
///
/// The defaults accept every unit a real application is expected to write. Lower them for services that open
/// files from unknown parties.
struct limits
{
    /// Size of the XML header, in bytes. With max_xml_elements, which counts the elements, it bounds the memory of an
    /// open. The parse takes up to about 10 times the size of a header made of attributes: each takes about 40 bytes
    /// besides its text, and no other limit counts them. An element takes up to about 150 bytes in the parse and the
    /// outline of the header, and the objects that the reader makes of it what their types do, about 2 KB for an image,
    /// so a header of images takes up to about 40 times its size. The diagnostics are at most 10,000, each of them a
    /// short message and the path of an element.
    std::uint64_t max_header_size = std::uint64_t{64} << 20;
    /// Nesting depth of XML elements.
    std::uint64_t max_xml_depth = 64;
    /// Number of XML elements in the header.
    std::uint64_t max_xml_elements = 1'000'000;
    /// Size of a single allocation, in bytes. A read allocates the data of a compressed block before it decodes them,
    /// and a block cannot declare more than its codec decodes from its stored bytes: 1032 times as many for zlib, 255
    /// for LZ4 and 32768 for Zstandard.
    std::uint64_t max_allocation = std::uint64_t{16} << 30;
    /// Data loaded when a unit is opened (ICC profiles, the pixels of thumbnails, and the values of properties and
    /// table cells in data blocks), in bytes. Each data block counts with the larger of its stored and its uncompressed
    /// size. The copies that Reference elements make, of what an image names for each image that names it and of an
    /// image listed again, count too, by the bytes they hold.
    std::uint64_t max_ancillary_data = std::uint64_t{256} << 20;
    /// Number of block index nodes in a data blocks file. A node can list up to 2^32 - 1 blocks, and every element of
    /// every node is read: only the size of the file bounds them, so the time that reading an index takes grows with
    /// that size, which a sparse file or a source of generated data can make large.
    std::uint64_t max_index_nodes = 65'536;
    /// Number of external files that the data blocks of a distributed unit name, each opened once and kept open while
    /// the unit is. reader::load_ancillary_data() opens them again before it closes the first ones.
    std::uint64_t max_external_files = 256;
    /// Window size of Zstandard frames, in bytes, rounded down to a power of two of at least 1 KiB. Only frames that do
    /// not declare their size need a window buffer; the others are decoded in place, within max_allocation. A read
    /// decodes such frames one at a time, also where it decodes the other subblocks of a block in parallel.
    std::uint64_t max_zstd_window = std::uint64_t{128} << 20;
};

} // namespace openxisf
