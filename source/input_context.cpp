/**
 * @file      input_context.cpp
 * @brief     Implementation of InputContext and element_index instantiations
 * @date      Fri Apr 25 14:17:18 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the InputContext constructor and provides explicit
 * template instantiations of element_index for all LP element types.
 */

#include <gtopt/input_context.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>

namespace gtopt
{

InputContext::InputContext(const SystemContext& system_context)
    : ElementContext(system_context.system())
    , m_system_context_(system_context)
{
}

// ──────────────────────────────────────────────────────────────────────────────
// Template definition for element_index()
//
// Kept in this TU (which includes system_lp.hpp) so that call sites only need
// the forward declaration, never system_lp.hpp itself.
// ──────────────────────────────────────────────────────────────────────────────

template<typename Element>
auto InputContext::element_index(const ObjectSingleId<Element>& id) const
    -> ElementIndex<Element>
{
  return system_lp().element_index(id);
}

// Explicit instantiations for the element types used in *_lp.cpp constructors.
// Add more types here if new components start calling ic.element_index().
template auto InputContext::element_index(
    const ObjectSingleId<GeneratorLP>&) const -> ElementIndex<GeneratorLP>;
template auto InputContext::element_index(
    const ObjectSingleId<ReserveZoneLP>&) const -> ElementIndex<ReserveZoneLP>;

}  // namespace gtopt
