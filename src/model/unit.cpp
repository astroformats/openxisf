// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/unit.h"

#include "container/external_blocks.h"
#include "container/file_layout.h"
#include "core/diagnostic_log.h"
#include "model/ancillary.h"
#include "model/ancillary_budget.h"
#include "model/header.h"
#include "model/outline.h"
#include "model/tables.h"
#include "model/value_reader.h"

#include <utility>

namespace openxisf::detail {

namespace {

// The parsed header, whose document holds a copy of the text of header; the text itself gives the character data that
// pugixml loses (character_data()) and the places of digests, so header outlives the result.
parsed_header parse_unit_header(const unit_header& header, const thread_safe_source& source, const limits& limits,
                                diagnostic_log& log, block_context& context)
{
    parsed_header parsed = parse_header(header.text, header.offset, limits, log);
    // The parsed text is that of the header without a byte order mark.
    const auto mark = static_cast<std::uint64_t>(parsed.text.data() - header.text.data());
    context = {.storage = header.storage,
               .header_end = header.end,
               .source_size = source.size(),
               .signed_unit = parsed.signature.has_value(),
               .header_text = parsed.text,
               .header_text_offset = header.offset + mark,
               .max_zstd_expansion = limits.max_zstd_expansion};
    return parsed;
}

} // namespace

unit::unit(std::unique_ptr<input_source> input, read_options requested, bool shared_with_writers)
    : source(std::move(input), shared_with_writers), options(std::move(requested)), limits(options.limits)
{
    open(!options.header_only);
}

void unit::load_ancillary_data()
{
    if (!ancillary_loaded) {
        open(true);
    }
}

void unit::open(bool load_ancillary)
{
    diagnostic_log log(options.strict, limits.max_diagnostics);
    block_context context;
    const unit_header text = read_unit_header(source, limits, log);
    parsed_header header = parse_unit_header(text, source, limits, log, context);
    // The walk reports the problems of the element structure, and counts the objects of the model before any is made.
    // The objects of the unit are read from the outline.
    const unit_outline outline = build_outline(header.root, limits, log);
    std::vector<data_block> described = describe_blocks(outline, context, log);
    // A header-only open leaves the external files closed until a block in one is read.
    const bool defer = context.storage == unit_storage::distributed && !load_ancillary;
    if (context.storage == unit_storage::distributed && !defer) {
        // The files that the unit holds open already, which a load of the ancillary data takes again.
        external_sources opened;
        add_external_sources(blocks, opened);
        if (deferred) {
            deferred->add_sources(opened);
        }
        locate_external_blocks(described, options.resolver, limits, log, opened);
    }
    ancillary_budget budget(limits.max_ancillary_data);
    value_reader values(source, limits, budget, load_ancillary, header.text, log);
    unit_objects objects{.properties = read_properties(outline, described, values, budget, log),
                         .tables = read_tables(outline, described, values, budget, log),
                         .ancillary = read_ancillary(outline, described, source, limits, budget, load_ancillary, log)};
    unit_images listed = read_images(outline, described, objects, source, limits, budget, load_ancillary, log);
    std::unique_ptr<deferred_external_files> files;
    if (defer) {
        std::vector<std::uint64_t> pixel_sizes(described.size());
        for (std::size_t i = 0; i < listed.infos.size(); ++i) {
            pixel_sizes[listed.blocks[i]] = listed.infos[i].data_size();
        }
        files = std::make_unique<deferred_external_files>(described, options.resolver, limits, std::move(pixel_sizes));
    }

    // Nothing below throws.
    storage = context.storage;
    signature = std::move(header.signature);
    blocks = std::move(described);
    properties = std::move(objects.properties);
    images = std::move(listed);
    diagnostics = log.release();
    ancillary_loaded = load_ancillary;
    deferred = std::move(files);
}

const data_block& unit::pixel_block(std::size_t index) const
{
    const std::size_t block = images.blocks[index];
    return deferred ? deferred->located(blocks, block) : blocks[block];
}

} // namespace openxisf::detail
