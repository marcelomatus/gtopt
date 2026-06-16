/**
 * @file      system_lp.hpp
 * @brief     SystemLP class for power system linear programming formulation
 * @author    marcelo
 * @copyright BSD-3-Clause
 * @date      Sat Mar 29 19:16:40 2025
 *
 * Defines SystemLP class which coordinates the formulation of power system
 * planning as a linear programming problem. Manages collections of system
 * components and their LP representations.
 */

#pragma once

#include <functional>
#include <optional>
#include <stdexcept>

#include <gtopt/allowance_pool_lp.hpp>
#include <gtopt/ammonia_node_lp.hpp>
#include <gtopt/ammonia_storage_lp.hpp>
#include <gtopt/battery_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/capacity_profile_lp.hpp>
#include <gtopt/carrier_converter_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/commitment_lp.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/demand_profile_lp.hpp>
#include <gtopt/emission_lp.hpp>
#include <gtopt/emission_source_lp.hpp>
#include <gtopt/emission_zone_lp.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/fuel_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/generator_profile_lp.hpp>
#include <gtopt/hydrogen_node_lp.hpp>
#include <gtopt/hydrogen_storage_lp.hpp>
#include <gtopt/inertia_provision_lp.hpp>
#include <gtopt/inertia_zone_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/line_commitment_lp.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/lng_terminal_lp.hpp>
#include <gtopt/lp_fingerprint.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/plant_lp.hpp>
#include <gtopt/pump_lp.hpp>
#include <gtopt/reserve_provision_lp.hpp>
#include <gtopt/reserve_zone_lp.hpp>
#include <gtopt/reservoir_discharge_limit_lp.hpp>
#include <gtopt/reservoir_lp.hpp>
#include <gtopt/reservoir_production_factor_lp.hpp>
#include <gtopt/reservoir_seepage_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/schedule.hpp>
#include <gtopt/simple_commitment_lp.hpp>
#include <gtopt/solver_options.hpp>
#include <gtopt/system.hpp>
#include <gtopt/system_context.hpp>
#include <gtopt/thermal_node_lp.hpp>
#include <gtopt/thermal_storage_lp.hpp>
#include <gtopt/turbine_lp.hpp>
#include <gtopt/user_constraint_lp.hpp>
#include <gtopt/volume_right_lp.hpp>
#include <gtopt/waterway_lp.hpp>

