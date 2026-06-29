/**
 * @file      decision_variable_lp.hpp
 * @brief     LP representation of a free continuous Decision Variable
 * @date      Mon May 19 21:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * One LP column per (scenario, stage, block) with optional bounds and
 * an optional per-block cost coefficient.  Registered with the AMPL
 * resolver as ``decision_variable("X").value`` so UserConstraint
 * expressions can reference it directly.
 *
 * Mirrors the lightweight pattern of :class:`FlowLP` but without any
 * junction coupling — the variable is purely a free knob that other
 * constraints constrain.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/decision_variable.hpp>
#include <gtopt/index_holder.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/user_constraint_enums.hpp>

namespace gtopt
{
class SceneLP;
class PhaseLP;

using DecisionVariableLPSId = ObjectSingleId<class DecisionVariableLP>;

class DecisionVariableLP : public ObjectLP<DecisionVariable>
{
public:
  /// AMPL accessor name: ``decision_variable("X").value``.
  static constexpr std::string_view ValueName {"value"};

  /// Dedicated `StateVariable::Key::class_name` prefix for AMPL **state**
  /// columns (piece 5).  Distinct from the element's own `DecisionVariable`
  /// class name AND from the engine state classes (reservoir efin's element
  /// class, the built-in α `sddp_alpha_lp_class`), so the previous-phase
  /// `StateVariable::Key` synthesized for an AMPL `link` round-trips through
  /// cut I/O without colliding with engine state.  Guard 4 of the piece-5
  /// review block.
  static constexpr LPClassName StateClassName {"UserStateVar"};

  /// AMPL accessor name for a `block_state` variable's cross-phase INCOMING
  /// value (the previous phase's end-of-phase value, pinned by the SDDP
  /// forward pass).  `prev(decision_variable("X").value)` at the first block
  /// of a stage resolves to this column.
  static constexpr std::string_view IncomingName {"value_in"};

  /// Dedicated `StateVariable::Key::class_name` for a `block_state`
  /// DecisionVariable's per-block storage state (the last block's column).
  /// Distinct from `StateClassName` (coarse state) and from engine state, so
  /// it round-trips through cut I/O without collision.
  static constexpr LPClassName BlockStateClassName {"UserReservoirState"};

  explicit DecisionVariableLP(const DecisionVariable& pdv,
                              const InputContext& ic);

  [[nodiscard]] constexpr auto&& decision_variable(this auto&& self) noexcept
  {
    return self.object();
  }

  /// Per-(scenario, stage, block) build.  Takes a NON-const `SystemContext&`
  /// (like ReservoirLP) because a `block_state` variable registers the
  /// last-block column as a cross-phase `StateVariable` and defers a state
  /// link — both non-const registry mutations.
  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  /// Planning-level `phase`-scope build (piece 4/5): one column per
  /// (scene, phase) cell.  No-op unless this variable's `scope` is `phase`.
  /// Takes a NON-const `SystemContext&` (like ReservoirLP) because a `state`
  /// variable registers a cross-phase `StateVariable` (a non-const registry
  /// mutation).
  [[nodiscard]] bool add_to_phase_lp(SystemContext& sc,
                                     const SceneLP& scene,
                                     const PhaseLP& phase,
                                     LinearProblem& lp);

  /// Planning-level `global`-scope build (piece 4/5): one un-indexed column
  /// per (scene, phase) cell.  No-op unless this variable's `scope` is
  /// `global`.  The user FCF α is a `global` `state` column.  Non-const
  /// `SystemContext&` for the same state-registration reason.
  [[nodiscard]] bool add_to_global_lp(SystemContext& sc,
                                      const SceneLP& scene,
                                      const PhaseLP& phase,
                                      LinearProblem& lp);

  [[nodiscard]] bool add_to_output(OutputContext& out) const;

  /// Resolved time-granularity scope (block default).  Read by `UserModelLP`
  /// and the planning-pass routing.
  [[nodiscard]] constexpr ConstraintScope scope() const noexcept
  {
    return m_scope_;
  }

  [[nodiscard]] auto&& value_cols_at(const ScenarioLP& scenario,
                                     const StageLP& stage) const
  {
    return value_cols.at({scenario.uid(), stage.uid()});
  }

  /// Read-only access to the full per-(scenario, stage) per-block column map
  /// populated by `add_to_lp`.  Additive accessor used by `UserModelLP` to
  /// re-emit a bundled variable's `:sol` / `:cost` under the model's own
  /// `output/UserModel/<tag>/...` namespace instead of `DecisionVariable/`.
  [[nodiscard]] constexpr const STBIndexHolder<ColIndex>& value_cols_holder()
      const noexcept
  {
    return value_cols;
  }

private:
  /// Shared per-cell column builder for the coarse (`phase` / `global`) scope:
  /// adds ONE LP column for the (scene, phase) cell, registers it (and, for a
  /// `state` variable, the cross-phase StateVariable + deferred link), and
  /// stores it in `value_cols` keyed by the cell's representative
  /// (scenario, stage).  Returns false on a hard error (already thrown).
  [[nodiscard]] bool build_cell_col(SystemContext& sc,
                                    const SceneLP& scene,
                                    const PhaseLP& phase,
                                    LinearProblem& lp);

  /// `block_state` storage-coupling for one (scenario, stage): create the
  /// INCOMING column (`value_in`) — first-stage fixed to `initial_value`,
  /// same-phase-later-stage aliasing the previous stage's end column, or a
  /// free cross-phase column linked via `defer_state_link` — and register the
  /// last block's `value` column as the cross-phase state.  Precondition: the
  /// per-block `value` columns for this (scenario, stage) are already in
  /// `value_cols`.  Called only when `m_block_state_`.  Throws on a
  /// multi-block non-chronological stage (the within-stage `prev()` lag would
  /// be ill-defined) or a missing previous-stage end column.
  void register_block_state(SystemContext& sc,
                            const ScenarioLP& scenario,
                            const StageLP& stage,
                            LinearProblem& lp,
                            double lower,
                            double upper);

  /// Per-(scenario, stage) per-block column index map; populated by
  /// :meth:`add_to_lp` and read by :meth:`add_to_output` to emit the
  /// ``DecisionVariable/value`` solution columns.
  STBIndexHolder<ColIndex> value_cols;

  /// Resolved time-granularity scope (parsed once at construction).  Unset
  /// JSON `scope` ⇒ `Block` (legacy per-(scenario, stage, block) build).
  ConstraintScope m_scope_ {ConstraintScope::Block};

  /// AMPL state-variable flag (piece 5) — registers the coarse column as a
  /// cross-phase `StateVariable` so it rides the SDDP backward pass.
  bool m_is_state_ {false};

  /// Cross-phase link flag (piece 5) — defers a link from this phase's
  /// column to the previous phase's same-variable column.
  bool m_link_ {false};

  /// Per-block storage-state flag — a `block`-scoped state variable that
  /// behaves like a reservoir volume: per-block columns, a cross-phase
  /// INCOMING column (`value_in`), and the last-block column registered as
  /// the cross-phase state.  Requires `link: true`; mutually exclusive with
  /// the coarse `state` flag.
  bool m_block_state_ {false};

  /// Per-(scenario, stage) INCOMING column for a `block_state` variable
  /// (the `value_in` / sini-equivalent).  Read within a phase to chain
  /// stages, and registered under `IncomingName` for `prev(...)`.
  STIndexHolder<ColIndex> incoming_cols;

  /// Per-(scenario, stage) end-of-stage (last-block) column for a
  /// `block_state` variable (the efin-equivalent / cross-phase state).
  STIndexHolder<ColIndex> efin_cols;
};

}  // namespace gtopt
