// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "container/external_blocks.h"

#include <openxisf/error.h>

#include "codec/compressed_block.h"
#include "container/blocks_file.h"
#include "core/quote.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace openxisf::detail {

namespace {

// A file, and the blocks it holds, in document order.
struct external_file
{
    external_reference reference{};
    std::vector<std::size_t> blocks{};
};

// Why the block index of a file cannot be read.
struct index_problem
{
    errc code = errc::invalid_blocks_file;
    std::string message{};
    error_origin origin{};
};

class external_locator
{
public:
    external_locator(std::vector<data_block>& blocks, const external_resolver& resolver, const limits& limits,
                     diagnostic_log& log)
        : blocks_(blocks), resolver_(resolver), limits_(limits), log_(log)
    {}

    void locate()
    {
        const std::vector<external_file> files = group();
        for (std::size_t number = 0; number < files.size(); ++number) {
            locate_file(files[number], number);
        }
    }

private:
    std::vector<external_file> group() const
    {
        std::vector<external_file> files;
        std::unordered_map<std::string, std::size_t> by_location;
        for (std::size_t i = 0; i < blocks_.size(); ++i) {
            const std::optional<block_descriptor>& descriptor = blocks_[i].descriptor;
            if (!descriptor || !is_external(descriptor->location.kind)) {
                continue;
            }
            external_reference reference = reference_of(descriptor->location);
            const auto [found, added] = by_location.try_emplace(location_text(reference), files.size());
            if (added) {
                files.push_back({.reference = std::move(reference)});
            }
            files[found->second].blocks.push_back(i);
        }
        return files;
    }

    void locate_file(const external_file& file, std::size_t number)
    {
        if (limits_.max_external_files != 0 && number >= limits_.max_external_files) {
            fail_all(file, errc::too_many_external_files,
                     "the external file " + quote(file.reference.location) + " is not opened: the unit names more " +
                         "external files than the limit of " + std::to_string(limits_.max_external_files));
            return;
        }
        const std::shared_ptr<const thread_safe_source> source = open(file);
        if (!source) {
            return;
        }
        index_problem problem;
        std::optional<block_index> index;
        const std::unordered_set<std::uint64_t> wanted = index_ids(file);
        if (!wanted.empty()) {
            index = read_index(file, wanted, *source, problem);
        }
        for (const std::size_t i : file.blocks) {
            data_block& block = blocks_[i];
            // Every block of a file has a descriptor, which only a failure removes.
            if (!block.descriptor) {
                continue;
            }
            block_descriptor& descriptor = *block.descriptor;
            if (descriptor.location.index_id && !index) {
                fail(block, problem.code, problem.message, "location", problem.origin);
            } else {
                place(block, descriptor, file, source, index ? &*index : nullptr);
            }
        }
    }

    // The identifiers of the block index elements that the blocks of file name.
    std::unordered_set<std::uint64_t> index_ids(const external_file& file) const
    {
        std::unordered_set<std::uint64_t> ids;
        for (const std::size_t i : file.blocks) {
            const std::optional<block_descriptor>& descriptor = blocks_[i].descriptor;
            if (descriptor && descriptor->location.index_id) {
                ids.insert(*descriptor->location.index_id);
            }
        }
        return ids;
    }

    // The file of the blocks, or null when it cannot be opened, which fails each of them.
    std::shared_ptr<const thread_safe_source> open(const external_file& file)
    {
        const std::string name = quote(file.reference.location);
        if (!resolver_) {
            fail_all(file, errc::unsupported_location,
                     "the external file " + name + " is not opened: the unit was not opened from a path, and the " +
                         "options give no resolver");
            return nullptr;
        }
        try {
            if (std::unique_ptr<input_source> input = resolver_(file.reference)) {
                return std::make_shared<thread_safe_source>(std::move(input));
            }
        } catch (const io_error& failure) {
            // The exception keeps its class whatever its code, as the resolver gave it.
            fail_all(file, failure.code(), "the external file " + name + " cannot be opened: " + failure.what(),
                     {.raised = error_origin::kind::io, .system_code = failure.system_code()});
            return nullptr;
        } catch (const unsupported_error& failure) {
            fail_all(file, failure.code(), "the external file " + name + " is not opened: " + failure.what(),
                     {.raised = error_origin::kind::unsupported});
            return nullptr;
        }
        fail_all(file, errc::unsupported_location, "the resolver does not open the external file " + name);
        return nullptr;
    }

