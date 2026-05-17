/**
 * @file      fuel_lp.cpp
 * @brief     Implementation of FuelLP (parameter carrier only)
 * @date      2026-05-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `FuelLP` resolves the three Fuel schedules (price, combustion EF,
 * upstream EF) at construction.  It contributes no LP variables or
 * rows — `add_to_lp` and `add_to_output` are inline no-ops in the
 * header.  GeneratorLP consumes the resolved schedules via the
 * `param_*` accessors when its `Generator.fuel` field references a
 * Fuel element.
 */

#include <gtopt/fuel_lp.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

FuelLP::FuelLP(const Fuel& fuel, const InputContext& ic)
    : Base(fuel, ic, Element::class_name)
    , price_(ic, Element::class_name, id(), std::move(object().price))
    , heat_content_(
          ic, Element::class_name, id(), std::move(object().heat_content))
    , combustion_ef_(ic,
                     Element::class_name,
                     id(),
                     std::move(object().combustion_emission_factor))
    , upstream_ef_(ic,
                   Element::class_name,
                   id(),
                   std::move(object().upstream_emission_factor))
{
}

}  // namespace gtopt
