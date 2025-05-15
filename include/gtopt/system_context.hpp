/**
 * @file      system_context.hpp
 * @brief     System execution context for power system optimization
 * @date      Sun Mar 23 21:54:14 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines SystemContext which manages:
 * - Active scenarios/stages/blocks for optimization
 * - Cost calculations and discount factors
 * - System element access and indexing
 * - Variable labeling and naming
 * - Constraint bounds and limits
 *
 * The context bridges between the simulation model and LP formulation,
 * tracking the current optimization state and providing helper methods
 * for variable and constraint setup.
 */

#pragma once

#include <utility>

#include <boost/multi_array.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/element_traits.hpp>
#include <gtopt/flat_helper.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/label_maker.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/options_lp.hpp>
#include <gtopt/overload.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

class Bus;
class BusLP;

using STBUids =
    std::tuple<std::vector<Uid>, std::vector<Uid>, std::vector<Uid>>;

using STUids = std::tuple<std::vector<Uid>, std::vector<Uid>>;
using TUids = std::vector<Uid>;

class SystemLP;
class SimulationLP;

class SystemContext
    : public LabelMaker
    , public FlatHelper
{
public:
  // Core Context Management
  explicit SystemContext(SimulationLP& psimulation, SystemLP& psystem);

  template<typename Self>
  [[nodiscard]] constexpr auto&& simulation(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_simulation_.get();
  }

  template<typename Self>
  [[nodiscard]] constexpr auto&& system(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_system_.get();
  }

  /// @brief Get optimization options
  /// @return Reference to OptionsLP configuration
  [[nodiscard]] constexpr auto&& options() const noexcept
  {
    return simulation().options();
  }

  // Scenario Accessors
  [[nodiscard]] constexpr auto scenario_uid(
      const ScenarioIndex& scenario_index) const
  {
    return scenarios()[scenario_index].uid();
  }

  [[nodiscard]] constexpr auto scenario_probability_factor(
      const ScenarioIndex& scenario_index) const
  {
    return scenarios()[scenario_index].probability_factor();
  }

  // Stage Accessors
  [[nodiscard]] constexpr auto stage_uid(const StageIndex& stage_index) const
  {
    return stages()[stage_index].uid();
  }

  [[nodiscard]] constexpr auto stage_discount_factor(
      const StageIndex& stage_index) const
  {
    return stages()[stage_index].discount_factor();
  }

  [[nodiscard]] constexpr auto stage_duration(const OptStageIndex& stage_index,
                                              double prev_duration = 0) const
  {
    return stage_index ? stages().at(stage_index.value()).duration()
                       : prev_duration;
  }

  [[nodiscard]] constexpr auto stage_duration(
      const StageIndex& stage_index) const
  {
    return stages()[stage_index].duration();
  }

  [[nodiscard]] constexpr auto&& stage_blocks(
      const StageIndex& stage_index) const
  {
    return stages()[stage_index].blocks();
  }

  template<typename LossFactor>
  constexpr auto stage_lossfactor(const StageIndex& stage_index,
                                  const LossFactor& lfact) const
  {
    return options().use_line_losses()
        ? std::max(lfact.at(stage_index).value_or(0.0), 0.0)
        : 0.0;
  }

  template<typename Reactance>
  constexpr auto stage_reactance(const StageIndex& stage_index,
                                 const Reactance& reactance) const
  {
    if (options().use_kirchhoff()) {
      return reactance.at(stage_index);
    }
    using ReturnType = decltype(reactance.at(stage_index));
    return ReturnType {};
  }

  template<typename FailCost>
  constexpr auto demand_fail_cost(const StageIndex& stage_index,
                                  const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage_index);
    return fc ? fc : options().demand_fail_cost();
  }

  template<typename FailCost>
  constexpr auto reserve_fail_cost(const StageIndex& stage_index,
                                   const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage_index);
    return fc ? fc : options().reserve_fail_cost();
  }

  // Active Elements Query

  [[nodiscard]] double block_cost(const ScenarioIndex& scenario_index,
                                  const StageIndex& stage_index,
                                  const BlockLP& block,
                                  double cost) const;

  using block_factor_matrix_t = boost::multi_array<std::vector<double>, 2>;
  [[nodiscard]] auto block_cost_factors() const -> block_factor_matrix_t;

  [[nodiscard]] double stage_cost(const StageIndex& stage_index,
                                  double cost) const;

  using stage_factor_matrix_t = std::vector<double>;
  [[nodiscard]] auto stage_cost_factors() const -> stage_factor_matrix_t;

  [[nodiscard]] double scenario_stage_cost(const ScenarioIndex& scenario_index,
                                           const StageIndex& stage_index,
                                           double cost) const;

  using scenario_stage_factor_matrix_t = boost::multi_array<double, 2>;
  [[nodiscard]] auto scenario_stage_cost_factors() const
      -> scenario_stage_factor_matrix_t;

  [[nodiscard]] auto stb_active_uids() const -> STBUids;
  [[nodiscard]] auto st_active_uids() const -> STUids;
  [[nodiscard]] auto t_active_uids() const -> TUids;

  [[nodiscard]] auto stb_uids() const -> STBUids;
  [[nodiscard]] auto st_uids() const -> STUids;
  [[nodiscard]] auto t_uids() const -> TUids;

  template<typename Max>
  constexpr auto block_max_at(const StageIndex& stage_index,
                              const BlockIndex& block_index,
                              const Max& lmax,
                              const double capacity_max = CoinDblMax) const
  {
    const auto lmax_at =
        lmax.at(stage_index, block_index).value_or(capacity_max);
    const auto lmax_block = std::min(capacity_max, lmax_at);

    return lmax_block;
  }

  template<typename Min, typename Max>
  constexpr auto block_maxmin_at(const StageIndex& stage_index,
                                 const BlockIndex& block_index,
                                 const Max& lmax,
                                 const Min& lmin,
                                 const double capacity_max,
                                 const double capacity_min = 0.0) const
      -> std::pair<double, double>
  {
    const auto lmin_at =
        lmin.at(stage_index, block_index).value_or(capacity_min);
    const auto lmin_block = std::max(capacity_min, lmin_at);

    const auto lmax_at =
        lmax.at(stage_index, block_index).value_or(capacity_max);
    const auto lmax_block = std::min(capacity_max, lmax_at);

    return {lmax_block, lmin_block};
  }

  template<typename Min, typename Max>
  constexpr auto stage_maxmin_at(const StageIndex& stage_index,
                                 const Min& lmax,
                                 const Max& lmin,
                                 const double capacity_max,
                                 const double capacity_min = 0.0) const
      -> std::pair<double, double>
  {
    const auto lmin_at = lmin.at(stage_index).value_or(capacity_min);
    const auto lmin_block = std::max(capacity_min, lmin_at);

    const auto lmax_at = lmax.at(stage_index).value_or(capacity_max);
    const auto lmax_block = std::min(capacity_max, lmax_at);

    return {lmax_block, lmin_block};
  }

  //
  //  set&get the variable data
  //

  [[nodiscard]] constexpr auto&& single_bus_id() const noexcept
  {
    return m_single_bus_id_;
  }

  template<typename Id>
  constexpr bool is_single_bus(const Id& id) const noexcept
  {
    try {
      if (m_single_bus_id_) {
        auto&& sid = m_single_bus_id_.value();
        return sid.index() == 0 ? std::get<0>(sid) == id.first
                                : std::get<1>(sid) == id.second;
      }
      return false;
    } catch (...) {
      return false;
    }
  }

  [[nodiscard]] auto get_bus_index(const ObjectSingleId<BusLP>& id) const
      -> ElementIndex<BusLP>;
  [[nodiscard]] auto get_bus(const ObjectSingleId<BusLP>& id) const
      -> const BusLP&;

  template<typename Element>
  constexpr auto&& elements() const
  {
    return get_elements<Element>(*this);
  }

  template<typename Element, template<typename> class Id>
  constexpr auto&& element(const Id<Element>& id) const
  {
    return get_element(*this, id);
  }

  template<typename Element, template<typename> class Id>
  constexpr auto element_index(const Id<Element>& id) const
  {
    return get_element_index(*this, id);
  }

  template<typename Element>
  constexpr auto add_element(Element&& element)
  {
    return push_back(*this, std::forward<Element>(element));
  }

  [[nodiscard]] constexpr const std::vector<ScenarioLP>& scenarios()
      const noexcept
  {
    return simulation().scenarios();
  }

  [[nodiscard]] constexpr const std::vector<StageLP>& stages() const noexcept
  {
    return simulation().stages();
  }

  [[nodiscard]] constexpr const std::vector<BlockLP>& blocks() const noexcept
  {
    return simulation().blocks();
  }

  // Label methods from LabelMaker are now directly available:
  // label(), t_label(), st_label(), stb_label()

  // Add method with deducing this and perfect forwarding
  constexpr const auto& add_state_variable_col(const Name& name,
                                               const StageIndex& stage_index,
                                               Index col)
  {
    return simulation().add_state_variable(
        StateVariable {name, stage_index, col});
  }

  // Get method with deducing this for automatic const handling
  [[nodiscard]] constexpr auto get_state_variable_col(
      const Name& name, const StageIndex& stage_index) const
  {
    const auto state_var = simulation().get_state_variable(
        StateVariable::key_t {name, stage_index});

    const auto result = state_var
        ? std::optional<Index>(state_var.value().get().first_col())
        : std::nullopt;

    return result;
  }

private:
  std::reference_wrapper<SimulationLP> m_simulation_;
  std::reference_wrapper<SystemLP> m_system_;

  std::vector<double> stage_discount_factors;

  std::optional<ObjectSingleId<BusLP>> m_single_bus_id_ {};
};

}  // namespace gtopt

static_assert(std::is_base_of_v<gtopt::LabelMaker, gtopt::SystemContext>,
              "SystemContext must inherit from LabelMaker");

static_assert(std::is_base_of_v<gtopt::FlatHelper, gtopt::SystemContext>,
              "SystemContext must inherit from LabelMaker");