    // The elements of the identifiers that the blocks of file name, or nothing when the index cannot be read, which
    // problem then describes.
    std::optional<block_index> read_index(const external_file& file, const std::unordered_set<std::uint64_t>& wanted,
                                          const thread_safe_source& source, index_problem& problem)
    {
        const std::string prefix = "the data blocks file " + quote(file.reference.location) + " cannot be read: ";
        try {
            block_index index = read_block_index(source, wanted, limits_);
            if (index.nonzero_reserved_fields != 0) {
                log_.warning(errc::reserved_field_not_zero,
                             std::to_string(index.nonzero_reserved_fields) + " reserved fields of the data blocks " +
                                 "file " + quote(file.reference.location) + " are not zero, and are ignored",
                             context_of(blocks_[file.blocks.front()]));
            }
            return index;
        } catch (const invalid_data_error& failure) {
            problem = {.code = failure.code(), .message = prefix + failure.what()};
        } catch (const limit_error& failure) {
            problem = {.code = failure.code(), .message = prefix + failure.what()};
        } catch (const io_error& failure) {
            problem = {.code = failure.code(),
                       .message = prefix + failure.what(),
                       .origin = {.raised = error_origin::kind::io, .system_code = failure.system_code()}};
        }
        return std::nullopt;
    }

    // Places the block of descriptor in its file. A failure removes the descriptor, which is not used afterwards.
    void place(data_block& block, block_descriptor& descriptor, const external_file& file,
               const std::shared_ptr<const thread_safe_source>& source, const block_index* index)
    {
        block_location& location = descriptor.location;
        if (!location.index_id) {
            // Spec §10.3: the block is the whole file.
            location.position = 0;
            location.size = source->size();
        } else if (index == nullptr ||
                   !place_indexed(block, descriptor, *location.index_id, file, *index, source->size())) {
            return;
        }
        if (location.size == 0) {
            log_.warning(errc::invalid_location, "the data block is empty, which only an inline block can be",
                         context_of(block));
        }
        // The subblocks must account for every byte of the block, now that its stored size is known (spec §10.6).
        if (const std::optional<block_compression>& compression = descriptor.compression) {
            try {
                check_subblocks(*compression, location.size);
            } catch (const invalid_data_error& failure) {
                const char* attribute = compression->subblocks.empty() ? "compression" : "subblocks";
                fail(block, failure.code(), failure.what(), attribute);
                return;
            }
        }
        descriptor.external = source;
    }

    // The place of a block in a data blocks file, from the element of its index-id (spec §9.4).
    bool place_indexed(data_block& block, block_descriptor& descriptor, std::uint64_t id, const external_file& file,
                       const block_index& index, std::uint64_t file_size)
    {
        const std::string element_text = "the block index element " + format_index_id(id) +
                                         " of the data blocks file " + quote(file.reference.location);
        const index_element* found = index.find(id);
        if (found == nullptr) {
            return fail(block, errc::index_id_not_found,
                        "the data blocks file " + quote(file.reference.location) + " has no block index element " +
                            format_index_id(id));
        }
        if (index.is_duplicate(id)) {
            return fail(block, errc::duplicate_index_id,
                        "several block index elements of the data blocks file " + quote(file.reference.location) +
                            " have the identifier " + format_index_id(id));
        }
        const index_element& element = *found;
        if (element.position == 0) {
            return fail(block, errc::index_id_not_found, element_text + " is free: it points to no data block");
        }
        if (element.position < blocks_file_header_size || element.position > file_size ||
            element.length > file_size - element.position) {
            return fail(block, errc::block_out_of_bounds,
                        "the data block of " + std::to_string(element.length) + " bytes at byte " +
                            std::to_string(element.position) + " that " + element_text +
                            " points to is not inside the file after its first " +
                            std::to_string(blocks_file_header_size) + " bytes; the file has " +
                            std::to_string(file_size));
        }
        if (!check_uncompressed_length(block, descriptor, element, element_text)) {
            return false;
        }
        descriptor.location.position = element.position;
        descriptor.location.size = element.length;
        return true;
    }

