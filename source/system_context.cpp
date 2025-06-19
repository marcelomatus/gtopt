#include <ranges>

#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/view/all.hpp>
#include <range/v3/view/filter.hpp>

namespace
{
using namespace gtopt;

}  // namespace

namespace gtopt
{
SystemContext::SystemContext(SimulationLP& simulation, SystemLP& system)
    : LabelMaker(
          simulation.options(), simulation.scenarios(), simulation.stages())
    , FlatHelper(simulation,
                 simulation.scenarios()
                     | std::views::filter(&ScenarioLP::is_active)
                     | std::views::transform(&ScenarioLP::uid)
                     | std::ranges::to<std::vector>(),
                 simulation.stages() | std::views::filter(&StageLP::is_active)
                     | std::views::transform(&StageLP::uid)
                     | std::ranges::to<std::vector>(),
                 active_stage_block_indices(simulation.stages()),
                 active_block_indices(simulation.stages()))
    , CostHelper(
          simulation.options(), simulation.scenarios(), simulation.stages())
    , m_simulation_(simulation)
    , m_system_(system)
{
  if (options().use_single_bus()) {
    const auto& buses = system.elements<BusLP>();
    if (!buses.empty()) {
      m_single_bus_id_.emplace(buses.front().uid());
    }
  }
}

auto SystemContext::get_bus_index(const ObjectSingleId<BusLP>& id) const
    -> ElementIndex<BusLP>
{
  return system().element_index(m_single_bus_id_.value_or(id));
}

auto SystemContext::get_bus(const ObjectSingleId<BusLP>& id) const
    -> const BusLP&
{
  return system().element(get_bus_index(id));
}

}  // namespace gtopt
