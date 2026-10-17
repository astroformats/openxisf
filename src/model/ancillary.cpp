// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include "model/ancillary.h"

#include <openxisf/error.h>

#include "core/quote.h"
#include "model/ancillary_attributes.h"

#include <cmath>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace openxisf::detail {

namespace {

class ancillary_reader
{
public:
    ancillary_reader(const unit_outline& outline, const std::vector<data_block>& blocks,
                     const thread_safe_source& source, const limits& limits, ancillary_budget& budget, bool load_blocks,
                     diagnostic_log& log)
        : outline_(outline), source_(source), limits_(limits), budget_(budget), load_blocks_(load_blocks), log_(log)
    {
        for (const data_block& block : blocks) {
            if (outline.elements[block.element].kind == element_kind::icc_profile) {
                block_of_.emplace(block.element, &block);
            }
        }
    }

    // The elements are returned as a prvalue: the move constructor of std::unordered_map may throw with some standard
    // libraries, so the struct that holds them is never moved.
    ancillary_elements read()
    {
        std::unordered_map<std::size_t, fits_keyword> keywords;
        std::unordered_map<std::size_t, std::vector<std::byte>> icc_profiles;
        std::unordered_map<std::size_t, rgb_working_space> working_spaces;
        std::unordered_map<std::size_t, display_function> display_functions;
        std::unordered_map<std::size_t, color_filter_array> filters;
        std::unordered_map<std::size_t, resolution> resolutions;
        for (std::size_t index = 0; index < outline_.elements.size(); ++index) {
            const element_kind kind = outline_.elements[index].kind;
            index_ = index;
            node_ = outline_.elements[index].node;
            path_.clear();
            switch (kind) {
            case element_kind::fits_keyword:
                keep(keywords, index, read_keyword());
                break;
            case element_kind::icc_profile:
                keep(icc_profiles, index, read_icc_profile(index));
                break;
            case element_kind::rgb_working_space:
                keep(working_spaces, index, read_working_space());
                break;
            case element_kind::display_function:
                keep(display_functions, index, read_display_function());
                break;
            case element_kind::color_filter_array:
                keep(filters, index, read_filter());
                break;
            case element_kind::resolution:
                keep(resolutions, index, read_resolution());
                break;
            default:
                break;
            }
        }
        return {.keywords = std::move(keywords),
                .icc_profiles = std::move(icc_profiles),
                .working_spaces = std::move(working_spaces),
                .display_functions = std::move(display_functions),
                .filters = std::move(filters),
                .resolutions = std::move(resolutions)};
    }

private:
    template <typename T>
    static void keep(std::unordered_map<std::size_t, T>& values, std::size_t index, std::optional<T> value)
    {
        if (value) {
            values.emplace(index, std::move(*value));
        }
    }

    // The path of the element being read, built only for diagnostics.
    const std::string& path()
    {
        if (path_.empty()) {
            path_ = outline_.path(index_);
        }
        return path_;
    }

    [[nodiscard]] error_context context(const char* attribute)
    {
        return {.element = path(), .attribute = attribute};
    }

    // The text of a mandatory attribute, or nothing when it is missing, which is an error with code.
    std::optional<std::string_view> required(const char* name, errc code)
    {
        const pugi::xml_attribute attribute = node_.attribute(name);
        if (attribute.empty()) {
            log_.error(code, "the " + std::string(node_.name()) + " element has no " + name + " attribute",
                       context(name));
            return std::nullopt;
        }
        return attribute.value();
    }

