/**
 * @file      converter.hpp
 * @brief     Defines the Converter structure for battery-generator coupling
 * @date      Wed Apr  2 01:40:47 2025
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * This module defines the Converter structure which couples a Battery to a
 * Generator (discharge path) and a Demand (charge path), enabling energy
 * storage dispatch modeling in the LP formulation.
 *
 * ### JSON Example
 * ```json
 * {
 *   "uid": 1,
 *   "name": "conv1",
 *   "battery": "bess1",
 *   "generator": "g_discharge",
 *   "demand": "d_charge",
 *   "conversion_rate": 1.0
 * }
 * ```
 *
 * Fields that accept a `number/array/string` value can hold:
 * - A scalar constant
 * - A 1-D inline array indexed by `[stage]`
 * - A filename string referencing a Parquet/CSV schedule in
 *   `input_directory/Converter/`
 */

#pragma once

#include <gtopt/demand.hpp>
#include <gtopt/generator.hpp>
#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct Converter
 * @brief Couples a battery to a generator (discharge) and a demand (charge)
 *
 * A converter links:
 * - A @ref Battery as the energy source/sink
 * - A @ref Generator for the discharge (generation) path
 * - A @ref Demand for the charge (load) path
 *
 * The `conversion_rate` scales the electrical output (MW) relative to the
 * rate at which stored energy (MWh/h) is withdrawn from the battery.
 *
 * @see Battery for energy storage parameters
 * @see ConverterLP for the LP formulation
 */
struct Converter
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `ConverterLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `Converter::class_name` directly (or
  /// `ConverterLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Converter"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status (default: active)

  SingleId battery {unknown_uid};  ///< ID of the linked battery
  SingleId generator {unknown_uid};  ///< ID of the discharge generator
  SingleId demand {unknown_uid};  ///< ID of the charge demand

  OptTRealFieldSched conversion_rate {};  ///< Electrical output per unit stored
                                          ///< energy withdrawn [dimensionless]
  ///< (ratio of generator_output [MW] to battery_discharge_rate
  ///< [MW-equivalent]); simplifies to dimensionless since MW/(MWh/h) = 1;
  ///< default = 1.0

  OptTRealFieldSched capacity {};  ///< Installed power capacity [MW]
  OptTRealFieldSched expcap {};  ///< Power capacity per expansion module [MW]
  OptTRealFieldSched
      expmod {};  ///< Maximum number of expansion modules [dimensionless]
  OptTRealFieldSched capmax {};  ///< Absolute maximum power capacity [MW]
  OptTRealFieldSched
      annual_capcost {};  ///< Annualized investment cost [$/MW-year]
  OptTRealFieldSched
      annual_derating {};  ///< Annual capacity derating factor [p.u./year]
  OptBool integer_expmod {};  ///< Integer-constrain the expmod variable

  /// Gate the inner ``Demand.{lmin,lmax}`` and ``Generator.{pmin,pmax}``
  /// bounds with per-block INTEGER commitment binaries.  When set,
  /// the LP adds two integer columns per block — ``u_charge``,
  /// ``u_discharge``, each in ``{0, 1}`` — plus C2-style rows:
  ///
  ///     load    ≥ lmin × u_charge       load    ≤ lmax × u_charge
  ///     gen     ≥ pmin × u_discharge    gen     ≤ pmax × u_discharge
  ///
  /// and resets the static col floors on ``load`` / ``gen`` to 0 so
  /// the bounds only fire when the corresponding binary is 1.
  /// Mirrors UC.jl's conditional ``Minimum charge/discharge rate
  /// (MW)`` semantics.  Always integer (commitment is a MIP feature
  /// by convention — for LP-only solves leave ``commitment`` unset
  /// and accept hard static floors).  Set by
  /// ``System::expand_batteries()`` when ``Battery.commitment`` is
  /// enabled.
  OptBool commitment {};
};

}  // namespace gtopt