namespace gtopt
{
class LinearInterface;

/**
 * @concept AddToLP
 * @brief Concept for types that can be added to an LP problem
 *
 * Requires types to provide:
 * - add_to_lp() method for LP formulation
 * - add_to_output() method for result output
 */

template<typename T>
concept AddToLP = requires(T obj,
                           SystemContext& system_context,
                           const ScenarioLP& scenario,
                           const StageLP& stage,
                           LinearProblem& lp,
                           OutputContext& output_context) {
  { obj.add_to_lp(system_context, scenario, stage, lp) } -> std::same_as<bool>;
  { obj.add_to_output(output_context) } -> std::same_as<bool>;
};

// Verify all required types satisfy AddToLP concept
static_assert(AddToLP<BusLP>);
static_assert(AddToLP<DemandLP>);
static_assert(AddToLP<GeneratorLP>);
static_assert(AddToLP<LineLP>);
static_assert(AddToLP<GeneratorProfileLP>);
static_assert(AddToLP<DemandProfileLP>);
static_assert(AddToLP<CapacityProfileLP>);
static_assert(AddToLP<BatteryLP>);
static_assert(AddToLP<ThermalNodeLP>);
static_assert(AddToLP<ThermalStorageLP>);
static_assert(AddToLP<HydrogenNodeLP>);
static_assert(AddToLP<HydrogenStorageLP>);
static_assert(AddToLP<AmmoniaNodeLP>);
static_assert(AddToLP<AmmoniaStorageLP>);
static_assert(AddToLP<CarrierConverterLP>);
static_assert(AddToLP<AllowancePoolLP>);
static_assert(AddToLP<ConverterLP>);
static_assert(AddToLP<ReserveZoneLP>);
static_assert(AddToLP<ReserveProvisionLP>);
// `FuelLP` is mostly a passive parameter carrier (price, heat content,
// emission factors), but contributes a per-(scenario, stage) cap row
// when `Fuel.max_offtake` is set — mirrors PLEXOS
// `FueMaxOffWeek_<fuel>` Constraints.  The cap aggregates
// heat-rate-weighted dispatch across every generator referencing the
// fuel, so FuelLP must run AFTER GeneratorLP in the visitor order
// (verified by its position in `lp_element_types_t`).  `EmissionLP`
// remains a passive parameter carrier.
static_assert(AddToLP<FuelLP>);
// `EmissionZoneLP` (Commit 3) owns the production col + balance row +
// optional cap row; `EmissionSourceLP` (same commit) injects its
// `-rate · dur` coefficient into the matching balance row.
static_assert(AddToLP<EmissionZoneLP>);
static_assert(AddToLP<EmissionSourceLP>);
static_assert(AddToLP<CommitmentLP>);
static_assert(AddToLP<SimpleCommitmentLP>);
static_assert(AddToLP<InertiaZoneLP>);
static_assert(AddToLP<InertiaProvisionLP>);

static_assert(AddToLP<JunctionLP>);
static_assert(AddToLP<WaterwayLP>);
static_assert(AddToLP<FlowLP>);
static_assert(AddToLP<ReservoirLP>);
static_assert(AddToLP<ReservoirSeepageLP>);
static_assert(AddToLP<ReservoirDischargeLimitLP>);
static_assert(AddToLP<TurbineLP>);
static_assert(AddToLP<PumpLP>);
static_assert(AddToLP<ReservoirProductionFactorLP>);
static_assert(AddToLP<FlowRightLP>);
static_assert(AddToLP<VolumeRightLP>);
static_assert(AddToLP<LngTerminalLP>);
static_assert(AddToLP<DecisionVariableLP>);
static_assert(AddToLP<PlantLP>);
static_assert(AddToLP<UserConstraintLP>);

/**
 * @concept HasUpdateLP
 * @brief Concept satisfied by LP element types that implement `update_lp()`.
 *
 * Used by SystemLP::update_lp() to iterate over the LP element collection
 * and dispatch `update_lp()` only to types that implement it.
 *
 * `HasUpdateLP` also identifies the elements that must remain alive after
 * `add_to_lp` completes: those types hold per-(scen, stg) state
 * (e.g. `m_bound_states_`, `m_states_`, `m_coeff_indices_`) that must
 * persist across SDDP iterations — dropping their collection would lose
 * that state.
 *
 * All other 22 collection types are pure `add_to_lp`-time consumers:
 * after `add_to_lp` populates each element's column / row indices, the
 * collection itself is never read again until `write_out` calls
 * `rebuild_collections_if_needed()` to revive every type from the parsed
 * `System` data.  In particular `ReservoirLP` is disposable: the
 * predecessor's final-volume is read directly from the previous phase's
 * solver via `li.get_col_sol()[eini_col]` and cached in
 * `ReservoirRefCache`, so `update_lp` never traverses
 * `prev_sys->element<ReservoirLP>` on any production code path.
 */
template<typename T>
concept HasUpdateLP = requires(T& obj,
                               SystemLP& system_lp,
                               const ScenarioLP& scenario,
                               const StageLP& stage) {
  { obj.update_lp(system_lp, scenario, stage) } -> std::same_as<int>;
};

/// Transitional alias — `HasUpdateLP<T>` is the canonical spelling for
/// "post-`add_to_lp` resident".  This alias is preserved only for the
/// parallel test-suite work in flight (`test_system_lp_lazy_rebuild`)
/// and SHOULD be inlined to `HasUpdateLP<T>` at every call site once
/// that integration lands.
template<typename T>
inline constexpr bool is_post_add_to_lp_resident_v = HasUpdateLP<T>;

/**
 * @class SystemLP
 * @brief Central coordinator for power system LP formulation
 *
 * Manages conversion of System components to LP representations and
 * coordinates:
 * - Creation of variables and constraints
 * - Time structure management (blocks, stages, scenarios)
 * - Grid component interactions
 * - Result output
 */
class SystemLP
{
public:
  SystemLP() = delete;
  SystemLP(const SystemLP&) = delete;
  SystemLP& operator=(const SystemLP&) noexcept = delete;

  /// Move constructor: member-wise moves, then re-points the embedded
  /// `m_system_context_` back-reference to `*this` (and rebuilds its
  /// `m_collection_ptrs_` table from `this->m_collections_`).  A defaulted
  /// move would leave SystemContext referring to the moved-from SystemLP,
  /// which only worked previously because PlanningLP::create_systems
  /// happened to never move SystemLP after construction.  Required for
  /// the parallel phase build path that emplaces SystemLPs into a
  /// `vector<optional<SystemLP>>` and then moves them into the final
  /// `vector<SystemLP>`.
  SystemLP(SystemLP&& other) noexcept;
  SystemLP& operator=(SystemLP&& other) noexcept;
  ~SystemLP() noexcept = default;

  /**
   * @brief Construct from System with simulation and options
   * @param system     The power system to model
   * @param simulation Reference to the SimulationLP (scenarios, phases, etc.)
   * @param phase      Phase LP to associate with this system
   * @param scene      Scene LP to associate with this system
   * @param flat_opts  Additional LP build options (default empty)
   */
  explicit SystemLP(const System& system,
                    SimulationLP& simulation,
                    PhaseLP phase,
                    SceneLP scene,
                    LpMatrixOptions flat_opts = {},
                    SystemKind kind = SystemKind::forward);

  /// Forward (regular) vs aperture (simplified backward-pass) system.
  /// State variables/links registered by this LP route to the matching
  /// registry in `SimulationLP` (see `SystemKind`).
  [[nodiscard]] constexpr auto kind() const noexcept -> SystemKind
  {
    return m_system_context_.kind();
  }