    // What parse returns for the text of the mandatory attribute name, or nothing when the attribute is missing or
    // cannot be read, which is an error with code.
    template <typename Parse>
    std::optional<std::invoke_result_t<Parse, std::string_view>> parse_required(const char* name, errc code,
                                                                                Parse parse)
    {
        const std::optional<std::string_view> text = required(name, code);
        if (!text) {
            return std::nullopt;
        }
        try {
            return parse(*text);
        } catch (const invalid_data_error& failure) {
            log_.error(failure.code(), failure.what(), context(name));
        }
        return std::nullopt;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Spec §11.6: a FITS keyword is kept as written, but for the padding of its name; only a missing name leaves
    // nothing to keep.

    std::optional<fits_keyword> read_keyword()
    {
        const std::optional<std::string_view> name = required("name", errc::invalid_fits_keyword);
        if (!name) {
            return std::nullopt;
        }
        // Names are left-justified without spaces inside, so trailing spaces are the padding of an encoder that wrote
        // eight characters, which the first edition of the specification did not forbid (spec §11.6.1).
        const std::string_view unpadded = name->substr(0, name->find_last_not_of(' ') + 1);
        fits_keyword keyword{.name = std::string(unpadded)};
        if (keyword.name.empty()) {
            // The blank keyword of FITS, which the XML schema of XISF does not allow. It is kept, like the other names
            // that break the rules, and the writer refuses it.
            log_.warning(errc::invalid_fits_keyword,
                         "the name of the FITS keyword is empty, the blank keyword of FITS, which XISF does not allow: "
                         "the name has one to eight characters",
                         context("name"));
        } else if (!is_fits_keyword_name(keyword.name)) {
            log_.warning(errc::invalid_fits_keyword,
                         quote(*name) + " is not a FITS keyword name of at most eight upper-case letters, "
                                        "digits, hyphens and underscores, without padding",
                         context("name"));
        } else if (unpadded.size() != name->size()) {
            log_.warning(errc::invalid_fits_keyword,
                         "the FITS keyword name " + quote(*name) +
                             " is padded with spaces, which XISF does not allow, and is read without them",
                         context("name"));
        }
        keyword.value = optional_text("value");
        keyword.comment = optional_text("comment");
        if (is_commentary_keyword(keyword.name) && !keyword.value.empty()) {
            log_.warning(errc::invalid_fits_keyword,
                         "a " + (keyword.name.empty() ? std::string("blank") : keyword.name) +
                             " keyword has no value, and its text belongs in its comment",
                         context("value"));
        }
        return keyword;
    }

    // The value and comment attributes are mandatory, but an empty text means the same as a missing one. Text that FITS
    // cannot hold is kept, and the writer refuses it.
    std::string optional_text(const char* name)
    {
        const pugi::xml_attribute attribute = node_.attribute(name);
        if (attribute.empty()) {
            log_.warning(errc::invalid_fits_keyword,
                         "the FITSKeyword element has no " + std::string(name) + " attribute, which is read as empty",
                         context(name));
        }
        std::string text = attribute.value();
        if (!is_fits_keyword_text(text)) {
            log_.warning(errc::invalid_fits_keyword,
                         "the " + std::string(name) +
                             " of the FITS keyword has characters other than printable ASCII, which FITS does not "
                             "allow (FITS 4.0 §4.1.1)",
                         context(name));
        }
        return text;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Spec §11.7: the profile is a data block, kept unaltered, even when it is not an ICC profile.

    std::optional<std::vector<std::byte>> read_icc_profile(std::size_t index)
    {
        const auto block = block_of_.find(index);
        if (block == block_of_.end()) {
            log_.error(errc::invalid_icc_profile, "the ICCProfile element has no data block", context("location"));
            return std::nullopt;
        }
        if (!block->second->descriptor) {
            // The block is unavailable, which describe_blocks() reported.
            return std::nullopt;
        }
        if (!load_blocks_) {
            return std::vector<std::byte>{};
        }
        std::optional<std::vector<std::byte>> profile =
            load_block(source_, *block->second->descriptor, limits_, budget_, path(), log_);
        // The profile is kept as it is, and the writer refuses it.
        if (profile && !profile->empty() && !has_icc_profile_header(*profile)) {
            log_.warning(errc::invalid_icc_profile,
                         "the ICC profile does not start with the header of an ICC profile, 128 bytes with the "
                         "signature 'acsp'",
                         {.element = path()});
        }
        return profile;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Spec §8.5.4.1, §11.8

    std::optional<rgb_working_space> read_working_space()
    {
        constexpr errc code = errc::invalid_rgb_working_space;
        const auto triplet = [](std::string_view text) { return parse_triplet(text, code); };
        const std::optional<std::optional<double>> gamma = parse_required("gamma", code, parse_gamma);
        if (!gamma) {
            return std::nullopt;
        }
        const std::optional<std::array<double, 3>> x = parse_required("x", code, triplet);
        if (!x) {
            return std::nullopt;
        }
        const std::optional<std::array<double, 3>> y = parse_required("y", code, triplet);
        if (!y) {
            return std::nullopt;
        }
        const std::optional<std::array<double, 3>> luminance = parse_required("Y", code, triplet);
        if (!luminance) {
            return std::nullopt;
        }
        rgb_working_space space{
            .gamma = *gamma, .x = *x, .y = *y, .luminance = *luminance, .name = node_.attribute("name").value()};
        std::array<double, 3> derived{};
        try {
            derived = check_rgb_working_space(space);
        } catch (const invalid_data_error& failure) {
            log_.error(failure.code(), failure.what(), {.element = path()});
            return std::nullopt;
        }
        // Spec §11.8.1: decoders may verify the luminance coefficients; those written are kept.
        for (std::size_t i = 0; i < 3; ++i) {
            if (std::abs(space.luminance[i] - derived[i]) > luminance_tolerance) {
                log_.warning(code,
                             "the luminance coefficients differ from those that the chromaticity coordinates give "
                             "relative to D50, " +
                                 std::to_string(derived[0]) + ":" + std::to_string(derived[1]) + ":" +
                                 std::to_string(derived[2]),
                             context("Y"));
                break;
            }
        }
        // The writer writes the derived ones, which must be in [0, 1], and refuses the element otherwise.
        if (!derived_luminance_in_range(derived)) {
            log_.warning(code,
                         "the luminance coefficients that the chromaticity coordinates give relative to D50, " +
                             std::to_string(derived[0]) + ":" + std::to_string(derived[1]) + ":" +
                             std::to_string(derived[2]) + ", are not in [0, 1]",
                         context("Y"));
        }
        return space;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Spec §8.5.6, §11.9

    std::optional<display_function> read_display_function()
    {
        constexpr errc code = errc::invalid_display_function;
        const auto quadruplet = [](std::string_view text) { return parse_quadruplet(text, code); };
        display_function function{.name = node_.attribute("name").value()};
        const std::array<std::pair<const char*, std::array<double, 4>*>, 5> parameters{{
            {"m", &function.midtones},
            {"s", &function.shadows},
            {"h", &function.highlights},
            {"l", &function.shadows_expansion},
            {"r", &function.highlights_expansion},
        }};
        for (const auto& [name, values] : parameters) {
            const std::optional<std::array<double, 4>> read = parse_required(name, code, quadruplet);
            if (!read) {
                return std::nullopt;
            }
            *values = *read;
        }
        try {
            check_display_function(function);
        } catch (const invalid_data_error& failure) {
            log_.error(failure.code(), failure.what(), {.element = path()});
            return std::nullopt;
        }
        return function;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Spec §11.10

    std::optional<color_filter_array> read_filter()
    {
        constexpr errc code = errc::invalid_color_filter_array;
        const std::optional<std::string_view> pattern = required("pattern", code);
        if (!pattern) {
            return std::nullopt;
        }
        const std::optional<std::uint64_t> width = parse_required("width", code, parse_cfa_size);
        if (!width) {
            return std::nullopt;
        }
        const std::optional<std::uint64_t> height = parse_required("height", code, parse_cfa_size);
        if (!height) {
            return std::nullopt;
        }
        color_filter_array filter{.pattern = std::string(*pattern),
                                  .width = *width,
                                  .height = *height,
                                  .name = node_.attribute("name").value()};
        try {
            check_color_filter_array(filter);
        } catch (const invalid_data_error& failure) {
            log_.error(failure.code(), failure.what(), context("pattern"));
            return std::nullopt;
        }
        return filter;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Spec §11.11

    std::optional<resolution> read_resolution()
    {
        constexpr errc code = errc::invalid_resolution;
        const std::optional<double> horizontal = parse_required("horizontal", code, parse_resolution_value);
        if (!horizontal) {
            return std::nullopt;
        }
        const std::optional<double> vertical = parse_required("vertical", code, parse_resolution_value);
        if (!vertical) {
            return std::nullopt;
        }
        resolution result{.horizontal = *horizontal, .vertical = *vertical};
        if (const pugi::xml_attribute unit = node_.attribute("unit"); !unit.empty()) {
            try {
                result.unit = parse_resolution_unit(unit.value());
            } catch (const invalid_data_error& failure) {
                log_.error(failure.code(), failure.what(), context("unit"));
                return std::nullopt;
            }
        }
        return result;
    }

    const unit_outline& outline_;
    const thread_safe_source& source_;
    const limits& limits_;
    ancillary_budget& budget_;
    bool load_blocks_;
    diagnostic_log& log_;
    // The data block of each ICCProfile element that has one.
    std::unordered_map<std::size_t, const data_block*> block_of_{};
    // The element being read, and its path once a diagnostic needed it.
    std::size_t index_ = no_element;
    pugi::xml_node node_{};
    std::string path_{};
};

} // namespace

ancillary_elements read_ancillary(const unit_outline& outline, const std::vector<data_block>& blocks,
                                  const thread_safe_source& source, const limits& limits, ancillary_budget& budget,
                                  bool load_blocks, diagnostic_log& log)
{
    return ancillary_reader(outline, blocks, source, limits, budget, load_blocks, log).read();
}

} // namespace openxisf::detail
