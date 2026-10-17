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
// pugixml loses (character_data()), so header outlives the result.
parsed_header parse_unit_header(const unit_header& header, const thread_safe_source& source, const limits& limits,
                                diagnostic_log& log, block_context& context)
{
    parsed_header parsed = parse_header(header.text, header.offset, limits, log);
    context = {.storage = header.storage,
               .header_end = header.end,
               .source_size = source.size(),
               .signed_unit = parsed.signature.has_value()};
    return parsed;
}

} // namespace

unit::unit(std::unique_ptr<input_source> input, read_options requested)
    : source(std::move(input)), options(std::move(requested)), limits(options.limits)
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
    diagnostic_log log(options.strict);
    block_context context;
    const unit_header text = read_unit_header(source, limits, log);
    parsed_header header = parse_unit_header(text, source, limits, log, context);
    // The walk reports the problems of the element structure. The objects of the unit are read from the outline.
    const unit_outline outline = build_outline(header.root, log);
    std::vector<data_block> described = describe_blocks(outline, context, log);
    if (context.storage == unit_storage::distributed) {
        locate_external_blocks(described, options.resolver, limits, log);
    }
    ancillary_budget budget(limits.max_ancillary_data);
    value_reader values(source, limits, budget, load_ancillary, header.text, log);
    unit_objects objects{.properties = read_properties(outline, described, values, budget, log),
                         .tables = read_tables(outline, described, values, budget, log),
                         .ancillary = read_ancillary(outline, described, source, limits, budget, load_ancillary, log)};
    unit_images listed = read_images(outline, described, objects, source, limits, budget, load_ancillary, log);

    // Nothing below throws.
    storage = context.storage;
    signature = std::move(header.signature);
    blocks = std::move(described);
    properties = std::move(objects.properties);
    images = std::move(listed);
    diagnostics = log.release();
    ancillary_loaded = load_ancillary;
}

} // namespace openxisf::detail
