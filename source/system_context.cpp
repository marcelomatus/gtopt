#include <gtopt/system_context.hpp>
#include <gtopt/system_lp.hpp>
#include <range/v3/view/all.hpp>
#include <range/v3/view/filter.hpp>

namespace
{
using namespace gtopt;

template<typename Index, typename Container>
constexpr auto active_indices(const Container& container)
{
  std::vector<Index> indices;
  indices.reserve(container.size());
  for (auto&& [i, e] : enumerate_active<Index>(container)) {
    indices.push_back(i);
  }
  return indices;
}

template<typename Index, typename Stage>
constexpr auto active_block_indices(const Stage& stages)
{
  std::vector<Index> indices;
  indices.reserve(stages.size());

  for (Index idx {}; const auto& s : stages) {
    for (size_t i = 0; i < s.blocks().size(); ++i) {
      if (s.is_active()) {
        indices.push_back(idx);
      }
      ++idx;
    }
  }
  return indices;
}

template<typename Index, typename Stage>
auto active_stage_block_indices(const Stage& stages)
{
  std::vector<std::vector<Index>> indices(stages.size());
  for (auto&& [i, e] : enumerate_active<Index>(stages)) {
    std::vector<Index> v;
    v.reserve(e.blocks().size());
    for (Index idx {}; idx < e.blocks().size(); ++idx) {
      v.push_back(idx);
    }

    indices[i] = std::move(v);
  }
  return indices;
}

constexpr auto cost_factor(const auto p_scale_obj,
                           const auto p_probability_factor,
                           const auto p_discount_factor,
                           const auto p_duration)
{
  return p_probability_factor * p_discount_factor * p_duration / p_scale_obj;
}

template<typename SystemContext, typename Operation = decltype(true_fnc)>
constexpr STBUids make_stb_uids(const SystemContext& sc,
                                Operation op = true_fnc)
{
  const auto size = sc.get_scenery_size() * sc.get_block_size();
  std::vector<Uid> scenery_uids;
  scenery_uids.reserve(size);
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);
  std::vector<Uid> block_uids;
  block_uids.reserve(size);

  for (auto&& scenery : sc.system().sceneries() | ranges::views::filter(op)) {
    for (auto&& stage : sc.system().stages() | ranges::views::filter(op)) {
      for (auto&& block : stage.blocks()) {
        scenery_uids.push_back(scenery.uid());
        stage_uids.push_back(stage.uid());
        block_uids.push_back(block.uid());
      }
    }
  }

  scenery_uids.shrink_to_fit();
  stage_uids.shrink_to_fit();
  block_uids.shrink_to_fit();

  return {scenery_uids, stage_uids, block_uids};
}

template<typename SystemContext, typename Operation = decltype(true_fnc)>
constexpr STUids make_st_uids(const SystemContext& sc, Operation op = true_fnc)
{
  const auto size = sc.get_scenery_size() * sc.get_stage_size();
  std::vector<Uid> scenery_uids;
  scenery_uids.reserve(size);
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);

  for (auto&& scenery : sc.system().sceneries() | ranges::views::filter(op)) {
    for (auto&& stage : sc.system().stages() | ranges::views::filter(op)) {
      scenery_uids.push_back(scenery.uid());
      stage_uids.push_back(stage.uid());
    }
  }

  scenery_uids.shrink_to_fit();
  stage_uids.shrink_to_fit();

  return {scenery_uids, stage_uids};
}

template<typename SystemContext, typename Operation = decltype(true_fnc)>
constexpr TUids make_t_uids(const SystemContext& sc, Operation op = true_fnc)
{
  const auto size = sc.get_stage_size();
  std::vector<Uid> stage_uids;
  stage_uids.reserve(size);

  for (auto&& stage : sc.system().stages() | ranges::views::filter(op)) {
    stage_uids.push_back(stage.uid());
  }

  stage_uids.shrink_to_fit();

  return stage_uids;
}

constexpr auto stage_factors(auto&& stages)
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

const SystemOptionsLP& SystemContext::options() const
{
  return system().options();
}

double SystemContext::block_cost(const BlockLP& block, const double cost) const
{
  return cost
      * cost_factor(options().scale_objective(),
                    scenery_probability_factor(),
                    stage_discount_factor(),
                    block.duration());
}

auto SystemContext::block_cost_factors() const -> std::vector<double>
{
  std::vector<double> factors(active_scenery_count() * active_block_count());

  const auto scale_obj = options().scale_objective();

  for (size_t idx {}; auto&& scenery : active(system().sceneries())) {
    const auto probability_factor = scenery.probability_factor();
    for (auto&& [ti, stage] : enumerate_active(system().stages())) {
      for (auto&& block : stage.blocks()) {
        const auto cfactor = cost_factor(scale_obj,
                                         probability_factor,
                                         stage_discount_factors[ti],
                                         block.duration());
        factors[idx++] = 1.0 / cfactor;
      }
    }
  }

  return factors;
}

double SystemContext::stage_cost(const double cost) const
{
  return cost
      * cost_factor(options().scale_objective(),
                    scenery_probability_factor(),
                    stage_discount_factor(),
                    stage_duration());
}

auto SystemContext::stage_cost_factors() const -> std::vector<double>
{
  std::vector<double> factors(active_scenery_count() * active_stage_count());

  const auto scale_obj = options().scale_objective();

  for (size_t idx = {}; auto&& scenery : active(system().sceneries())) {
    const auto probability_factor = scenery.probability_factor();
    for (auto&& [ti, stage] : enumerate_active(system().stages())) {
      const auto cfactor = cost_factor(scale_obj,
                                       probability_factor,
                                       stage_discount_factors[ti],
                                       stage.duration());
      factors[idx++] = 1.0 / cfactor;
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

SystemContext::SystemContext(SystemLP& psystem)
    : m_system_(psystem)
    , active_sceneries(active_indices<SceneryIndex>(psystem.sceneries()))
    , active_stages(active_indices<StageIndex>(psystem.stages()))
    , active_blocks(active_block_indices<BlockIndex>(psystem.stages()))
    , active_stage_blocks(
          active_stage_block_indices<BlockIndex>(psystem.stages()))
    , scenery_size(psystem.sceneries().size())
    , stage_size(psystem.stages().size())
    , block_size(psystem.blocks().size())
    , stage_discount_factors(stage_factors(psystem.stages()))
{
}

auto SystemContext::get_bus_index(const ObjectSingleId<BusLP>& id) const
    -> ElementIndex<BusLP>
{
  const auto use_single_bus = options().use_single_bus();

  if (use_single_bus && !m_single_bus_id_) {
    m_single_bus_id_ = id;
  }

  const auto bus =
      use_single_bus && m_single_bus_id_ ? m_single_bus_id_.value() : id;

  return system().element_index(bus);
}

auto SystemContext::get_bus(const ObjectSingleId<BusLP>& id) const
    -> const BusLP&
{
  return system().element(get_bus_index(id));
}

auto SystemContext::sceneries() const -> const std::vector<SceneryLP>&
{
  return system().sceneries();
}

auto SystemContext::stages() const -> const std::vector<StageLP>&
{
  return system().stages();
}

auto SystemContext::blocks() const -> const std::vector<BlockLP>&
{
  return system().blocks();
}

}  // namespace gtopt
