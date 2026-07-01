/**
 * @file      simulation.hpp
 * @brief     Linear programming simulation for power system planning
 * @author    marcelo
 * @copyright BSD-3-Clause
 * @version   1.0.0
 * @date      Sun Apr  6 18:18:54 2025
 *
 * Provides functionality for creating, solving, and analyzing linear
 * programming models for power system planning with strong exception safety
 * guarantees.
 */

#pragma once

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <gtopt/ampl_variable.hpp>
#include <gtopt/block_lp.hpp>
#include <gtopt/integer_variable.hpp>
#include <gtopt/phase_lp.hpp>
#include <gtopt/planning_options_lp.hpp>
#include <gtopt/scenario_lp.hpp>
#include <gtopt/scene_lp.hpp>
#include <gtopt/simulation.hpp>
#include <gtopt/stage_lp.hpp>
#include <gtopt/state_variable.hpp>
#include <gtopt/strong_index_vector.hpp>
#include <spdlog/details/log_msg.h>
#include <spdlog/spdlog.h>

namespace gtopt
{

class PlanningLP;
struct ArrowIndexCache;  // gtopt/arrow_index_cache.hpp (pimpl; see members)
/**
 * @class SimulationLP
 * @brief Linear programming representation of a power system simulation
 *
 * Encapsulates the LP transformation of a power system simulation model,
 * providing access to all components in their LP form. Maintains references
 * to the original simulation and options objects.
 */
class SimulationLP
{
public:
  // Non-copyable, non-movable: SimulationLP holds dense
  // `StrongIndexVector` registries keyed by (scene, phase) which would
  // be expensive to move and cheap to construct in-place.  All callers
  // (PlanningLP, tests) construct it in-place, so the deletions below
  // do not affect real usage.
  SimulationLP(SimulationLP&&) = delete;
  SimulationLP(const SimulationLP&) = delete;
  SimulationLP& operator=(SimulationLP&&) = delete;
  SimulationLP& operator=(const SimulationLP&) = delete;
  ~SimulationLP() noexcept = default;

  /**
   * @brief Constructs a SimulationLP from a Simulation
   * @param simulation Reference to the base simulation model
   * @param options Reference to LP solver options
   * @throws std::runtime_error If component validation fails
   * @throws std::bad_alloc If memory allocation fails
   */
  explicit SimulationLP(
      const Simulation& simulation,
      const PlanningOptionsLP& options,
      std::shared_ptr<ArrowIndexCache> shared_index_cache = {});

  // Accessors
  /**
   * @brief Gets the underlying simulation model
   * @return Reference to the simulation object
   */
  template<typename Self>
  [[nodiscard]] constexpr auto&& simulation(this Self& self) noexcept
  {
    return std::forward<Self>(self).m_simulation_.get();
  }

  /**
   * @brief Gets the LP solver options
   * @return Const reference to the options object
   */
  [[nodiscard]] constexpr const PlanningOptionsLP& options() const noexcept
  {
    return m_options_.get();
  }

  /**
   * @brief Gets all scene LP representations
   * @return Const reference to vector of SceneLP objects
   */
  [[nodiscard]] constexpr const auto& scenes() const noexcept
  {
    return m_scene_array_;
  }

  /**
   * @brief Gets all scenario LP representations
   * @return Const reference to vector of ScenarioLP objects
   */
  [[nodiscard]] constexpr const auto& scenarios() const noexcept
  {
    return m_scenario_array_;
  }

  /**
   * @brief Gets all phase LP representations
   * @return Const reference to vector of PhaseLP objects
   */
  [[nodiscard]] constexpr const auto& phases() const noexcept
  {
    return m_phase_array_;
  }

  /**
   * @brief Number of phases as a signed ``Index``.
   *
   * Convenience wrapper that avoids the repeated
   * ``static_cast<Index>(sim.phases().size())`` idiom at SDDP call sites.
   */
  [[nodiscard]] constexpr auto phase_count() const noexcept -> Index
  {
    return static_cast<Index>(m_phase_array_.size());
  }

  /**
   * @brief Number of scenes as a signed ``Index``.
   *
   * Convenience wrapper that avoids the repeated
   * ``static_cast<Index>(sim.scenes().size())`` idiom at SDDP call sites.
   */
  [[nodiscard]] constexpr auto scene_count() const noexcept -> Index
  {
    return static_cast<Index>(m_scene_array_.size());
  }

  /**
   * @brief Convert a `SceneIndex` (0-based array position) to the
   *        matching `SceneUid` (1-based identity from JSON input).
   *
   * Convenience wrapper that replaces the repeated
   * ``sim.scenes()[scene_index].uid()`` idiom at SDDP call sites
   * (notably the cut-loader paths in ``source/sddp_cut_io.cpp``,
   * where the uid is fed straight into ``make_iteration_context``).
   *
   * The name mirrors the free-function
   * ``gtopt::uid_of(IterationIndex)`` in
   * ``<gtopt/iteration.hpp>`` — one idiom (``uid_of``) for every
   * dimension's Index-to-Uid conversion.  Method overload resolves
   * on the index type: ``sim.uid_of(SceneIndex{k})`` → ``SceneUid``,
   * ``sim.uid_of(PhaseIndex{k})`` → ``PhaseUid``.
   */
  [[nodiscard]] constexpr auto uid_of(SceneIndex scene_index) const noexcept
      -> SceneUid
  {
    return m_scene_array_[scene_index].uid();
  }

  /**
   * @brief Convert a `PhaseIndex` (0-based) to the matching
   *        `PhaseUid` (1-based).  Mirror of ``uid_of(SceneIndex)``
   *        — see that helper for rationale.
   */
  [[nodiscard]] constexpr auto uid_of(PhaseIndex phase_index) const noexcept
      -> PhaseUid
  {
    return m_phase_array_[phase_index].uid();
  }

  /**
   * @brief Gets the index of the last phase.
   * @pre The simulation has at least one phase.
   * @return `previous(PhaseIndex{phases().size()})`.
   */
  [[nodiscard]] constexpr auto last_phase_index() const noexcept -> PhaseIndex
  {
    return previous(PhaseIndex {m_phase_array_.size()});
  }

  /**
   * @brief Gets the index of the last scene.
   * @pre The simulation has at least one scene.
   */
  [[nodiscard]] constexpr auto last_scene_index() const noexcept -> SceneIndex
  {
    return previous(SceneIndex {m_scene_array_.size()});
  }

  [[nodiscard]] constexpr const auto& blocks() const noexcept
  {
    return m_block_array_;
  }

  [[nodiscard]] constexpr const auto& stages() const noexcept
  {
    return m_stage_array_;
  }

  /**
   * @brief Gets the aperture definitions from the underlying Simulation
   * @return Const reference to the aperture_array
   */
  [[nodiscard]] constexpr const auto& apertures() const noexcept
  {
    return m_simulation_.get().aperture_array;
  }

  [[nodiscard]] constexpr auto previous_stage(const StageLP& stage)
  {
    if (!stage.index()) {
      throw std::out_of_range("No previous stage for the first stage");
    }
    return m_stage_array_[previous(stage.index())];
  }

