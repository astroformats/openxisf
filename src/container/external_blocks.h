// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/io.h>
#include <openxisf/limits.h>

#include "container/block_attributes.h"
#include "container/data_block.h"
#include "core/diagnostic_log.h"
#include "io/thread_safe_source.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// The external data blocks of a distributed unit (spec §10.2, §10.3): the files that hold them, opened through a
// resolver, and their places in those files.

namespace openxisf::detail {

/// The external reference of a url() or path() location, which a resolver opens.
[[nodiscard]] external_reference reference_of(const block_location& location);

/// The location of a file as a unit writes it, for messages: url(URL), path(/path) or path(@header_dir/path).
[[nodiscard]] std::string location_text(const external_reference& reference);

/// The sources of the external files of a unit, by the location that names each (location_text()), so that a unit
/// opened again from its source takes them again rather than ask the resolver for them.
using external_sources = std::unordered_map<std::string, std::shared_ptr<const thread_safe_source>>;

/// Gives each available external block of blocks the file that holds it, and its place in that file. Each file is
/// opened once, in the order of the first block that names it: the source of opened for its location, or else through
/// resolver. The block index of a data blocks file is read once, for the identifiers that the blocks name
/// (read_block_index()). A block that cannot be located becomes unavailable, with an error in log about its location
/// attribute:
/// - no resolver, a null source, the io_error or unsupported_error of the resolver or of the source, more files than
///   limits.max_external_files, and a data blocks file that cannot be read or whose index is malformed; the block keeps
///   the class and the system error of an exception of the resolver, and the io_error of the source, for a read of it
///   (data_block::origin), and so does the exception of a strict log;
/// - an index-id that no element has, or only a free one, or that several elements have;
/// - an element whose block is not inside the file after its first 16 bytes, whose uncompressed length is not zero
///   for an uncompressed block, or differs from the uncompressed size of a compressed one;
/// - subblocks whose sizes do not add up to the stored and uncompressed sizes of the block, and a Zstandard block
///   beyond limits.max_zstd_expansion.
///
/// Reserved fields of a data blocks file that are not zero are a warning for each file, and so are an empty block and
/// an element of a compressed block without its uncompressed length. Other exceptions of the resolver and of the
/// sources pass through.
void locate_external_blocks(std::vector<data_block>& blocks, const external_resolver& resolver, const limits& limits,
                            diagnostic_log& log, const external_sources& opened = {});

/// Adds to sources the files of the located external blocks of blocks.
void add_external_sources(const std::vector<data_block>& blocks, external_sources& sources);

/// The external files of a distributed unit, each located when a block in it is first needed rather than when the unit
/// is opened, as locate_external_blocks() locates it. Any number of threads may ask for blocks at once: each file is
/// located once, by the first thread that needs it, while the others wait, so the resolver is called one file at a
/// time.
class deferred_external_files
{
public:
    /// The files of the external blocks of blocks, opened through resolver within limits. pixel_sizes gives, for each
    /// block that holds the pixel data of an image, the size of those data, which the block is checked against once it
    /// is located (errc::pixel_data_size_mismatch); 0 for the others.
    deferred_external_files(const std::vector<data_block>& blocks, external_resolver resolver,
                            const openxisf::limits& limits, std::vector<std::uint64_t> pixel_sizes);

    deferred_external_files(const deferred_external_files&) = delete;
    deferred_external_files& operator=(const deferred_external_files&) = delete;
    ~deferred_external_files();

    /// The block of blocks, the same vector as given to the constructor, at index, located: the block itself unless
    /// it is an available external block, otherwise a copy of it with its file and its place in it, or unavailable
    /// with the error found, as locate_external_blocks() leaves it. The copy lasts as long as this object. Throws what
    /// the resolver and the file throw that locate_external_blocks() lets pass through.
    [[nodiscard]] const data_block& located(const std::vector<data_block>& blocks, std::size_t index) const;

    /// Adds to sources the files opened so far.
    void add_sources(external_sources& sources) const;

private:
    struct state;
    std::unique_ptr<state> state_;
};

} // namespace openxisf::detail
