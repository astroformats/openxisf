// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Ezequiel Ruiz

#include <openxisf/astrometry.h>
#include <openxisf/error.h>
#include <openxisf/property.h>

#include "algorithms/astrometric_model.h"
#include "model/property_catalog.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openxisf {

struct astrometric_solution::model
{
    explicit model(const property_list& properties) : data(detail::read_astrometric_model(properties)) {}

    detail::astrometric_model data;
};

namespace {

// The index of a layer in the arrays of the model, or nothing for a value that is no layer.
std::optional<std::size_t> index_of(astrometric_layer layer) noexcept
{
    const auto value = static_cast<std::size_t>(layer);
    if (value < 1 || value > 3) {
        return std::nullopt;
    }
    return value - 1;
}

} // namespace

std::string_view projection_system_name(projection_system system) noexcept
{
    switch (system) {
    case projection_system::gnomonic:
        return "Gnomonic";
    case projection_system::stereographic:
        return "Stereographic";
    case projection_system::zenithal_equal_area:
        return "ZenithalEqualArea";
    case projection_system::orthographic:
        return "Orthographic";
    case projection_system::plate_carree:
        return "PlateCarree";
    case projection_system::mercator:
        return "Mercator";
    case projection_system::hammer_aitoff:
        return "HammerAitoff";
    }
    return {};
}

astrometric_solution::astrometric_solution(const property_list& properties)
    : model_(std::make_shared<const model>(properties))
{}

std::optional<astrometric_layer> astrometric_solution::layer() const noexcept
{
    const std::array<astrometric_status, 3>& status = model_->data.status;
    if (status[0] != astrometric_status::available) {
        return std::nullopt;
    }
    if (status[1] != astrometric_status::available) {
        return astrometric_layer::linear;
    }
    if (status[2] != astrometric_status::available) {
        return astrometric_layer::projective;
    }
    return astrometric_layer::distortion;
}

astrometric_status astrometric_solution::status(astrometric_layer layer) const noexcept
{
    const std::optional<std::size_t> index = index_of(layer);
    return index ? model_->data.status[*index] : astrometric_status::absent;
}

const std::string& astrometric_solution::problem(astrometric_layer layer) const noexcept
{
    static const std::string none;
    const std::optional<std::size_t> index = index_of(layer);
    return index ? model_->data.problem[*index] : none;
}

const std::string& astrometric_solution::version() const noexcept
{
    return model_->data.version;
}

const std::optional<astrometric_projection>& astrometric_solution::projection() const noexcept
{
    return model_->data.projection;
}

const std::optional<projective_transformations>& astrometric_solution::projective() const noexcept
{
    return model_->data.projective;
}

astrometric_layer astrometric_solution::checked_layer(std::optional<astrometric_layer> layer) const
{
    if (!layer) {
        const std::string& reason = problem(astrometric_layer::linear);
        throw usage_error(errc::invalid_argument, "the astrometric solution has no layer available" +
                                                      (reason.empty() ? std::string() : ": " + reason));
    }
    if (status(*layer) != astrometric_status::available) {
        throw usage_error(errc::invalid_argument, "layer " + std::to_string(static_cast<int>(*layer)) +
                                                      " of the astrometric solution is not available");
    }
    return *layer;
}

std::optional<celestial_point> astrometric_solution::image_to_celestial(image_point point) const
{
    return detail::image_to_celestial(model_->data, point, checked_layer(layer()));
}

std::optional<celestial_point> astrometric_solution::image_to_celestial(image_point point,
                                                                        astrometric_layer layer) const
{
    return detail::image_to_celestial(model_->data, point, checked_layer(layer));
}

std::optional<image_point> astrometric_solution::celestial_to_image(celestial_point point) const
{
    return detail::celestial_to_image(model_->data, point, checked_layer(layer()));
}

std::optional<image_point> astrometric_solution::celestial_to_image(celestial_point point,
                                                                    astrometric_layer layer) const
{
    return detail::celestial_to_image(model_->data, point, checked_layer(layer));
}

std::size_t remove_astrometric_solution(property_list& properties)
{
    const auto in_namespace = [](const property& item) { return detail::is_astrometric_solution_id(item.id); };
    const auto removed = static_cast<std::size_t>(std::ranges::count_if(properties, in_namespace));
    if (removed == 0) {
        return 0;
    }
    // The other properties are copied in one pass, where erasing each property of the namespace would go through the
    // list again, and the list is replaced only then, so that a failure leaves it as it was.
    std::vector<property> kept;
    kept.reserve(properties.size() - removed);
    std::ranges::remove_copy_if(properties, std::back_inserter(kept), in_namespace);
    properties = property_list(std::move(kept));
    return removed;
}

} // namespace openxisf
