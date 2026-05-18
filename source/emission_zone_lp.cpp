/**
 * @file      emission_zone_lp.cpp
 * @brief     Implementation of EmissionZoneLP (parameter carrier only)
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <gtopt/emission_zone_lp.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

EmissionZoneLP::EmissionZoneLP(const EmissionZone& zone, const InputContext& ic)
    : Base(zone, ic, Element::class_name)
    , cap_(ic, Element::class_name, id(), std::move(object().cap))
    , cap_cost_(ic, Element::class_name, id(), std::move(object().cap_cost))
    , price_(ic, Element::class_name, id(), std::move(object().price))
{
}

}  // namespace gtopt
