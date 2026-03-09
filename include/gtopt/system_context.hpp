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

class BusLP;
class SystemLP;
class SimulationLP;

// Forward declarations for all LP component types accessed via element()
class BatteryLP;
class ConverterLP;
class DemandLP;
class DemandProfileLP;
class FiltrationLP;
class FlowLP;
class GeneratorLP;
class GeneratorProfileLP;
class JunctionLP;
class LineLP;
class ReserveProvisionLP;
class ReserveZoneLP;
class ReservoirLP;
class TurbineLP;
class WaterwayLP;

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
  [[nodiscard]] constexpr auto stage_lossfactor(const StageLP& stage,
                                                const LossFactor& lfact) const
  {
    return options().use_line_losses()
        ? std::max(lfact.at(stage.uid()).value_or(0.0), 0.0)
        : 0.0;
  }

  template<typename Reactance>
  [[nodiscard]] constexpr auto stage_reactance(const StageLP& stage,
                                               const Reactance& reactance) const
  {
    if (options().use_kirchhoff()) {
      return reactance.at(stage.uid());
    }
    using ReturnType = decltype(reactance.at(stage.uid()));
    return ReturnType {};
  }

  template<typename FailCost>
  [[nodiscard]] constexpr auto demand_fail_cost(const StageLP& stage,
                                                const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage.uid());
    return fc ? fc : options().demand_fail_cost();
  }

  template<typename FailCost>
  [[nodiscard]] constexpr auto reserve_fail_cost(const StageLP& stage,
                                                 const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage.uid());
    return fc ? fc : options().reserve_fail_cost();
  }

  //
  //  minmax util methods
  //

  template<typename Max>
  [[nodiscard]] constexpr auto block_max_at(
      const StageLP& stage,
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
  [[nodiscard]] constexpr auto block_maxmin_at(
      const StageLP& stage,
      const BlockLP& block,
      const Max& lmax,
      const Min& lmin,
      const double capacity_max,
      const double capacity_min = 0.0) const -> std::pair<double, double>
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
  [[nodiscard]] constexpr auto stage_maxmin_at(
      const StageLP& stage,
      const Min& lmax,
      const Max& lmin,
      const double capacity_max,
      const double capacity_min = 0.0) const -> std::pair<double, double>
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
  [[nodiscard]] constexpr auto&& element(const Id<Element>& id) const
  {
    // Qualify with gtopt:: so the free function is found instead of the
    // member get_element() overloads that would otherwise shadow it.
    return gtopt::get_element(*this, id);
  }

  template<typename Element, template<typename> class Id>
  constexpr auto element_index(const Id<Element>& id) const
  {
    return gtopt::get_element_index(*this, id);
  }

  template<typename Element>
  constexpr auto add_element(Element&& element)
  {
    return push_back(*this, std::forward<Element>(element));
  }

  //
  //  get_bus, single_bus and related
  //

  [[nodiscard]] auto get_bus_index(const ObjectSingleId<BusLP>& id) const
      -> ElementIndex<BusLP>;
  [[nodiscard]] auto get_bus(const ObjectSingleId<BusLP>& id) const
      -> const BusLP&;

  //
  //  Non-template element accessors — declared here, defined in
  //  system_context.cpp (which includes system_lp.hpp).  Using template
  //  declarations with no inline body + explicit instantiations in the
  //  .cpp keeps *_lp.cpp call sites free of the system_lp.hpp dependency
  //  while avoiding per-type boilerplate here.
  //
  //  BusLP ObjectSingleId: routes through get_bus() (explicit specialisation
  //  in system_context.cpp); all other types use system().element(id).
  //
  template<typename Element>
  [[nodiscard]] auto get_element(const ObjectSingleId<Element>& id) const
      -> const Element&;

  template<typename Element>
  [[nodiscard]] auto get_element(const ElementIndex<Element>& id) const
      -> const Element&;

  // Methods to handle the state_variables
  template<typename Key>
  constexpr auto add_state_variable(Key&& key, ColIndex col)
  {
    return simulation().add_state_variable(std::forward<Key>(key), col);
  }

  template<typename Key>
  [[nodiscard]] constexpr auto get_state_variable(Key&& key) const noexcept
  {
    return simulation().state_variable(std::forward<Key>(key));
  }

private:
  std::reference_wrapper<SimulationLP> m_simulation_;
  std::reference_wrapper<SystemLP> m_system_;
};

}  // namespace gtopt

static_assert(std::is_base_of_v<gtopt::LabelMaker, gtopt::SystemContext>,
              "SystemContext must inherit from LabelMaker");

static_assert(std::is_base_of_v<gtopt::FlatHelper, gtopt::SystemContext>,
              "SystemContext must inherit from FlatHelper");

static_assert(std::is_base_of_v<gtopt::CostHelper, gtopt::SystemContext>,
              "SystemContext must inherit from CostHelper");
