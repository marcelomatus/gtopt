/**
 * @file      user_constraint.hpp
 * @brief     User-defined constraint data structure for LP formulation
 * @date      Wed Mar 12 03:00:00 2026
 * @author    copilot
 * @copyright BSD-3-Clause
 *
 * This module defines the UserConstraint struct, which stores a user-defined
 * linear constraint expression. The expression string follows an AMPL-inspired
 * syntax that references gtopt elements (generators, demands, lines, batteries)
 * and their LP variables, with optional domain restrictions over scenarios,
 * stages, and blocks.
 *
 * ### Expression syntax (AMPL-inspired)
 *
 * ```text
 * generator("TORO").generation + generator("uid:23").generation <= 300,
 *     for(stage in {4,5,6}, block in 1..30)
 * ```
 *
 * See constraint_parser.hpp for the full grammar and ConstraintExpr AST.
 */

#pragma once

#include <gtopt/basic_types.hpp>
#include <gtopt/constraint_directive.hpp>
#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/user_constraint_enums.hpp>

namespace gtopt
{

/**
 * @brief Stores a user-defined linear constraint expression
 *
 * The expression field contains the full constraint in AMPL-inspired syntax.
 * It is parsed at LP construction time into a ConstraintExpr AST
 * (see constraint_parser.hpp) and then applied to the LinearProblem.
 *
 * ### `constraint_type` and dual-value scaling
 *
 * The optional `constraint_type` field controls how the dual (shadow price) of
 * the constraint row is scaled when written to the output files.  When absent,
 * `"power"` is assumed.  See `ConstraintScaleType` for the full table.
 *
 * | Value                 | Dual output scaling                          |
 * |-----------------------|----------------------------------------------|
 * | `"power"` (default)   | `scale_obj / (prob × discount × Δt)`         |
 * | `"energy"`            | `scale_obj / (prob × discount × Δt)`         |
 * | `"raw"` / `"unitless"`| `scale_obj / discount`                       |
 *
 * ### Soft constraints with visible slacks
 *
 * When `penalty` is set to a positive value, the constraint becomes
 * **soft**: the LP assembly automatically creates slack column(s) and
 * folds them into the row so the constraint can be violated at the
 * supplied per-unit penalty cost.  All auto-created slacks are
 * registered in the AMPL variable registry (so other constraints and
 * reports can reference them) and emitted to output as
 * `UserConstraint/slack(_pos|_neg).{csv,parquet}`, making any unserved
 * water/energy demand traceable per constraint without primal-dual
 * gap reconstruction.
 *
 * - `LESS_EQUAL`    (`expr <= rhs`):  one slack column `slack`,
 *   row gets `-1.0 * slack`, cost `+penalty * slack`.
 * - `GREATER_EQUAL` (`expr >= rhs`):  one slack column `slack`,
 *   row gets `+1.0 * slack`, cost `+penalty * slack`.
 * - `EQUAL`         (`expr =  rhs`):  two slack columns `slack_pos`,
 *   `slack_neg`, row gets `+1·slack_pos − 1·slack_neg`, both with
 *   cost `+penalty`.
 * - `RANGE`: not yet supported with `penalty`; an explicit pair of
 *   one-sided constraints should be used instead.
 *
 * ### Penalty unit conversion (`penalty_class`)
 *
 * By default (`penalty_class` absent or `"raw"`) the `penalty` scalar is
 * fed directly into the slack column's LP objective coefficient.  When
 * `penalty_class` is set to `"hydro_flow"`, the `penalty` is interpreted
 * as $/m³ (volume) and converted to $/(m³/s) per block via
 * `× duration[h] × 3600`, mirroring how `hydro_fail_cost` is applied to
 * FlowRight fail columns.  This lets an irrigation balance authored with
 * `penalty = hydro_fail_cost` degrade gracefully without hard
 * infeasibility while still being priced in the same unit system as the
 * surrounding hydro network.  See `PenaltyClass` for the full table.
 */
struct UserConstraint
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `UserConstraintLP` exposes no separate `ClassName` member; callers
  /// reach the constant via `UserConstraint::class_name` directly (or
  /// `UserConstraintLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"UserConstraint"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable constraint name
  OptBool active {};  ///< Activation status (default: active)
  Name expression {};  ///< Constraint expression in AMPL-inspired syntax
  OptName description {};  ///< Optional free-text description of the constraint
  OptName constraint_type {};  ///< Scaling hint: "power" (default), "energy",
                               ///< "raw", or "unitless"
  OptReal penalty {};  ///< Per-unit slack cost.  When set and > 0, the
                       ///< constraint is relaxed via auto-created slack
                       ///< columns.  See "Soft constraints" above.
  OptName penalty_class {};  ///< Penalty unit hint: "raw" (default) or
                             ///< "hydro_flow".  See `PenaltyClass`.

