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
 * - FlatHelper: For data flattening operations
 *
 * @note Thread safety: Not thread-safe - assumes single-threaded optimization
 * @see SimulationLP, SystemLP for related classes
 */

#pragma once

#include <cassert>
#include <type_traits>
#include <utility>

#include <gtopt/block_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/cost_helper.hpp>
#include <gtopt/element_traits.hpp>
#include <gtopt/flat_helper.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/linear_problem.hpp>
#include <gtopt/lp_element_types.hpp>
#include <gtopt/overload.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/simulation_lp.hpp>
#include <gtopt/single_id.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/utils.hpp>

namespace gtopt
{

class SystemLP;
class SimulationLP;

class SystemContext
    : public FlatHelper
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
  [[nodiscard]] constexpr auto hydro_fail_cost(const StageLP& stage,
                                               const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage.uid());
    return fc ? fc : options().hydro_fail_cost();
  }

  template<typename FailCost>
  [[nodiscard]] constexpr auto reserve_fail_cost(const StageLP& stage,
                                                 const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage.uid());
    return fc ? fc : options().reserve_fail_cost();
  }

  template<typename StateCost>
  [[nodiscard]] constexpr auto state_fail_cost(const StageLP& stage,
                                               const StateCost& scost) const
  {
    const auto sc = scost.optval(stage.uid());
    return sc ? sc : options().state_fail_cost();
  }

  //
  //  minmax util methods
  //

  template<typename Max>
  [[nodiscard]] constexpr auto block_max_at(
      const StageLP& stage,
      const BlockLP& block,
      const Max& lmax,
      const double capacity_max = LinearProblem::DblMax) const
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
  [[nodiscard]] constexpr auto&& elements() const
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
  //  Fully-inline element accessors — use void* dispatch via m_collection_ptrs_
  //  (populated in the constructor, which includes system_lp.hpp) so that
  //  *_lp.cpp call sites need neither system_lp.hpp nor explicit
  //  instantiations.
  //
  //  BusLP ObjectSingleId: routes through get_bus() to honour the single-bus
  //  override.  All other ObjectSingleId and all ElementIndex overloads cast
  //  m_collection_ptrs_[lp_type_index_v<Element>] to Collection<Element>*.
  //

  template<typename Element>
  [[nodiscard]] auto get_element(const ObjectSingleId<Element>& id) const
      -> const Element&
  {
    if constexpr (std::is_same_v<Element, BusLP>) {
      return get_bus(id);
    } else {
      constexpr auto idx = lp_type_index_v<Element>;
      assert(m_collection_ptrs_[idx] != nullptr  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
             && "Collection pointer not initialized — SystemContext constructed before SystemLP?");
      return static_cast<const Collection<Element>*>(m_collection_ptrs_[idx])
          ->element(id);
    }
  }

  template<typename Element>
  [[nodiscard]] auto get_element(const ElementIndex<Element>& id) const
      -> const Element&
  {
    constexpr auto idx = lp_type_index_v<Element>;
    assert(m_collection_ptrs_[idx] != nullptr  // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
           && "Collection pointer not initialized — SystemContext constructed before SystemLP?");
    return static_cast<const Collection<Element>*>(m_collection_ptrs_[idx])
        ->element(id);
  }

  // Methods to handle the state_variables
  template<typename Key>
  constexpr auto add_state_variable(Key&& key,
                                    ColIndex col,
                                    double scost,
                                    double var_scale,
                                    LpContext context)
  {
    return simulation().add_state_variable(
        std::forward<Key>(key), col, scost, var_scale, std::move(context));
  }

  /// Atomic helper: add a new state-variable column to the LP AND register
  /// it in the state-variable map.  Sets `is_state = true` on the column so
  /// LabelMaker emits its label at `LpNamesLevel::minimal` (cascade/SDDP
  /// cut I/O needs it).  The column's `context` field is also used as the
  /// StateVariable's context, keeping the two in sync.
  ///
  /// This is the preferred API for creating state variables — calling
  /// `lp.add_col()` and `add_state_variable()` separately risks forgetting
  /// one step and silently breaking cut I/O.
  template<typename Key, typename Col>
  auto add_state_col(LinearProblem& lp,
                     Key&& key,
                     Col&& col,
                     double scost = 0.0,
                     double var_scale = 1.0) -> ColIndex
  {
    col.is_state = true;
    auto context = col.context;
    const auto idx = lp.add_col(std::forward<Col>(col));
    add_state_variable(
        std::forward<Key>(key), idx, scost, var_scale, std::move(context));
    return idx;
  }

  /// Atomic helper: mark an already-added column as a state variable AND
  /// register it in the state-variable map.  Used when the state-variable
  /// role is decided after the column was first added (e.g. storage_lp
  /// registers the last block's energy column as efin after the whole
  /// block loop is done).
  template<typename Key>
  void add_state_col(LinearProblem& lp,
                     Key&& key,
                     ColIndex col_idx,
                     double scost,
                     double var_scale,
                     LpContext context)
  {
    lp.col_at(col_idx).is_state = true;
    add_state_variable(
        std::forward<Key>(key), col_idx, scost, var_scale, std::move(context));
  }

  template<typename Key>
  [[nodiscard]] constexpr auto get_state_variable(Key&& key) const noexcept
  {
    return simulation().state_variable(std::forward<Key>(key));
  }

  // ── PAMPL / user-constraint variable registry forwarders ────────────────
  //
  // Each LP element calls these from `add_to_lp` once per (scenario, stage)
  // to register its PAMPL-visible columns.  Mirrors `add_state_variable`.
  //
  // `class_name` and `attribute` must refer to storage with static (or at
  // least solve-long) lifetime — in practice the `constexpr string_view`
  // constants on each `*LP` class (e.g. `GeneratorLP::GenerationName`).

  /// Register a per-block column map (e.g., generator.generation).
  /// Stores a pointer to `block_cols`; the element must keep it alive
  /// for the rest of the solve.
  ///
  /// Marked `const` (logical-const): the underlying `SimulationLP` is
  /// held via `reference_wrapper`, so const SystemContext methods may
  /// still mutate the simulation's ampl variable registry.  This lets
  /// elements whose `add_to_lp` takes `const SystemContext&` register
  /// their PAMPL-visible columns without signature churn.
  void add_ampl_variable(std::string_view class_name,
                         Uid element_uid,
                         std::string_view attribute,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         const BIndexHolder<ColIndex>& block_cols) const
  {
    m_simulation_.get().add_ampl_variable(class_name,
                                          element_uid,
                                          attribute,
                                          scenario.uid(),
                                          stage.uid(),
                                          block_cols);
  }

  /// Register a stage-level scalar column (e.g., eini, efin, capainst).
  void add_ampl_variable(std::string_view class_name,
                         Uid element_uid,
                         std::string_view attribute,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         ColIndex stage_col) const
  {
    m_simulation_.get().add_ampl_variable(class_name,
                                          element_uid,
                                          attribute,
                                          scenario.uid(),
                                          stage.uid(),
                                          stage_col);
  }

  /// Register an element's name so that `generator("G1")` resolves
  /// "G1" to its Uid.  Call once per element (constructor or first
  /// `add_to_lp`).
  void register_ampl_element(std::string_view class_name,
                             std::string_view element_name,
                             Uid element_uid) const
  {
    m_simulation_.get().register_ampl_element(
        class_name, element_name, element_uid);
  }

  /// Look up a registered variable column.  Returns nullopt if the
  /// (class, uid, attribute, scenario, stage, block) combination was
  /// never registered.
  [[nodiscard]] std::optional<ColIndex> find_ampl_col(
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      ScenarioUid scenario_uid,
      StageUid stage_uid,
      BlockUid block_uid) const
  {
    return simulation().find_ampl_col(
        class_name, element_uid, attribute, scenario_uid, stage_uid, block_uid);
  }

  /// Resolve an element name to its Uid within a class.
  [[nodiscard]] std::optional<Uid> lookup_ampl_element_uid(
      std::string_view class_name, std::string_view element_name) const
  {
    return simulation().lookup_ampl_element_uid(class_name, element_name);
  }

  /// Register a compound PAMPL attribute at class level.
  /// See `SimulationLP::add_ampl_compound`.
  void add_ampl_compound(std::string_view class_name,
                         std::string_view compound_name,
                         std::vector<AmplCompoundLeg> legs) const
  {
    m_simulation_.get().add_ampl_compound(
        class_name, compound_name, std::move(legs));
  }

  /// Look up a compound PAMPL attribute by (class, compound_name).
  [[nodiscard]] const std::vector<AmplCompoundLeg>* find_ampl_compound(
      std::string_view class_name,
      std::string_view compound_name) const noexcept
  {
    return simulation().find_ampl_compound(class_name, compound_name);
  }

  /// Register filter metadata for one element (F9).
  /// See `SimulationLP::register_ampl_element_metadata`.
  void register_ampl_element_metadata(std::string_view class_name,
                                      Uid element_uid,
                                      AmplElementMetadata metadata) const
  {
    m_simulation_.get().register_ampl_element_metadata(
        class_name, element_uid, std::move(metadata));
  }

  /// Look up an element's metadata bundle (F9).  Returns nullptr when
  /// the element has no registered metadata.
  [[nodiscard]] const AmplElementMetadata* find_ampl_element_metadata(
      std::string_view class_name, Uid element_uid) const noexcept
  {
    return simulation().find_ampl_element_metadata(class_name, element_uid);
  }

private:
  std::reference_wrapper<SimulationLP> m_simulation_;
  std::reference_wrapper<SystemLP> m_system_;

  // One void* per LP element type; each points to the Collection<T> inside the
  // owning SystemLP.  Index for type T is lp_type_index_v<T>.
  // Populated once in the constructor (system_context.cpp includes
  // system_lp.hpp).
  lp_collection_ptrs_t m_collection_ptrs_ {};
};

}  // namespace gtopt

static_assert(std::is_base_of_v<gtopt::FlatHelper, gtopt::SystemContext>,
              "SystemContext must inherit from FlatHelper");

static_assert(std::is_base_of_v<gtopt::CostHelper, gtopt::SystemContext>,
              "SystemContext must inherit from CostHelper");
