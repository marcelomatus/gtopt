/**
 * @file      battery.hpp
 * @brief     Header for battery energy storage components
 * @date      Wed Apr  2 01:40:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines data structures for representing battery energy storage
 * systems (BESS) in power system planning models. A battery stores and
 * releases electrical energy between time blocks, subject to capacity and
 * efficiency constraints. It is coupled to the electrical network through a
 * Converter that links it to a discharge Generator and a charge Demand.
 *
 * ### Standalone battery (unified definition)
 *
 * When the optional `bus` field is set (without `source_generator`), the
 * system automatically generates the associated Generator (discharge path),
 * Demand (charge path), and Converter during preprocessing — no separate
 * elements are needed.  Both charge and discharge connect to the same
 * external bus:
 *
 * ```json
 * {
 *   "uid": 1,
 *   "name": "bess1",
 *   "bus": 3,
 *   "input_efficiency": 0.95,
 *   "output_efficiency": 0.95,
 *   "emin": 0,
 *   "emax": 100,
 *   "capacity": 100,
 *   "pmax_charge": 60,
 *   "pmax_discharge": 60,
 *   "gcost": 0
 * }
 * ```
 *
 * ### Generation-coupled battery (hybrid / behind-the-meter)
 *
 * When both `bus` and `source_generator` are set, the battery is in
 * *generation-coupled* mode: the `source_generator` directly feeds the
 * battery charge path through an auto-created internal bus.
 * `System::expand_batteries()` will:
 *  - Create an internal bus (name = battery.name + "_int_bus")
 *  - Connect the discharge Generator to the external `bus`
 *  - Connect the charge Demand to the internal bus
 *  - Set the `source_generator`'s bus to the internal bus
 *
 * The `source_generator` should have no `bus` set, or its `bus` will be
 * overwritten with the internal bus.
 *
 * ```json
 * {
 *   "uid": 1,
 *   "name": "bess1",
 *   "bus": 3,
 *   "source_generator": "solar1",
 *   "input_efficiency": 0.95,
 *   "output_efficiency": 0.95,
 *   "emin": 0,
 *   "emax": 100,
 *   "capacity": 100,
 *   "pmax_charge": 60,
 *   "pmax_discharge": 60,
 *   "gcost": 0
 * }
 * ```
 *
 * ### Traditional multi-element definition
 *
 * Without the `bus` field, a separate Converter, Generator, and Demand must
 * be defined manually to couple the battery to the electrical network:
 *
 * ```json
 * {
 *   "uid": 1,
 *   "name": "bess1",
 *   "input_efficiency": 0.95,
 *   "output_efficiency": 0.95,
 *   "emin": 0,
 *   "emax": 100,
 *   "capacity": 100
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 1-D inline array indexed by `[stage]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Battery/`
 */

#pragma once

#include <gtopt/object.hpp>

namespace gtopt
{
/**
 * @struct Battery
 * @brief Represents a battery energy storage system (BESS)
 *
 * @details A battery can charge (absorb energy) and discharge (deliver
 * energy) within its operational constraints. The state of charge (SoC)
 * is tracked between time blocks, accounting for round-trip efficiency
 * losses.
 *
 * The energy balance per block is:
 *
 * ```text
 * SoC[t+1] = SoC[t] × (1 − annual_loss/8760) + input_efficiency × charge
 *            − discharge / output_efficiency
 * ```
 *
 * When the `bus` field is set, `System::expand_batteries()` auto-generates
 * the corresponding Generator, Demand, and Converter elements so the user
 * only needs to define a single Battery object.  This approach follows the
 * convention used by PyPSA `StorageUnit` and pandapower `storage`.
 *
 * When `source_generator` is also set, the battery operates in
 * *generation-coupled* mode: a dedicated internal bus is created for the
 * charge path so that the source generator feeds the battery directly.
 *
 * @see Converter for the generator/demand coupling
 * @see BatteryLP for the LP formulation
 */
struct Battery
{
  /// @name Default physical constants
  /// @{
  static constexpr Real default_energy_scale = 1.0;  ///< [dimensionless]
  /// @}

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable battery name
  OptActive active {};  ///< Activation status (default: active)
  OptName
      type {};  ///< Optional battery type tag (e.g. "li-ion", "flow", "pumped")

