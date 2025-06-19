#include <ranges>

#include <gtopt/simulation_lp.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/view/all.hpp>
#include <range/v3/view/filter.hpp>

namespace
{
using namespace gtopt;

/**
 * Creates a vector of active indices from a container.
 * Uses move semantics for efficient return value planning and
 * is marked noexcept to enable compiler plannings.
 *
 * @param container The container to extract indices from
 * @return Vector of active indices, moved rather than copied
 */

template<std::ranges::input_range Stages, typename Index = BlockUid>
constexpr auto active_block_indices(const Stages& stages) noexcept
{
    return stages
        | std::views::filter(&StageLP::is_active)    // Filter active stages
        | std::views::transform(std::mem_fn(&StageLP::blocks)) // Get blocks ranges
        | std::views::join                          // Flatten nested ranges
        | std::views::transform(&BlockLP::uid)      // Extract UIDs
        | std::ranges::to<std::vector<Index>>();    // Collect directly to vector
}

template<std::ranges::input_range Stages, typename Index = BlockUid>
constexpr auto active_stage_block_indices(const Stages& stages) noexcept
{
    return stages
        | std::views::filter(&StageLP::is_active)
        | std::views::transform(
            [](const auto& stage) {
                return stage.blocks()
                    | std::views::transform(&BlockLP::uid)
                    | std::ranges::to<std::vector<Index>>();
            })
        | std::ranges::to<std::vector>();
}

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
                 active_stage_block_indices<BlockUid>(simulation.stages()),
                 active_block_indices<BlockUid>(simulation.stages()))
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
