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
 * The optional ``cost`` adds the column to the LP objective.  How it
 * folds in depends on ``cost_type``; the default is ``"raw"`` (face value,
 * NO probability/discount/duration weighting), matching the PLEXOS penalty
 * DecisionVariables.  Bounds default to the free LP range
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

  /// Objective contribution per unit of ``value``.
  ///
  /// How it folds into the objective depends on ``cost_type``:
  ///  - ``"raw"`` (default): ``value`` is a face-value money / unitless
  ///    amount (a PLEXOS penalty knob, reserve VoRS, the FCF cost-to-go
  ///    ``alpha_fcf``, …), so the term is just ``cost · value`` at face
  ///    value — NO probability, discount, or block-duration weighting.
  ///  - ``"power"``: ``value`` is a rate (MW), so the term is
  ///    ``cost · value · block_duration`` (``CostHelper::block_ecost`` —
  ///    same convention as ``Generator.gcost``).
  ///  - ``"energy"``: ``value`` is already a total (MWh), so the term is
  ///    ``cost · value`` with NO block-duration multiply — only
  ///    probability × discount.
  OptReal cost {};

  /// Cost interpretation for the objective: ``"raw"`` (default, face value,
  /// no prob/discount/duration), ``"power"`` (duration-weighted) or
  /// ``"energy"`` (prob × discount, not duration-weighted).  Parsed as
  /// ``ConstraintScaleType``.
  OptName cost_type {};

  /// Optional single-block scope (block ``uid``).  When set, the LP
  /// creates the column ONLY on that block instead of one per block —
  /// used for end-of-horizon quantities like the FCF cost-to-go
  /// ``alpha_fcf``, which must be a single last-block variable (a
  /// per-block column lets the unconstrained blocks distort the
  /// objective).  When unset, the default per-(scenario, stage, block)
  /// column set is created.
  OptUid block {};

  /// Optional objective constant for a mean-shifted (rebased) variable.
  /// When the variable is α-rebased as ``value = value' + obj_constant``
  /// (the LP column holds ``value'``), the objective loses the constant
  /// term ``cost · obj_constant``.  When set, the LP adds that term back
  /// via ``LinearProblem::add_obj_constant`` (using the column's resolved
  /// cost coefficient, so the discount is applied correctly), keeping the
  /// reported objective algebraically equal to the un-rebased model.
  /// Intended for single-``block`` columns (added once per created
  /// column); the FCF cost-to-go ``alpha_fcf`` sets it to the mean cut
  /// RHS.
  OptReal obj_constant {};
};

}  // namespace gtopt
