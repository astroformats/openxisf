// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "container/data_block.h"

#include "codec/compressed_block.h"
#include "core/allocation.h"
#include "core/checked_math.h"
#include "core/data_encoding.h"
#include "core/scratch_buffer.h"
#include "core/text_grammar.h"
#include "io/range_check.h"
#include "xml/xml_document.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace openxisf::detail {

namespace {

// The attributes that describe a block besides its location, with the code of their diagnostics.
struct block_attribute
{
    const char* name = nullptr;
    errc code = errc::invalid_location;
};

constexpr block_attribute byte_order_attribute{.name = "byteOrder", .code = errc::invalid_byte_order};
constexpr block_attribute checksum_attribute{.name = "checksum", .code = errc::invalid_checksum};
constexpr block_attribute compression_attribute{.name = "compression", .code = errc::invalid_compression};
constexpr block_attribute subblocks_attribute{.name = "subblocks", .code = errc::invalid_subblocks};
constexpr std::array<block_attribute, 4> block_attributes{byte_order_attribute, checksum_attribute,
                                                          compression_attribute, subblocks_attribute};

// The elements that can serialize a data block: images, thumbnails and ICC profiles, and the values of properties and
// table cells.
bool serializes_blocks(element_kind kind) noexcept
{
    return kind == element_kind::image || kind == element_kind::thumbnail || kind == element_kind::property ||
           kind == element_kind::icc_profile || kind == element_kind::cell;
}

// pugixml's iterators are not C++20 iterators, so the children are walked by hand.
bool has_child_elements(const pugi::xml_node& element)
{
    for (pugi::xml_node child = element.first_child(); !child.empty(); child = child.next_sibling()) {
        if (child.type() == pugi::node_element) {
            return true;
        }
    }
    return false;
}

// Turns the uppercase hexadecimal digits of text, from start on, into lowercase ones. True when there were any.
bool lowercase_hex_digits(std::string& text, std::size_t start) noexcept
{
    bool changed = false;
    for (std::size_t i = start; i < text.size(); ++i) {
        if (text[i] >= 'A' && text[i] <= 'F') {
            text[i] = static_cast<char>(text[i] - 'A' + 'a');
            changed = true;
        }
    }
    return changed;
}

// Turns the name that starts text, before its first colon or all of it without one, into lowercase when it has
// uppercase letters and is_name() takes it in lowercase. True when it did; a name that is none in any case stays as
// written, for the error about it.
bool lowercase_name(std::string& text, bool (*is_name)(std::string_view) noexcept)
{
    std::string name = text.substr(0, text.find(':'));
    bool changed = false;
    for (char& c : name) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
            changed = true;
        }
    }
    if (!changed || !is_name(name)) {
        return false;
    }
    text.replace(0, name.size(), name);
    return true;
}

// A block attribute as found, with where it was found.
struct attribute_value
{
    std::string_view text{};
    error_context context{};
};

// Describes the block of one element (spec §10). Tolerated problems are warnings; the first problem that leaves the
// block without a meaning is an error, which makes it unavailable, and ends the description.
class element_block
{
public:
    element_block(const unit_outline& outline, std::size_t index, const block_context& context, diagnostic_log& log)
        : outline_(outline), element_(outline.elements[index]), context_(context), log_(log), block_{.element = index}
    {}

    // The block, or nothing when the element has no location attribute.
    std::optional<data_block> describe()
    {
        const pugi::xml_attribute location = element_.node.attribute("location");
        if (!location) {
            ignore_block_parts();
            return std::nullopt;
        }
        block_.path = outline_.path(block_.element);
        if (locate(location.value()) && find_data_element() && read_attributes() && read_contents() &&
            check_subblocks() && verify()) {
            block_.descriptor = std::move(descriptor_);
        }
        return std::move(block_);
    }

private:
    // Records an error that makes the block unavailable, and returns false. In strict mode, the log throws instead. The
    // block keeps the diagnostic, which the log may not list.
    bool fail(errc code, std::string message, error_context context)
    {
        block_.problem = diagnostic{.severity = severity::error, .code = code, .message = message, .context = context};
        log_.error(code, std::move(message), std::move(context));
        return false;
    }

