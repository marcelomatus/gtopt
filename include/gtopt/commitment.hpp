/**
 * @file      commitment.hpp
 * @brief     Unit commitment data for generator on/off scheduling
 * @date      Tue Apr  8 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * Defines the Commitment structure which specifies unit commitment parameters
 * for a generator: startup/shutdown costs, no-load cost, minimum up/down
 * times, and initial conditions.  Each Commitment entry links to exactly one
 * Generator via a foreign key.
 *
 * Commitment constraints are only enforced on stages marked as chronological.
 * On non-chronological stages (e.g. duration-weighted representative blocks)
 * the commitment is silently skipped and the generator dispatches normally.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "thermal1_uc",
 *   "generator": "thermal1",
 *   "startup_cost": 5000,
 *   "shutdown_cost": 1000,
 *   "noload_cost": 50,
 *   "min_up_time": 4,
 *   "min_down_time": 2,
 *   "initial_status": 1,
 *   "initial_hours": 8
 * }
 * ```
 *
 * @see CommitmentLP for the LP formulation (three-bin u/v/w)
 * @see Generator for the linked generation unit
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct Commitment
 * @brief Unit commitment parameters for a generator
 *
 * Links to a Generator and defines the three-bin UC model parameters:
 * - u (status): binary, 1 = online
 * - v (startup): binary, 1 = started up this block
 * - w (shutdown): binary, 1 = shut down this block
 *
 * Constraints:
 * - C1 (logic): u[t] - u[t-1] = v[t] - w[t]
 * - C2 (gen limits): Pmin*u <= p <= Pmax*u
 * - C3 (exclusion): v[t] + w[t] <= 1
 */
