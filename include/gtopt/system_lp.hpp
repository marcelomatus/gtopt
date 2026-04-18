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

#include <gtopt/battery_lp.hpp>
#include <gtopt/bus_lp.hpp>
#include <gtopt/collection.hpp>
#include <gtopt/commitment_lp.hpp>
#include <gtopt/converter_lp.hpp>
#include <gtopt/demand_lp.hpp>
#include <gtopt/demand_profile_lp.hpp>
#include <gtopt/flow_lp.hpp>
#include <gtopt/flow_right_lp.hpp>
#include <gtopt/generator_lp.hpp>
#include <gtopt/generator_profile_lp.hpp>
#include <gtopt/inertia_provision_lp.hpp>
#include <gtopt/inertia_zone_lp.hpp>
#include <gtopt/junction_lp.hpp>
#include <gtopt/line_lp.hpp>
#include <gtopt/linear_interface.hpp>
#include <gtopt/lng_terminal_lp.hpp>
#include <gtopt/lp_fingerprint.hpp>
#include <gtopt/lp_matrix_options.hpp>
#include <gtopt/output_context.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
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
static_assert(AddToLP<BatteryLP>);
static_assert(AddToLP<ConverterLP>);
static_assert(AddToLP<ReserveZoneLP>);
static_assert(AddToLP<ReserveProvisionLP>);
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
static_assert(AddToLP<UserConstraintLP>);

/**
 * @concept HasUpdateLP
 * @brief Concept satisfied by LP element types that implement `update_lp()`.
 *
 * Used by SystemLP::update_lp() to iterate over the LP element collection
 * and dispatch `update_lp()` only to types that implement it.
 */