    // The result of parse, or nothing when it threw the error of a malformed or unsupported value, which is recorded.
    template <typename Parse>
    std::optional<std::invoke_result_t<Parse>> attempt(Parse&& parse, const error_context& context)
    {
        try {
            return std::forward<Parse>(parse)();
        } catch (const invalid_data_error& failure) {
            fail(failure.code(), failure.what(), context);
        } catch (const unsupported_error& failure) {
            fail(failure.code(), failure.what(), context);
        } catch (const limit_error& failure) {
            fail(failure.code(), failure.what(), context);
        }
        return std::nullopt;
    }

    // The Data elements that the element contains.
    std::vector<std::size_t> data_elements() const
    {
        std::vector<std::size_t> found;
        for (std::size_t i = block_.element + 1; i < element_.end; ++i) {
            if (outline_.elements[i].parent == block_.element && outline_.elements[i].kind == element_kind::data) {
                found.push_back(i);
            }
        }
        return found;
    }

    void ignore_data_elements(std::string_view reason)
    {
        for (const std::size_t data : data_elements()) {
            log_.warning(errc::invalid_location, "the Data element is ignored, since " + std::string(reason),
                         {.element = outline_.path(data)});
        }
    }

    // Without a location, the element has no block, and what would describe one means nothing.
    void ignore_block_parts()
    {
        ignore_data_elements("the element has no data block");
        for (const block_attribute& attribute : block_attributes) {
            if (!element_.node.attribute(attribute.name).empty()) {
                log_.warning(attribute.code,
                             "the attribute " + std::string(attribute.name) +
                                 " is ignored, since the element has no data block",
                             {.element = outline_.path(block_.element), .attribute = attribute.name});
            }
        }
    }

    // The location, and the rules of spec §10.1 and §10.2 about where blocks of each kind can be.
    bool locate(std::string_view text)
    {
        const error_context context{.element = block_.path, .attribute = "location"};
        const std::optional<block_location> location = attempt([text] { return parse_location(text); }, context);
        if (!location) {
            return false;
        }
        const bool external = is_external(location->kind);
        if (location->kind == location_kind::attachment && context_.storage == unit_storage::distributed) {
            return fail(errc::invalid_location, "a header file cannot have attached data blocks", context);
        }
        if (external && context_.storage == unit_storage::monolithic) {
            return fail(errc::invalid_location, "a monolithic file cannot have external data blocks", context);
        }
        descriptor_.location = *location;
        return true;
    }

    // An embedded block is in its only Data element (spec §10.3). The Data elements of other blocks are ignored.
    bool find_data_element()
    {
        if (descriptor_.location.kind != location_kind::embedded) {
            ignore_data_elements("the data block is not embedded");
            return true;
        }
        const std::vector<std::size_t> found = data_elements();
        if (found.size() != 1) {
            return fail(
                errc::invalid_location,
                "the data block is embedded, but the element has " +
                    (found.empty() ? std::string("no Data element") : std::to_string(found.size()) + " Data elements"),
                {.element = block_.path, .attribute = "location"});
        }
        data_ = found.front();
        data_path_ = outline_.path(data_);
        return true;
    }

