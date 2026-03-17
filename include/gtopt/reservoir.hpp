/**
 * @file      reservoir.hpp
 * @brief     Defines the Reservoir structure representing a water reservoir
 * @date      Wed Jul 30 23:11:55 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This file defines the `gtopt::Reservoir` structure, which models a water
 * reservoir within a hydro-thermal power system. A reservoir stores water
 * that drives turbines, subject to volume limits, spill constraints, and
 * evaporation losses.
 *
 * ### Unit conventions
 * - Volume fields (`emin`, `emax`, `eini`, `efin`, `capacity`): **dam³**
 *   (decacubic metres; 1 dam³ = 1 000 m³)
 * - Flow fields (`spillway_capacity`, `fmin`, `fmax`): **m³/s**
 * - The default `flow_conversion_rate = 0.0036` converts m³/s × h → dam³:
 *   `volume_dam3 = 0.0036 × flow_m3s × duration_h`
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "res1",
 *   "junction": "j1",
 *   "emin": 100,
 *   "emax": 5000,
 *   "eini": 2500
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 1-D inline array indexed by `[stage]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Reservoir/`
 */

#pragma once

#include <gtopt/field_sched.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @brief Water reservoir in a hydro cascade system
 *
 * The reservoir accumulates and releases water between time blocks. The
 * volume balance per block is:
 * ```
 * V[t+1] = V[t] × (1 − annual_loss/8760 × duration)
 *        + flow_conversion_rate × (inflows − outflows) × duration
 * ```
 * where inflows/outflows include waterway flows, turbine discharges, natural
 * inflows (Flow), and seepage (Filtration).
 *
 * @see Junction for the hydraulic node the reservoir is attached to
 * @see ReservoirLP for the LP formulation
 */
struct Reservoir
{
  /// @name Default physical constants
  /// @{
  static constexpr Real default_spillway_capacity = 6'000.0;  ///< [m³/s]
  static constexpr Real default_fmin = -10'000.0;  ///< [m³/s]
  static constexpr Real default_fmax = +10'000.0;  ///< [m³/s]
  static constexpr Real default_vol_scale = 1'000.0;  ///< [dimensionless]
  static constexpr Real default_flow_conversion_rate =
      0.0036;  ///< [dam³/(m³/s·h)]
  /// @}

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId junction {unknown_uid};  ///< ID of the associated hydraulic junction

  OptReal spillway_capacity {
      default_spillway_capacity};  ///< Maximum uncontrolled spill capacity
                                   ///< [m³/s]
  OptReal
      spillway_cost {};  ///< Penalty cost per unit of spilled water [$/dam³]

  OptTRealFieldSched capacity {};  ///< Total usable storage capacity [dam³]
  OptTRealFieldSched annual_loss {};  ///< Annual fractional evaporation/seepage
                                      ///< loss [p.u./year]
  OptTRealFieldSched emin {};  ///< Minimum allowed stored volume [dam³]
  OptTRealFieldSched emax {};  ///< Maximum allowed stored volume [dam³]
  OptTRealFieldSched
      ecost {};  ///< Shadow cost of stored water (water value) [$/dam³]
  OptReal eini {};  ///< Initial stored volume at start of horizon [dam³].
                    ///< Sets an equality constraint vol_start = eini in the
                    ///< first stage of the first phase only.
  OptReal efin {};  ///< Minimum required stored volume at end of horizon
                    ///< [dam³].  Sets a >= constraint vol_end >= efin in the
                    ///< last stage of the last phase (not an equality).

  OptReal fmin {
      default_fmin};  ///< Minimum net flow into the reservoir junction [m³/s]
  OptReal fmax {
      default_fmax};  ///< Maximum net flow into the reservoir junction [m³/s]

  OptReal vol_scale {
      default_vol_scale};  ///< Volume scale factor: LP variable =
                           ///< physical_volume / vol_scale [dimensionless]
  OptReal flow_conversion_rate {
      default_flow_conversion_rate};  ///< Converts m³/s × hours into dam³
                                      ///< [dam³/(m³/s·h)]

  /// Whether to propagate volume state across stage/phase boundaries via
  /// StateVariables (SDDP-style coupling). When true (the default for
  /// reservoirs), the final volume of one phase is carried over as the initial
  /// volume of the next. When false, an efin==eini constraint is added to close
  /// each phase independently.
  OptBool use_state_variable {};

  /// Enable PLP-style daily cycle operation (see Battery::daily_cycle).
  /// Default for reservoirs is false (disabled); can be enabled explicitly
  /// for small reservoirs that operate on a daily cycle.
  OptBool daily_cycle {};
};

}  // namespace gtopt
