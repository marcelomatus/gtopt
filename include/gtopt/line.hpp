/**
 * @file      line.hpp
 * @brief     Header for transmission line components
 * @date      Sun Apr 20 02:12:30 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing AC transmission lines
 * in power system planning models. In DC power-flow (Kirchhoff) mode the
 * reactance determines the flow split between parallel paths. The line also
 * supports capacity-expansion planning.
 *
 * ### JSON Example (transmission line)
 * ```json
 * {
 *   "uid": 1,
 *   "name": "l1_4",
 *   "bus_a": "b1",
 *   "bus_b": "b4",
 *   "reactance": 0.0576,
 *   "tmax_ab": 250,
 *   "tmax_ba": 250
 * }
 * ```
 *
 * ### JSON Example (phase-shifting transformer)
 * ```json
 * {
 *   "uid": 5,
 *   "name": "pst_1_2",
 *   "bus_a": 1,
 *   "bus_b": 2,
 *   "reactance": 0.05,
 *   "type": "transformer",
 *   "tap_ratio": 1.02,
 *   "phase_shift_deg": -5.0,
 *   "tmax_ab": 300,
 *   "tmax_ba": 300
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 2-D inline array indexed by `[stage][block]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Line/`
 */

#pragma once

#include <gtopt/capacity.hpp>
#include <gtopt/line_enums.hpp>

namespace gtopt
{

/**
 * @struct Line
 * @brief Represents a transmission line (branch) connecting two buses
 *
 * A line carries active power flow `f ∈ [-tmax_ba, tmax_ab]` between
 * `bus_a` and `bus_b`. In Kirchhoff mode the flow is constrained by
 * `f = V² / X · (θ_a − θ_b)` where V is the line voltage and X the
 * reactance.  When voltage is omitted (defaults to 1.0) both V and X
 * are in per-unit; when voltage is in kV, reactance must be in Ω.
 * Optional expansion variables allow the solver to invest in additional
 * transfer capacity.
 *
 * @see Bus for connected bus definitions
 * @see LineLP for the LP formulation
 */
struct Line
{
  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Line name
  OptActive active {};  ///< Activation status (default: active)
  OptName type {};  ///< Optional line type tag (e.g. "ac", "dc", "transformer")

  SingleId bus_a {unknown_uid};  ///< Sending-end (from) bus ID
  SingleId bus_b {unknown_uid};  ///< Receiving-end (to) bus ID

  OptTRealFieldSched voltage {};  ///< Nominal voltage level [kV].
                                  ///< Omit or set to 1.0 for per-unit mode.
  OptTRealFieldSched resistance {};  ///< Series resistance [Ω].
                                     ///< Use p.u. when voltage is omitted.
  OptTRealFieldSched reactance {};  ///< Series reactance for DC power flow [Ω].
                                    ///< Use p.u. when voltage is omitted
                                    ///< (default). Susceptance: B = V² / X.
  OptTRealFieldSched lossfactor {};  ///< Lumped loss factor [p.u.]
  OptBool use_line_losses {};  ///< @deprecated Use `line_losses_mode` instead.
                               ///< Kept for backward compatibility:
                               ///< `true` maps to the global default mode,
                               ///< `false` maps to `none`.
  OptName
      line_losses_mode {};  ///< Loss model selection (per-line override).
                            ///< See LineLossesMode for valid values:
                            ///< `"none"`, `"linear"`, `"piecewise"`,
                            ///< `"bidirectional"`, `"adaptive"`, `"dynamic"`.
                            ///< When unset, inherits from ModelOptions.
  OptInt loss_segments {};  ///< Number of piecewise-linear segments for
                            ///< quadratic losses (default: from Options)

  OptName loss_allocation_mode {};  ///< How losses are allocated between
                                    ///< sender and receiver buses:
                                    ///< `"receiver"` (default), `"sender"`,
                                    ///< or `"split"` (50/50, PLP default).

  /// Parse line_losses_mode string to enum (nullopt = inherit from global).
  [[nodiscard]] constexpr std::optional<LineLossesMode> line_losses_mode_enum()
      const noexcept
  {
    if (line_losses_mode.has_value()) {
      return enum_from_name<LineLossesMode>(*line_losses_mode);
    }
    return std::nullopt;
  }

  /// Parse loss_allocation_mode string to enum (receiver if unset).
  [[nodiscard]] constexpr LossAllocationMode loss_allocation_mode_enum()
      const noexcept
  {
    if (loss_allocation_mode.has_value()) {
      return enum_from_name<LossAllocationMode>(*loss_allocation_mode)
          .value_or(LossAllocationMode::receiver);
    }
    return LossAllocationMode::receiver;
  }

  OptTBRealFieldSched tmax_ba {};  ///< Maximum power flow in B→A direction [MW]
  OptTBRealFieldSched tmax_ab {};  ///< Maximum power flow in A→B direction [MW]
  OptTRealFieldSched tcost {};  ///< Variable transmission cost [$/MWh]

  OptTRealFieldSched capacity {};  ///< Installed transfer capacity [MW]
  OptTRealFieldSched expcap {};  ///< Capacity added per expansion module [MW]
  OptTRealFieldSched
      expmod {};  ///< Maximum number of expansion modules [dimensionless]
  OptTRealFieldSched
      capmax {};  ///< Absolute maximum capacity after expansion [MW]
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/MW-year]
  OptTRealFieldSched
      annual_derating {};  ///< Annual capacity derating factor [p.u./year]
  OptBool integer_expansion {};  ///< Integer-constrain the expansion modules

  /// Off-nominal tap ratio [p.u.].  For transformers only; ignored for
  /// plain lines.  When set to a value other than 1.0 the effective
  /// susceptance of the branch in the DC power-flow (Kirchhoff) constraint
  /// is scaled by `1/tap_ratio`, i.e. `B_eff = V²/(tap_ratio · X)`.
  /// Defaults to 1.0 (nominal tap, no correction).
  OptTRealFieldSched tap_ratio {};

  /// Phase-shift angle [degrees].  Models a phase-shifting transformer
  /// (PST) by adding a constant angle offset φ to the Kirchhoff equality
  /// constraint: `f = B_eff · (θ_a − θ_b − φ)`.  Positive values reduce
  /// the natural power flow from bus_a to bus_b.  Defaults to 0.0.
  OptTRealFieldSched phase_shift_deg {};
};

}  // namespace gtopt