  [[nodiscard]] constexpr auto prev_stage(const StageLP& stage) const noexcept
      -> std::pair<const StageLP*, const PhaseLP*>
  {
    if (!stage.index()) {
      if (const auto phase_index = stage.phase_index(); !phase_index) {
        return {nullptr, nullptr};
      }
      auto&& prev_phase = phases()[previous(stage.phase_index())];
      auto&& prev_stage = prev_phase.stages().back();
      return {&prev_stage, &prev_phase};
    }

    const auto prev_stage_index = previous(stage.index());
    return {&m_stage_array_[prev_stage_index], nullptr};
  }

  // Get method with deducing this for automatic const handling
  using lp_key_t = StateVariable::LPKey;
  using state_variable_key_t = StateVariable::Key;
  using state_variable_map_t = flat_map<state_variable_key_t, StateVariable>;
  using global_variable_map_t =
      StrongIndexVector<SceneIndex,
                        StrongIndexVector<PhaseIndex, state_variable_map_t>>;

  // ── Integer-variable registry (commitment-layout choke-point) ────────────
  // Parallel to the state-variable registry above.  Every integer LP
  // column in gtopt is registered here via
  // `SystemContext::add_integer_variable` so cut audits, integer-
  // feasibility checks, and the future commitment-layout binding stack
  // can iterate the set without grepping for `is_integer = true`.
  // See `docs/design/commitment-layout.md §5`.
  using integer_variable_key_t = IntegerVariable::Key;
  using integer_variable_map_t =
      flat_map<integer_variable_key_t, IntegerVariable>;
  using global_integer_variable_map_t =
      StrongIndexVector<SceneIndex,
                        StrongIndexVector<PhaseIndex, integer_variable_map_t>>;

  /// Select the forward or aperture state-variable registry.  The two maps
  /// have identical `[scene][phase]` shape but hold the columns of two
  /// physically distinct LPs (see `SystemKind`).  Routing by
  /// `key.lp_key.kind` keeps every forward consumer on `m_global_variable_map_`
  /// untouched while the aperture-system build/lookup uses the second map.
  template<typename Self>
  [[nodiscard]]
  constexpr auto&& variable_map(this Self&& self, SystemKind kind) noexcept
  {
    return (kind == SystemKind::aperture)
        ? std::forward<Self>(self).m_aperture_variable_map_
        : std::forward<Self>(self).m_global_variable_map_;
  }

  // Add method with deducing this and perfect forwarding
  template<typename Key = state_variable_key_t>
  [[nodiscard]]
  constexpr auto add_state_variable(Key&& key,
                                    ColIndex col,
                                    double scost,
                                    double var_scale,
                                    LpContext context) -> const StateVariable&
  {
    auto&& map = variable_map(
        key.lp_key.kind)[key.lp_key.scene_index][key.lp_key.phase_index];

    const auto [it, inserted] = map.try_emplace(std::forward<Key>(key),
                                                key.lp_key,
                                                col,
                                                scost,
                                                var_scale,
                                                std::move(context));

    if (!inserted) {
      // Idempotent re-registration is allowed: throw-away flatten passes
      // (e.g. `rebuild_collections_if_needed` under compress mode)
      // replay every add_state_variable call.  Same col index = benign
      // re-registration; different col = a real bug we still want to
      // catch.
      if (it->second.col() != col) {
        const auto msg =
            std::format("duplicated variable {}:{} in simulation map",
                        key.class_name.full_name(),
                        key.col_name);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }
    }

    return it->second;
  }

  [[nodiscard]]
  constexpr const auto& state_variables() noexcept
  {
    return m_global_variable_map_;
  }

  template<typename Self>
  [[nodiscard]]
  constexpr auto&& state_variables(
      this Self&& self,
      SceneIndex scene_index,
      PhaseIndex phase_index,
      SystemKind kind = SystemKind::forward) noexcept
  {
    auto&& vec = std::forward<Self>(self).variable_map(kind);
    return vec[scene_index][phase_index];
  }

  // ── Integer-variable registry access ────────────────────────────────────

  /// Select the forward / aperture integer-variable registry, mirroring
  /// `variable_map(SystemKind)` for state variables.
  template<typename Self>
  [[nodiscard]]
  constexpr auto&& integer_variable_map(this Self&& self,
                                        SystemKind kind) noexcept
  {
    return (kind == SystemKind::aperture)
        ? std::forward<Self>(self).m_aperture_integer_map_
        : std::forward<Self>(self).m_global_integer_map_;
  }

  /// Register one integer LP column.  Idempotent — re-registering the
  /// same key with the same column index is a no-op; re-registering
  /// with a different column index is a hard error (same contract as
  /// `add_state_variable`).
  template<typename Key = integer_variable_key_t>
  [[nodiscard]]
  constexpr auto add_integer_variable(Key&& key,
                                      ColIndex col,
                                      IntegerDomain domain,
                                      IntegerScope scope,
                                      GroupUid group_uid,
                                      std::span<const BlockUid> blocks)
      -> const IntegerVariable&
  {
    auto&& map = integer_variable_map(
        key.lp_key.kind)[key.lp_key.scene_index][key.lp_key.phase_index];

    auto block_vec = std::vector<BlockUid>(blocks.begin(), blocks.end());

    const auto [it, inserted] = map.try_emplace(std::forward<Key>(key),
                                                key.lp_key,
                                                col,
                                                domain,
                                                scope,
                                                group_uid,
                                                std::move(block_vec));

    if (!inserted) {
      if (it->second.col() != col) {
        const auto msg =
            std::format("duplicated integer variable {}:{} in simulation map",
                        key.class_name.full_name(),
                        key.col_name);
        SPDLOG_CRITICAL(msg);
        throw std::runtime_error(msg);
      }
    }

    return it->second;
  }

  [[nodiscard]]
  constexpr const auto& integer_variables() noexcept
  {
    return m_global_integer_map_;
  }

  template<typename Self>
  [[nodiscard]]
  constexpr auto&& integer_variables(
      this Self&& self,
      SceneIndex scene_index,
      PhaseIndex phase_index,
      SystemKind kind = SystemKind::forward) noexcept
  {
    auto&& vec = std::forward<Self>(self).integer_variable_map(kind);
    return vec[scene_index][phase_index];
  }

  template<typename Self, typename Key = integer_variable_key_t>
  [[nodiscard]]
  constexpr auto integer_variable(this Self&& self, Key&& key) noexcept
  {
    using value_type =
        std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>,
                           const IntegerVariable,
                           IntegerVariable>;
    using result_t = std::optional<std::reference_wrapper<value_type>>;

    auto&& map = std::forward<Self>(self).integer_variables(
        key.lp_key.scene_index, key.lp_key.phase_index, key.lp_key.kind);

    const auto it = map.find(std::forward<Key>(key));
    return (it != map.end()) ? result_t {it->second} : result_t {};
  }