  explicit SystemLP(const System& system,
                    SimulationLP& simulation,
                    const LpMatrixOptions& flat_opts = {})
      : SystemLP(system,
                 simulation,
                 PhaseLP {
                     Phase {
                         .uid = 0,
                         .name = {},
                         .active = {},
                     },
                     simulation.options(),
                     simulation.simulation(),
                     first_phase_index(),
                 },
                 SceneLP {
                     Scene {
                         .uid = 0,
                         .name = {},
                         .active = {},
                     },
                     simulation.simulation(),
                     first_scene_index(),
                 },
                 flat_opts)
  {
  }

  /// Tuple of collections for all LP component types.
  /// `UserConstraintLP` is placed LAST so that user-constraint rows are
  /// added to the LP after all other elements whose columns they reference.
  // NOTE: this tuple ordering governs the LP-construction order via
  // `visit_elements`.  ConverterLP intentionally runs AFTER
  // CommitmentLP / SimpleCommitmentLP so that the battery's
  // synthesized u_commit columns (created by `expand_batteries`)
  // are already stamped on the LP and reusable for charge-side
  // gating — "one true source for u_commit".  Keep in sync with
  // `lp_element_types_t` in `lp_element_types.hpp`.
  using collections_t = std::tuple<Collection<BusLP>,
                                   Collection<DemandLP>,
                                   Collection<GeneratorLP>,
                                   Collection<LineLP>,
                                   Collection<GeneratorProfileLP>,
                                   Collection<DemandProfileLP>,
                                   Collection<CapacityProfileLP>,
                                   Collection<BatteryLP>,
                                   Collection<ThermalNodeLP>,
                                   Collection<ThermalStorageLP>,
                                   Collection<HydrogenNodeLP>,
                                   Collection<HydrogenStorageLP>,
                                   Collection<AmmoniaNodeLP>,
                                   Collection<AmmoniaStorageLP>,
                                   Collection<CarrierConverterLP>,
                                   Collection<AllowancePoolLP>,
                                   Collection<ReserveZoneLP>,
                                   Collection<ReserveProvisionLP>,
                                   Collection<FuelLP>,
                                   Collection<EmissionLP>,
                                   Collection<EmissionZoneLP>,
                                   Collection<EmissionSourceLP>,
                                   Collection<CommitmentLP>,
                                   Collection<SimpleCommitmentLP>,
                                   Collection<LineCommitmentLP>,
                                   Collection<ConverterLP>,
                                   Collection<InertiaZoneLP>,
                                   Collection<InertiaProvisionLP>,
                                   Collection<JunctionLP>,
                                   Collection<WaterwayLP>,
                                   Collection<FlowLP>,
                                   Collection<ReservoirLP>,
                                   Collection<ReservoirSeepageLP>,
                                   Collection<ReservoirDischargeLimitLP>,
                                   Collection<TurbineLP>,
                                   Collection<PumpLP>,
                                   Collection<ReservoirProductionFactorLP>,
                                   Collection<FlowRightLP>,
                                   Collection<VolumeRightLP>,
                                   Collection<LngTerminalLP>,
                                   Collection<DecisionVariableLP>,
                                   Collection<PlantLP>,
                                   Collection<UserConstraintLP>>;

  /// @return The full collections tuple.
  ///
  /// @note No built-collections assertion fires here because legitimate
  /// callers (``SystemContext`` ctor, ``rebind_system``) capture
  /// per-collection pointer addresses by reference at SystemLP
  /// construction time — when the collections are still empty.
  /// Element-reading callers should use ``elements<X>()`` /
  /// ``element<X>(id)`` (asserts) or ``visit_elements(collections(),
  /// ...)`` after explicit ``rebuild_collections_if_needed()``.
  template<typename Self>
  [[nodiscard]] constexpr auto&& collections(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_collections_;
  }

  /// Diagnostic: are the per-cell XLP collection wrappers currently
  /// resident?  Under compress they are dropped after construction and
  /// rebuilt-and-retained on first `update_lp_for_phase` — used by the
  /// memory-accounting pass to count how many cells hold their ~per-cell
  /// collection footprint simultaneously.
  [[nodiscard]] constexpr bool collections_resident() const noexcept
  {
    return m_collections_built_;
  }

  /**
   * @brief Access the underlying system
   * @return Reference to the system
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& system(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_system_.get();
  }

  /**
   * @brief Get system context
   * @return Const reference to system context
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& system_context(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_system_context_;
  }

  /**
   * @brief Access linear interfaces
   * @return Linear interfaces container
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& linear_interface(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_linear_interface_;
  }

  /// Walk the live (scenario, stage, block) cells of this (scene, phase)
  /// LP and accumulate the four convergence-indicator sums into
  /// `linear_interface().solver_stats()`.  Reads slack column values
  /// directly from the just-solved backend; must be called *after*
  /// `linear_interface().resolve()` returned successfully and *before*
  /// `release_backend()`.
  ///
  /// Single-writer on the owning thread — the same regime as the
  /// SDDP forward-pass solve itself, so no locking is required.
  ///
  /// @param scene_index Scene index this SystemLP belongs to.  Currently
  ///        used only for trace tagging — the SystemLP already owns its
  ///        `scene()` / `phase()` references.
  /// @param phase_index Phase index this SystemLP belongs to (same).
  ///
  /// May throw `std::runtime_error` if `elements<X>()` is called before
  /// `rebuild_collections_if_needed()` under `low_memory != off`.  The
  /// SDDP forward-pass call site guarantees the collections are live
  /// (we are inside a `li.resolve()` success path that just touched
  /// them), so the throw is a contract violation rather than an
  /// expected failure mode.
  void accumulate_convergence_indicators(SceneIndex scene_index,
                                         PhaseIndex phase_index);

  /**
   * @brief Get system options
   * @return Const reference to options
   */
  [[nodiscard]] constexpr const auto& options() const noexcept
  {
    return system_context().options();
  }