    // A block attribute: on the Data element of an embedded block, where it belongs, or on the element itself. On the
    // element, an attribute of an embedded block is misplaced but unambiguous, so it is used, with a warning, unless
    // the Data element has it too.
    std::optional<attribute_value> attribute(const block_attribute& which)
    {
        const pugi::xml_attribute own = element_.node.attribute(which.name);
        if (data_ == no_element) {
            if (!own) {
                return std::nullopt;
            }
            return attribute_value{.text = own.value(), .context = {.element = block_.path, .attribute = which.name}};
        }
        const pugi::xml_attribute inner = outline_.elements[data_].node.attribute(which.name);
        if (!own.empty()) {
            log_.warning(which.code,
                         "the attribute " + std::string(which.name) +
                             " of an embedded block belongs to its Data element" +
                             (inner.empty() ? "" : ", and is ignored"),
                         {.element = block_.path, .attribute = which.name});
            if (!inner) {
                return attribute_value{.text = own.value(),
                                       .context = {.element = block_.path, .attribute = which.name}};
            }
        }
        if (!inner) {
            return std::nullopt;
        }
        return attribute_value{.text = inner.value(), .context = {.element = data_path_, .attribute = which.name}};
    }

    bool read_attributes()
    {
        if (const std::optional<attribute_value> value = attribute(byte_order_attribute)) {
            // ICC profiles are big-endian structures, kept unaltered (spec §10.4, §11.7).
            if (element_.kind == element_kind::icc_profile) {
                log_.warning(errc::invalid_byte_order, "an ICCProfile element cannot have a byteOrder attribute",
                             value->context);
            } else {
                const std::optional<byte_order> order =
                    attempt([&value] { return parse_byte_order(value->text); }, value->context);
                if (!order) {
                    return false;
                }
                descriptor_.order = *order;
            }
        }
        if (const std::optional<attribute_value> value = attribute(checksum_attribute)) {
            checksum_context_ = value->context;
            descriptor_.checksum = read_checksum(*value);
            if (!descriptor_.checksum) {
                return false;
            }
            locate_digest(value->text, descriptor_.checksum->algorithm);
        } else if (context_.signed_unit && is_stored_apart(descriptor_.location.kind)) {
            // Spec §10.5: a signature covers the header, so the data of a block stored apart from it are covered only
            // through the checksum of the block.
            log_.warning(errc::missing_checksum,
                         "the unit is signed, but the data block has no checksum, so the signature does not cover its "
                         "data",
                         {.element = block_.path});
        }
        if (const std::optional<attribute_value> value = attribute(compression_attribute)) {
            compression_context_ = value->context;
            // The specification names codecs in lowercase; another case names the same codec, so it is accepted with a
            // warning.
            std::string text(value->text);
            if (lowercase_name(text, is_codec_name)) {
                log_.warning(errc::invalid_compression, "the codec name has uppercase letters", value->context);
            }
            descriptor_.compression = attempt([&text] { return parse_compression(text); }, value->context);
            if (!descriptor_.compression) {
                return false;
            }
        }
        if (const std::optional<attribute_value> value = attribute(subblocks_attribute)) {
            subblocks_context_ = value->context;
            if (!descriptor_.compression) {
                log_.warning(errc::invalid_subblocks,
                             "the attribute subblocks is ignored, since the data block is not compressed",
                             value->context);
                return true;
            }
            std::optional<std::vector<subblock>> subblocks =
                attempt([&value] { return parse_subblocks(value->text); }, value->context);
            if (!subblocks) {
                return false;
            }
            descriptor_.compression->subblocks = std::move(*subblocks);
        }
        return true;
    }

    // An algorithm name in another case than that of the specification, and uppercase hexadecimal digits, name the same
    // algorithm and digest, so they are accepted with a warning. Those of an unknown algorithm are reported as written.
    std::optional<block_checksum> read_checksum(const attribute_value& value)
    {
        std::string text(value.text);
        if (lowercase_name(text, is_checksum_algorithm_name)) {
            log_.warning(errc::invalid_checksum, "the checksum algorithm name has uppercase letters", value.context);
        }
        const std::size_t colon = text.find(':');
        if (colon != std::string::npos && is_checksum_algorithm_name(std::string_view(text).substr(0, colon)) &&
            lowercase_hex_digits(text, colon + 1)) {
            log_.warning(errc::invalid_checksum, "the digest has uppercase hexadecimal digits", value.context);
        }
        return attempt([&text] { return parse_checksum(text); }, value.context);
    }

