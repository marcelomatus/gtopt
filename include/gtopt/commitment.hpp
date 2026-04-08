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
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId generator {unknown_uid};  ///< FK to the Generator

  OptTRealFieldSched startup_cost {};  ///< Startup cost [$/start]
  OptTRealFieldSched shutdown_cost {};  ///< Shutdown cost [$/stop]
  OptReal noload_cost {};  ///< No-load cost when committed [$/hr]

  OptReal min_up_time {};  ///< Minimum up time [hours]
  OptReal min_down_time {};  ///< Minimum down time [hours]

  OptReal ramp_up {};  ///< Ramp-up limit [MW/hr] while online
  OptReal ramp_down {};  ///< Ramp-down limit [MW/hr] while online
  OptReal startup_ramp {};  ///< Max output in startup block [MW]
  OptReal shutdown_ramp {};  ///< Max output in shutdown block [MW]

  OptReal initial_status {};  ///< Initial on/off (1.0 = online, 0.0 = offline)
  OptReal initial_hours {};  ///< Hours in current state at t=0

  OptBool relax {};  ///< LP relaxation: u/v/w continuous in [0,1]
  OptBool must_run {};  ///< Force committed: u = 1 always

  /// @name Piecewise heat rate curve
  /// When both arrays are present, the generation range [Pmin, Pmax] is
  /// decomposed into K segments with individual heat rates.
  /// `pmax_segments` = [P̄₁, ..., P̄ₖ] cumulative power breakpoints [MW].
  /// `heat_rate_segments` = [h₁, ..., hₖ] heat rate per segment [GJ/MWh].
  /// Segment k covers [P̄_{k-1}, P̄ₖ] where P̄₀ = Pmin.
  /// The effective generation cost per segment is `fuel_cost × h_k`.
  /// The effective emission per segment is `fuel_emission_factor × h_k`.
  /// @{
  Array<Real> pmax_segments {};
  Array<Real> heat_rate_segments {};
  OptTRealFieldSched fuel_cost {};  ///< Fuel cost [$/GJ], stage-schedulable
  OptTRealFieldSched fuel_emission_factor {};  ///< Emission factor [tCO2/GJ]
  /// @}

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
