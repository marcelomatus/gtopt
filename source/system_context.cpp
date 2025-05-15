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

constexpr auto cost_factor(const auto p_scale_obj,
                           const auto p_probability_factor,
                           const auto p_discount_factor,
                           const auto p_duration)
{
  return p_probability_factor * p_discount_factor * p_duration / p_scale_obj;
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

constexpr auto stage_factors(const auto& stages) noexcept
{
  std::vector<double> factors(stages.size(), 1.0);

  double discount_factor = 1.0;
  for (auto&& [ti, st] : enumerate_active(stages)) {
    factors[ti] = discount_factor;
    discount_factor *= st.discount_factor();
  }

  return factors;
}

}  // namespace

namespace gtopt
{

double SystemContext::block_cost(const ScenarioIndex& scenario_index,
                                 const StageIndex& stage_index,
                                 const BlockLP& block,
                                 const double cost) const
{
  return cost
      * cost_factor(options().scale_objective(),
                    scenario_probability_factor(scenario_index),
                    stage_discount_factor(stage_index),
                    block.duration());
}

auto SystemContext::block_cost_factors() const -> block_factor_matrix_t
{
  const auto n_scenarios = static_cast<Index>(active_scenario_count());
  const auto n_stages = static_cast<Index>(active_block_count());
  block_factor_matrix_t factors(boost::extents[n_scenarios][n_stages]);

  const auto scale_obj = options().scale_objective();

  for (auto&& [si, scenario] :
       enumerate_active<Index>(simulation().scenarios()))
  {
    const auto probability_factor = scenario.probability_factor();
    for (auto&& [ti, stage] : enumerate_active<Index>(simulation().stages())) {
      factors[si][ti].resize(stage.blocks().size());
      for (auto&& [bi, block] : enumerate<Index>(stage.blocks())) {
        const auto cfactor = cost_factor(scale_obj,
                                         probability_factor,
                                         stage_discount_factors[ti],
                                         block.duration());
        factors[si][ti][bi] = 1.0 / cfactor;
      }
    }
  }

  return factors;
}

double SystemContext::stage_cost(const StageIndex& stage_index,
                                 const double cost) const
{
  const auto probability_factor = 1.0;

  return cost
      * cost_factor(options().scale_objective(),
                    probability_factor,
                    stage_discount_factor(stage_index),
                    stage_duration(stage_index));
}

auto SystemContext::stage_cost_factors() const -> stage_factor_matrix_t
{
  const auto n_stages = static_cast<Index>(active_block_count());
  stage_factor_matrix_t factors(n_stages);

  const auto scale_obj = options().scale_objective();
  const auto probability_factor = 1.0;
  for (auto&& [ti, stage] : enumerate_active<Index>(simulation().stages())) {
    const auto cfactor = cost_factor(scale_obj,
                                     probability_factor,
                                     stage_discount_factors[ti],
                                     stage.duration());
    factors[ti] = 1.0 / cfactor;
  }

  return factors;
}

double SystemContext::scenario_stage_cost(const ScenarioIndex& scenario_index,
                                          const StageIndex& stage_index,
                                          const double cost) const
{
  return cost
      * cost_factor(options().scale_objective(),
                    scenario_probability_factor(scenario_index),
                    stage_discount_factor(stage_index),
                    stage_duration(stage_index));
}

auto SystemContext::scenario_stage_cost_factors() const
    -> scenario_stage_factor_matrix_t
{
  const auto n_scenarios = static_cast<Index>(active_scenario_count());
  const auto n_stages = static_cast<Index>(active_block_count());
  scenario_stage_factor_matrix_t factors(boost::extents[n_scenarios][n_stages]);

  const auto scale_obj = options().scale_objective();

  for (auto&& [si, scenario] :
       enumerate_active<Index>(simulation().scenarios()))
  {
    const auto probability_factor = scenario.probability_factor();
    for (auto&& [ti, stage] : enumerate_active<Index>(simulation().stages())) {
      const auto cfactor = cost_factor(scale_obj,
                                       probability_factor,
                                       stage_discount_factors[ti],
                                       stage.duration());
      factors[si][ti] = 1.0 / cfactor;
    }
  }

  return factors;
}

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
    , m_simulation_(psimulation)
    , m_system_(psystem)
    , stage_discount_factors(stage_factors(psimulation.stages()))
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