    // Where the digest of an attached block is in the source, value being its checksum attribute as parsed, of the
    // given algorithm: on the element itself, as for every attachment, and written as it was parsed, algorithm:digest
    // with the digest at its size, so that a digest of the same algorithm overwrites it, and the unit stays as it was
    // but for its digits.
    void locate_digest(std::string_view value, hash_algorithm algorithm)
    {
        if (descriptor_.location.kind != location_kind::attachment || context_.header_text.empty()) {
            return;
        }
        const std::optional<std::size_t> start = element_offset(element_.node);
        if (!start) {
            return;
        }
        const std::optional<text_span> written = attribute_text(context_.header_text, *start, "checksum");
        if (!written || context_.header_text.substr(written->offset, written->length) != value) {
            return;
        }
        const std::size_t colon = value.find(':');
        if (colon == std::string_view::npos || value.size() - colon - 1 != 2 * digest_size(algorithm)) {
            return;
        }
        descriptor_.digest_offset = context_.header_text_offset + written->offset + colon + 1;
    }

    // The stored bytes of an inline or embedded block, and the place of an attached one.
    bool read_contents()
    {
        switch (descriptor_.location.kind) {
        case location_kind::attachment:
            return check_bounds();
        case location_kind::inline_data:
            // Spec §11.5, §11.12: an image cannot be inline, because its element can have child elements. Spec §10.3:
            // the element of an inline block has no child elements. Its character data are still the block.
            if (element_.kind == element_kind::image || element_.kind == element_kind::thumbnail) {
                log_.warning(errc::invalid_location,
                             "an " + std::string(element_name(element_.kind)) +
                                 " element cannot have an inline data block",
                             {.element = block_.path, .attribute = "location"});
            } else if (has_child_elements(element_.node)) {
                log_.warning(errc::invalid_location, "an element with an inline data block cannot have child elements",
                             {.element = block_.path, .attribute = "location"});
            }
            return decode(character_data(element_.node), descriptor_.location.encoding, {.element = block_.path});
        case location_kind::embedded:
            return decode_embedded();
        case location_kind::url:
        case location_kind::path:
            // Its file and its place in it are found once every block is described (locate_external_blocks()).
            break;
        }
        return true;
    }

    bool check_bounds()
    {
        const block_location& location = descriptor_.location;
        const error_context context{.element = block_.path, .attribute = "location", .offset = location.position};
        if (location.position < context_.header_end) {
            return fail(errc::block_out_of_bounds,
                        "the attached data block starts at byte " + std::to_string(location.position) +
                            ", inside the header, which ends at byte " + std::to_string(context_.header_end),
                        context);
        }
        if (location.position > context_.source_size || location.size > context_.source_size - location.position) {
            return fail(errc::block_out_of_bounds,
                        "the attached data block of " + std::to_string(location.size) + " bytes at byte " +
                            std::to_string(location.position) + " goes past the end of the file, which has " +
                            std::to_string(context_.source_size) + " bytes",
                        context);
        }
        if (location.size == 0) {
            warn_empty();
        }
        return true;
    }

    bool decode_embedded()
    {
        const pugi::xml_node data = outline_.elements[data_].node;
        const pugi::xml_attribute encoding = data.attribute("encoding");
        if (!encoding) {
            return fail(errc::invalid_location, "the Data element of an embedded block has no encoding attribute",
                        {.element = data_path_});
        }
        const std::optional<block_encoding> parsed = attempt([&encoding] { return parse_encoding(encoding.value()); },
                                                             {.element = data_path_, .attribute = "encoding"});
        if (!parsed || !decode(character_data(data), *parsed, {.element = data_path_})) {
            return false;
        }
        if (descriptor_.data.empty()) {
            warn_empty();
        }
        return true;
    }

