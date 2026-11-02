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
/// A header beyond max_header_size, max_xml_depth, max_xml_elements or max_model_objects fails the open with
/// limit_error. What an open loads beyond the other limits is left unavailable, with an error diagnostic, as other
/// problems confined to one object are: ancillary data, and the external files of a distributed unit and their block
/// indexes. limit_error is then thrown by a strict open (read_options::strict), and by a later read of what was left
/// unavailable, such as reader::read_pixels() of an image whose data block was not located. A read of pixel data that
/// needs an allocation beyond max_allocation, or a Zstandard window beyond max_zstd_window, throws limit_error; a read
/// into memory of the caller that keeps the storage model of the image allocates only the stored bytes of compressed
/// data, and for byte-shuffled data the buffer that they are unshuffled from (reader::read_pixels()).
///
/// The defaults accept every unit a real application is expected to write. Lower them for services that open
/// files from unknown parties.
struct limits
{
    /// Size of the XML header, in bytes. With max_xml_elements, which counts the elements, and max_model_objects,
    /// which counts the objects made of them, it bounds the memory of an open. The parse takes up to about 10 times the
    /// size of a header made of attributes: each takes about 40 bytes besides its text, and no other limit counts them.
    /// An element takes up to about 150 bytes in the parse and the outline of the header, and the objects that the
    /// reader makes of it what max_model_objects says, so a header of images takes up to about 45 times its size while
    /// it is opened. The diagnostics are at most max_diagnostics, each of them a short message and the path of an
    /// element.
    std::uint64_t max_header_size = std::uint64_t{64} << 20;
    /// Nesting depth of XML elements.
    std::uint64_t max_xml_depth = 64;
    /// Number of XML elements in the header.
    std::uint64_t max_xml_elements = 1'000'000;
    /// Size of a single allocation, in bytes. A read allocates the data of a compressed block before it decodes them,
    /// and a block cannot declare more than its codec decodes from its stored bytes: 1032 times as many for zlib, 255
    /// for LZ4 and 32768 for Zstandard, or max_zstd_expansion.
    std::uint64_t max_allocation = std::uint64_t{16} << 30;
    /// Data loaded when a unit is opened (ICC profiles, the pixels of thumbnails, and the values of properties and
    /// table cells in data blocks), in bytes. Each data block counts with the larger of its stored and its uncompressed
    /// size. The copies that Reference elements make, of what an image names for each image that names it and of an
    /// image listed again, count too, by the bytes they hold.
    std::uint64_t max_ancillary_data = std::uint64_t{256} << 20;
    /// Number of block index nodes in a data blocks file. A node can list up to 2^32 - 1 blocks, and every element of
    /// every node is read, within max_index_elements.
    std::uint64_t max_index_nodes = 65'536;
    /// Number of external files that the data blocks of a distributed unit name, each opened once and kept open while
    /// the unit is, also by reader::load_ancillary_data(). Each holds a file descriptor with file_resolver(): the
    /// default stays within half of the 256 that a macOS application has by default.
    std::uint64_t max_external_files = 128;
    /// Window size of Zstandard frames, in bytes, rounded down to a power of two of at least 1 KiB. Only frames that do
    /// not declare their size need a window buffer; the others are decoded in place, within max_allocation. A read
    /// decodes such frames one at a time, also where it decodes the other subblocks of a block in parallel.
    std::uint64_t max_zstd_window = std::uint64_t{128} << 20;
    /// Number of diagnostics that an open lists (reader::diagnostics()). When there are more, the last one stands for
    /// itself and all those after it, and says how many they are. A strict open still fails at its first error.
    std::uint64_t max_diagnostics = 10'000;
    /// Number of block index elements in a data blocks file, over all its nodes, counted from the length of each node
    /// before its elements are read. Every element is read, so the time that reading an index takes grows with their
    /// number, which only the size of the file bounds otherwise, and a sparse file or a source of generated data can
    /// make large: the default bounds the walk to about a second, where a node of 2^32 - 1 elements would take
    /// minutes. The writer writes one element for each data block of a unit.
    std::uint64_t max_index_elements = std::uint64_t{1} << 24;
    /// Number of objects that the reader makes of the elements of the header, one for each Image, Thumbnail, Property,
    /// FITSKeyword, Table and Field element and each element that describes an image (ICCProfile, RGBWorkingSpace,
    /// DisplayFunction, ColorFilterArray, Resolution), counted before they are made, whether they are then available or
    /// not. An image takes about 2 KB in the reader, and up to twice that while the unit is opened, a property up to
    /// about 250 bytes and a FITS keyword up to about 130, and an element with a data block about 600 bytes more, so
    /// the default bounds them at about 2 GB. The rows and cells of tables are not counted, so that a large table
    /// needs only max_xml_elements raised: they take about 100 bytes each, 700 with a data block. The copies that
    /// Reference elements make are counted by max_ancillary_data.
    std::uint64_t max_model_objects = 1'000'000;
    /// Bytes that a Zstandard block may declare for each of its stored bytes, and each of its subblocks for each of
    /// its own. The default is the bound of the format, 32768, by which 32 KiB of stored bytes make a read allocate
    /// 1 GiB before the decoding can fail: a service that reads data of a known kind lowers it, to 256 for example, as
    /// it lowers max_allocation. A block beyond it is unavailable, with an error diagnostic.
    std::uint64_t max_zstd_expansion = 32768;
};

} // namespace openxisf