  /**
   * @brief Add element to appropriate collection
   * @param e Element to add
   * @return Result of collection push_back
   */
  template<typename Element>
  constexpr auto push_back(Element&& e) noexcept(noexcept(
      std::declval<Collection<Element>>().push_back(std::forward<Element>(e))))
  {
    return std::get<Collection<Element>>(m_collections_)
        .push_back(std::forward<Element>(e));
  }

  /**
   * @brief Get all elements of specific type
   * @return Reference to elements container
   *
   * @throws std::runtime_error if ``m_collections_built_ == false`` —
   * the caller forgot to invoke ``rebuild_collections_if_needed()``
   * after a ``release_backend()`` under non-``off`` low_memory.
   * Throwing (rather than asserting) lets test fixtures and outer
   * frames catch the contract violation and shut down cleanly with a
   * useful message; without the check the access silently returns an
   * empty range and the next subscript triggers a vector OOB abort.
   */
  template<typename Element, typename Self>
  [[nodiscard]] constexpr auto&& elements(this Self&& self)
  {
    if (!self.m_collections_built_) {
      throw std::runtime_error(
          "SystemLP::elements<X>() read with empty collections — call "
          "rebuild_collections_if_needed() after release_backend() under "
          "low_memory != off");
    }
    return std::get<Collection<Element>>(
               std::forward<Self>(self).m_collections_)
        .elements();
  }

  /**
   * @brief Get element by ID
   * @param self     The object instance (deduced via explicit object parameter)
   * @param id       Element ID
   * @return Reference to the element
   *
   * @throws std::runtime_error — same low_memory caveat as
   * ``elements()``.  Collections must be live
   * (``m_collections_built_ == true``) at the point of access.
   */
  template<typename Element, typename Self, template<typename> class Id>
  [[nodiscard]] constexpr auto&& element(this Self&& self,
                                         const Id<Element>& id)
  {
    if (!self.m_collections_built_) {
      throw std::runtime_error(
          "SystemLP::element<X>() read with empty collections — call "
          "rebuild_collections_if_needed() after release_backend() under "
          "low_memory != off");
    }
    return std::get<Collection<Element>>(
               std::forward<Self>(self).m_collections_)
        .element(id);
  }

  /**
   * @brief Get index of element by ID
   * @param id Element ID
   * @return Index of the element
   */
  template<typename Element, template<typename> class Id>
  [[nodiscard]] constexpr auto element_index(const Id<Element>& id) const
  {
    return std::get<Collection<Element>>(m_collections_).element_index(id);
  }

  template<typename Element, template<typename> class Id>
  [[nodiscard]] constexpr bool contains(const Id<Element>& id) const noexcept
  {
    return std::get<Collection<Element>>(m_collections_).contains(id);
  }

  /**
   * @brief Get system name
   * @return System name as string view
   */
  [[nodiscard]] constexpr NameView name() const noexcept
  {
    return system().name;
  }

  void create_lp(const LpMatrixOptions& flat_opts_in = {});

  /**
   * @brief Update LP elements for all (scenario, stage) pairs in this system.
   *
   * Iterates over all scenarios and stages in this SystemLP and dispatches
   * `element.update_lp()` to every collection element that satisfies the
   * `HasUpdateLP` concept.  May update coefficients, bounds, or RHS values.
   *
   * @return Total number of LP modifications across all elements
   */
  [[nodiscard]] int update_lp();

  /**
   * @brief Writes the LP problem to a file.
   * @param filename Base file name (phase/scene labels and .lp extension
   *                 are appended automatically).
   * @return The full path of the written file (with .lp extension).
   */
  [[nodiscard]] std::expected<std::string, Error> write_lp(
      const std::string& filename) const;

  /**
   * @brief Resolves the linear programming problem
   * @param solver_options Configuration options for the LP solver
   * @return Expected with solver status code (0 = optimal) or error
   */
  [[nodiscard]] std::expected<int, Error> resolve(
      const SolverOptions& solver_options = {});

  /**
   * @brief Write output for all linear interfaces
   */
  void write_out();