    // Base64 data without padding, and uppercase hexadecimal digits, are unambiguous, so they are accepted with a
    // warning, and decoded as if they were written as the specification requires.
    bool decode(std::string text, block_encoding encoding, const error_context& context)
    {
        if (encoding == block_encoding::base64) {
            const auto digits =
                static_cast<std::size_t>(std::ranges::count_if(text, [](char c) { return !is_white_space(c); }));
            if (text.find('=') == std::string::npos && digits % 4 >= 2) {
                log_.warning(errc::invalid_base64, "the Base64 data have no padding", context);
                text.append(4 - (digits % 4), '=');
            }
        } else if (lowercase_hex_digits(text, 0)) {
            log_.warning(errc::invalid_hex, "the hexadecimal data have uppercase digits", context);
        }

        std::optional<std::vector<std::byte>> bytes = attempt(
            [&text, encoding] { return encoding == block_encoding::base64 ? decode_base64(text) : decode_hex(text); },
            context);
        if (!bytes) {
            return false;
        }
        descriptor_.data = std::move(*bytes);
        return true;
    }

    // Spec §10: only an inline block, the value of an empty vector or matrix, can have no bytes.
    void warn_empty()
    {
        log_.warning(errc::invalid_location, "the data block is empty, which only an inline block can be",
                     {.element = block_.path, .attribute = "location"});
    }

    // The subblocks must account for every byte of the block, stored and uncompressed, and each must fit the decoder of
    // its codec (spec §10.6). A block without subblocks is one subblock.
    bool check_subblocks()
    {
        // The stored size of an external block is known once it is located.
        if (!descriptor_.compression || is_external(descriptor_.location.kind)) {
            return true;
        }
        const block_compression& compression = *descriptor_.compression;
        const std::uint64_t stored = descriptor_.location.kind == location_kind::attachment ? descriptor_.location.size
                                                                                            : descriptor_.data.size();
        const error_context& context = compression.subblocks.empty() ? compression_context_ : subblocks_context_;
        return attempt(
                   [this, &compression, stored] {
                       detail::check_subblocks(compression, stored);
                       check_zstd_expansion(compression, stored, context_.max_zstd_expansion);
                       return true;
                   },
                   context)
            .has_value();
    }

    // The bytes of an inline or embedded block are at hand, so they are verified now (spec §10.5); an attached or
    // external block is verified when it is read.
    bool verify()
    {
        if (!descriptor_.checksum || is_stored_apart(descriptor_.location.kind)) {
            return true;
        }
        const block_checksum& checksum = *descriptor_.checksum;
        if (compute_digest(checksum.algorithm, descriptor_.data) != checksum.digest) {
            return fail(errc::checksum_mismatch,
                        "the " + std::string(hash_name(checksum.algorithm)) +
                            " digest of the data block differs from its checksum",
                        checksum_context_);
        }
        return true;
    }

    const unit_outline& outline_;
    const outline_element& element_;
    const block_context& context_;
    diagnostic_log& log_;
    data_block block_;
    block_descriptor descriptor_{};
    // The Data element of an embedded block, and its path.
    std::size_t data_ = no_element;
    std::string data_path_{};
    // Where the checksum, compression and subblocks attributes were found, for later diagnostics about them.
    error_context checksum_context_{};
    error_context compression_context_{};
    error_context subblocks_context_{};
};

// The own file of an available external block, or null for a block of the source of the unit. Throws unsupported_error
// with errc::unsupported_location for an external block that has not been located.
const thread_safe_source* external_file(const block_descriptor& descriptor)
{
    if (!is_external(descriptor.location.kind)) {
        return nullptr;
    }
    if (!descriptor.external) {
        throw unsupported_error(errc::unsupported_location, "the external data block has not been located");
    }
    return descriptor.external.get();
}

[[noreturn]] void throw_checksum_mismatch(const block_checksum& checksum)
{
    throw integrity_error(errc::checksum_mismatch, "the " + std::string(hash_name(checksum.algorithm)) +
                                                       " digest of the data block differs from its checksum");
}