  /// Per-day sum: emit ONE LP row per 24 h day instead of the default
  /// one-row-per-block expansion.  The per-block terms are accumulated
  /// into a running row and flushed (`lp.add_row`) at each *day-ending
  /// block* — the block whose cumulative duration crosses a 24 h
  /// boundary, or the stage's last in-domain block.  When set together
  /// with ``constraint_type == "energy"`` each block contributes its
  /// Δt-weighted value (`coeff · Δt_b · col_b`), so the LHS is a daily
  /// ENERGY sum `Σ_day gen·Δt` [MWh]; otherwise the daily sum is
  /// unweighted (a per-day COUNT, e.g. `Σ_day startup ≤ N`).  Models
  /// PLEXOS `RHS Day` constraints (RALCO_max_e1/e2, CANUTILLARreserve,
  /// Diesel_OffTakeDay, *_Crew).  Default unset ⇒ one row per block.
  OptBool daily_sum {};

  /// Per-(stage, block) override of the constraint's RHS.
  ///
  /// When set, the scalar RHS implied by the inline ``<op> NUMBER``
  /// at the end of ``expression`` is replaced by ``rhs.optval(stage,
  /// block)`` for every LP row instantiated from this constraint.
  /// Blocks where the schedule returns ``std::nullopt`` fall back to
  /// the expression's parsed scalar so legacy JSON keeps working.
  ///
  /// Accepts the full ``OptTBRealFieldSched`` payload shape:
  ///
  /// - **scalar** (``double``): broadcast to every (stage, block).
  /// - **T-schedule** (one value per stage): broadcast across blocks.
  /// - **TB matrix** (``[[stage_blocks]]``): per-(stage, block).
  /// - **string**: name of a parquet field providing the per-block
  ///   profile (resolved via ``input_directory``); also doubles as the
  ///   AMPL parameter name exported for ``user_constraint("X").rhs``
  ///   references in other constraints' expressions.
  ///
  /// PLEXOS PATTERN / Constraint_RHS profiles map cleanly onto this
  /// field via plexos2gtopt's writer.
  OptTBRealFieldSched rhs {};

  /// Typed constraint-family directive.  Replaces the legacy name-regex
  /// classification (``_RegRange_``, ``Gas_MaxOpDay``, MinProvision) and
  /// hardcoded soft-penalty ladder that lived inside the
  /// ``plexos2gtopt`` converter — see ``ConstraintDirective`` for the
  /// payload schema and the rationale.
  ///
  /// Optional and defaulted to ``std::nullopt`` so every JSON written
  /// before the migration round-trips unchanged: when no directive is
  /// present ``UserConstraintLP`` behaves exactly as before, falling
  /// back to ``penalty`` and ``daily_sum`` for soft-tier and
  /// aggregation policy.  Directive scaffolding lands in Step 1 of the
  /// AMPL/PAMPL modernization plan (2026-05-30); migration of the
  /// three plexos2gtopt detection sites (RegRange / Gas_MaxOpDay /
  /// MinProvision) follows in independent steps.
  OptConstraintDirective directive {};

  /// Time-granularity at which this constraint's LP rows are instantiated
  /// (``"block"`` default, ``"stage"``, ``"phase"``, ``"global"``).  Parsed
  /// as ``ConstraintScope``.  ORTHOGONAL to the ``for(...)`` clause: ``scope``
  /// reduces granularity (one row per stage / per cell), the ``for(...)``
  /// clause restricts which instances are emitted.
  ///
  /// - ``block`` (default): one LP row per (scenario, stage, block) — the
  ///   historical behaviour; per-block element references resolve directly.
  /// - ``stage``: one LP row per (scenario, stage).  Per-block references
  ///   inside the expression must be wrapped in a ``sum{b in stage}``
  ///   time-aggregator so the coarse row reaches the per-block columns.
  ///   Built in the per-(scenario, stage) operational pass.
  /// - ``phase`` / ``global``: one LP row per (scene, phase) cell, built in
  ///   the planning pass (``add_to_phase_lp`` / ``add_to_global_lp``) — the
  ///   FCF α terminal cut / annual-cap shape.
  ///
  /// Default unset ⇒ ``block`` (every legacy JSON round-trips unchanged).
  OptName scope {};

  /// User-controlled name for the auto-created slack column on soft
  /// constraints (overrides the LP-internal default ``"slack"``).
  ///
  /// **Source.** Populated by the PAMPL parser when a top-level
  /// ``var <ident> [, <ident>]*;`` declaration matches the
  /// constraint's name by naming convention (``var slack_<NAME>``
  /// binds to constraint ``NAME``).  JSON consumers can set this
  /// directly without the PAMPL bridge.  Empty ⇒ LP uses the static
  /// ``UserConstraintLP::SlackName`` default.
  ///
  /// **Scope.** Affects the LP-internal column label
  /// (``SparseCol::variable_name``) only — the parquet output schema
  /// stays aggregated as ``UserConstraint/slack_sol.parquet`` (keyed by
  /// constraint uid) so downstream tooling never has to discover the
  /// per-UC slack name.  The benefit is purely audit / debug:
  ///
  ///   * CPLEX log lines reference ``slack_HYDRO_FLOOR_X`` instead of
  ///     a generic ``slack``;
  ///   * ``.pampl`` files become self-documenting via the matching
  ///     ``var slack_HYDRO_FLOOR_X;`` declaration.
  ///
  /// Equality constraints (which produce two slack columns) suffix
  /// ``_pos`` / ``_neg`` on top of the user-supplied name.
  OptName slack_name {};
};

}  // namespace gtopt