  /**
   * @brief Retrieves a state variable by its key
   * @param self  The object instance (deduced via explicit object parameter)
   * @param key   The key to search for
   * @return Optional reference to the state variable if found (const or
   * non-const)
   */
  template<typename Self, typename Key = state_variable_key_t>
  [[nodiscard]]
  constexpr auto state_variable(this Self&& self, Key&& key) noexcept
  {
    using value_type =
        std::conditional_t<std::is_const_v<std::remove_reference_t<Self>>,
                           const StateVariable,
                           StateVariable>;
    using result_t = std::optional<std::reference_wrapper<value_type>>;

    auto&& map = std::forward<Self>(self).state_variables(
        key.lp_key.scene_index, key.lp_key.phase_index, key.lp_key.kind);

    const auto it = map.find(std::forward<Key>(key));
    return (it != map.end()) ? result_t {it->second} : result_t {};
  }

  // ── Per-scene α-rebase offsets (persist across cell rebuilds) ────────────
  //
  // The SDDP method computes one mean-shift offset c̄ per scene during
  // boundary-cut load (`m_scene_alpha_offsets_`).  At the end of the solve it
  // copies that vector here via `set_alpha_offsets`, so that
  // `FutureCostLP::add_to_output` can SELF-FIND the rebase constant at write
  // time.  The simulation outlives every per-cell LP rebuild (it is not a
  // disposable per-(scene, phase) collection), so this survives the
  // `low_memory = compress` rebuild that drops a per-cell stash.  Zero-filled
  // (empty) until the SDDP method publishes the offsets; `alpha_offset` then
  // returns 0 for any scene index.

  /// Publish the per-scene α-rebase offsets (copied from the SDDP method's
  /// `m_scene_alpha_offsets_`).  Read-only afterwards.
  void set_alpha_offsets(StrongIndexVector<SceneIndex, double> offsets)
  {
    m_alpha_offsets_ = std::move(offsets);
  }

  /// Per-scene α-rebase offset c̄ ($).  Returns 0 when the scene index is out
  /// of range or no offsets were published (mean-shift disabled / no cuts).
  [[nodiscard]] double alpha_offset(SceneIndex si) const noexcept
  {
    return static_cast<std::size_t>(si) < m_alpha_offsets_.size()
        ? m_alpha_offsets_[si]
        : 0.0;
  }

  // ── (scenario, stage) → (scene, phase) factored lookup ──────────────────
  //
  // Scenes partition scenarios and phases partition stages, so the
  // owning LP for a `(scenario_uid, stage_uid)` pair factors into two
  // independent 1-D lookups.  Both maps are populated once in the
  // constructor and never mutated, so reads are lock-free.
  //
  // In monolithic mode there is exactly one scene and one phase, and
  // every lookup returns `(SceneIndex{0}, PhaseIndex{0})`.

  /// Look up the scene that owns scenarios bearing the given uid.
  /// Returns `nullopt` when the uid is unknown (e.g. inactive scenario).
  [[nodiscard]] std::optional<SceneIndex> scene_of(
      ScenarioUid scenario_uid) const noexcept
  {
    const auto it = m_scene_of_scenario_.find(scenario_uid);
    return (it != m_scene_of_scenario_.end())
        ? std::optional<SceneIndex> {it->second}
        : std::nullopt;
  }

  /// Look up the phase that owns stages bearing the given uid.
  /// Returns `nullopt` when the uid is unknown (e.g. inactive stage).
  [[nodiscard]] std::optional<PhaseIndex> phase_of(
      StageUid stage_uid) const noexcept
  {
    const auto it = m_phase_of_stage_.find(stage_uid);
    return (it != m_phase_of_stage_.end())
        ? std::optional<PhaseIndex> {it->second}
        : std::nullopt;
  }

  /// Look up the unique LP that owns the `(scenario, stage)` tuple.
  /// Returns `nullopt` if either uid is unknown.
  [[nodiscard]] std::optional<std::pair<SceneIndex, PhaseIndex>> owning_lp_of(
      ScenarioUid scenario_uid, StageUid stage_uid) const noexcept
  {
    const auto scene = scene_of(scenario_uid);
    if (!scene) {
      return std::nullopt;
    }
    const auto phase = phase_of(stage_uid);
    if (!phase) {
      return std::nullopt;
    }
    return std::pair {*scene, *phase};
  }

  /// Returns the unique owning LP iff every tuple in @p refs maps to
  /// the same `(scene, phase)`.  Used by row dispatch to detect
  /// constraints that would cross LPs.  An empty span returns
  /// `nullopt` (no constraint to assign).
  [[nodiscard]] std::optional<std::pair<SceneIndex, PhaseIndex>>
  unique_owning_lp_of(
      std::span<const std::pair<ScenarioUid, StageUid>> refs) const noexcept
  {
    if (refs.empty()) {
      return std::nullopt;
    }
    const auto first = owning_lp_of(refs.front().first, refs.front().second);
    if (!first) {
      return std::nullopt;
    }
    for (const auto& [s_uid, t_uid] : refs.subspan(1)) {
      const auto cur = owning_lp_of(s_uid, t_uid);
      if (!cur || *cur != *first) {
        return std::nullopt;
      }
    }
    return first;
  }

  // ── PAMPL / user-constraint variable registry ─────────────────────────────
  //
  // Storage is partitioned per-(scene, phase), matching the topology of
  // `state_variables`.  Each `(scene, phase)` cell holds the variable
  // and metadata maps for the LP that owns that scene/phase pair.
  //
  // Threading model (see `planning_lp.cpp`): scenes are constructed in
  // parallel work-pool threads, but phases within a scene run
  // **sequentially** because each phase consumes state variables produced
  // by the previous one.  Therefore each `(scene, phase)` cell is written
  // by exactly one thread, in sequence — **no per-cell mutex is needed**.
  //
  // Element-name and class-level compound registries are populated
  // exactly once by the first `SystemLP` constructor that runs against
  // this `SimulationLP` — `std::call_once` guards the population via
  // `m_ampl_registry_flag_`.  Regardless of whether construction happens
  // via `PlanningLP::create_systems` (parallel scenes race to be first,
  // and the losers skip) or a unit test that creates a `SimulationLP` +
  // `SystemLP` pair directly, the registry ends up populated before any
  // `add_to_lp` runs and stays read-only afterwards.

  /// Per-LP (scene, phase) cell: holds the variable and metadata maps
  /// for one `LinearProblem`.  Each cell is owned by exactly one
  /// scene-thread for the duration of all its phases, so no mutex.
  struct AmplLpCell
  {
    AmplVariableMap variables;
    AmplElementMetadataMap metadata;
  };

  using ampl_lp_registry_t =
      StrongIndexVector<SceneIndex, StrongIndexVector<PhaseIndex, AmplLpCell>>;

  /// Enable or disable AMPL variable registration.  When false (the
  /// default), `add_ampl_variable` is a no-op and the per-cell variable
  /// maps stay empty — avoiding allocation overhead when no consumer
  /// (user constraints, PAMPL expressions) needs them.
  ///
  /// The flag is set by `SystemLP` when user constraints exist or by
  /// `PlanningLP` when PAMPL evaluation is requested.
  void set_need_ampl_variables(bool v) noexcept { m_need_ampl_variables_ = v; }

  [[nodiscard]] constexpr bool need_ampl_variables() const noexcept
  {
    return m_need_ampl_variables_;
  }