  /// Process-wide write_out instrumentation.  These accumulate the
  /// per-cell wall time and cell count across every `SystemLP::write_out`
  /// call in the process — used by the runner to print an honest "total
  /// per-cell write_out wall" line at the end of the run.
  ///
  /// **Why this exists**: the `Write output time X.Xs` line in the
  /// runner only measures the final `PlanningLP::write_out()` flush.
  /// Under SDDP every cell is written during the forward pass (each
  /// iteration's `oc.write` is dispatched to a thread pool), so by the
  /// time the final flush runs there is nothing left to write — the
  /// "Write output time" line shows 0.x s for runs that actually wrote
  /// gigabytes of parquet, confusingly.  This counter restores the real
  /// number.
  ///
  /// Wall time semantics: this is *cumulative per-cell wall*, not
  /// process wall.  Per-cell writes run in parallel under the cell
  /// pool, so the wall clock spent in writes is `total_write_ms /
  /// concurrency`.  The cumulative number is the useful one to compare
  /// against other costs (e.g. cumulative solve time).
  [[nodiscard]] static double total_write_ms() noexcept;
  [[nodiscard]] static std::size_t total_write_cells() noexcept;

  /// True when `write_out()` has already emitted this cell's element
  /// tables (set by the SDDP simulation pass right after its final
  /// per-cell solve).  Callers that iterate every cell to write output
  /// should skip cells that return true to avoid overwriting the
  /// sim-pass output with values read from a rehydrated (possibly
  /// un-solved) backend under `compress` / `rebuild`.
  [[nodiscard]] constexpr bool output_written() const noexcept
  {
    return m_output_written_;
  }

  /// Reset the `output_written()` flag so the next `write_out()` call
  /// re-emits this cell.  Intended for the SDDP simulation-pass
  /// feasibility-recovery loop: when a later phase fails its solve and
  /// installs a feasibility cut on this phase's LP, this phase must be
  /// re-solved (yielding new state-variable values) and its output
  /// re-written to reflect the new trajectory.  File writes overwrite
  /// in place (parquet: hive partition + "part"; csv: per-(scene,phase)
  /// shard), so clearing the flag is all that is needed.
  constexpr void clear_output_written() noexcept { m_output_written_ = false; }

  /// Skip this cell in `PlanningLP::write_out`.  Set when the owning
  /// SDDP method has permanently excluded the scene (elastic filter
  /// produced no feasibility cut, so no valid solution exists for
  /// this cell).  Avoids the wasted rebuild + re-solve that the
  /// write path would otherwise do to "rehydrate" the cell.
  [[nodiscard]] constexpr bool output_skipped() const noexcept
  {
    return m_output_skipped_;
  }
  constexpr void set_output_skipped(bool v) noexcept { m_output_skipped_ = v; }

  /// Access the LP fingerprint computed during create_lp().
  [[nodiscard]] constexpr const LpFingerprint& fingerprint() const noexcept
  {
    return m_fingerprint_;
  }

  [[nodiscard]] constexpr const auto& phase() const noexcept
  {
    return m_phase_;
  }

  [[nodiscard]] constexpr const auto& scene() const noexcept
  {
    return m_scene_;
  }

  [[nodiscard]] constexpr const auto& single_bus_id() const noexcept
  {
    return m_single_bus_id_;
  }

  template<typename Id>
  [[nodiscard]] constexpr bool is_single_bus(const Id& id) const
  {
    if (m_single_bus_id_) {
      auto&& sid = *m_single_bus_id_;
      return sid.index() == 0 ? std::get<0>(sid) == id.first
                              : std::get<1>(sid) == id.second;
    }
    return false;
  }

private:
  std::reference_wrapper<const System> m_system_;
  SystemContext m_system_context_;
  collections_t m_collections_;
  PhaseLP m_phase_;
  SceneLP m_scene_;
  LinearInterface m_linear_interface_;
  LpFingerprint m_fingerprint_;
  std::optional<ObjectSingleId<BusLP>> m_single_bus_id_ {};

  /// Flat-assembly options captured at construction.  Retained for
  /// the lifetime of the SystemLP so write-out / accessor paths can
  /// resolve scale_objective and the active memory mode at any time.
  LpMatrixOptions m_flat_opts_ {};

  /// True once the LP fingerprint has been computed.
  bool m_fingerprint_was_set_ {false};

  /// True once `create_collections` has been called.  Set eagerly in
  /// the ctor for both `off` and `compress` modes.  Before the flag
  /// is set, `m_collections_` is default-constructed (empty) and no
  /// LP element wrapper has been allocated for this cell.
  bool m_collections_built_ {false};

  /// True once the *full* collection set (HasUpdateLP + disposable
  /// element types) has been populated.  Goes false after
  /// `clear_disposable_collections()` empties the 22 non-resident
  /// types — at that point `m_collections_built_` STAYS true (the
  /// HasUpdateLP types are still alive, with their per-(scen, stg)
  /// `update_lp` state preserved across SDDP iterations).
  ///
  /// `rebuild_collections_if_needed()` triggers if either flag is
  /// false: a full rebuild (create_collections + flatten) re-populates
  /// both the disposable types and the HasUpdateLP types' XLP indices.
  /// `update_lp` deliberately skips `rebuild_collections_if_needed`
  /// because its `visit_elements` only acts on HasUpdateLP types,
  /// which are always alive — iterating empty disposable collections
  /// is a fast no-op.
  bool m_disposable_collections_built_ {false};