    // Spec §9.4: the uncompressed length of an element is that of a compressed block, and zero for any other. A
    // compressed block whose element leaves it zero is unambiguous, since the header gives its size.
    bool check_uncompressed_length(data_block& block, const block_descriptor& descriptor, const index_element& element,
                                   const std::string& element_text)
    {
        const std::optional<block_compression>& compression = descriptor.compression;
        const std::string length = std::to_string(element.uncompressed_length);
        if (!compression) {
            if (element.uncompressed_length != 0) {
                return fail(block, errc::invalid_index_element,
                            element_text + " gives an uncompressed length of " + length +
                                " bytes, but the data block is not compressed");
            }
            return true;
        }
        if (element.uncompressed_length == 0) {
            log_.warning(errc::invalid_index_element,
                         element_text + " gives no uncompressed length for the compressed data block",
                         context_of(block));
            return true;
        }
        if (element.uncompressed_length != compression->uncompressed_size) {
            return fail(block, errc::invalid_index_element,
                        element_text + " gives an uncompressed length of " + length +
                            " bytes, and the compression attribute " + std::to_string(compression->uncompressed_size));
        }
        return true;
    }

    static error_context context_of(const data_block& block, const char* attribute = "location")
    {
        return {.element = block.path, .attribute = attribute};
    }

    // Makes the block unavailable, with an error, which a read of the block throws again as origin says. In strict
    // mode, the log throws instead. The block keeps the diagnostic, which the log may not list.
    bool fail(data_block& block, errc code, std::string message, const char* attribute = "location",
              const error_origin& origin = {})
    {
        block.problem = diagnostic{
            .severity = severity::error, .code = code, .message = message, .context = context_of(block, attribute)};
        log_.error(code, std::move(message), context_of(block, attribute), origin);
        block.origin = origin;
        block.descriptor.reset();
        return false;
    }

    void fail_all(const external_file& file, errc code, const std::string& message, const error_origin& origin = {})
    {
        for (const std::size_t i : file.blocks) {
            fail(blocks_[i], code, message, "location", origin);
        }
    }

    std::vector<data_block>& blocks_;
    const external_resolver& resolver_;
    const limits& limits_;
    diagnostic_log& log_;
};

} // namespace

external_reference reference_of(const block_location& location)
{
    if (location.kind == location_kind::url) {
        return {.form = location_form::url, .location = location.reference};
    }
    if (location.reference.starts_with(header_directory_prefix)) {
        return {.form = location_form::relative_path,
                .location = location.reference.substr(header_directory_prefix.size())};
    }
    return {.form = location_form::absolute_path, .location = location.reference};
}

std::string location_text(const external_reference& reference)
{
    switch (reference.form) {
    case location_form::url:
        return "url(" + reference.location + ")";
    case location_form::absolute_path:
        return "path(" + reference.location + ")";
    case location_form::relative_path:
        break;
    }
    return "path(" + std::string(header_directory_prefix) + reference.location + ")";
}

void locate_external_blocks(std::vector<data_block>& blocks, const external_resolver& resolver, const limits& limits,
                            diagnostic_log& log)
{
    external_locator(blocks, resolver, limits, log).locate();
}

} // namespace openxisf::detail
