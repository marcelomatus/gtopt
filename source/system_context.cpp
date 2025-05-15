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
template<typename Index, typename Container>
constexpr auto active_indices(const Container& container) noexcept
{
  return std::ranges::views::transform(enumerate_active<Index>(container),
                                       [](const auto& pair)
                                       { return pair.first; })
      | std::ranges::to<std::vector<Index>>();
}

template<typename Index, typename Stage>
constexpr auto active_block_indices(const Stage& stages) noexcept
{
  std::vector<Index> indices;
  Index idx {};

  for (const auto& stage : stages) {
    if (stage.is_active()) {
      const auto block_count = stage.blocks().size();
      const auto block_indices =
          std::views::iota(idx, idx + static_cast<Index>(block_count));
      indices.insert(indices.end(), block_indices.begin(), block_indices.end());
    }
    idx += static_cast<Index>(stage.blocks().size());
  }

  return indices;
}

template<typename Index, typename Stages>
auto active_stage_block_indices(const Stages& stages) noexcept
{
  return enumerate_active<Index>(stages)
      | std::views::transform(
             [](const auto& pair)
             {
               const auto& blocks = pair.second.blocks();
               return std::views::iota(Index {0},
                                       static_cast<Index>(blocks.size()))
                   | std::ranges::to<std::vector<Index>>();
             })
      | std::ranges::to<std::vector<std::vector<Index>>>();
}


}  // namespace

namespace gtopt
{


SystemContext::SystemContext(SimulationLP& psimulation, SystemLP& psystem)
    : LabelMaker(
          psimulation.options(), psimulation.scenarios(), psimulation.stages())
    , FlatHelper(active_indices<ScenarioIndex>(psimulation.scenarios()),
                 active_indices<StageIndex>(psimulation.stages()),
                 active_stage_block_indices<BlockIndex>(psimulation.stages()),
                 active_block_indices<BlockIndex>(psimulation.stages()))
    , CostHelper(
          psimulation.options(), psimulation.scenarios(), psimulation.stages())
    , m_simulation_(psimulation)
    , m_system_(psystem)
{
  if (options().use_single_bus()) {
    const auto& buses = system().elements<BusLP>();
    if (!buses.empty()) {
      m_single_bus_id_ = buses.front().uid();
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
