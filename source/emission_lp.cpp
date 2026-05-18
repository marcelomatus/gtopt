/**
 * @file      emission_lp.cpp
 * @brief     Implementation of EmissionLP (parameter carrier only)
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `EmissionLP` resolves the three Emission schedules (price, cap,
 * cap_cost) at construction.  It contributes no LP variables or rows
 * in Commit 1 — `add_to_lp` and `add_to_output` are inline no-ops in
 * the header.  Future commits (Layer 2) will wire `cap` to a soft LP
 * row and `price` to per-segment cost coefficients on thermal
 * generators.
 */

#include <gtopt/emission_lp.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

EmissionLP::EmissionLP(const Emission& emission, const InputContext& ic)
    : Base(emission, ic, Element::class_name)
    , price_(ic, Element::class_name, id(), std::move(object().price))
    , cap_(ic, Element::class_name, id(), std::move(object().cap))
    , cap_cost_(ic, Element::class_name, id(), std::move(object().cap_cost))
{
}

}  // namespace gtopt
