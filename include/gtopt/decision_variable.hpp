/**
 * @file      decision_variable.hpp
 * @brief     Free continuous Decision Variable referenced by UserConstraints
 * @date      Mon May 19 21:55:00 2026
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the DecisionVariable structure — a per-(scenario, stage, block)
 * free continuous LP column that user constraints can reference via the
 * AMPL-style accessor ``decision_variable("X").value``.  Mirrors the
 * PLEXOS ``Decision Variable`` class (class_id 72) used in the CEN PCP
 * daily bundle for reserve-allocation knobs (BESS_*_LW/RS, CPF_*, …) and
 * generic free parameters.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "bess_angamos_lw",
 *   "lower_bound": -1000,
 *   "upper_bound":  1000,
 *   "cost": 0
 * }
 * ```
 *
 * The optional ``cost`` adds the column to the LP objective with
 * ``cost × value × duration`` (block-energy-scaled, same convention as
 * other gtopt LP elements).  Bounds default to the free LP range
 * (``[-LP_INFINITY, LP_INFINITY]``) when unset.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief A free continuous LP column with optional bounds and cost.
 *
 * @see DecisionVariableLP for the LP build path
 */
struct DecisionVariable
{
  static constexpr LPClassName class_name {"DecisionVariable"};

  Uid uid {unknown_uid};
  Name name {};
  OptActive active {};
  OptName type {};  ///< Optional element type/category tag
  OptName description {};  ///< Optional free-text description (e.g. conversion
                           ///< provenance)

  /// Lower bound on the LP column.  When unset the LP treats the
  /// column as ``≥ -LP_INFINITY`` (free below); set to ``0`` to enforce
  /// a non-negative variable.
  OptReal lower_bound {};

  /// Upper bound on the LP column.  When unset the LP treats the
  /// column as ``≤ LP_INFINITY`` (free above).
  OptReal upper_bound {};

  /// Per-MW objective contribution.  Scaled by block duration via
  /// ``CostHelper::block_ecost`` so the units match ``Generator.gcost``.
  OptReal cost {};
};

}  // namespace gtopt
