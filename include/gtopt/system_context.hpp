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

#include <format>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <typeinfo>
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
  explicit SystemContext(SimulationLP& simulation,
                         SystemLP& system,
                         SystemKind kind = SystemKind::forward);

  /// Which LP registry (forward vs aperture) this context's state-variable
  /// and link registrations are routed to.  Set once at construction.
  [[nodiscard]] constexpr auto kind() const noexcept -> SystemKind
  {
    return m_kind_;
  }

  /// Re-point the back-reference to a new SystemLP owner (used by
  /// `SystemLP`'s move-ctor/assign so the embedded SystemContext stays
  /// consistent after the SystemLP is relocated).  Also rebuilds
  /// `m_collection_ptrs_` from the new owner's collections tuple,
  /// because each pointer is an interior pointer into the SystemLP that
  /// just moved.  Defined out-of-line in `system_context.cpp` because
  /// reaching `SystemLP::collections()` requires `system_lp.hpp`.
  void rebind_system(SystemLP& sys) noexcept;

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

  /// Resolves a `Demand.fcost` (`OptTBRealSched`) at the (stage, block)
  /// grain, falling back to the global `model_options.demand_fail_cost`
  /// when unset.  Returned in `[$/MWh]`.
  template<typename FailCost>
  [[nodiscard]] constexpr auto demand_fail_cost(const StageLP& stage,
                                                const BlockLP& block,
                                                const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage.uid(), block.uid());
    return fc ? fc : options().demand_fail_cost();
  }

  /// Resolves a `ReserveZone.urcost` / `ReserveZone.drcost` /
  /// `InertiaZone.cost` (`OptTBRealSched`) at the (stage, block) grain,
  /// falling back to the global `model_options.reserve_shortage_cost`.
  /// Returned in `[$/MW]` (reserves) or `[$/MWs]` (inertia).
  template<typename FailCost>
  [[nodiscard]] constexpr auto reserve_shortage_cost(
      const StageLP& stage, const BlockLP& block, const FailCost& fcost) const
  {
    const auto fc = fcost.optval(stage.uid(), block.uid());
    return fc ? fc : options().reserve_shortage_cost();
  }

  /// Resolves an `OptTBRealSched`-typed state cost (e.g. `Reservoir.scost`,
  /// `LngTerminal.scost`) at the (stage, block) grain, falling back to
  /// the global `model_options.state_violation_cost`.  Returned in
  /// `[$/MWh]` (energy-equivalent — multiplied by element-specific
  /// `mean_production_factor` at the caller).
  template<typename StateCost>
  [[nodiscard]] constexpr auto state_violation_cost(
      const StageLP& stage, const BlockLP& block, const StateCost& scost) const
  {
    const auto sc = scost.optval(stage.uid(), block.uid());
    return sc ? sc : options().state_violation_cost();
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
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      if (m_collection_ptrs_[idx] == nullptr) {
        throw std::logic_error(std::format(
            "SystemContext::get_element(ObjectSingleId): collection<{}> "
            "not registered (SystemContext used before "
            "SystemLP::register_collections())",
            typeid(Element).name()));
      }
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
      return static_cast<const Collection<Element>*>(m_collection_ptrs_[idx])
          ->element(id);
    }
  }

  template<typename Element>
  [[nodiscard]] auto get_element(const ElementIndex<Element>& id) const
      -> const Element&
  {
    constexpr auto idx = lp_type_index_v<Element>;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    if (m_collection_ptrs_[idx] == nullptr) {
      throw std::logic_error(std::format(
          "SystemContext::get_element(ElementIndex): collection<{}> "
          "not registered (SystemContext used before "
          "SystemLP::register_collections())",
          typeid(Element).name()));
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    return static_cast<const Collection<Element>*>(m_collection_ptrs_[idx])
        ->element(id);
  }

  /// Rebuild-pass flag.  Set to `true` by
  /// `SystemLP::rebuild_collections_if_needed` while a throw-away
  /// flatten regenerates the per-element XLP state (under
  /// `LowMemoryMode::compress`, after `release_backend()` dropped the
  /// disposable element wrappers).  Cross-phase / engine registry
  /// side-effects (`add_state_variable`, `add_integer_variable`,
  /// `defer_state_link`) become no-ops when the flag is set — the
  /// initial pass already populated those registries with matching col
  /// indices, and re-registering on rebuild would duplicate cross-phase
  /// links or burn CPU for zero gain.
  ///
  /// EXCEPTION — the per-cell AMPL variable/metadata registry
  /// (`add_ampl_variable`, `register_ampl_element_metadata`): the
  /// production solve path frees it after the initial flatten
  /// (`LpMatrixOptions::release_ampl_after_flatten` →
  /// `SimulationLP::release_ampl_cell`).  A subsequent rebuild flatten
  /// (e.g. `write_out`) re-runs `UserConstraintLP::add_to_lp`, which
  /// resolves element columns through that registry — so it MUST be
  /// repopulated, or user constraints fail with "unknown attribute".
  /// `set_replay_ampl_registry(true)` makes those two registrations
  /// run even on the silent pass; the writes are idempotent (same
  /// deterministic col indices) so replaying when the registry was NOT
  /// released is harmless.
  [[nodiscard]] constexpr bool silent_flatten_pass() const noexcept
  {
    return m_silent_flatten_pass_;
  }

  constexpr void set_silent_flatten_pass(bool v) noexcept
  {
    m_silent_flatten_pass_ = v;
  }

  /// Whether the AMPL variable/metadata registry should be repopulated
  /// even during a silent rebuild pass (see `silent_flatten_pass`).
  [[nodiscard]] constexpr bool replay_ampl_registry() const noexcept
  {
    return m_replay_ampl_registry_;
  }

  constexpr void set_replay_ampl_registry(bool v) noexcept
  {
    m_replay_ampl_registry_ = v;
  }

  // Methods to handle the state_variables
  //
  // Return-type note: this wrapper is `void` (the underlying
  // `SimulationLP::add_state_variable` returns the registered
  // `StateVariable&`, but no caller uses the return value — and making
  // the rebuild-pass early-return return a reference would require
  // looking the entry up again, defeating the purpose of gating).
  template<typename Key>
  constexpr void add_state_variable(Key&& key,
                                    ColIndex col,
                                    double scost,
                                    double var_scale,
                                    LpContext context)
  {
    if (m_silent_flatten_pass_) {
      // Rebuild pass: registry already holds an entry with the same col
      // (fresh flatten is deterministic).  Skip the SimulationLP call
      // entirely — no lock, no map lookup, no idempotency branch.
      return;
    }
    auto stamped = std::forward<Key>(key);
    stamped.lp_key.kind = m_kind_;  // route to forward/aperture registry
    std::ignore = simulation().add_state_variable(
        std::move(stamped), col, scost, var_scale, std::move(context));
  }

  /// Atomic helper: add a new state-variable column to the LP AND register
  /// it in the state-variable map.  Sets `is_state = true` on the column so
  /// it is recognized as a state variable; column names are available at
  /// `LpNamesLevel::all`, but state variable I/O uses the
  /// StateVariable map (ColIndex-based) directly.  The column's `context`
  /// field is also used as the StateVariable's context, keeping the two
  /// in sync.
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
    auto stamped = std::forward<Key>(key);
    stamped.lp_key.kind = m_kind_;  // look up in this context's registry
    return simulation().state_variable(std::move(stamped));
  }

  // ── Integer-variable choke-point ────────────────────────────────────────
  //
  // The "no integer column without `IntegerVariable`" invariant from
  // `docs/design/commitment-layout.md §5` is enforced here: every
  // producer of an integer LP column (`Commitment`, `SimpleCommitment`,
  // `Converter`, `CapacityObject`, future `Battery` / `Pump` /
  // `ReserveProvision`) routes through `add_integer_col` so cut
  // audits, integer-feasibility checks, and the Phase-2 binding stack
  // can iterate the set without grepping for `is_integer = true`.
  //
  // Phase 0 only adds the LP column and the registry entry — producers
  // still call `add_ampl_variable` themselves.  Auto-AMPL is Phase 1.

  /// Register one integer LP column in the simulation's integer-variable
  /// map.  Mirrors `add_state_variable` (void wrapper around
  /// `SimulationLP::add_integer_variable`).
  ///
  /// Honours the silent-flatten gate: under rebuild, the registry
  /// already holds an equivalent entry, so this call is a no-op (same
  /// rationale as the state-variable wrapper).
  template<typename Key>
  constexpr void add_integer_variable(Key&& key,
                                      ColIndex col,
                                      IntegerDomain domain,
                                      IntegerScope scope,
                                      GroupUid group_uid,
                                      std::span<const BlockUid> blocks)
  {
    if (m_silent_flatten_pass_) {
      return;
    }
    auto stamped = std::forward<Key>(key);
    stamped.lp_key.kind = m_kind_;  // route to forward / aperture registry
    std::ignore = simulation().add_integer_variable(
        std::move(stamped), col, domain, scope, group_uid, blocks);
  }

  /// Atomic helper: add an integer LP column AND register it in the
  /// integer-variable map.  The preferred API for every producer of an
  /// integer column.
  ///
  /// `domain` selects {Binary, Integer, Relaxed}; `Relaxed` keeps the
  /// column continuous (`is_integer = false`).  The producer is
  /// responsible for the relax precedence: per-element `relax` flag
  /// wins over the global option, which wins over the call-site
  /// default — `Commitment.relax` and `SimpleCommitment.relax` are
  /// already resolved before this call.
  ///
  /// `blocks` is the per-block fan-out for the registry's
  /// `IntegerVariable::blocks()` accessor.  For `Block` scope it is
  /// the single block uid; for `Group` scope it is every block in the
  /// group; for `Stage` scope it is every block of the stage; for
  /// `Phase` scope it should be empty (the registry returns
  /// `std::nullopt` to callers regardless of what is passed).
  template<typename Key, typename Col>
  auto add_integer_col(LinearProblem& lp,
                       Key&& key,
                       Col&& col,
                       IntegerDomain domain,
                       IntegerScope scope,
                       GroupUid group_uid,
                       std::span<const BlockUid> blocks) -> ColIndex
  {
    col.is_integer = (domain != IntegerDomain::Relaxed);
    const auto idx = lp.add_col(std::forward<Col>(col));
    add_integer_variable(
        std::forward<Key>(key), idx, domain, scope, group_uid, blocks);
    return idx;
  }

  template<typename Key>
  [[nodiscard]] constexpr auto get_integer_variable(Key&& key) const noexcept
  {
    auto stamped = std::forward<Key>(key);
    stamped.lp_key.kind = m_kind_;
    return simulation().integer_variable(std::move(stamped));
  }

  /// Queue a deferred dependent-variable link to be resolved later by
  /// the per-scene tightening pass.  Use this in element `add_to_lp`
  /// instead of calling `get_state_variable(prev_key)->add_dependent_variable`
  /// directly: under parallel phase construction within a scene, the
  /// previous phase's `StateVariable` may not yet exist when phase N+1
  /// runs, and concurrent vector growth on its dependent-variable list
  /// would race.
  ///
  /// `prev_key` identifies the producing `StateVariable` in the previous
  /// phase (its `lp_key.scene_index` / `lp_key.phase_index` are the
  /// previous phase's identity, set by the caller via
  /// `StateVariable::key(scenario, *prev_stage, ...)`).  `here_col` is
  /// the dependent column just added to *this* phase's LP — its
  /// `(scene, phase)` identity is taken from the bound `SystemLP`, so
  /// the caller need not pass it explicitly.
  ///
  /// Defined out-of-line in `system_context.cpp` because reaching
  /// `system().scene()` / `system().phase()` and `system().defer_state_link`
  /// requires the full `system_lp.hpp` definition.
  void defer_state_link(StateVariable::Key prev_key, ColIndex here_col) const;

  // ── PAMPL / user-constraint variable registry forwarders ────────────────
  //
  // Each LP element calls these from `add_to_lp` once per (scenario, stage)
  // to register its PAMPL-visible columns.  Mirrors `add_state_variable`.
  //
  // `class_name` and `attribute` must refer to storage with static (or at
  // least solve-long) lifetime — in practice the `constexpr string_view`
  // constants on each `*LP` class (e.g. `GeneratorLP::GenerationName`).

  /// Register a per-block column map (e.g., generator.generation).
  /// Stores a copy of `block_cols`; the element need not keep it alive.
  ///
  /// Marked `const` (logical-const): the underlying `SimulationLP` is
  /// held via `reference_wrapper`, so const SystemContext methods may
  /// still mutate the simulation's ampl variable registry.  This lets
  /// elements whose `add_to_lp` takes `const SystemContext&` register
  /// their PAMPL-visible columns without signature churn.
  ///
  /// Routes to the per-(scene, phase) cell of the current SystemLP —
  /// `system().scene().index()` / `system().phase().index()` — so the
  /// write needs no synchronization (one writer per cell, sequential
  /// across phases within a scene).  Defined out-of-line in
  /// `system_context.cpp` because reaching the SystemLP accessors
  /// requires the full `system_lp.hpp` definition.
  void add_ampl_variable(std::string_view class_name,
                         Uid element_uid,
                         std::string_view attribute,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         const BIndexHolder<ColIndex>& block_cols) const;

  /// Same as above but with per-block additive offsets.  Used by
  /// shifted-variable encodings (e.g., demand's Option C
  /// `neg_fail = load − lmax`).  The offsets feed into the user-
  /// constraint resolver's `param_shift` accumulator so references to
  /// `demand.load` resolve to (col + offset) physically.
  void add_ampl_variable(std::string_view class_name,
                         Uid element_uid,
                         std::string_view attribute,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         const BIndexHolder<ColIndex>& block_cols,
                         const BIndexHolder<double>& block_offsets) const;

  /// Register a stage-level scalar column (e.g., eini, efin, capainst).
  void add_ampl_variable(std::string_view class_name,
                         Uid element_uid,
                         std::string_view attribute,
                         const ScenarioLP& scenario,
                         const StageLP& stage,
                         ColIndex stage_col) const;

  /// Register a per-block **sum of LP cols** for a virtual aggregator
  /// attribute.  Used by `LineLP::add_to_lp` under `piecewise_direct`
  /// line-loss mode so AMPL references to `line.flowp` / `line.flown`
  /// expand to `Σ flowp_seg_k` / `Σ flown_seg_k` without ever
  /// materialising an aggregator LP column.  See
  /// `AmplVariable::block_cols_sum` for the data layout.
  void add_ampl_variable(
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      const ScenarioLP& scenario,
      const StageLP& stage,
      const BIndexHolder<std::vector<ColIndex>>& block_cols_sum) const;

  /// Register a per-block **weighted sum of LP cols** — every leg
  /// carries its own coefficient.  Used by `FuelLP::add_to_lp` to
  /// expose `fuel("X").offtake = Σ_g heat_rate_g · dur_b ·
  /// generation_g[b]` without creating an aggregator LP column +
  /// equality binding row (the substitute-out anti-pattern fix,
  /// mirror of the `EmissionZone.production` removal).  Resolver
  /// stamps `row[col] += base_coef · weight` per leg.  See
  /// `AmplVariable::block_cols_weighted_sum` for the data layout.
  void add_ampl_variable(
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      const ScenarioLP& scenario,
      const StageLP& stage,
      const BIndexHolder<std::vector<std::pair<ColIndex, double>>>&
          block_cols_weighted_sum) const;

  /// Look up a registered variable column.  Returns nullopt if the
  /// (class, uid, attribute, scenario, stage, block) combination was
  /// never registered.  Reads from the current SystemLP's
  /// `(scene, phase)` cell — the resolver always queries within the
  /// LP that owns the row being assembled.
  [[nodiscard]] std::optional<ColIndex> find_ampl_col(
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      ScenarioUid scenario_uid,
      StageUid stage_uid,
      BlockUid block_uid) const;

  /// Look up the per-block additive offset registered alongside a
  /// shifted variable (e.g., demand's Option C col).  Returns `0.0`
  /// when the variable has no offsets — the common case.
  [[nodiscard]] double find_ampl_offset(std::string_view class_name,
                                        Uid element_uid,
                                        std::string_view attribute,
                                        ScenarioUid scenario_uid,
                                        StageUid stage_uid,
                                        BlockUid block_uid) const;

  /// Look up a registered **sum-of-cols** attribute (virtual
  /// aggregator).  Returns an empty span when the attribute is not
  /// registered as a sum (callers fall back to `find_ampl_col`).
  [[nodiscard]] std::span<const ColIndex> find_ampl_cols(
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      ScenarioUid scenario_uid,
      StageUid stage_uid,
      BlockUid block_uid) const;

  /// Look up a registered **weighted sum-of-cols** attribute.  Returns
  /// an empty span when the attribute is not registered with weighted
  /// legs (callers fall through to single-col / unweighted-sum paths
  /// in that case).  Stamping rule: `row[col] += base_coef · weight`
  /// per leg.
  [[nodiscard]] std::span<const std::pair<ColIndex, double>>
  find_ampl_weighted_cols(std::string_view class_name,
                          Uid element_uid,
                          std::string_view attribute,
                          ScenarioUid scenario_uid,
                          StageUid stage_uid,
                          BlockUid block_uid) const;

  /// Look up a registered AMPL variable by key (no block).  Returns a
  /// node-stable pointer to the registry entry, or `nullptr` when
  /// unregistered.  The resolver uses this to hash the key ONCE per
  /// (term, block) and then read every per-block shape (col_at /
  /// cols_at / weighted_cols_at / offset_at) directly off the entry.
  [[nodiscard]] const AmplVariable* find_ampl_variable(
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      ScenarioUid scenario_uid,
      StageUid stage_uid) const;

  /// Resolve an element name to its Uid within a class.
  [[nodiscard]] std::optional<Uid> lookup_ampl_element_uid(
      std::string_view class_name, std::string_view element_name) const
  {
    return simulation().lookup_ampl_element_uid(class_name, element_name);
  }

  /// Is the (class, element_uid, attribute) triple registered as an LP
  /// variable somewhere in the simulation?  Used by the user-constraint
  /// resolver to distinguish "element/attribute exist, just no column
  /// for this specific (scene, phase, scenario, stage, block)"
  /// (treat-as-zero) from "attribute is a typo or unsupported on this
  /// element" (strict-mode error).  Delegates to
  /// `SimulationLP::find_ampl_variable_for_element`.
  [[nodiscard]] bool find_ampl_variable_for_element(
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute) const
  {
    return simulation().find_ampl_variable_for_element(
        class_name, element_uid, attribute);
  }

  /// Is the (class, attribute) pair registered as an LP variable for
  /// any element of the class?  Used to extend the user-constraint
  /// resolver's silent-0 leniency to the LP-attr-dormant case: the
  /// element name is known but ITS particular column was never
  /// materialised because it has zero capacity over the entire
  /// horizon — yet the attribute IS a valid LP variable on the class
  /// (some other element registers it).  PLEXOS treats the term as a
  /// constant 0 contribution to the LHS; gtopt should match.
  /// Delegates to `SimulationLP::find_ampl_class_attribute`.
  [[nodiscard]] bool find_ampl_class_attribute(std::string_view class_name,
                                               std::string_view attribute) const
  {
    return simulation().find_ampl_class_attribute(class_name, attribute);
  }

  /// Look up a compound PAMPL attribute by (class, compound_name).
  [[nodiscard]] const std::vector<AmplCompoundLeg>* find_ampl_compound(
      std::string_view class_name,
      std::string_view compound_name) const noexcept
  {
    return simulation().find_ampl_compound(class_name, compound_name);
  }

  /// Diagnostic-only: registered element names for @p class_name nearest
  /// to @p target (edit-distance ordered).  Used to build a
  /// "did you mean ...?" hint when a user constraint references an
  /// unknown element name.  Only called on the strict-mode error path.
  [[nodiscard]] std::vector<std::string_view> ampl_element_name_candidates(
      std::string_view class_name,
      std::string_view target,
      std::size_t max_names = 5) const
  {
    return simulation().ampl_element_name_candidates(
        class_name, target, max_names);
  }

  /// Diagnostic-only: attribute names registered as LP variables for the
  /// (class_name, element_uid) pair.  Used to build a "valid attributes
  /// are ..." hint when a user constraint references a known element with
  /// an unknown attribute.  Only called on the strict-mode error path.
  [[nodiscard]] std::vector<std::string_view> ampl_attribute_candidates(
      std::string_view class_name,
      Uid element_uid,
      std::size_t max_attrs = 12) const
  {
    return simulation().ampl_attribute_candidates(
        class_name, element_uid, max_attrs);
  }

  /// Look up a class-level scalar (e.g. `options.scale_objective`).
  /// Returns nullopt when the (class, attribute) pair is not in the
  /// allow-list registered by `system_lp.cpp::register_all_ampl_element_names`.
  [[nodiscard]] std::optional<double> find_ampl_scalar(
      std::string_view class_name, std::string_view attribute) const noexcept
  {
    return simulation().find_ampl_scalar(class_name, attribute);
  }

  /// Returns the suppression reason if the class or (class, attribute)
  /// pair has been marked unavailable by the current planning mode
  /// (e.g. `line` under `use_single_bus`, `bus.theta` when Kirchhoff
  /// is disabled).  A non-empty result means the resolver should
  /// silently drop the referencing term instead of throwing.
  [[nodiscard]] std::optional<std::string_view> find_ampl_suppression(
      std::string_view class_name, std::string_view attribute) const noexcept
  {
    return simulation().find_ampl_suppression(class_name, attribute);
  }

  /// Look up the class-level parameter resolver for (class, attribute).
  /// Returns nullptr when the pair is not registered — see
  /// `SimulationLP::register_ampl_param` for the population side.  Used
  /// by `element_column_resolver.cpp::resolve_single_param` to replace
  /// the legacy per-class if/else dispatch with a single map lookup +
  /// indirect call.
  [[nodiscard]] AmplParamFn find_ampl_param(
      std::string_view class_name, std::string_view attribute) const noexcept
  {
    return simulation().find_ampl_param(class_name, attribute);
  }

  /// Look up the class-level iterator function for `class_name`.
  /// Returns nullptr when the class is not registered (means the class
  /// does not participate in `sum(class(all)...)` enumeration).
  [[nodiscard]] AmplIterFn find_ampl_iter(
      std::string_view class_name) const noexcept
  {
    return simulation().find_ampl_iter(class_name);
  }

  /// Register filter metadata for one element (F9).
  /// See `SimulationLP::register_ampl_element_metadata`.
  void register_ampl_element_metadata(std::string_view class_name,
                                      Uid element_uid,
                                      AmplElementMetadata metadata) const;

  /// Look up an element's metadata bundle (F9).  Returns nullptr when
  /// the element has no registered metadata.
  [[nodiscard]] const AmplElementMetadata* find_ampl_element_metadata(
      std::string_view class_name, Uid element_uid) const noexcept;