// Reads the stored bytes of an available block into destination, which has their size, and verifies them unless verify
// is false, in pieces when there is a progress function. The errors have no context.
void read_stored_into(const thread_safe_source& source, const block_descriptor& descriptor,
                      std::span<std::byte> destination, const block_progress& progress, std::size_t piece_size,
                      bool verify = true)
{
    if (!is_stored_apart(descriptor.location.kind)) {
        std::ranges::copy(descriptor.data, destination.begin());
        if (progress) {
            progress(descriptor.data.size());
        }
        return;
    }
    const thread_safe_source* external = external_file(descriptor);
    const thread_safe_source& file = external != nullptr ? *external : source;

    if (!progress) {
        file.read(descriptor.location.position, destination);
    } else {
        for (std::size_t done = 0; done < destination.size();) {
            const std::size_t piece = std::min(std::max<std::size_t>(piece_size, 1), destination.size() - done);
            file.read(descriptor.location.position + done, destination.subspan(done, piece));
            done += piece;
            progress(done);
        }
    }
    // Spec §10.5 and §10.6.2: nothing is used, and so nothing is decompressed, before the digest matches. Bytes that
    // fail it do not stay in the memory of the caller either.
    if (verify && descriptor.checksum &&
        compute_digest(descriptor.checksum->algorithm, destination) != descriptor.checksum->digest) {
        std::ranges::fill(destination, std::byte{0});
        throw_checksum_mismatch(*descriptor.checksum);
    }
}

// The progress of the decompression of a block, which the progress of the block counts after the stored bytes read.
subblock_progress decompression_progress(const block_progress& progress, std::uint64_t read)
{
    if (!progress) {
        return {};
    }
    return [&progress, read](std::uint64_t done) { progress(read + done); };
}

// The size of the stored bytes of an available block, which a read that holds them allocates: an attached or external
// block must be within the allocation limit.
std::size_t stored_allocation(const block_descriptor& descriptor, const limits& limits)
{
    const std::uint64_t size = stored_size(descriptor);
    if (is_stored_apart(descriptor.location.kind)) {
        check_allocation(size, limits, "the data block has");
    }
    return checked_cast<std::size_t>(size);
}

// The stored bytes of an available block, verified unless verify is false, read in pieces when there is a progress
// function. The errors have no context.
std::vector<std::byte> read_stored_bytes(const thread_safe_source& source, const block_descriptor& descriptor,
                                         const limits& limits, const block_progress& progress, std::size_t piece_size,
                                         bool verify = true)
{
    std::vector<std::byte> data(stored_allocation(descriptor, limits));
    read_stored_into(source, descriptor, data, progress, piece_size, verify);
    return data;
}

// The stored bytes of an available compressed block, as read_stored_bytes() returns them, in a buffer that is only
// decompressed from.
scratch_buffer read_compressed_bytes(const thread_safe_source& source, const block_descriptor& descriptor,
                                     const limits& limits, const block_progress& progress, std::size_t piece_size,
                                     bool verify = true)
{
    scratch_buffer stored(stored_allocation(descriptor, limits));
    read_stored_into(source, descriptor, stored.bytes(), progress, piece_size, verify);
    return stored;
}

// The data of an available block, as read_block_data() returns them, verified unless verify is false.
std::vector<std::byte> read_data(const thread_safe_source& source, const block_descriptor& descriptor,
                                 const limits& limits, const block_progress& progress, std::size_t piece_size,
                                 bool verify)
{
    if (!descriptor.compression) {
        return read_stored_bytes(source, descriptor, limits, progress, piece_size, verify);
    }
    const scratch_buffer stored = read_compressed_bytes(source, descriptor, limits, progress, piece_size, verify);
    return decompress_block(stored.bytes(), *descriptor.compression, limits,
                            decompression_progress(progress, stored.bytes().size()));
}

