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

/**
 * Creates a structure of scenario, stage, and block UIDs.
 * Leverages move semantics for all vectors to avoid unnecessary copies
 * and is marked noexcept to enable compiler plannings.
 *
 * @param sc System context containing scenarios, stages, and blocks
 * @param op Optional operation to filter elements
 * @return Struct containing vectors of UIDs, using move semantics
 */
template<typename SystemContext, typename Operation = decltype(true_fnc)>
constexpr STBUids make_stb_uids(const SystemContext& sc,
                                Operation op = true_fnc) noexcept
{
  const auto size = sc.scenarios().size() * sc.blocks().size();
  std::vector<Uid> scenario_uids;
  scenario_uids.reserve(size);
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);
  std::vector<Uid> block_uids;
  block_uids.reserve(size);

  for (auto&& scenario :
       sc.simulation().scenarios() | ranges::views::filter(op))
  {
    for (auto&& stage : sc.simulation().stages() | ranges::views::filter(op)) {
      for (auto&& block : stage.blocks()) {
        scenario_uids.push_back(scenario.uid());
        stage_uids.push_back(stage.uid());
        block_uids.push_back(block.uid());
      }
    }
  }

  scenario_uids.shrink_to_fit();
  stage_uids.shrink_to_fit();
  block_uids.shrink_to_fit();

  // Explicitly move all vectors into the return struct to avoid copying
  return {
      std::move(scenario_uids), std::move(stage_uids), std::move(block_uids)};
}

template<typename SystemContext, typename Operation = decltype(true_fnc)>
constexpr STUids make_st_uids(const SystemContext& sc,
                              Operation op = true_fnc) noexcept
{
  const auto size = sc.scenarios().size() * sc.stages().size();
  std::vector<Uid> scenario_uids;
  scenario_uids.reserve(size);
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);

  for (auto&& scenario :
       sc.simulation().scenarios() | ranges::views::filter(op))
  {
    for (auto&& stage : sc.simulation().stages() | ranges::views::filter(op)) {
      scenario_uids.push_back(scenario.uid());
      stage_uids.push_back(stage.uid());
    }
  }

  scenario_uids.shrink_to_fit();
  stage_uids.shrink_to_fit();

  return {std::move(scenario_uids), std::move(stage_uids)};
}

template<typename SystemContext, typename Operation = decltype(true_fnc)>
constexpr TUids make_t_uids(const SystemContext& sc,
                            Operation op = true_fnc) noexcept
{
  const auto size = sc.stages().size();
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);

  for (auto&& stage : sc.simulation().stages() | ranges::views::filter(op)) {
    stage_uids.push_back(stage.uid());
  }

  stage_uids.shrink_to_fit();

  return stage_uids;
}

}  // namespace

namespace gtopt
{

auto SystemContext::stb_active_uids() const -> STBUids
{
  return make_stb_uids(*this, active_fnc);
}

auto SystemContext::stb_uids() const -> STBUids
{
  return make_stb_uids(*this);
}

auto SystemContext::st_active_uids() const -> STUids
{
  return make_st_uids(*this, active_fnc);
}

auto SystemContext::st_uids() const -> STUids
{
  return make_st_uids(*this);
}

auto SystemContext::t_active_uids() const -> TUids
{
  return make_t_uids(*this, active_fnc);
}

auto SystemContext::t_uids() const -> TUids
{
  return make_t_uids(*this);
}

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