  /// True once `write_out()` has emitted this cell's element tables.
  /// Set by the SDDP simulation pass, which calls `write_out()` right
  /// after the final per-cell solve while the backend (and hence
  /// `col_sol` / `row_dual`) is still live — this is the one guaranteed
  /// point where the output reflects the same LP the SDDP iteration
  /// actually solved, regardless of `low_memory_mode`.  Subsequent
  /// invocations (from `PlanningLP::write_out`) skip the cell so that
  /// output is solution-invariant between `off` and `compress`.
  bool m_output_written_ {false};

  /// True when this cell belongs to a scene the simulation pass
  /// declared infeasible (no valid primal/dual vectors exist).  Set
  /// by the sim pass's post-failure cleanup; consumed by
  /// `PlanningLP::write_out` to skip the cell without triggering a
  /// rehydrate + re-solve round-trip that would produce meaningless
  /// output.
  bool m_output_skipped_ {false};

  /// When true, `release_backend()` also drops the disposable XLP
  /// collection wrappers (`clear_disposable_collections()`), bounding the
  /// resident-collections footprint to the active working set instead of
  /// keeping all `num_cells` resident for the whole solve (the dominant
  /// floor under compress on large models).  Set by the solve method only
  /// when a process memory limit is configured — the rebuild-on-next-touch
  /// cost (the reason P3 stopped dropping by default) is paid only when
  /// memory is actually the binding constraint.  Default false preserves
  /// the keep-resident behaviour when no limit is set.
  bool m_drop_collections_on_release_ {false};

  /// Deferred dependent-variable links recorded during this phase's
  /// `add_to_lp` pass.  Under parallel phase construction within a
  /// scene, phase N+1 cannot safely call `add_dependent_variable` on
  /// phase N's `StateVariable` (it may not yet exist, and concurrent
  /// vector growth is not thread-safe).  Instead, the dependent side
  /// records a `PendingStateLink` here, and a sequential tightening
  /// pass over `phase_systems[scene_index]` resolves each link after
  /// the parallel build joins.  Storage lives on `SystemLP` (not on
  /// `SimulationLP`) because every link is intra-scene by construction
  /// — there is no cross-scene access pattern, and partitioning a
  /// centralized registry by scene would be needless indirection.
  ///
  /// Written by exactly one thread (the phase task that owns this
  /// `SystemLP`); drained by exactly one thread during tightening.
  std::vector<PendingStateLink> m_pending_state_links_;

  /// Transient pointer to the previous phase's LinearInterface, set by
  /// dispatch_update_lp() before calling update_lp().  Allows update_lp
  /// elements to look up the previous phase's efin when computing vini
  /// for cross-phase boundaries.  nullptr when phase == 0 or not set.
  const SystemLP* m_prev_phase_sys_ {nullptr};

public:
  /// Set the previous phase's SystemLP (nullptr for phase 0).
  constexpr void set_prev_phase_sys(const SystemLP* prev_sys) noexcept
  {
    m_prev_phase_sys_ = prev_sys;
  }

  /// Get the previous phase's SystemLP (nullptr when not set).
  [[nodiscard]] constexpr const SystemLP* prev_phase_sys() const noexcept
  {
    return m_prev_phase_sys_;
  }

  // ── Deferred state-variable linking ───────────────────────────────────
  //
  // See `m_pending_state_links_` for the rationale.  Called from element
  // `add_to_lp` (via `SystemContext::defer_state_link`) when phase N+1
  // would otherwise call `add_dependent_variable` on phase N's
  // `StateVariable` directly.  The `here_key` carries this phase's
  // `(scene, phase)` identity so the tightening pass can construct the
  // dependent `LPVariable` without re-deriving it from `*this`.

  void defer_state_link(StateVariable::Key prev_key,
                        LPKey here_key,
                        ColIndex here_col)
  {
    m_pending_state_links_.emplace_back(PendingStateLink {
        .prev_key = prev_key,
        .here_key = here_key,
        .here_col = here_col,
    });
  }

  template<typename Self>
  [[nodiscard]] constexpr auto&& pending_state_links(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_pending_state_links_;
  }

  // ── Low-memory mode API (thin forwarding to LinearInterface) ──────────

  void set_low_memory(LowMemoryMode mode,
                      CompressionCodec codec = CompressionCodec::lz4)
  {
    m_linear_interface_.set_low_memory(mode, codec);
    // Keep m_flat_opts_ in sync so write-out / accessor paths see the
    // active mode + codec.
    m_flat_opts_.low_memory_mode = mode;
    m_flat_opts_.memory_codec = codec;
  }

  /// Opt into dropping the disposable XLP collection wrappers on every
  /// `release_backend()` (see `m_drop_collections_on_release_`).  Enabled
  /// by the solve method when a process memory limit is configured, so the
  /// resident-collections floor is bounded to the active working set at the
  /// cost of a rebuild on the next touch.  No-op under `LowMemoryMode::off`
  /// (collections must stay live there).
  void set_drop_collections_on_release(bool enable) noexcept
  {
    m_drop_collections_on_release_ = enable;
  }