  /// Release the per-(scene, phase) AMPL variable + metadata maps once the
  /// cell's LP has been flattened.  Gated by
  /// `LpMatrixOptions::release_ampl_after_flatten` (set on the production
  /// solve path) so direct PlanningLP/SystemLP tests that inspect the
  /// registry post-build keep it.  The maps are only read to resolve user
  /// constraints during the build and never after `flatten`, so on
  /// user-constraint-heavy cases (CEN PLEXOS, irrigation) this reclaims the
  /// registry's memory — hundreds of MB at CEN scale — for the solve.
  /// Move-assigning fresh empty maps (rather than `clear()`) returns the
  /// node storage to the allocator.  Safe from the owning build thread:
  /// each cell touches only its own `[scene][phase]` slot, and a later
  /// rebuild re-populates it via `add_ampl_variable`.
  void release_ampl_cell(SceneIndex scene_index,
                         PhaseIndex phase_index) noexcept
  {
    auto& cell = m_ampl_lp_cells_[scene_index][phase_index];
    cell.variables = AmplVariableMap {};
    cell.metadata = AmplElementMetadataMap {};
  }

  /// Register a per-block variable map (e.g., generator.generation).
  /// No-op when `need_ampl_variables()` is false.
  void add_ampl_variable(SceneIndex scene_index,
                         PhaseIndex phase_index,
                         std::string_view class_name,
                         Uid element_uid,
                         std::string_view attribute,
                         ScenarioUid scenario_uid,
                         StageUid stage_uid,
                         const BIndexHolder<ColIndex>& block_cols)
  {
    if (!m_need_ampl_variables_) {
      return;
    }
    m_ampl_lp_cells_[scene_index][phase_index].variables.insert_or_assign(
        AmplVariableKey {
            .class_name = class_name,
            .element_uid = element_uid,
            .attribute = attribute,
            .scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
        },
        AmplVariable {
            .block_cols = block_cols,
        });
  }

  /// Register a per-block variable map together with per-block additive
  /// offsets.  Used by shifted-variable encodings (e.g., demand's
  /// Option C neg_fail = load − lmax substitution) so user constraints
  /// referencing `demand.load` resolve to (col + offset) physically,
  /// with the offset shifted onto the row's RHS by the resolver.
  /// No-op when `need_ampl_variables()` is false.
  void add_ampl_variable(SceneIndex scene_index,
                         PhaseIndex phase_index,
                         std::string_view class_name,
                         Uid element_uid,
                         std::string_view attribute,
                         ScenarioUid scenario_uid,
                         StageUid stage_uid,
                         const BIndexHolder<ColIndex>& block_cols,
                         const BIndexHolder<double>& block_offsets)
  {
    if (!m_need_ampl_variables_) {
      return;
    }
    m_ampl_lp_cells_[scene_index][phase_index].variables.insert_or_assign(
        AmplVariableKey {
            .class_name = class_name,
            .element_uid = element_uid,
            .attribute = attribute,
            .scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
        },
        AmplVariable {
            .block_cols = block_cols,
            .extras = std::make_unique<AmplVariable::Extras>(
                AmplVariable::Extras {.block_offsets = block_offsets}),
        });
  }

  /// Register a stage-level scalar column (e.g., eini, efin, capainst).
  /// Returns the same value for every block in the stage.
  void add_ampl_variable(SceneIndex scene_index,
                         PhaseIndex phase_index,
                         std::string_view class_name,
                         Uid element_uid,
                         std::string_view attribute,
                         ScenarioUid scenario_uid,
                         StageUid stage_uid,
                         ColIndex stage_col)
  {
    if (!m_need_ampl_variables_) {
      return;
    }
    m_ampl_lp_cells_[scene_index][phase_index].variables.insert_or_assign(
        AmplVariableKey {
            .class_name = class_name,
            .element_uid = element_uid,
            .attribute = attribute,
            .scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
        },
        AmplVariable {
            .stage_col = stage_col,
        });
  }

  /// Register a per-block **sum of columns** for a virtual aggregator
  /// attribute (e.g., `line.flowp = Σ flowp_seg_k` under
  /// `piecewise_direct` line-loss mode — no aggregator LP col, the
  /// segments are summed at resolution time).  When the user-constraint
  /// resolver hits this registration it stamps each col in the list with
  /// the leg coefficient: `row[col] += base · coef` per col.
  ///
  /// Like the other overloads, no-op when `need_ampl_variables()` is
  /// false.  The `BIndexHolder<std::vector<ColIndex>>` is copied by
  /// value for the same lifetime reasons as the single-col overload
  /// (the source map may move when later `add_to_lp` calls grow the
  /// owner's `STBIndexHolder`).
  void add_ampl_variable(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      ScenarioUid scenario_uid,
      StageUid stage_uid,
      const BIndexHolder<std::vector<ColIndex>>& block_cols_sum)
  {
    if (!m_need_ampl_variables_) {
      return;
    }
    m_ampl_lp_cells_[scene_index][phase_index].variables.insert_or_assign(
        AmplVariableKey {
            .class_name = class_name,
            .element_uid = element_uid,
            .attribute = attribute,
            .scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
        },
        AmplVariable {
            .extras = std::make_unique<AmplVariable::Extras>(
                AmplVariable::Extras {.block_cols_sum = block_cols_sum}),
        });
  }

  /// Register a per-block **weighted sum of columns** for a virtual
  /// aggregator attribute whose legs each carry a coefficient.  Used
  /// by `FuelLP::add_to_lp` to expose
  /// `fuel("X").offtake = Σ_g heat_rate_g · dur_b · generation_g[b]`
  /// without creating an LP `Y_f[b]` column + equality binding row
  /// (the legacy substitute-out anti-pattern, mirror of the
  /// `EmissionZone.production` removal).  Resolver stamps
  /// `row[col] += base_coef · weight` per leg.  No-op when
  /// `need_ampl_variables()` is false.  Copied by value for the same
  /// lifetime reasons as the other sum-of-cols overload.
  void add_ampl_variable(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      ScenarioUid scenario_uid,
      StageUid stage_uid,
      const BIndexHolder<std::vector<std::pair<ColIndex, double>>>&
          block_cols_weighted_sum)
  {
    if (!m_need_ampl_variables_) {
      return;
    }
    m_ampl_lp_cells_[scene_index][phase_index].variables.insert_or_assign(
        AmplVariableKey {
            .class_name = class_name,
            .element_uid = element_uid,
            .attribute = attribute,
            .scenario_uid = scenario_uid,
            .stage_uid = stage_uid,
        },
        AmplVariable {
            .extras =
                std::make_unique<AmplVariable::Extras>(AmplVariable::Extras {
                    .block_cols_weighted_sum = block_cols_weighted_sum}),
        });
  }

  /// Look up a registered variable.  Returns nullopt if the
  /// (class, uid, attribute, scenario, stage, block) combination was not
  /// registered — e.g., when the column is conditional (soft_emin, drain)
  /// and was never created.
  [[nodiscard]] std::optional<ColIndex> find_ampl_col(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      ScenarioUid scenario_uid,
      StageUid stage_uid,
      BlockUid block_uid) const
  {
    const auto& cell = m_ampl_lp_cells_[scene_index][phase_index];
    const auto it = cell.variables.find(AmplVariableKey {
        .class_name = class_name,
        .element_uid = element_uid,
        .attribute = attribute,
        .scenario_uid = scenario_uid,
        .stage_uid = stage_uid,
    });
    if (it == cell.variables.end()) {
      return std::nullopt;
    }
    return it->second.col_at(block_uid);
  }