private:
  std::reference_wrapper<SimulationLP> m_simulation_;
  std::reference_wrapper<SystemLP> m_system_;

  /// Forward (default) vs aperture registry routing.  Stamped into every
  /// `StateVariable::Key` / `LPKey` this context registers or looks up, so
  /// the aperture system's state variables land in the parallel registry.
  SystemKind m_kind_ {SystemKind::forward};

  // One void* per LP element type; each points to the Collection<T> inside the
  // owning SystemLP.  Index for type T is lp_type_index_v<T>.
  // Populated once in the constructor (system_context.cpp includes
  // system_lp.hpp).
  lp_collection_ptrs_t m_collection_ptrs_ {};

  /// When true, element `add_to_lp` calls skip registry side-effects
  /// (state-variable registration, AMPL registration, cross-phase link
  /// deferral, metadata registration).  Set by
  /// `SystemLP::rebuild_in_place()` during the rebuild pass only.  See
  /// `set_silent_flatten_pass`.
  bool m_silent_flatten_pass_ {false};

  /// When true, AMPL variable/metadata registration runs even under
  /// `m_silent_flatten_pass_` so a rebuild flatten repopulates a
  /// registry that the production path released after the initial pass
  /// (see `set_replay_ampl_registry`).
  bool m_replay_ampl_registry_ {false};
};

}  // namespace gtopt

static_assert(std::is_base_of_v<gtopt::FlatHelper, gtopt::SystemContext>,
              "SystemContext must inherit from FlatHelper");

static_assert(std::is_base_of_v<gtopt::CostHelper, gtopt::SystemContext>,
              "SystemContext must inherit from CostHelper");
