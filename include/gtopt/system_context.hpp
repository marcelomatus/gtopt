/**
 * @file      system_context.hpp
 * @brief     Central execution context for power system optimization
 * @date      Sun Mar 23 21:54:14 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * @class SystemContext
 * @brief Manages optimization state and provides core functionality for LP
 * formulation
 *
 * This is the central coordinator that:
 * - Tracks active scenarios/stages/blocks
 * - Handles cost calculations with time discounting
 * - Provides element access and indexing
 * - Manages variable labeling and naming
 * - Handles constraint bounds and limits
 *
 * Key Responsibilities:
 * - Bridges simulation model and LP formulation
 * - Maintains optimization state
 * - Provides helper methods for variable/constraint setup
 * - Handles time-discounted cost calculations
 * - Manages active element filtering
 *
 * Inherits from:
 * - LabelMaker: For variable labeling/naming
 * - FlatHelper: For data flattening operations
 *
 * @note Thread safety: Not thread-safe - assumes single-threaded optimization
 * @see SimulationLP, SystemLP for related classes
 */

#pragma once

#include <utility>

#include <boost/multi_array.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/cost_helper.hpp>
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
class SystemLP;
class SimulationLP;

class SystemContext
    : public LabelMaker
    , public FlatHelper
    , public CostHelper
{
public:
  // Core Context Management
  explicit SystemContext(SimulationLP& simulation, SystemLP& system);

  //
  //  get methods
  //
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

  [[nodiscard]] constexpr auto&& options() const noexcept
  {
    return simulation().options();
  }

  //
  // Option methods
  //

  template<typename LossFactor>
  constexpr auto stage_lossfactor(const StageLP& stage,
                                  const LossFactor& lfact) const
  {
    return options().use_line_losses()
        ? std::max(lfact.at(stage.uid()).value_or(0.0), 0.0)
        : 0.0;
  }

  template<typename Reactance>
  constexpr auto stage_reactance(const StageLP& stage,
                                 const Reactance& reactance) const
  {
    if (options().use_kirchhoff()) {
      return reactance.at(stage.uid());
    }
    using ReturnType = decltype(reactance.at(stage.uid()));
    return ReturnType {};
  }

  template<typename FailCost>
  constexpr auto demand_fail_cost(const StageLP& stage,
                                  const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage.uid());
    return fc ? fc : options().demand_fail_cost();
  }

  template<typename FailCost>
  constexpr auto reserve_fail_cost(const StageLP& stage,
                                   const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage.uid());
    return fc ? fc : options().reserve_fail_cost();
  }

  //
  //  minmax util methods
  //

  template<typename Max>
  constexpr auto block_max_at(const StageLP& stage,
                              const BlockLP& block,
                              const Max& lmax,
                              const double capacity_max = CoinDblMax) const
  {
    const auto lmax_at =
        lmax.at(stage.uid(), block.uid()).value_or(capacity_max);
    const auto lmax_block = std::min(capacity_max, lmax_at);

    return lmax_block;
  }

  template<typename Min, typename Max>
  constexpr auto block_maxmin_at(const StageLP& stage,
                                 const BlockLP& block,
                                 const Max& lmax,
                                 const Min& lmin,
                                 const double capacity_max,
                                 const double capacity_min = 0.0) const
      -> std::pair<double, double>
  {
    const auto lmin_at =
        lmin.at(stage.uid(), block.uid()).value_or(capacity_min);
    const auto lmin_block = std::max(capacity_min, lmin_at);

    const auto lmax_at =
        lmax.at(stage.uid(), block.uid()).value_or(capacity_max);
    const auto lmax_block = std::min(capacity_max, lmax_at);

    return {lmax_block, lmin_block};
  }

  template<typename Min, typename Max>
  constexpr auto stage_maxmin_at(const StageLP& stage,
                                 const Min& lmax,
                                 const Max& lmin,
                                 const double capacity_max,
                                 const double capacity_min = 0.0) const
      -> std::pair<double, double>
  {
    const auto lmin_at = lmin.at(stage.uid()).value_or(capacity_min);
    const auto lmin_block = std::max(capacity_min, lmin_at);

    const auto lmax_at = lmax.at(stage.uid()).value_or(capacity_max);
    const auto lmax_block = std::min(capacity_max, lmax_at);

    return {lmax_block, lmin_block};
  }

  //
  //  add&get elements
  //

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

  //
  //
  //
  [[nodiscard]] double stage_duration(const OptStageIndex& stage_index,
                                      double prev_duration = 0) const noexcept;

  //
  //  get_bus, single_bus and related
  //

  [[nodiscard]] constexpr auto&& single_bus_id() const noexcept
  {
    return m_single_bus_id_;
  }

  template<typename Id>
  constexpr bool is_single_bus(const Id& id) const noexcept
  {
    if (m_single_bus_id_) {
      auto&& sid = m_single_bus_id_.value();
      return sid.index() == 0 ? std::get<0>(sid) == id.first
                              : std::get<1>(sid) == id.second;
    }
    return false;
  }

  [[nodiscard]] auto get_bus_index(const ObjectSingleId<BusLP>& id) const
      -> ElementIndex<BusLP>;
  [[nodiscard]] auto get_bus(const ObjectSingleId<BusLP>& id) const
      -> const BusLP&;

#ifdef NONE
  // Methods to handle the state_variables
  constexpr const auto& add_state_variable_col(const Name& name,
                                               const PhaseLP& phase,
                                               Index col)
  {
    return simulation().add_state_variable(
        StateVariable {name, phase.index(), col});
  }

  [[nodiscard]] constexpr auto get_state_variable_col(
      const Name& name, const PhaseIndex& stage.index()) const
  {
    const auto state_var = simulation().get_state_variable(
        StateVariable::key_t {name, stage.index()});

    const auto result = state_var
        ? std::optional<Index>(state_var.value().get().first_col())
        : std::nullopt;

    return result;
  }

#endif

private:
  std::reference_wrapper<SimulationLP> m_simulation_;
  std::reference_wrapper<SystemLP> m_system_;

  std::optional<ObjectSingleId<BusLP>> m_single_bus_id_ {};
};

}  // namespace gtopt

static_assert(std::is_base_of_v<gtopt::LabelMaker, gtopt::SystemContext>,
              "SystemContext must inherit from LabelMaker");

static_assert(std::is_base_of_v<gtopt::FlatHelper, gtopt::SystemContext>,
              "SystemContext must inherit from FlatHelper");

static_assert(std::is_base_of_v<gtopt::CostHelper, gtopt::SystemContext>,
              "SystemContext must inherit from CostHelper");