  /// Look up the per-block additive offset for a registered variable.
  /// Returns `0.0` when the variable was not registered with offsets
  /// (the common case) or when no offset is recorded for this block.
  /// Used by the user-constraint resolver to fold shifted-variable
  /// offsets into the row RHS via the existing param_shift mechanism.
  [[nodiscard]] double find_ampl_offset(SceneIndex scene_index,
                                        PhaseIndex phase_index,
                                        std::string_view class_name,
                                        Uid element_uid,
                                        std::string_view attribute,
                                        ScenarioUid scenario_uid,
                                        StageUid stage_uid,
                                        BlockUid block_uid) const
  {
    const auto& cell = m_ampl_lp_cells_[scene_index][phase_index];
    const auto it = cell.variables.find(AmplVariableKey {
        .class_name = class_name,
        .element_uid = element_uid,
        .attribute = attribute,
        .scenario_uid = scenario_uid,
        .stage_uid = stage_uid,
    });
    if (it == cell.variables.end()) {
      return 0.0;
    }
    return it->second.offset_at(block_uid);
  }

  /// Look up a registered **sum-of-cols** attribute.  Returns an empty
  /// span when the attribute is not registered or is registered as a
  /// single col (callers should fall through to `find_ampl_col` in that
  /// case).  Used by the user-constraint resolver to handle virtual
  /// aggregators like `line.flowp` under `piecewise_direct` mode where
  /// the attribute expands to a sum of segment LP cols.
  [[nodiscard]] std::span<const ColIndex> find_ampl_cols(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      ScenarioUid scenario_uid,
      StageUid stage_uid,
      BlockUid block_uid) const
  {
    const auto& cell = m_ampl_lp_cells_[scene_index][phase_index];
    const auto it = cell.variables.find(AmplVariableKey {
        .class_name = class_name,
        .element_uid = element_uid,
        .attribute = attribute,
        .scenario_uid = scenario_uid,
        .stage_uid = stage_uid,
    });
    if (it == cell.variables.end()) {
      return {};
    }
    return it->second.cols_at(block_uid);
  }

  /// Look up a registered **weighted sum-of-cols** attribute.  Returns
  /// an empty span when the attribute is not registered with weighted
  /// legs (callers fall through to single-col / unweighted-sum paths in
  /// that case).  Used by the user-constraint resolver to stamp
  /// `row[col] += base_coef · weight` for every leg of attributes like
  /// `fuel("X").offtake` (registered by `FuelLP::add_to_lp` as
  /// `Σ heat_rate_g · dur_b · generation_g[b]`).
  [[nodiscard]] std::span<const std::pair<ColIndex, double>>
  find_ampl_weighted_cols(SceneIndex scene_index,
                          PhaseIndex phase_index,
                          std::string_view class_name,
                          Uid element_uid,
                          std::string_view attribute,
                          ScenarioUid scenario_uid,
                          StageUid stage_uid,
                          BlockUid block_uid) const
  {
    const auto& cell = m_ampl_lp_cells_[scene_index][phase_index];
    const auto it = cell.variables.find(AmplVariableKey {
        .class_name = class_name,
        .element_uid = element_uid,
        .attribute = attribute,
        .scenario_uid = scenario_uid,
        .stage_uid = stage_uid,
    });
    if (it == cell.variables.end()) {
      return {};
    }
    return it->second.weighted_cols_at(block_uid);
  }

  /// Look up a registered AMPL variable by its full key (no block).
  /// Returns a pointer to the registry entry (node-stable while the
  /// registry lives — the resolver only uses it within the same build,
  /// before `release_ampl_cell`), or `nullptr` when unregistered.  Lets
  /// the user-constraint resolver hash the key ONCE per (term, block) and
  /// then read every per-block shape (`col_at` / `cols_at` /
  /// `weighted_cols_at` / `offset_at`) directly off the entry, instead of
  /// repeating the hash+find for each of those four lookups.
  [[nodiscard]] const AmplVariable* find_ampl_variable(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute,
      ScenarioUid scenario_uid,
      StageUid stage_uid) const
  {
    const auto& cell = m_ampl_lp_cells_[scene_index][phase_index];
    const auto it = cell.variables.find(AmplVariableKey {
        .class_name = class_name,
        .element_uid = element_uid,
        .attribute = attribute,
        .scenario_uid = scenario_uid,
        .stage_uid = stage_uid,
    });
    return it == cell.variables.end() ? nullptr : &it->second;
  }

  /// Is the (class, element_uid, attribute) triple registered as an LP
  /// variable somewhere — any (scene, phase, scenario, stage)?  Used by
  /// the user-constraint resolver to distinguish "attribute is real
  /// but the element has no column in THIS specific (scene, phase,
  /// scenario, stage, block)" (treat-as-zero) from "attribute is a
  /// typo or not supported on this element" (strict-mode error).
  ///
  /// Scans every (scene, phase) cell, matching any AmplVariableKey
  /// with the given class+uid+attribute regardless of scenario/stage.
  /// Cost is O(cells × variables) but only triggered on the failure
  /// path of `resolve_col_to_row` — never on the hot success path.
  [[nodiscard]] bool find_ampl_variable_for_element(
      std::string_view class_name,
      Uid element_uid,
      std::string_view attribute) const
  {
    for (const auto& by_phase : m_ampl_lp_cells_) {
      for (const auto& cell : by_phase) {
        for (const auto& [key, _] : cell.variables) {
          if (key.class_name == class_name && key.element_uid == element_uid
              && key.attribute == attribute)
          {
            return true;
          }
        }
      }
    }
    return false;
  }

  /// Is the (class, attribute) pair registered as an LP variable for
  /// **any** element of the class, somewhere in the simulation?  This
  /// is the LP-attr-dormant leniency check used by the user-constraint
  /// resolver for the "element is known and the class supports the
  /// attribute, but THIS particular element has zero capacity for the
  /// entire horizon and therefore got no LP column" case.
  ///
  /// Example: ``PANGUE_U1`` has ``pmax = 0`` in every block →
  /// ``GeneratorLP::add_to_lp`` never creates a ``generation`` column
  /// for it, and ``add_ampl_variable`` is gated on a non-empty column
  /// map (``if (!gcols.empty())``).  So
  /// ``find_ampl_variable_for_element("generator", PANGUE_U1, "generation")``
  /// returns ``false``.  But ANY other non-zero-pmax generator in the
  /// system registers ``generation``, so this method returns ``true``
  /// — telling the resolver that the attribute is a valid LP variable
  /// on the class (just not materialised for this particular element)
  /// and the term should silently contribute 0.
  ///
  /// Critically, this still rejects:
  ///   * Unknown class (no element of that class registers anything).
  ///   * Unknown attribute on a class (no element of the class
  ///     registers this attribute — typo on ``generation`` →
  ///     ``generattion`` returns false).
  ///
  /// Scans every (scene, phase) cell.  Cost is O(cells × variables)
  /// but only triggered on the failure path of ``resolve_col_to_row``
  /// — never on the hot success path.
  [[nodiscard]] bool find_ampl_class_attribute(std::string_view class_name,
                                               std::string_view attribute) const
  {
    for (const auto& by_phase : m_ampl_lp_cells_) {
      for (const auto& cell : by_phase) {
        for (const auto& [key, _] : cell.variables) {
          if (key.class_name == class_name && key.attribute == attribute) {
            return true;
          }
        }
      }
    }
    return false;
  }