  /// Release the solver backend and (under any non-`off` low-memory
  /// mode) drop the per-cell collection wrappers.  The memory ceiling
  /// under compress becomes `active_workers × per-cell` instead
  /// of `num_cells × per-cell` resident forever.
  ///
  /// Post-release rehydration paths:
  ///  - For LP solve / mutate (add_col, add_row, set_*): call
  ///    ``ensure_lp_built()`` (or just call the mutator — it auto-fires
  ///    ``ensure_backend()``).  Backend only; collections stay empty.
  ///  - For element/collection READS (``elements<X>()``,
  ///    ``collections()``, ``write_out``, backward-pass aperture
  ///    bound updates): call ``rebuild_collections_if_needed()`` first.
  ///    That repopulates ``m_collections_`` via a throw-away flatten
  ///    and leaves the solver backend alone (so cached primal/dual
  ///    from the last solve are still served by LinearInterface
  ///    getters under compress).
  ///
  /// Debug builds assert in ``elements<X>()`` / ``element()`` /
  /// ``collections()`` if the read is performed without a prior
  /// rebuild — release builds silently see empty containers and
  /// likely OOB on subsequent indexing.
  void release_backend()
  {
    m_linear_interface_.release_backend();
    // P3 default — keep `m_collections_` alive across release_backend
    // cycles.  The XLP wrappers are ~30 MB/cell; keeping them resident for
    // all cells dominates the compress-mode floor on large models
    // (≈ num_cells × 30 MB).  P3 chose to keep them because re-running
    // `flatten_from_collections` inside `rebuild_collections_if_needed` on
    // every `update_lp_for_phase` was costly when memory was NOT the
    // binding constraint.
    //
    // When a process memory limit IS configured, that trade flips: drop
    // the disposable wrappers here so the resident-collections footprint
    // is bounded to the active working set (`active_workers × per-cell`)
    // instead of `num_cells × per-cell`.  `clear_disposable_collections()`
    // keeps the `HasUpdateLP` collections that `update_lp_for_phase`
    // needs, so this stays correctness-safe; read sites (write_out,
    // aperture bound updates, elastic fcut) already call
    // `rebuild_collections_if_needed()` to rehydrate on demand.  The
    // rebuild cost is paid only under memory pressure — exactly when it is
    // worth trading CPU for RAM.
    // FULL drop (not just disposables): the kept `HasUpdateLP` wrappers
    // are the dominant ~25 MB/cell floor, so reclaiming them is the point.
    // `update_lp()` and other collection readers rebuild on demand from
    // the shared `System`.
    if (m_drop_collections_on_release_) {
      clear_collections_for_eviction();
    }
  }

  void reconstruct_backend() { m_linear_interface_.reconstruct_backend(); }

  /// Ensure the LP backend is live and ready to solve.  Pure backend
  /// reconstruct — does NOT rebuild ``m_collections_``.  Callers that
  /// need the XLP element wrappers populated (e.g. to read
  /// ``elements<X>()`` / ``collections()`` after a prior
  /// ``release_backend()`` under non-``off`` low-memory) must follow
  /// up with ``rebuild_collections_if_needed()``.
  ///
  /// History: a previous version of this function ran a shadow flatten
  /// to also populate ``m_collections_``, but that allocated hundreds
  /// of MB per cell — under compress / rebuild it ballooned RSS by
  /// ~40% on the juan case (tens of thousands of flattens per run via
  /// the per-phase solve hot path).  Collections are now rehydrated
  /// only at the explicit call sites that need them
  /// (``rebuild_collections_if_needed()``).
  ///
  /// Per-mode behavior:
  ///  - `off`: no-op (backend always live).
  ///  - `snapshot` / `compress`: reconstruct backend from snapshot if
  ///    released.
  ///  - `rebuild`: invoke the SystemLP-owned rebuild callback (which
  ///
  /// Callers that subsequently mutate the LP (add_col, add_row, set_*)
  /// can skip the explicit call — those mutations invoke ensure_backend
  /// themselves.
  void ensure_lp_built();

  /// Under `LowMemoryMode::compress`, rebuild the per-element XLP
  /// state (generation_cols, capacity_rows, …) if it was dropped by a
  /// prior `release_backend()`.  Runs `create_collections()` followed
  /// by a throw-away flatten whose sole purpose is the `add_to_lp()`
  /// side effects on the XLP wrappers — the produced
  /// `FlatLinearProblem` is discarded, the solver backend is
  /// untouched.  No-op under `off` (collections never drop).
  /// Invoked from `ensure_lp_built()` so any caller that does
  /// `ensure_lp_built → read collections` works transparently.
  void rebuild_collections_if_needed();

