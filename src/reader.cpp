// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/reader.h>

#include "io/paths.h"
#include "model/pixels.h"
#include "model/unit.h"

#include <utility>

namespace openxisf {

struct reader::state : detail::unit
{
    using detail::unit::unit;
};

namespace {

// A unit opened from a path finds its external files from the directory of that path, unless the options say otherwise.
read_options with_resolver(read_options options, std::string_view path)
{
    if (!options.resolver) {
        options.resolver = file_resolver(detail::parent_directory(path));
    }
    return options;
}

} // namespace

reader::reader(std::string_view path, read_options options)
    : reader(std::make_unique<file_source>(path), with_resolver(std::move(options), path))
{}

reader::reader(std::unique_ptr<input_source> source, read_options options)
    : state_(std::make_unique<state>(std::move(source), std::move(options)))
{}

reader::reader(reader&& other) noexcept = default;
reader& reader::operator=(reader&& other) noexcept = default;
reader::~reader() = default;

unit_storage reader::storage() const noexcept
{
    return state_->storage;
}

signature_status reader::signature() const noexcept
{
    return state_->signature ? signature_status::not_verified : signature_status::none;
}

std::string_view reader::signature_xml() const noexcept
{
    return state_->signature ? std::string_view(state_->signature->element) : std::string_view();
}

std::string_view reader::signed_xml() const noexcept
{
    return state_->signature ? std::string_view(state_->signature->signed_element) : std::string_view();
}

const property_list& reader::metadata() const noexcept
{
    return state_->properties.metadata;
}

const property_list& reader::properties() const noexcept
{
    return state_->properties.standalone;
}

std::span<const table> reader::tables() const noexcept
{
    return state_->images.tables;
}

std::span<const image_info> reader::images() const noexcept
{
    return state_->images.infos;
}

const image_info& reader::image(std::size_t index) const
{
    return detail::image_at(*state_, index);
}

block_info reader::block(std::size_t index) const
{
    return detail::block_of(*state_, index);
}

std::vector<std::byte> reader::read_pixels(std::size_t index, const pixel_read_options& options) const
{
    return detail::read_pixels(*state_, index, options);
}

void reader::read_pixels(std::size_t index, std::span<std::byte> destination, const pixel_read_options& options) const
{
    detail::read_pixels(*state_, index, destination, options);
}

std::size_t reader::sample_count_of(std::size_t index, sample_format format) const
{
    return detail::typed_sample_count(*state_, index, format);
}

std::size_t reader::sample_count_of(std::size_t index, sample_format format, const pixel_read_options& options) const
{
    return detail::typed_sample_count(*state_, index, format, options);
}

void reader::check_sample_format(std::size_t index, sample_format format) const
{
    (void)detail::typed_image_at(*state_, index, format);
}

std::span<const diagnostic> reader::diagnostics() const noexcept
{
    return state_->diagnostics;
}

bool reader::ancillary_data_loaded() const noexcept
{
    return state_->ancillary_loaded;
}

void reader::load_ancillary_data()
{
    state_->load_ancillary_data();
}

} // namespace openxisf