  /// Register an element's name so that user-expressions like
  /// `generator("G1")` resolve to its Uid.
  ///
  /// Called exclusively from `system_lp.cpp` inside a `std::call_once`
  /// keyed on `ampl_registry_flag()`.  This guarantees the registry is
  /// populated exactly once per `SimulationLP` regardless of whether
  /// construction goes through `PlanningLP::create_systems` (parallel
  /// over scenes) or directly via tests that build a single `SystemLP`.
  /// During the parallel scene-build loop the map is read-only, so no
  /// mutex is needed.
  void register_ampl_element(std::string_view class_name,
                             std::string_view element_name,
                             Uid element_uid)
  {
    m_ampl_element_names_.insert_or_assign(
        AmplElementNameKey {class_name, element_name}, element_uid);
  }

  /// Look up an element Uid by (class_name, name).  Read-only during
  /// the parallel scene-build loop — safe without synchronization.
  [[nodiscard]] std::optional<Uid> lookup_ampl_element_uid(
      std::string_view class_name, std::string_view element_name) const
  {
    const auto it = m_ampl_element_names_.find(
        AmplElementNameKey {class_name, element_name});
    return (it != m_ampl_element_names_.end()) ? std::optional<Uid> {it->second}
                                               : std::nullopt;
  }

  /// Reverse lookup: element name by (LPClassName, uid).  Linear scan
  /// — only use on rare / diagnostic paths (e.g. SDDP fcut logging).
  /// The registry stores snake_case class_name keys; `class_name`
  /// carries both forms precomputed, so we compare against
  /// `class_name.snake_case()` directly — zero runtime conversion.
  /// Returns a `string_view` into the same stable storage as the
  /// registry key (element `Id::name`, owned by `System`), or empty
  /// view when no match is found.
  [[nodiscard]] std::string_view lookup_ampl_element_name(
      const LPClassName& class_name, Uid element_uid) const noexcept
  {
    const auto class_snake = class_name.snake_case();
    for (const auto& [key, uid] : m_ampl_element_names_) {
      if (uid == element_uid && class_snake == key.first) {
        return key.second;
      }
    }
    return {};
  }

  /// Collect up to @p max_names registered element names for @p class_name,
  /// sorted by ascending edit-distance to @p target so the closest
  /// candidate appears first.  Diagnostic-only: used to build a
  /// "did you mean ...?" hint when a user constraint references an
  /// unknown element name.  Linear scan over the name registry — only
  /// ever called on the strict-mode error path, never on the hot path.
  [[nodiscard]] std::vector<std::string_view> ampl_element_name_candidates(
      std::string_view class_name,
      std::string_view target,
      std::size_t max_names = 5) const
  {
    // Crude Levenshtein distance — adequate for short element names; the
    // computation only runs once, immediately before throwing.
    const auto edit_distance = [](std::string_view a,
                                  std::string_view b) -> std::size_t
    {
      std::vector<std::size_t> prev(b.size() + 1);
      std::vector<std::size_t> cur(b.size() + 1);
      for (std::size_t j = 0; j <= b.size(); ++j) {
        prev[j] = j;
      }
      for (std::size_t i = 1; i <= a.size(); ++i) {
        cur[0] = i;
        for (std::size_t j = 1; j <= b.size(); ++j) {
          const std::size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
          cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
        }
        std::swap(prev, cur);
      }
      return prev[b.size()];
    };

    std::vector<std::pair<std::size_t, std::string_view>> scored;
    for (const auto& [key, uid] : m_ampl_element_names_) {
      if (key.first == class_name) {
        scored.emplace_back(edit_distance(target, key.second), key.second);
      }
    }
    std::ranges::sort(scored,
                      [](const auto& lhs, const auto& rhs)
                      { return lhs.first < rhs.first; });
    std::vector<std::string_view> out;
    for (const auto& [dist, name] : scored) {
      if (out.size() >= max_names) {
        break;
      }
      out.push_back(name);
    }
    return out;
  }

  /// Collect up to @p max_attrs distinct attribute names registered as LP
  /// variables for the (class_name, element_uid) pair, across every
  /// (scene, phase, scenario, stage).  Diagnostic-only: used to build a
  /// "valid attributes are ..." hint when a user constraint references a
  /// known element with an unknown attribute.  Scans the per-cell
  /// variable registry — only called on the strict-mode error path.
  [[nodiscard]] std::vector<std::string_view> ampl_attribute_candidates(
      std::string_view class_name,
      Uid element_uid,
      std::size_t max_attrs = 12) const
  {
    std::vector<std::string_view> out;
    for (const auto& by_phase : m_ampl_lp_cells_) {
      for (const auto& cell : by_phase) {
        for (const auto& [key, _] : cell.variables) {
          if (key.class_name == class_name && key.element_uid == element_uid
              && std::ranges::find(out, key.attribute) == out.end())
          {
            out.push_back(key.attribute);
            if (out.size() >= max_attrs) {
              return out;
            }
          }
        }
      }
    }
    return out;
  }

  /// Register a compound PAMPL attribute for a class.  The compound
  /// name resolves to the linear combination `Σ leg.coefficient *
  /// <leg.source_attribute>` using the already-registered per-element
  /// AMPL variables.  Registration is class-level (once per class
  /// definition, not per element) — the compound "line.flow" is the
  /// same recipe for every `LineLP`.
  ///
  /// Called exclusively from `system_lp.cpp` inside the same
  /// `std::call_once` block that registers element names; no
  /// synchronization needed.
  void add_ampl_compound(std::string_view class_name,
                         std::string_view compound_name,
                         std::vector<AmplCompoundLeg> legs)
  {
    m_ampl_compounds_.try_emplace(
        AmplCompoundKey {
            .class_name = class_name,
            .compound_name = compound_name,
        },
        std::move(legs));
  }

  /// Look up a compound attribute by (class, compound_name).  Returns
  /// nullptr when the attribute is not a registered compound (most
  /// attributes are ordinary single-column variables).  Read-only
  /// during the parallel scene-build loop — safe without
  /// synchronization.
  [[nodiscard]] const std::vector<AmplCompoundLeg>* find_ampl_compound(
      std::string_view class_name,
      std::string_view compound_name) const noexcept
  {
    const auto it = m_ampl_compounds_.find(AmplCompoundKey {
        .class_name = class_name,
        .compound_name = compound_name,
    });
    return (it != m_ampl_compounds_.end()) ? &it->second : nullptr;
  }