[[noreturn]] void throw_unavailable(const data_block& block)
{
    if (block.problem) {
        throw_unit_error(block.problem->code, block.problem->message, block.problem->context, block.origin);
    }
    throw usage_error(errc::invalid_argument, "the data block has neither a descriptor nor a problem",
                      {.element = block.path});
}

// The values of the API that name a location, a codec and a checksum algorithm. The block_location of the API is not
// the one of this namespace, which holds the value of a location attribute.

openxisf::block_location api_location(location_kind kind) noexcept
{
    switch (kind) {
    case location_kind::inline_data:
        return openxisf::block_location::inline_data;
    case location_kind::embedded:
        return openxisf::block_location::embedded;
    case location_kind::attachment:
        return openxisf::block_location::attachment;
    case location_kind::url:
    case location_kind::path:
        break;
    }
    return openxisf::block_location::external;
}

codec api_codec(compression_codec value) noexcept
{
    switch (value) {
    case compression_codec::zlib:
        return codec::zlib;
    case compression_codec::lz4:
        return codec::lz4;
    case compression_codec::lz4hc:
        return codec::lz4hc;
    case compression_codec::zstd:
        break;
    }
    return codec::zstd;
}

checksum_algorithm api_algorithm(hash_algorithm algorithm) noexcept
{
    switch (algorithm) {
    case hash_algorithm::sha1:
        return checksum_algorithm::sha1;
    case hash_algorithm::sha256:
        return checksum_algorithm::sha256;
    case hash_algorithm::sha512:
        return checksum_algorithm::sha512;
    case hash_algorithm::sha3_256:
        return checksum_algorithm::sha3_256;
    case hash_algorithm::sha3_512:
        break;
    }
    return checksum_algorithm::sha3_512;
}

} // namespace

std::vector<data_block> describe_blocks(const unit_outline& outline, const block_context& context, diagnostic_log& log)
{
    std::vector<data_block> blocks;
    for (std::size_t index = 0; index < outline.elements.size(); ++index) {
        if (serializes_blocks(outline.elements[index].kind)) {
            if (std::optional<data_block> block = element_block(outline, index, context, log).describe()) {
                blocks.push_back(std::move(*block));
            }
        }
    }
    return blocks;
}

void check_available(const data_block& block)
{
    if (!block.descriptor) {
        throw_unavailable(block);
    }
}

std::vector<std::byte> read_stored_block(const thread_safe_source& source, const data_block& block,
                                         const limits& limits)
{
    if (!block.descriptor) {
        throw_unavailable(block);
    }
    const block_descriptor& descriptor = *block.descriptor;
    return with_context_of(block, [&] { return read_stored_bytes(source, descriptor, limits, {}, 0); });
}

std::vector<std::byte> read_block(const thread_safe_source& source, const data_block& block, const limits& limits,
                                  const block_progress& progress, std::size_t piece_size, bool verify)
{
    if (!block.descriptor) {
        throw_unavailable(block);
    }
    const block_descriptor& descriptor = *block.descriptor;
    return with_context_of(block, [&] { return read_data(source, descriptor, limits, progress, piece_size, verify); });
}

void read_block_into(const thread_safe_source& source, const data_block& block, const limits& limits,
                     std::span<std::byte> destination, const block_progress& progress, std::size_t piece_size,
                     bool verify)
{
    if (!block.descriptor) {
        throw_unavailable(block);
    }
    const block_descriptor& descriptor = *block.descriptor;
    if (destination.size() != data_size(descriptor)) {
        throw usage_error(errc::invalid_argument,
                          "the destination has " + std::to_string(destination.size()) + " bytes, and the data block " +
                              std::to_string(data_size(descriptor)),
                          {.element = block.path});
    }
    with_context_of(block, [&] {
        if (!descriptor.compression) {
            read_stored_into(source, descriptor, destination, progress, piece_size, verify);
            return;
        }
        const scratch_buffer stored = read_compressed_bytes(source, descriptor, limits, progress, piece_size, verify);
        decompress_block_into(stored.bytes(), *descriptor.compression, limits, destination,
                              decompression_progress(progress, stored.bytes().size()));
    });
}

