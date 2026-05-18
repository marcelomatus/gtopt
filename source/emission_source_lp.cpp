/**
 * @file      emission_source_lp.cpp
 * @brief     Implementation of EmissionSourceLP (parameter carrier only)
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 */

#include <gtopt/emission_source_lp.hpp>
#include <gtopt/system_context.hpp>

namespace gtopt
{

EmissionSourceLP::EmissionSourceLP(const EmissionSource& src,
                                   const InputContext& ic)
    : Base(src, ic, Element::class_name)
    , rate_(ic, Element::class_name, id(), std::move(object().rate))
{
}

}  // namespace gtopt