  /// External bus connection for the unified battery definition.
  /// When set, System::expand_batteries() auto-generates a discharge
  /// Generator, a charge Demand, and a linking Converter.
  OptSingleId bus {};

  /// Optional reference to a co-located generator that directly charges the
  /// battery (generation-coupled / hybrid battery configuration).
  /// When set together with `bus`, expand_batteries() creates an internal
  /// bus so that:
  ///  - The discharge Generator connects to the external `bus`
  ///  - The charge Demand connects to the internal bus
  ///  - The source generator's bus is set to the internal bus
  /// The source generator's own `bus` field is overwritten.
  /// Without `source_generator` the battery is standalone: both charge
  /// and discharge connect to the same external `bus`.
  OptSingleId source_generator {};

  OptTRealFieldSched input_efficiency {};  ///< Charging efficiency [p.u.]
  OptTRealFieldSched output_efficiency {};  ///< Discharging efficiency [p.u.]
  OptTRealFieldSched
      annual_loss {};  ///< Annual self-discharge rate [p.u./year]

  OptTRealFieldSched emin {};  ///< Minimum state of charge [MWh]
  OptTRealFieldSched
      emax {};  ///< Maximum state of charge (usable capacity) [MWh]
  OptTRealFieldSched
      ecost {};  ///< Storage usage cost (penalty for SoC) [$/MWh]
  OptReal eini {};  ///< Initial state of charge [MWh].  Sets an equality
                    ///< constraint SoC_start = eini in the first stage of the
                    ///< first phase only.
  OptReal efin {};  ///< Minimum required terminal state of charge [MWh].
                    ///< Sets a >= constraint SoC_end >= efin in the last stage
                    ///< of the last phase (not an equality).

  OptTRealFieldSched
      soft_emin {};  ///< Soft minimum SoC per stage [MWh].
                     ///< Creates a penalized constraint: efin + slack >=
                     ///< soft_emin.
                     ///< @see Reservoir::soft_emin for full documentation.
  OptTRealFieldSched soft_emin_cost {};  ///< Penalty cost per unit of soft_emin
                                         ///< violation [$/MWh].

  OptTRealFieldSched
      pmax_charge {};  ///< Max charging power [MW] (unified definition)
  OptTRealFieldSched
      pmax_discharge {};  ///< Max discharging power [MW] (unified definition)
  OptTRealFieldSched
      gcost {};  ///< Discharge generation cost [$/MWh] (unified definition)

  OptTRealFieldSched capacity {};  ///< Installed energy capacity [MWh]
  OptTRealFieldSched expcap {};  ///< Energy capacity per expansion module [MWh]
  OptTRealFieldSched
      expmod {};  ///< Maximum number of expansion modules [dimensionless]
  OptTRealFieldSched capmax {};  ///< Absolute maximum energy capacity [MWh]
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/MWh-year]
  OptTRealFieldSched
      annual_derating {};  ///< Annual capacity derating factor [p.u./year]

  /// Whether to propagate SoC state across stage/phase boundaries via
  /// StateVariables (SDDP-style coupling). When false (the default for
  /// batteries), each phase is solved independently with no state carry-over;
  /// an efin==eini constraint is added automatically to close each phase.
  OptBool use_state_variable {};

  /// Enable PLP-style daily cycle operation: block durations in the energy
  /// balance are scaled by 24/stage_duration so that emin/emax constraints
  /// represent a daily cycle regardless of the stage length. Implies
  /// decoupled stage/phase behaviour (use_state_variable forced false).
  /// Default for batteries is true (enabled); can be disabled explicitly.
  OptBool daily_cycle {};

  /// Energy scale factor: the LP energy variable is divided by this value so
  /// that the LP works in scaled units (physical_energy / energy_scale).
  /// Default is 1.0 (no scaling).  Output values are rescaled back to
  /// physical units so results are invariant to the choice of energy_scale.
  OptReal energy_scale {};
};

}  // namespace gtopt
