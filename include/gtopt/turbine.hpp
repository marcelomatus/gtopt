/**
 * @file      turbine.hpp
 * @brief     Defines the Turbine structure representing a hydroelectric turbine
 * @date      Thu Jul 31 01:50:54 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Turbine structure which couples a hydraulic
 * waterway to an electrical generator, converting water flow [m³/s] into
 * electrical power [MW].
 *
 * ### Conversion relationship
 * ```text
 * power [MW] = efficiency [p.u.] × production_factor [MW·s/m³] × flow [m³/s]
 * ```
 *
 * The effective conversion rate is `efficiency × production_factor`.
 * If `efficiency` is not set, it defaults to 1.0.
 *
 * ### Variable production factor (hydraulic head)
 * When `main_reservoir` is set, the turbine's conversion rate can vary
 * with the reservoir volume (hydraulic head).  The piecewise-linear
 * production factor curve is provided by a matching `ReservoirProductionFactor`
 * element.  During SDDP forward iterations the conversion-rate LP
 * coefficient is updated based on the current reservoir volume.
 *
 * ### Connection modes
 *
 * A turbine connects to the water system via **one** of:
 * - `waterway` — traditional mode: reads water flow from a separate
 *   `Waterway` element's flow column in the junction balance LP.
 * - `flow` — simplified mode: reads discharge directly from a `Flow`
 *   element's fixed schedule.  No junctions or waterways needed.
 *   Useful for simple run-of-river (pasada) units.
 * - `junction_a` (+ optional `junction_b`) — **built-in waterway mode**:
 *   the turbine owns its own per-block flow column (`Turbine/flow`,
 *   units m³/s) that debits `junction_a`, credits `junction_b` (when
 *   set), and converts to power — replacing the separate penstock
 *   Waterway.  `junction_b` unset = terminal/drain (no synthetic ocean
 *   junction needed).  Mode priority: `flow` > `junctions` > `waterway`.
 *
 * When `flow` is set, `waterway` and `junction_a/b` are ignored.
 * When `junction_a` is set (without `flow`), the turbine owns its own
 * flow column and the (possibly-stale) `waterway` reference is ignored.
 * The turbine's power constraint becomes: `power = efficiency ×
 * production_factor × flow` (equality, or ≤ when `drain=true`).
 * Aperture updates automatically change the flow discharge in `flow`
 * mode, so the turbine power bound varies correctly across scenarios.
 *
 * ### JSON Example (waterway mode)
 * ```json
 * {
 *   "uid": 1,
 *   "name": "t1",
 *   "waterway": "w1_2",
 *   "generator": "g_hydro",
 *   "production_factor": 0.0025,
 *   "capacity": 100,
 *   "main_reservoir": "res1"
 * }
 * ```
 *
 * ### JSON Example (flow mode — pasada)
 * ```json
 * {
 *   "uid": 2,
 *   "name": "t_pasada",
 *   "flow": "f_river",
 *   "generator": "g_pasada",
 *   "production_factor": 1.0
 * }
 * ```
 *
 * ### JSON Example (built-in waterway, terminal drain)
 * ```json
 * {
 *   "uid": 3,
 *   "name": "t_terminal",
 *   "junction_a": "res_intake",
 *   "generator": "g_hydro_terminal",
 *   "production_factor": 2.0
 * }
 * ```
 * ``junction_b`` is omitted, so the turbined flow drains directly out of
 * the modelled system (no synthetic ocean / sink junction needed).
 *
 * ### JSON Example (built-in waterway, cascade)
 * ```json
 * {
 *   "uid": 4,
 *   "name": "t_cascade",
 *   "junction_a": "res_upstream",
 *   "junction_b": "res_downstream",
 *   "generator": "g_hydro_cascade",
 *   "production_factor": 2.0
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 1-D inline array indexed by `[stage]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Turbine/`
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/single_id.hpp>

namespace gtopt
{

/**
 * @brief Hydroelectric turbine converting water flow into electrical power
 *
 * A turbine draws water from a waterway, converts it at `efficiency ×
 * production_factor` into electrical power at the linked generator, and passes
 * the remaining flow to the downstream junction of the waterway.
 *
 * When `main_reservoir` is specified, the turbine's conversion rate may be
 * updated dynamically by the SDDP solver using the piecewise-linear
 * production factor curve from the corresponding `ReservoirProductionFactor`
 * element.
 *
 * @see Waterway for the water channel
 * @see Generator for the power output representation
 * @see ReservoirProductionFactor for the piecewise-linear production factor
 * @see TurbineLP for the LP formulation
 */