  // ── Scalar parameter registry (Phase 1d) ─────────────────────────────
  //
  // Singleton classes like `options.*` expose globally-scoped read-only
  // constants to PAMPL expressions.  The set of exposed scalars is an
  // explicit allow-list registered once from `system_lp.cpp` inside the
  // same `std::call_once` block that fills the element-name registry —
  // so we never expose an internal solver knob just because it happens
  // to be a `double` field on `PlanningOptionsLP`.

  /// Register a class-level scalar (e.g. `options.annual_discount_rate`).
  /// `class_name` and `attribute` must point to storage that lives at
  /// least as long as `SimulationLP` (in practice: `std::string` for
  /// `class_name`, `constexpr` literal for `attribute`).
  ///
  /// Called exclusively from `system_lp.cpp` inside the AMPL globals
  /// `std::call_once`; no synchronization needed.
  void add_ampl_scalar(std::string_view class_name,
                       std::string_view attribute,
                       double value)
  {
    m_ampl_scalars_.insert_or_assign(
        AmplScalarKey {
            .class_name = class_name,
            .attribute = attribute,
        },
        value);
  }

  /// Look up a class-level scalar.  Returns nullopt when the
  /// (class, attribute) pair is not in the allow-list.  Read-only
  /// during the parallel scene-build loop — safe without
  /// synchronization.
  [[nodiscard]] std::optional<double> find_ampl_scalar(
      std::string_view class_name, std::string_view attribute) const noexcept
  {
    const auto it = m_ampl_scalars_.find(AmplScalarKey {
        .class_name = class_name,
        .attribute = attribute,
    });
    return (it != m_ampl_scalars_.end()) ? std::optional<double> {it->second}
                                         : std::nullopt;
  }

  // ── AMPL class / attribute suppression registry ──────────────────────
  //
  // Certain planning modes make whole classes or specific attributes
  // unavailable in the LP — e.g. `use_single_bus` suppresses all line
  // columns, `!use_kirchhoff` suppresses `bus.theta`.  User constraints
  // that reference such classes/attributes should *silently* drop the
  // term (with an INFO log) rather than throw: the reference is not a
  // typo, it just doesn't apply in the current mode.
  //
  // Populated from `system_lp.cpp`'s AMPL `std::call_once` block, which
  // knows the options and translates them into explicit suppression
  // entries.  Each entry stores a short human-readable reason that
  // surfaces in the drop log so users can tell *why* the term was
  // ignored.
  //
  // Tier 1 (class suppressed)      — entire class unavailable.
  // Tier 2 (attribute suppressed)  — specific attribute of a class.
  // Typo guard                     — unknown class/attr still throws.

  /// Mark a class as suppressed (e.g. `line` under `use_single_bus`).
  /// `reason` should outlive `SimulationLP` — a constexpr literal is
  /// fine, a caller-owned `std::string` is not.
  void suppress_ampl_class(std::string_view class_name, std::string_view reason)
  {
    m_ampl_suppressed_classes_.insert_or_assign(class_name, reason);
  }

  /// Mark a specific attribute as suppressed (e.g. `bus.theta` when
  /// Kirchhoff is disabled).  Same lifetime requirements as
  /// `suppress_ampl_class`.
  void suppress_ampl_attribute(std::string_view class_name,
                               std::string_view attribute,
                               std::string_view reason)
  {
    m_ampl_suppressed_attrs_.insert_or_assign(std::pair {class_name, attribute},
                                              reason);
  }

  /// Return the suppression reason if either the class or the
  /// (class, attribute) pair is suppressed; nullopt otherwise.
  /// Class-level suppression takes precedence over attribute-level.
  [[nodiscard]] std::optional<std::string_view> find_ampl_suppression(
      std::string_view class_name, std::string_view attribute) const noexcept
  {
    if (const auto it = m_ampl_suppressed_classes_.find(class_name);
        it != m_ampl_suppressed_classes_.end())
    {
      return it->second;
    }
    if (const auto it =
            m_ampl_suppressed_attrs_.find(std::pair {class_name, attribute});
        it != m_ampl_suppressed_attrs_.end())
    {
      return it->second;
    }
    return std::nullopt;
  }

  // ── Class-level parameter dispatch table ─────────────────────────────
  //
  // Each `*LP` class registers its `param_*` accessors with the
  // simulation under `(class_name, attribute)` keys.  The
  // user-constraint resolver in `element_column_resolver.cpp` then
  // performs a single map lookup followed by a function-pointer call,
  // replacing the former 200-line per-class if/else chain.  Same
  // call_once lifetime as element names / compounds — populated once,
  // read-only thereafter, no synchronization needed at lookup time.

  /// Register one (class, attribute) -> resolver entry.  Called once
  /// per attribute from `system_lp.cpp`'s AMPL call_once block.
  void register_ampl_param(std::string_view class_name,
                           std::string_view attribute,
                           AmplParamFn fn)
  {
    m_ampl_params_.insert_or_assign(
        AmplParamKey {.class_name = class_name, .attribute = attribute}, fn);
  }

  /// Look up the resolver for (class, attribute).  Returns nullptr when
  /// the pair is not registered — the caller falls back to the legacy
  /// path or returns nullopt.
  [[nodiscard]] AmplParamFn find_ampl_param(
      std::string_view class_name, std::string_view attribute) const noexcept
  {
    const auto it = m_ampl_params_.find(
        AmplParamKey {.class_name = class_name, .attribute = attribute});
    return (it != m_ampl_params_.end()) ? it->second : nullptr;
  }

  /// Register a class-level iterator function.  Called once per class
  /// from `system_lp.cpp`'s AMPL call_once block.
  void register_ampl_iter(std::string_view class_name, AmplIterFn fn)
  {
    m_ampl_iters_.insert_or_assign(class_name, fn);
  }

  /// Look up the iterator for a class.  Returns nullptr when the class
  /// is not registered.
  [[nodiscard]] AmplIterFn find_ampl_iter(
      std::string_view class_name) const noexcept
  {
    const auto it = m_ampl_iters_.find(class_name);
    return (it != m_ampl_iters_.end()) ? it->second : nullptr;
  }

  // ── Element metadata registry (F9) ───────────────────────────────────
  //
  // Elements register a small `{key → value}` bundle so that
  // multi-predicate `sum(...)` filters (F4) can evaluate predicates at
  // row-assembly time without re-reading element-specific fields.
  //
  // Stored per `(scene, phase)` cell — same partitioning as variables —
  // so writes need no synchronization.  Metadata content is independent
  // of (scene, phase), but per-cell duplication is small and avoids any
  // contention with the parallel scene builds.
  //
  // Keys are `string_view`s into constexpr identifiers (e.g. "type",
  // "bus", "zone", "cap") and values are either strings or numbers.
  // Safe to call multiple times per element; the second call overwrites.
  void register_ampl_element_metadata(SceneIndex scene_index,
                                      PhaseIndex phase_index,
                                      std::string_view class_name,
                                      Uid element_uid,
                                      AmplElementMetadata metadata)
  {
    m_ampl_lp_cells_[scene_index][phase_index].metadata[AmplMetadataKey {
        .class_name = class_name,
        .element_uid = element_uid,
    }] = std::move(metadata);
  }

