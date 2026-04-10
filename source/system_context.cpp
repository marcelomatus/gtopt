/**
 * @file      system_context.cpp
 * @brief     Implementation of SystemContext initialization
 * @date      Thu Jun 19 11:13:31 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module implements the SystemContext constructor, which wires together
 * the simulation, system LP, cost helpers, and element collection pointers.
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
    : FlatHelper(simulation)
    , CostHelper(
          simulation.options(), simulation.scenarios(), simulation.stages())
    , m_simulation_(simulation)
    , m_system_(system)
{
  // Populate m_collection_ptrs_: one void* per LP element type.
  // std::apply decomposes the collections tuple, and the C++20 template
  // lambda matches each Collection<Ts> by type.  lp_type_index_v<Ts> maps
  // each element type to its slot in m_collection_ptrs_, so the ordering in
  // SystemLP::collections_t need not match lp_element_types_t.
  // The fold `((ptr[i] = &coll), ...)` stores each collection's address into
  // the compile-time-indexed slot; the parens ensure sequenced evaluation.
  std::apply([this]<typename... Ts>(const Collection<Ts>&... colls) noexcept
             { ((m_collection_ptrs_[lp_type_index_v<Ts>] = &colls), ...); },
             system.collections());
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

}  // namespace gtopt