struct Turbine
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `TurbineLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `Turbine::class_name` directly (or
  /// `TurbineLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Turbine"};

  Uid uid {unknown_uid};  ///< Unique turbine identifier.
  Name name {};  ///< Human-readable name (used in LP row labels and CSV
                 ///< outputs).
  OptActive active {};  ///< Operational status (default: active when unset).
  OptName type {};  ///< Optional element type / category tag (free-text).
  OptName description {};  ///< Optional free-text description
                           ///< (e.g. conversion provenance from PLP / PLEXOS).

  /// Waterway uid or name to read the flow from (legacy "waterway"
  /// connection mode).  IGNORED when ``flow`` or ``junction_a`` is set
  /// — mode priority: ``flow`` > ``junctions`` > ``waterway``.  When
  /// active, the turbine's conversion row binds the generator's power
  /// to ``waterway.flow_cols`` (units m³/s).
  OptSingleId waterway {};
  /// Flow uid or name (alternative to ``waterway`` for pasada / run-of-
  /// river units).  When set, the turbine reads the fixed discharge
  /// directly from the ``Flow`` element's schedule [m³/s].  Aperture
  /// updates change the discharge so the turbine power bound varies
  /// across scenarios.  Highest priority of the three connection modes.
  OptSingleId flow {};
  /// Upstream (intake) junction reference (uid or name) — enables the
  /// built-in waterway mode when set.  The turbine then owns its own
  /// per-block flow column ``Turbine/flow`` (units m³/s) that debits
  /// this junction's water balance.  Mutually exclusive with ``flow``;
  /// takes priority over ``waterway`` when both are set.
  OptSingleId junction_a {};
  /// Downstream junction reference (uid or name) — OPTIONAL companion
  /// to ``junction_a`` in built-in waterway mode.  When set, the
  /// turbine credits this junction's balance with ``+1.0 × flow``
  /// (lossless penstock).  When unset, the turbined flow drains out
  /// of the modelled system (terminal / run-to-sea plants — no
  /// synthetic ocean junction needed).
  OptSingleId junction_b {};
  /// Generator uid or name that represents the turbine's electrical
  /// output (REQUIRED).  The conversion row binds
  /// ``generator.generation_cols`` to the chosen water-source flow via
  /// ``power = efficiency × production_factor × flow``.
  SingleId generator {unknown_uid};

  /// Spill flag.  When ``true``, the conversion row is relaxed to
  /// ``power ≤ efficiency × production_factor × flow`` (≤ instead of
  /// =), letting excess water bypass the turbine without generating
  /// power — used for terminal hydro plants with a controlled spillway.
  OptBool drain {};

  /// Water-to-power production factor [MW·s/m³ = MW / (m³/s)].
  /// Multiplied by ``efficiency`` to form the effective conversion
  /// rate in the LP.  **Per-(stage, block)** schedulable: accepts a
  /// scalar, a ``[stage]`` 1-D array, a ``[stage][block]`` 2-D matrix,
  /// or a filename string referencing a Parquet/CSV schedule under
  /// ``input_directory/Turbine/``.  Scalar / per-stage forms broadcast
  /// to every block (back-compat).  Per-block support lets a
  /// single-stage PLEXOS conversion carry head-dependent PF variation
  /// across the horizon instead of collapsing it to one value.
  OptTBRealFieldSched production_factor {};
  /// Turbine efficiency [p.u., dimensionless] (default 1.0).
  /// Effective conversion rate is ``efficiency × production_factor``.
  /// Accepts the same value forms as ``production_factor``.
  OptTRealFieldSched efficiency {};
  /// Maximum turbine flow [m³/s] — caps the per-block flow column
  /// (``waterway.flow``, ``flow.discharge``, or built-in
  /// ``Turbine/flow``) via a per-block ``≤ capacity`` row.  Note this
  /// is a FLOW bound, not a power bound: the corresponding power cap
  /// is ``capacity × efficiency × production_factor`` [MW].
  /// **Per-(stage, block)** schedulable — accepts the same value forms
  /// as ``production_factor`` (scalar / per-stage broadcast to every
  /// block for back-compat).
  OptTBRealFieldSched capacity {};

  /// Optional reservoir uid or name whose volume drives the turbine's
  /// conversion rate (hydraulic-head effects).  When set, the SDDP
  /// solver updates the conversion-rate LP coefficient at each
  /// forward-pass iteration using the current reservoir volume and
  /// the matching ``ReservoirProductionFactor`` element's piecewise-
  /// linear curve.  The matching element must reference this turbine's
  /// UID in its ``turbine`` field and the reservoir's UID in its
  /// ``reservoir`` field.
  OptSingleId main_reservoir {};
};

}  // namespace gtopt
