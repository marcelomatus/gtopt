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
  return system().element(get_bus_index(id));
}

}  // namespace gtopt