template<typename T>
concept HasUpdateLP = requires(T& obj,
                               SystemLP& system_lp,
                               const ScenarioLP& scenario,
                               const StageLP& stage) {
  { obj.update_lp(system_lp, scenario, stage) } -> std::same_as<int>;
};

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
                    LpMatrixOptions flat_opts = {});

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
  using collections_t = std::tuple<Collection<BusLP>,
                                   Collection<DemandLP>,
                                   Collection<GeneratorLP>,
                                   Collection<LineLP>,
                                   Collection<GeneratorProfileLP>,
                                   Collection<DemandProfileLP>,
                                   Collection<BatteryLP>,
                                   Collection<ConverterLP>,
                                   Collection<ReserveZoneLP>,
                                   Collection<ReserveProvisionLP>,
                                   Collection<CommitmentLP>,
                                   Collection<SimpleCommitmentLP>,
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
                                   Collection<UserConstraintLP>>;

  template<typename Self>
  [[nodiscard]] constexpr auto&& collections(this Self&& self) noexcept
  {
    return std::forward<Self>(self).m_collections_;
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
   */
  template<typename Element, typename Self>
  [[nodiscard]] constexpr auto&& elements(this Self&& self) noexcept
  {
    return std::get<Collection<Element>>(
               std::forward<Self>(self).m_collections_)
        .elements();
  }

  /**
   * @brief Get element by ID
   * @param self     The object instance (deduced via explicit object parameter)
   * @param id       Element ID
   * @return Reference to the element
   */
  template<typename Element, typename Self, template<typename> class Id>
  [[nodiscard]] constexpr auto&& element(this Self&& self,
                                         const Id<Element>& id)
  {
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

  /// Flat-assembly options captured at construction.  Kept alive so that
  /// `LowMemoryMode::rebuild` can re-invoke `create_lp(m_flat_opts_)`
  /// inside every `ensure_lp_built()` call.
  LpMatrixOptions m_flat_opts_ {};

  /// True once the LP fingerprint has been computed.  In rebuild mode
  /// subsequent rebuilds skip recomputation (the model is deterministic
  /// and re-hashing would be wasted work).
  bool m_fingerprint_was_set_ {false};

  /// True once `create_collections` has been called.  Non-rebuild modes
  /// set this in the ctor (eager build).  Rebuild mode flips it on the
  /// first `rebuild_in_place` call, amortising the ~0.4s per-cell
  /// collection construction cost across the SDDP iterations that
  /// actually touch the cell.  Before the flag is set, `m_collections_`
  /// is default-constructed (empty) and no LP element wrapper has been
  /// allocated for this cell.
  bool m_collections_built_ {false};

  /// Exact (ncols, nrows) from the first successful flatten.  Used as
  /// the reserve hint on subsequent `LowMemoryMode::rebuild` flatten
  /// passes — matches the actual sizes and avoids the vector-growth
  /// reallocations that the shape heuristic tends to trigger.  Zero
  /// until the first flatten completes.
  size_t m_last_flat_ncols_ {0};
  size_t m_last_flat_nrows_ {0};

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
    // Keep m_flat_opts_ in sync so rebuild_in_place() re-flattens with the
    // correct mode + codec.  Also install the rebuild callback: callers
    // may flip from `off` → `rebuild` mid-run (SDDPMethod::initialize_solver
    // does exactly that on already-constructed SystemLPs), and
    // ensure_backend needs the callback for the lazy rebuild to fire.
    m_flat_opts_.low_memory_mode = mode;
    m_flat_opts_.memory_codec = codec;
    install_rebuild_callback();
  }

  /// Release the solver backend + (under any non-`off` low-memory mode)
  /// drop the per-cell collection wrappers.  The memory ceiling under
  /// compress/rebuild becomes the active-workers × per-cell-collections
  /// footprint (plus the compressed snapshot), not 816 cells × collections.
  /// The next access lazily re-builds them: `rebuild_in_place` for
  /// rebuild mode, `write_out` for compress mode.
  void release_backend() noexcept
  {
    m_linear_interface_.release_backend();
    if (m_flat_opts_.low_memory_mode != LowMemoryMode::off) {
      m_collections_ = collections_t {};
      m_collections_built_ = false;
    }
  }

  void reconstruct_backend(std::span<const double> col_sol = {},
                           std::span<const double> row_dual = {})
  {
    m_linear_interface_.reconstruct_backend(col_sol, row_dual);
  }

  /// Ensure the LP is built and ready to solve.  Thin wrapper around
  /// `LinearInterface::ensure_backend()`, which handles all three
  /// `low_memory_mode` paths uniformly:
  ///  - `off`: no-op (backend is always live).
  ///  - `snapshot` / `compress`: reconstruct from snapshot if released.
  ///  - `rebuild`: invoke the SystemLP-owned rebuild callback if released.
  ///
  /// Callers that subsequently mutate the LP (add_col, add_row, set_*)
  /// can skip the explicit call — those mutations invoke ensure_backend
  /// themselves.  Keep this entry point for pure-read code paths that
  /// would otherwise read stale cached row/col counts.
  void ensure_lp_built() { m_linear_interface_.ensure_backend(); }

  /// Regenerate the flat LP from the live element collections and load
  /// it into the existing `m_linear_interface_` in place.  Public only
  /// so that `LinearInterface::ensure_backend()` can reach it via its
  /// `m_rebuild_owner_` back-pointer; the intended caller is always
  /// that one path.  Does NOT replace `m_linear_interface_` — it
  /// mutates its backend via `install_flat_as_rebuild`, which is safe
  /// even when invoked from inside a LinearInterface method
  /// (e.g. `add_col` → `ensure_backend`).
  void rebuild_in_place();

private:
  /// Install the rebuild callback on `m_linear_interface_` so that any
  /// access to a released backend lazily regenerates the LP from
  /// collections.  Called from the constructor (when the LP is first
  /// configured) and from every move ctor / move assignment, since the
  /// captured `this` pointer is invalidated by a move.  No-op outside
  /// `LowMemoryMode::rebuild`.
  void install_rebuild_callback();

public:
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
