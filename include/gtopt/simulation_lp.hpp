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

#include <functional>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtopt/ampl_variable.hpp>
#include <gtopt/block_lp.hpp>
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
  explicit SimulationLP(const Simulation& simulation,
                        const PlanningOptionsLP& options);

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

  // Add method with deducing this and perfect forwarding
  template<typename Key = state_variable_key_t>
  [[nodiscard]]
  constexpr auto add_state_variable(Key&& key,
                                    ColIndex col,
                                    double scost,
                                    double var_scale,
                                    LpContext context) -> const StateVariable&
  {
    auto&& map =
        m_global_variable_map_[key.lp_key.scene_index][key.lp_key.phase_index];

    const auto [it, inserted] = map.try_emplace(std::forward<Key>(key),
                                                key.lp_key,
                                                col,
                                                scost,
                                                var_scale,
                                                std::move(context));

    if (!inserted) {
      // Idempotent re-registration is allowed: LowMemoryMode::rebuild
      // re-runs create_lp() for the same (scene, phase), which replays
      // every add_state_variable call.  Same col index = benign rebuild;
      // different col = a real bug we still want to catch.
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
  constexpr auto&& state_variables(this Self&& self,
                                   SceneIndex scene_index,
                                   PhaseIndex phase_index) noexcept
  {
    auto&& vec = std::forward<Self>(self).m_global_variable_map_;
    return vec[scene_index][phase_index];
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
        key.lp_key.scene_index, key.lp_key.phase_index);

    const auto it = map.find(std::forward<Key>(key));
    return (it != map.end()) ? result_t {it->second} : result_t {};
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
            .stage_col = ColIndex {unknown_index},
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
            .block_cols = {},
            .stage_col = stage_col,
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

  // (scenario, stage) → (scene, phase) factored lookup tables.
  // Populated once in the constructor; read-only afterwards, so no
  // synchronization is needed at lookup time.
  flat_map<ScenarioUid, SceneIndex> m_scene_of_scenario_;
  flat_map<StageUid, PhaseIndex> m_phase_of_stage_;

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
};

}  // namespace gtopt
