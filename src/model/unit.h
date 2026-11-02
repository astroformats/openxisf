// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#pragma once

#include <openxisf/error.h>
#include <openxisf/io.h>
#include <openxisf/limits.h>
#include <openxisf/reader.h>

#include "container/data_block.h"
#include "container/external_blocks.h"
#include "io/thread_safe_source.h"
#include "model/header.h"
#include "model/images.h"
#include "model/properties.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace openxisf::detail {

/// An opened unit: everything that a reader holds. Opening reads and checks the header, describes the data blocks, and
/// reads the properties, the tables, the images and what describes them; problems confined to one object of the unit
/// are diagnostics (spec §7). Only load_ancillary_data() changes it after construction.
struct unit
{
    /// Opens the unit in input. Throws what the constructors of reader document. shared_with_writers says that other
    /// threads may write bytes of input while the unit is read (thread_safe_source::shared_with_writers()).
    unit(std::unique_ptr<input_source> input, read_options requested, bool shared_with_writers = false);

    /// Opens the unit again from its source with the ancillary data loaded, as reader::load_ancillary_data() documents.
    /// The external files of a distributed unit that are open are taken again, by their locations, rather than opened
    /// through the resolver. The unit is unchanged when it throws.
    void load_ancillary_data();

    /// The data block of the pixel data of the image at index in images. The block of a file that a header-only open
    /// left closed (deferred) is located first, once for each file: what a read of it throws then is the error of the
    /// block, and what the resolver throws that the other errors of a file are not (locate_external_blocks()).
    [[nodiscard]] const data_block& pixel_block(std::size_t index) const;

    thread_safe_source source;
    /// The options that the unit was opened with; their limits also apply to what is read from it later.
    read_options options;
    openxisf::limits limits;
    /// True when the ancillary data in data blocks are loaded.
    bool ancillary_loaded = true;
    unit_storage storage = unit_storage::monolithic;
    /// The detached signature and the root element that it signs, kept as written.
    std::optional<header_signature> signature{};
    std::vector<data_block> blocks{};
    /// The properties of the unit and its standalone properties; the images and thumbnails hold their own.
    unit_properties properties{};
    unit_images images{};
    std::vector<diagnostic> diagnostics{};
    /// The external files of a distributed unit opened with read_options::header_only, each located when the pixels of
    /// an image in it are first read, until the ancillary data are loaded; null otherwise. The blocks in such a file
    /// are described, but not located: no file, no place in it, no check of their subblocks or their size.
    std::unique_ptr<deferred_external_files> deferred{};

private:
    // Reads the unit from source, with or without the ancillary data, and replaces what the unit holds only once
    // everything is read.
    void open(bool load_ancillary);
};

} // namespace openxisf::detail
