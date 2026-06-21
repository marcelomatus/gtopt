/**
 * @file      waterway.hpp
 * @brief     Header for waterway components in hydro power systems
 * @date      Wed Jul 30 11:40:33 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Waterway structure representing a water channel
 * connecting two hydraulic junctions. Waterways carry water flow between
 * junctions and are the analogue of transmission lines in the hydraulic
 * network.
 *
 * Flow units: **m³/s** (cubic metres per second).
 *
 * ### JSON Example (cascade — both junctions set)
 * ```json
 * {
 *   "uid": 1,
 *   "name": "w1_2",
 *   "junction_a": "j1",
 *   "junction_b": "j2",
 *   "fmin": 0,
 *   "fmax": 300
 * }
 * ```
 *
 * ### JSON Example (outflow — `junction_b` omitted)
 * ```json
 * {
 *   "uid": 2,
 *   "name": "w_outflow",
 *   "junction_a": "j_terminal",
 *   "fcost": 3.6
 * }
 * ```
 * The carried flow drains directly out of the system at `junction_a`,
 * so no synthetic ocean / sink junction is required.  Useful for
 * spillage arcs (PLP `Vert_*`) when keeping the flow visible is more
 * informative than collapsing it onto a `Junction.drain` column.
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 2-D inline array indexed by `[stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Waterway/`
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @struct Waterway
 * @brief Water channel connecting two hydraulic junctions
 *
 * A waterway carries water from an upstream junction (`junction_a`) to a
 * downstream junction (`junction_b`).  Flow is constrained between `fmin`
 * and `fmax`.  An optional `lossfactor` models seepage or evaporation in
 * transit.  Turbines are attached to waterways to generate electricity.
 *
 * @see Junction for hydraulic node definitions
 * @see Turbine for the power-generation coupling
 * @see WaterwayLP for the LP formulation
 */
struct Waterway
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `WaterwayLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `Waterway::class_name` directly (or
  /// `WaterwayLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Waterway"};

  Uid uid {unknown_uid};  ///< Unique waterway identifier.
  Name name {};  ///< Human-readable name (used in LP row labels and CSV
                 ///< outputs).
  OptActive active {};  ///< Operational status (default: active when unset).
  OptName type {};  ///< Optional element type / category tag (free-text).
  OptName description {};  ///< Optional free-text description
                           ///< (e.g. conversion provenance from PLP / PLEXOS).

  /// Upstream junction reference (uid or name).  REQUIRED — the waterway
  /// debits this junction's water balance by ``-1.0 × flow`` per block.
  SingleId junction_a {unknown_uid};
  /// Downstream junction reference (uid or name) — OPTIONAL.  When set,
  /// the waterway credits this junction's balance with
  /// ``+(1 - lossfactor) × flow``.  When unset, the waterway acts as an
  /// **outflow**: it debits ``junction_a`` and the carried flow drains
  /// out of the modelled system (no downstream credit), so no synthetic
  /// ocean / sink junction is required.  Mirrors ``Turbine.junction_b``'s
  /// built-in waterway drain mode.
  OptSingleId junction_b {};

  /// Per-stage capacity ceiling [m³/s] applied to the flow column as a
  /// fallback when ``fmax`` is unset.  Accepts scalar, ``[stage]`` 1-D
  /// array, or a filename string referencing a Parquet/CSV schedule
  /// under ``input_directory/Waterway/``.
  OptTRealFieldSched capacity {};
  /// Transit loss coefficient [p.u., dimensionless, 0..1].  The
  /// downstream junction credit becomes ``+(1 - lossfactor) × flow``
  /// — models seepage / evaporation in transit.  Defaults to ``0.0``
  /// (lossless).
  OptTRealFieldSched lossfactor {0.0};

  /// Minimum required water flow [m³/s] (per-stage × per-block).
  /// The LP enforces ``flow ≥ fmin`` on every block.  Defaults to
  /// ``0.0``.  Accepts scalar, ``[stage][block]`` 2-D array, or a
  /// filename string referencing a Parquet/CSV schedule.
  OptTBRealFieldSched fmin {0.0};
  /// Optional penalty cost [$/(m³/s)/h] that turns the ``fmin`` floor into
  /// a SOFT constraint, mirroring ``Generator.pmin_fcost``.  When set (> 0)
  /// and ``fmin > 0``, the hard ``flow ≥ fmin`` is relaxed to
  /// ``flow + unserved ≥ fmin`` with a non-negative ``unserved`` slack
  /// priced at ``fmin_fcost`` — so a forced river / ecological flow (e.g.
  /// Caudal_Eco, a bypass minimum) can under-deliver at a penalty when the
  /// upstream reservoir is water-short, instead of rendering the LP
  /// infeasible.  Water still routes ``junction_a → junction_b``
  /// (non-consumptive); only the floor becomes soft.  Unset keeps ``fmin``
  /// hard.  Per-(stage, block) schedulable.
  OptTBRealFieldSched fmin_fcost {};
  /// Maximum allowed water flow [m³/s] (per-stage × per-block).  When
  /// unset the LP treats the column as having no upper bound (+∞),
  /// same semantics as ``fmax = DblMax`` after the flatten-time clamp.
  /// When set, ``fmax`` overrides ``capacity``.
  OptTBRealFieldSched fmax {};

  /// Per-flow cost [$/(m³/s)/h] charged on the ``waterway_flow``
  /// column — applied via ``CostHelper::block_ecost(...)`` so the LP
  /// pays ``fcost × flow × duration`` per block.  Used to model PLP
  /// ``qrb``-style spillway penalties on ``_ver`` arcs (rebalse cost
  /// from ``plpvrebemb.dat``).  Per-stage scheduling.
  OptTRealFieldSched fcost {};
};

}  // namespace gtopt