  /// Move-assign empty every `Collection<X>` whose element type does
  /// NOT satisfy `HasUpdateLP<X>`.  After
  /// `add_to_lp`/`flatten`/`tighten_scene_phase_links` complete, those
  /// collections are pure dead weight: their data has already been
  /// flattened into the LP snapshot, the only remaining readers (write_out)
  /// rehydrate via `rebuild_collections_if_needed()`, and `update_lp` does
  /// not touch them.
  ///
  /// No-op outside `LowMemoryMode::compress` — under `off` the collections
  /// must stay live for `release_backend` to be a no-op on the existing
  /// live LP.
  ///
  /// Collection objects' addresses inside `m_collections_` stay valid
  /// (the tuple slot doesn't move); only their internal element vectors
  /// are emptied.  `SystemContext::m_collection_ptrs_` therefore keeps
  /// pointing at valid (now-empty) Collection objects.  Any subsequent
  /// `elements<X>()` call returns an empty range; `element<X>(uid)`
  /// would throw out-of-range — production paths that reach this code
  /// after the drop must first call `rebuild_collections_if_needed()`.
  void clear_disposable_collections();

  /// Memory-limited eviction: drop the ENTIRE collection tuple (disposable
  /// AND `HasUpdateLP` types) while keeping the LinearInterface snapshot +
  /// replay intact, so the cell can reconstruct on next touch.  Unlike
  /// `clear_disposable_collections()` (keeps HasUpdateLP for `update_lp`),
  /// this reclaims the dominant compress-mode floor — but requires every
  /// collection reader, including `update_lp()`, to call
  /// `rebuild_collections_if_needed()` first (they do).  No-op under `off`
  /// or when already empty.  See `release_backend()`'s memory-limited path.
  void clear_collections_for_eviction() noexcept;

  /// End-of-life drop for a cell whose parquet output is already on
  /// disk and which will never see another LP touch (read, mutate, or
  /// solve) in the process lifetime.  Strictly more aggressive than
  /// `clear_disposable_collections()` + `release_backend()` +
  /// `clear_snapshot()` because it also drops:
  ///
  ///   * The HasUpdateLP collections that `clear_disposable_collections()`
  ///     deliberately preserves for subsequent `update_lp_for_phase`
  ///     calls (~30 MB / cell on juan-scale).  Those calls will never
  ///     happen on the write_out path.
  ///   * The cut replay journal in `LinearInterface::m_replay_`.  On
  ///     recovery hot-starts that loaded tens of thousands of cuts
  ///     into the journal this is the dominant per-cell residue —
  ///     several GiB on the 2-year case.
  ///   * The cached primal/dual/reduced-cost vectors in `m_cache_`.
  ///     No downstream consumer reads them after write_out.
  ///
  /// Must be called *after* `write_out()` completes for this cell.
  /// The caller (typically `PlanningLP::write_out`'s `emit_cell`
  /// closure) must have already invoked `release_backend()` and
  /// `clear_snapshot()` — this method only adds drops; it does not
  /// duplicate those calls.  Idempotent: safe to invoke multiple
  /// times.  After this returns, the cell still has its scalar
  /// metadata (UIDs, options, fingerprint) but any
  /// `elements<X>()` / `get_col_sol()` access would now read from an
  /// empty cache — production paths must avoid touching such cells.
  void drop_for_write_out_done() noexcept;

  /// Forward accessor to the LP's cumulative solver counters.
  [[nodiscard]] constexpr const SolverStats& solver_stats() const noexcept
  {
    return m_linear_interface_.solver_stats();
  }

  /// Forward the clone-merge helper so callers that spin up a cloned
  /// LP for elastic filtering can fold its counters back in before it
  /// is destroyed.
  constexpr void merge_solver_stats(const SolverStats& other) noexcept
  {
    m_linear_interface_.merge_solver_stats(other);
  }

  void record_dynamic_col(SparseCol col)
  {
    m_linear_interface_.record_dynamic_col(std::move(col));
  }

  bool update_dynamic_col_lowb(std::string_view class_name,
                               std::string_view variable_name,
                               double new_lowb) noexcept
  {
    return m_linear_interface_.update_dynamic_col_lowb(
        class_name, variable_name, new_lowb);
  }

  bool update_dynamic_col_bounds(std::string_view class_name,
                                 std::string_view variable_name,
                                 double new_lowb,
                                 double new_uppb) noexcept
  {
    return m_linear_interface_.update_dynamic_col_bounds(
        class_name, variable_name, new_lowb, new_uppb);
  }

  bool update_dynamic_col_bounds(std::string_view class_name,
                                 std::string_view variable_name,
                                 Uid variable_uid,
                                 double new_lowb,
                                 double new_uppb) noexcept
  {
    return m_linear_interface_.update_dynamic_col_bounds(
        class_name, variable_name, variable_uid, new_lowb, new_uppb);
  }

  void record_cut_row(SparseRow row)
  {
    m_linear_interface_.record_cut_row(std::move(row));
  }

  void record_cut_deletion(std::span<const int> deleted_indices)
  {
    m_linear_interface_.record_cut_deletion(deleted_indices);
  }

  [[nodiscard]] bool is_backend_released() const noexcept
  {
    return m_linear_interface_.is_backend_released();
  }

  [[nodiscard]] LowMemoryMode low_memory_mode() const noexcept
  {
    return m_linear_interface_.low_memory_mode();
  }
};

}  // namespace gtopt
