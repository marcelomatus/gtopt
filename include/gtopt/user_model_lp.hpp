/**
 * @file      user_model_lp.hpp
 * @brief     LP representation of a UserModel (generic AMPL capture) element
 * @date      Sun Jun 22 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A `UserModel` bundles a set of user variable declarations (cols) + user
 * constraint declarations (rows) into ONE element and captures the named ones
 * to the solution under `output/UserModel/<tag>/{sol,cost,dual,slack}`.
 *
 * `UserModelLP` REUSES the existing element machinery rather than forking it:
 * it owns one internal `DecisionVariableLP` per declared variable and one
 * internal `UserConstraintLP` per declared constraint, and delegates the LP
 * build to them verbatim — so the shared expr resolver, the `add_ampl_variable`
 * routing, the `silent_flatten_pass()` rebuild gate, the soft-slack folding,
 * the scope routing (`block|stage|phase|global`) and the aux-col lowering are
 * the SAME code paths as the standalone elements.  Only the OUTPUT is
 * re-namespaced: `add_to_output` reads each delegate's col/row holders (via the
 * additive read-only getters) and emits them under the model's `tag` instead of
 * the delegates' own `DecisionVariable/` / `UserConstraint/` directories.
 *
 * `UserConstraint` / `DecisionVariable` are UNCHANGED — irrigation keeps using
 * them as standalone elements.  See docs/design/future_cost_and_user_model.md
 * (piece 3).
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <gtopt/decision_variable_lp.hpp>
#include <gtopt/object_lp.hpp>
#include <gtopt/user_constraint_lp.hpp>
#include <gtopt/user_model.hpp>

namespace gtopt
{

class SystemContext;
class ScenarioLP;
class StageLP;
class SceneLP;
class PhaseLP;
class LinearProblem;
class OutputContext;

using UserModelLPSId = ObjectSingleId<class UserModelLP>;

class UserModelLP : public ObjectLP<UserModel>
{
public:
  /// Output stream stems (no magic strings — see feedback_no_magic_strings).
  static constexpr std::string_view SolName {"sol"};
  static constexpr std::string_view CostName {"cost"};
  static constexpr std::string_view DualName {"dual"};
  static constexpr std::string_view SlackName {"slack"};

  explicit UserModelLP(const UserModel& um, InputContext& ic);

  [[nodiscard]] constexpr auto&& user_model(this auto&& self) noexcept
  {
    return self.object();
  }

  /// Operational build: drives every internal `DecisionVariableLP` then every
  /// internal `UserConstraintLP` for one (scenario, stage) pair (block/stage
  /// scope).  Coarse (phase/global) scopes are no-ops here — they route through
  /// the planning passes below, exactly like the standalone elements.
  /// Non-const `SystemContext&` (like ReservoirLP / DecisionVariableLP)
  /// because a bundled `block_state` variable registers a cross-phase state.
  [[nodiscard]] bool add_to_lp(SystemContext& sc,
                               const ScenarioLP& scenario,
                               const StageLP& stage,
                               LinearProblem& lp);

  /// Planning-level phase-scope build (one row/col per (scene, phase) cell)
  /// for any bundled var/constraint declared with `scope: phase`.  Non-const
  /// `SystemContext&` because a bundled `state` variable registers a
  /// cross-phase `StateVariable` (a non-const registry mutation).
  [[nodiscard]] bool add_to_phase_lp(SystemContext& sc,
                                     const SceneLP& scene,
                                     const PhaseLP& phase,
                                     LinearProblem& lp);

  /// Planning-level global-scope build (one un-indexed row/col per cell) for
  /// any bundled var/constraint declared with `scope: global`.  Non-const
  /// `SystemContext&` for the same state-registration reason.
  [[nodiscard]] bool add_to_global_lp(SystemContext& sc,
                                      const SceneLP& scene,
                                      const PhaseLP& phase,
                                      LinearProblem& lp);

  /// Captures the bundled NAMED cols (`:sol`/`:cost`) and rows
  /// (`:dual`/`:slack`) under `output/UserModel/<tag>/<name>_{sol,cost,
  /// dual,slack}`.  Aux cols/rows created by abs/min/max lowering are
  /// internal LP detail and are NOT captured (see the policy note in the cpp).
  [[nodiscard]] bool add_to_output(OutputContext& out) const;

private:
  /// Owned output field stems for one bundled constraint.  The
  /// `OutputContext` field key is `std::array<string_view, 3>` and keeps the
  /// view for the whole solve, so the composed `"<tag>/<name>"` strings must
  /// be owned here (the same lifetime trick `UserConstraintLP::m_slack_label_`
  /// uses).
  struct ConFields
  {
    std::string field;  ///< `<tag>/<constraint-name>` — :dual stem
    std::string slack;  ///< `<tag>/<constraint-name>/slack` — :sol/:cost stem
    std::string slack_neg;  ///< `<tag>/<constraint-name>/slack_neg` stem
  };

  /// Output namespacing tag (`tag` or, when unset, the element name).
  std::string m_tag_ {};
  /// Per-variable owned `<tag>/<var-name>` stems, indexed with `m_variables_`.
  std::vector<std::string> m_var_fields_ {};
  /// Per-constraint owned stems, indexed with `m_constraints_`.
  std::vector<ConFields> m_con_fields_ {};

  /// Internal delegates — one per declared variable / constraint.  They own
  /// the actual LP-build + AMPL-registry logic; UserModelLP only orchestrates
  /// and re-namespaces the output.
  std::vector<DecisionVariableLP> m_variables_ {};
  std::vector<UserConstraintLP> m_constraints_ {};
};

// Pin the data-struct constant value so an accidental rename of the
// `UserModel::class_name` literal fails the build (output dirs depend on the
// exact string `"UserModel"`).
static_assert(UserModelLP::Element::class_name == LPClassName {"UserModel"},
              "UserModel::class_name must remain \"UserModel\"");

}  // namespace gtopt
