/**
 * @file      user_model.hpp
 * @brief     UserModel element — a self-contained AMPL model fragment
 * @date      Sun Jun 22 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the `UserModel` structure — a first-class element that bundles, in
 * ONE element, a set of user variable declarations (cols) + user constraint
 * declarations (rows).  It is the generic AMPL-capture vehicle of the
 * FutureCost / UserModel refactor (piece 3): the named cols/rows it owns are
 * captured to the solution under
 * `output/UserModel/<tag>/{sol,cost,dual,slack}`, and piece 5 will build
 * `AmplFutureCost` (a global α var + user cuts) on top of it.
 *
 * Rather than inventing a new AST, `UserModel` REUSES the existing
 * `DecisionVariable` (cols) and `UserConstraint` (rows) structures verbatim:
 * `variable_array` holds the column declarations and `constraint_array` holds
 * the row declarations.  `UserModelLP` then drives one internal
 * `DecisionVariableLP` / `UserConstraintLP` per declaration through the SAME
 * resolver / AMPL-registry / scope-routing machinery as the standalone
 * elements — additive, no fork.
 *
 * `UserConstraint` and `DecisionVariable` keep working as standalone elements
 * (irrigation depends on them); `UserModel` is a NEW, additive aggregation.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "fcf_recourse",
 *   "tag": "fcf",
 *   "variable_array": [
 *     {"uid": 1, "name": "alpha", "scope": "global", "cost": 1,
 *      "cost_type": "raw"}
 *   ],
 *   "constraint_array": [
 *     {"uid": 1, "name": "FcfCut", "scope": "global",
 *      "expression": "decision_variable('alpha').value >= 0"}
 *   ]
 * }
 * ```
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/decision_variable.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>
#include <gtopt/user_constraint.hpp>

namespace gtopt
{

/**
 * @brief A self-contained AMPL model fragment: user vars + user constraints.
 *
 * @see UserModelLP for the LP build / output path.
 */
struct UserModel
{
  static constexpr LPClassName class_name {"UserModel"};

  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};
  OptName description {};  ///< Optional free-text provenance.

  /// Output namespacing tag — the captured cols/rows land under
  /// `output/UserModel/<tag>/<name>_{sol,cost,dual,slack}`.  When unset the
  /// element `name` is used as the tag (so the capture is always namespaced).
  OptName tag {};

  /// Column declarations (free continuous LP vars).  Reuses the
  /// `DecisionVariable` structure verbatim — each entry becomes one internal
  /// `DecisionVariableLP` driven through the standard build path (per-block,
  /// stage, phase or global per its own `scope`).  Captured to
  /// `output/UserModel/<tag>/<var-name>_{sol,cost}`.
  Array<DecisionVariable> variable_array {};

  /// Constraint declarations (LP rows).  Reuses the `UserConstraint`
  /// structure verbatim — each entry becomes one internal `UserConstraintLP`
  /// driven through the standard build path (incl. soft slacks + scope
  /// routing + aux-col lowering).  Captured to
  /// `output/UserModel/<tag>/<constraint-name>_{dual,slack}`.
  Array<UserConstraint> constraint_array {};
};

}  // namespace gtopt
