/**
 * @file      system_context.cpp
 * @brief     Header of
 * @date      Thu Jun 19 11:13:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module
 */

#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <spdlog/spdlog.h>

namespace
{
using namespace gtopt;

}  // namespace

namespace gtopt
{

SystemContext::SystemContext(SimulationLP& simulation, SystemLP& system)
    : LabelMaker(simulation.options())
    , FlatHelper(simulation)
    , CostHelper(
          simulation.options(), simulation.scenarios(), simulation.stages())
    , m_simulation_(simulation)
    , m_system_(system)
{
}

auto SystemContext::get_bus_index(const ObjectSingleId<BusLP>& id) const
    -> ElementIndex<BusLP>
{
  return system().element_index(system().single_bus_id().value_or(id));
}

auto SystemContext::get_bus(const ObjectSingleId<BusLP>& id) const
    -> const BusLP&
{
  try {
    return system().element(get_bus_index(id));
  } catch (const std::out_of_range& e) {
    SPDLOG_ERROR(
        std::format("Bus with ID {} not found: {}", id.uid(), e.what()));
    throw;
  }
}

// ──────────────────────────────────────────────────────────────────────────────
// Template definitions for get_element()
//
// Kept in this TU (which includes system_lp.hpp) so that *_lp.cpp call sites
// only need the forward declarations + explicit instantiation references from
// system_context.hpp, never system_lp.hpp itself.  Explicit instantiation
// definitions below pull the symbols into the binary for every LP component
// type that callers might request.
// ──────────────────────────────────────────────────────────────────────────────

template<typename Element>
auto SystemContext::get_element(const ObjectSingleId<Element>& id) const
    -> const Element&
{
  return system().element(id);
}

// BusLP: ObjectSingleId must honour the single-bus override.
template<>
auto SystemContext::get_element(const ObjectSingleId<BusLP>& id) const
    -> const BusLP&
{
  return get_bus(id);
}

template<typename Element>
auto SystemContext::get_element(const ElementIndex<Element>& id) const
    -> const Element&
{
  return system().element(id);
}

// Explicit instantiations — one macro call per LP component type.
// BusLP: ObjectSingleId is handled by the explicit specialisation above,
// so only ElementIndex<BusLP> is instantiated here via the macro.
#define INSTANTIATE_GET_ELEMENT(T) \
  template auto SystemContext::get_element(const ObjectSingleId<T>&) const \
      -> const T&; \
  template auto SystemContext::get_element(const ElementIndex<T>&) const \
      -> const T&;

// BusLP ObjectSingleId is an explicit specialisation — only instantiate
// the ElementIndex overload for BusLP.
template auto SystemContext::get_element(const ElementIndex<BusLP>&) const
    -> const BusLP&;

INSTANTIATE_GET_ELEMENT(BatteryLP)
INSTANTIATE_GET_ELEMENT(ConverterLP)
INSTANTIATE_GET_ELEMENT(DemandLP)
INSTANTIATE_GET_ELEMENT(DemandProfileLP)
INSTANTIATE_GET_ELEMENT(FiltrationLP)
INSTANTIATE_GET_ELEMENT(FlowLP)
INSTANTIATE_GET_ELEMENT(GeneratorLP)
INSTANTIATE_GET_ELEMENT(GeneratorProfileLP)
INSTANTIATE_GET_ELEMENT(JunctionLP)
INSTANTIATE_GET_ELEMENT(LineLP)
INSTANTIATE_GET_ELEMENT(ReserveProvisionLP)
INSTANTIATE_GET_ELEMENT(ReserveZoneLP)
INSTANTIATE_GET_ELEMENT(ReservoirLP)
INSTANTIATE_GET_ELEMENT(TurbineLP)
INSTANTIATE_GET_ELEMENT(WaterwayLP)

#undef INSTANTIATE_GET_ELEMENT

}  // namespace gtopt