std::vector<std::byte> read_block_data(const thread_safe_source& source, const block_descriptor& descriptor,
                                       const limits& limits, const block_progress& progress, std::size_t piece_size)
{
    return read_data(source, descriptor, limits, progress, piece_size, true);
}

void read_stored_part(const thread_safe_source& source, const block_descriptor& descriptor, std::uint64_t offset,
                      std::span<std::byte> destination)
{
    const thread_safe_source* external = external_file(descriptor);
    (external != nullptr ? *external : source).read(descriptor.location.position + offset, destination);
}

std::span<const std::byte> stored_in_memory(const thread_safe_source& source, const block_descriptor& descriptor)
{
    if (!is_stored_apart(descriptor.location.kind)) {
        return descriptor.data;
    }
    const thread_safe_source* external = external_file(descriptor);
    const thread_safe_source& file = external != nullptr ? *external : source;
    if (file.bytes().empty()) {
        return {};
    }
    check_read_range(file.size(), descriptor.location.position, checked_cast<std::size_t>(descriptor.location.size),
                     file.description());
    return file.bytes().subspan(checked_cast<std::size_t>(descriptor.location.position),
                                checked_cast<std::size_t>(descriptor.location.size));
}

void verify_stored(const block_descriptor& descriptor, std::span<const std::byte> stored)
{
    if (descriptor.checksum && compute_digest(descriptor.checksum->algorithm, stored) != descriptor.checksum->digest) {
        throw_checksum_mismatch(*descriptor.checksum);
    }
}

scratch_buffer read_stored_buffer(const thread_safe_source& source, const block_descriptor& descriptor,
                                  const limits& limits, const block_progress& progress, std::size_t piece_size)
{
    return read_compressed_bytes(source, descriptor, limits, progress, piece_size);
}

std::uint64_t stored_size(const block_descriptor& descriptor) noexcept
{
    return is_stored_apart(descriptor.location.kind) ? descriptor.location.size : descriptor.data.size();
}

std::uint64_t data_size(const block_descriptor& descriptor) noexcept
{
    return descriptor.compression ? descriptor.compression->uncompressed_size : stored_size(descriptor);
}

std::uint64_t work_size(const block_descriptor& descriptor) noexcept
{
    // The stored size is within the source, but a hostile uncompressed size can take the sum beyond 64 bits; it then
    // saturates, since a read of such a block fails before its progress matters.
    const std::uint64_t stored = stored_size(descriptor);
    if (!descriptor.compression) {
        return stored;
    }
    const std::uint64_t uncompressed = descriptor.compression->uncompressed_size;
    return uncompressed > std::numeric_limits<std::uint64_t>::max() - stored ? std::numeric_limits<std::uint64_t>::max()
                                                                             : stored + uncompressed;
}

block_info info_of(const block_descriptor& descriptor) noexcept
{
    block_info info{.location = api_location(descriptor.location.kind),
                    .byte_order = descriptor.order == byte_order::big ? std::endian::big : std::endian::little,
                    .stored_size = stored_size(descriptor),
                    .uncompressed_size = data_size(descriptor)};
    if (descriptor.compression) {
        const block_compression& compression = *descriptor.compression;
        info.codec = api_codec(compression.codec);
        info.byte_shuffle = compression.item_size != 0;
        if (compression.subblocks.empty()) {
            info.subblock_size = compression.uncompressed_size;
            info.subblock_count = 1;
        } else {
            info.subblock_size =
                std::ranges::max(compression.subblocks, {}, &subblock::uncompressed_size).uncompressed_size;
            info.subblock_count = compression.subblocks.size();
        }
    }
    if (descriptor.checksum) {
        info.checksum = api_algorithm(descriptor.checksum->algorithm);
    }
    return info;
}

} // namespace openxisf::detail