struct Commitment
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `CommitmentLP` exposes no separate `ClassName` member; callers
  /// reach the constant via `Commitment::class_name` directly (or
  /// `CommitmentLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Commitment"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)
  OptName type {};  ///< Optional element type/category tag
  OptName description {};  ///< Optional free-text description (e.g. conversion
                           ///< provenance)

  SingleId generator {unknown_uid};  ///< FK to the Generator

  // ``fuel`` / ``pmax_segments`` / ``heat_rate_segments`` / ``fuel_cost``
  // / ``fuel_emission_factor`` were removed from Commitment on
  // 2026-05-20 — those fields are dispatch-cost properties of the
  // *Generator*, not of the commitment binary.  Use
  // ``Generator.fuel`` / ``Generator.pmax_segments`` /
  // ``Generator.heat_rate_segments`` instead; ``GeneratorLP`` builds
  // the piecewise cost as a pure-LP convex-slack formulation that
  // works with or without a Commitment binary (see
  // ``source/generator_lp.cpp:272+``).

  OptTRealFieldSched startup_cost {};  ///< Startup cost [$/start]
  OptTRealFieldSched shutdown_cost {};  ///< Shutdown cost [$/stop]
  OptReal noload_cost {};  ///< No-load cost when committed [$/hr]

  /// Minimum stable level when committed [MW].  Distinct from
  /// ``Generator.pmin`` (which is the *always-on* hard floor that
  /// applies regardless of commitment).  When this field is set,
  /// ``commitment_lp.cpp`` uses it as the per-unit minimum
  /// (``gen ≥ pmin × u_commit``) and leaves ``Generator.pmin`` alone
  /// as the unconditional dispatch floor.  Defaults to falling back
  /// on ``lp.get_col_lowb(gcol)`` (legacy behaviour) when unset.
  /// PLEXOS analogue: ``Generator.Min Stable Level`` is
  /// commitment-conditional per the official Energy Exemplar docs,
  /// so the plexos2gtopt converter writes Min Stable Level here
  /// (not into Generator.pmin) and keeps Generator.pmin = 0.
  ///
  /// Per-(stage, block) schedule: PLEXOS ``Min Stable Level`` is a
  /// time series (e.g. CEN PCP coal units carry 98.53 MW for most of
  /// the week and 170.53 MW for a few hours), so this is a
  /// ``FieldSched`` accepting either a scalar (constant floor, the
  /// common case) or a per-block vector.  ``commitment_lp.cpp``
  /// resolves it block-by-block.
  OptTBRealFieldSched pmin {};

  OptReal min_up_time {};  ///< Minimum up time [hours]
  OptReal min_down_time {};  ///< Minimum down time [hours]

  OptReal ramp_up {};  ///< Ramp-up limit [MW/hr] while online
  OptReal ramp_down {};  ///< Ramp-down limit [MW/hr] while online
  OptReal startup_ramp {};  ///< Max output in startup block [MW]
  OptReal shutdown_ramp {};  ///< Max output in shutdown block [MW]

  OptReal initial_status {};  ///< Initial on/off (1.0 = online, 0.0 = offline)
  OptReal initial_hours {};  ///< Hours in current state at t=0
  /// Initial power output [MW] at ``t = -1``.  When set, the
  /// first-block ramp-up / ramp-down rows include the
  /// ``p_prev = initial_power`` term:
  ///
  ///   p[0] − initial_power ≤ RU·u_init + SU·(1 − u_init)
  ///   initial_power − p[0] ≤ RD·u[0] + SD·w[0]
  ///
  /// instead of the looser ``p_prev = 0`` form that the previous
  /// "for simplicity" stub used (commit ID predates this field).
  ///
  /// When ``std::nullopt`` (default), the legacy ``p_prev = 0``
  /// behaviour is preserved — correct for cold starts and any
  /// generator where the converter / user didn't supply a value.
  ///
  /// Required to round-trip UC.jl's ``Initial power (MW)`` on
  /// hot-start generators with ``pmin > 0`` (e.g. RTS-GMLC
  /// ``216_STEAM_1``: pmin = 62, ramp_up = 60, initial_power = 62 —
  /// without this field the first-block LP is infeasible because
  /// the pmin floor of 62 collides with the ramp-up cap of 60).
  OptReal initial_power {};

  OptBool relax {};  ///< LP relaxation: u/v/w continuous in [0,1]
  OptBool must_run {};  ///< Force committed: u = 1 always

  /// Per-(stage, block) forced commitment status — pins the ``u`` variable
  /// to a specific value at each (stage, block) where it is set.  Values
  /// are interpreted as ``1.0 → committed``, ``0.0 → not committed``;
  /// blocks where the schedule has no entry leave ``u`` free.  Generalises
  /// the all-or-nothing ``must_run`` flag to schedules like UC.jl's
  /// per-block ``Commitment status: [true, false, true, ...]`` field
  /// (case14/fixed.json), and is also useful for hot-start / scenario-
  /// tracing replay where a previous solve's commitment is pinned.
  /// Overrides ``must_run`` per block — if both are set, the per-block
  /// ``fixed_status`` value wins for the blocks it covers and
  /// ``must_run`` covers the remainder.
  OptTBRealFieldSched fixed_status {};

  /// Binary variable period [hours].  When set, u/v/w variables are created
  /// at a coarser time resolution than the generation blocks.  For example,
  /// `commitment_period = 2.0` with 15-minute blocks yields one binary
  /// variable per 2-hour window (8 blocks), while generation variables
  /// remain at 15-minute resolution.  Default (nullopt) = one per block.
  OptReal commitment_period {};

  // Piecewise heat-rate curve (``pmax_segments`` /
  // ``heat_rate_segments``) and the legacy ``fuel_cost`` /
  // ``fuel_emission_factor`` schedules were removed from Commitment
  // on 2026-05-20.  They are dispatch-cost properties belonging on
  // ``Generator``; see the comment near ``generator`` above for
  // migration guidance.

  /// @name Startup cost tiers (hot/warm/cold)
  /// When all five fields are present, the single startup_cost is replaced
  /// by three tier-dependent costs based on offline duration.
  /// hot_start_time < cold_start_time; offline < hot → hot cost, etc.
  /// @{
  OptReal hot_start_cost {};  ///< Startup cost when recently offline [$/start]
  OptReal warm_start_cost {};  ///< Startup cost at medium offline [$/start]
  OptReal cold_start_cost {};  ///< Startup cost when long offline [$/start]
  OptReal hot_start_time {};  ///< Max offline hours for hot start [h]
  OptReal cold_start_time {};  ///< Min offline hours for cold start [h]
  /// @}
};

}  // namespace gtopt