  /// Look up an element's metadata bundle.  Returns nullptr when the
  /// element hasn't registered any (e.g. a type without filter-relevant
  /// fields).
  [[nodiscard]] const AmplElementMetadata* find_ampl_element_metadata(
      SceneIndex scene_index,
      PhaseIndex phase_index,
      std::string_view class_name,
      Uid element_uid) const noexcept
  {
    const auto& cell = m_ampl_lp_cells_[scene_index][phase_index];
    const auto it = cell.metadata.find(AmplMetadataKey {
        .class_name = class_name,
        .element_uid = element_uid,
    });
    return (it != cell.metadata.end()) ? &it->second : nullptr;
  }

private:
  std::reference_wrapper<const Simulation> m_simulation_;
  std::reference_wrapper<const PlanningOptionsLP> m_options_;
  std::vector<BlockLP> m_block_array_;
  std::vector<StageLP> m_stage_array_;
  std::vector<PhaseLP> m_phase_array_;
  std::vector<ScenarioLP> m_scenario_array_;
  std::vector<SceneLP> m_scene_array_;

  global_variable_map_t m_global_variable_map_;

  /// Parallel state-variable registry for the SDDP backward-pass aperture
  /// systems (`SystemKind::aperture`).  Same `[scene][phase]` shape as
  /// `m_global_variable_map_`; stays entirely empty unless an
  /// `aperture_system_file` is in effect.  Routed via `variable_map()`.
  global_variable_map_t m_aperture_variable_map_;

  /// Integer-variable registries — forward and aperture systems.  Same
  /// `[scene][phase]` partitioning as the state-variable registries.
  /// Populated by `add_integer_variable`; queried by the (future)
  /// layout binding resolver and by integer-feasibility audits.
  global_integer_variable_map_t m_global_integer_map_;
  global_integer_variable_map_t m_aperture_integer_map_;

  // (scenario, stage) → (scene, phase) factored lookup tables.
  // Populated once in the constructor; read-only afterwards, so no
  // synchronization is needed at lookup time.
  flat_map<ScenarioUid, SceneIndex> m_scene_of_scenario_;
  flat_map<StageUid, PhaseIndex> m_phase_of_stage_;

  // Per-scene α-rebase offset c̄, published by the SDDP method at end of
  // solve via `set_alpha_offsets` and read by `FutureCostLP::add_to_output`.
  // Empty until published; see `alpha_offset` for the out-of-range guard.
  StrongIndexVector<SceneIndex, double> m_alpha_offsets_ {};

  // When true, add_ampl_variable() populates the per-cell variable
  // maps.  When false (default), it is a no-op — the maps stay empty,
  // saving allocation/hashing overhead for runs without user constraints
  // or PAMPL evaluation.
  bool m_need_ampl_variables_ {false};

  // PAMPL variable registry — populated by each LP element's add_to_lp
  // and queried by element_column_resolver.cpp.
  //
  // Per-(scene, phase) cells: each cell is written by exactly one
  // scene-thread, sequentially across phases (see planning_lp.cpp), so
  // no per-cell mutex is needed.  Allocated once in the constructor
  // with shape `[m_scene_array_.size()][m_phase_array_.size()]`.
  ampl_lp_registry_t m_ampl_lp_cells_;

  // Globally-shared read-only registries (element-name lookup and
  // class-level compounds).  Populated exactly once — `SystemLP`'s
  // constructor calls into `register_all_ampl_element_names` via the
  // `std::call_once` below — so parallel scene-builds race to be first
  // and only one thread actually writes.  Reads are lock-free
  // thereafter.
  mutable std::once_flag m_ampl_registry_flag_;
  AmplElementNameMap m_ampl_element_names_;
  AmplCompoundMap m_ampl_compounds_;
  AmplScalarMap m_ampl_scalars_;

  // Suppression registries: populated from system_lp.cpp's AMPL
  // call_once block.  Keys use `std::string_view` into constexpr
  // literals (class names, attribute names, reason strings).
  flat_map<std::string_view, std::string_view> m_ampl_suppressed_classes_;
  flat_map<std::pair<std::string_view, std::string_view>, std::string_view>
      m_ampl_suppressed_attrs_;

  // Class-level parameter and iterator dispatch tables.  Populated
  // exactly once via `register_all_ampl_element_names`'s call_once
  // block; read-only thereafter, no synchronization needed.
  AmplParamMap m_ampl_params_;
  AmplIterMap m_ampl_iters_;

  // Shared, scene/phase-invariant arrow input-index cache (pimpl).  The
  // (cname, fname, uid) -> (Stage, Block) row index built by the long-direct
  // reader is identical for every (scene, phase) cell, so it is cached once
  // here and reused, instead of being rebuilt (and the parquet re-read) per
  // cell inside each InputContext.  Held behind a forward-declared
  // ArrowIndexCache so this header need not include uid_traits.hpp (that
  // would form an include cycle via uididx_traits.hpp).  Guarded by
  // m_array_index_mtx_ because the per-(scene, phase) SystemLP builds run
  // concurrently (one task per cell); the cache is also lazily created under
  // that lock.  See InputContext::get_array_index / make_array_index.
  mutable std::mutex m_array_index_mtx_;
  mutable std::shared_ptr<ArrowIndexCache> m_arrow_index_cache_;

public:
  /// Exposed so that `SystemLP`'s constructor can wrap the one-shot
  /// AMPL-registry population in `std::call_once`.  Not part of the
  /// public user API — `friend class SystemLP` would be purer but
  /// pulls in a circular include.  Returning the flag by reference
  /// keeps the policy decision (what to register) in `system_lp.cpp`
  /// where all the LP element headers are already visible.
  [[nodiscard]] std::once_flag& ampl_registry_flag() const noexcept
  {
    return m_ampl_registry_flag_;
  }

  /// Shared arrow input-index cache (see the member docs).  Used by
  /// make_array_index so the scene/phase-invariant (Stage, Block) row index
  /// is built once across all per-cell builds rather than rebuilt per
  /// InputContext.  Lazily creates the cache on first use; callers MUST hold
  /// array_index_mutex() (make_array_index does), which also serialises that
  /// creation.  Defined out-of-line in simulation_lp.cpp because
  /// ArrowIndexCache is only forward-declared here (pimpl).
  [[nodiscard]] ArrowIndexCache& arrow_index_cache() const;

  /// Returns the shared cache pointer so callers can propagate it across
  /// SimulationLP instances.  The cascade passes it to each per-level
  /// PlanningLP so the (cname, fname, uid) -> (Stage, Block) index stays warm
  /// across levels instead of being rebuilt cold each level (a fresh
  /// PlanningLP per level owns a fresh SimulationLP by value, so without this
  /// the index-cache fix would be scoped to a single level).  Lazily creates
  /// on first use.  Defined out-of-line (ArrowIndexCache is forward-declared).
  [[nodiscard]] std::shared_ptr<ArrowIndexCache> arrow_index_cache_ptr() const;

  /// Mutex guarding arrow_index_cache() against the concurrent
  /// per-(scene, phase) SystemLP builds.
  [[nodiscard]] std::mutex& array_index_mutex() const noexcept
  {
    return m_array_index_mtx_;
  }
};

}  // namespace gtopt
