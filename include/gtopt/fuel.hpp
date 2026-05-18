/**
 * @file      fuel.hpp
 * @brief     Defines the Fuel structure (price + heat content + emission
 * factors)
 * @date      2026-05-16
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * A `Fuel` is a passive data carrier that bundles the time-schedulable
 * price, heat content and emission factors of a primary energy source
 * (natural gas, diesel, coal, …).  Generators that reference a Fuel
 * via `Generator.fuel` together with a `heat_rate` derive:
 *
 *   effective_gcost = fuel.price × heat_rate + Generator.gcost
 *   effective_ef    = (fuel.combustion_emission_factor
 *                      + fuel.upstream_emission_factor) × heat_rate
 *                     + Generator.emission_factor
 *
 * ## Unit-flexibility contract (PLEXOS / SDDP style)
 *
 * All `Fuel` quantities can be expressed in ANY consistent fuel unit
 * (tonne, MMBtu, Nm³, GJ).  The LP math is invariant under the choice
 * of unit because every fuel-rate quantity multiplies through the
 * generator dispatch in a balanced way — the units cancel as long as
 * the user keeps them consistent across `price`, `heat_rate`,
 * `combustion_emission_factor`, `upstream_emission_factor` for a
 * single fuel.  Example consistent triples:
 *
 *   $/tonne · tCO₂/tonne · tonne/MWh  →  $/MWh, tCO₂/MWh
 *   $/MMBtu · tCO₂/MMBtu · MMBtu/MWh  →  $/MWh, tCO₂/MWh
 *   $/GJ    · tCO₂/GJ    · GJ/MWh     →  $/MWh, tCO₂/MWh
 *
 * `heat_content` (energy per physical fuel unit, e.g. GJ/tonne or
 * GJ/Nm³) is OPTIONAL and used ONLY for output reporting — it lets
 * the solver emit both physical (`fuel_consumption_physical.parquet`)
 * and energy (`fuel_consumption_energy.parquet`) views of fuel burn
 * regardless of which unit the user picked for the LP coefficients.
 * When `heat_content` is unset, only the physical-unit consumption is
 * emitted.  Mirrors `PLEXOS.Fuel.Heat Content` and SDDP's
 * `Combustível.Poder Calorífico`.
 *
 * ## Emission-factor naming
 *
 * Two stage-schedulable factors follow the IPCC / IEA / EPA split:
 *
 *   combustion_emission_factor
 *       CO₂ released at the burner per unit of fuel.  Synonyms:
 *       PLEXOS "Emission Production Rate", SDDP "Coeficiente de
 *       Emissão", IPCC "combustion emission factor", "stack",
 *       "direct", "tank-to-stack".
 *
 *   upstream_emission_factor
 *       CO₂ released producing and transporting the fuel per unit.
 *       Synonyms: "well-to-tank" / WTT, "pre-combustion",
 *       "fuel-cycle".  Neither PLEXOS nor SDDP carry this natively;
 *       gtopt adds it for lifecycle (well-to-burner-tip) accounting.
 *
 * Total = combustion + upstream ≡ "well-to-burner-tip" / lifecycle.
 *
 * @see Generator.fuel / Generator.heat_rate{,_segments}
 * @see FuelLP for the LP-side wrapper
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct Fuel
 * @brief Time-schedulable fuel price, heat content, and emission factors
 *
 * Referenced by `Generator.fuel`.  Every field is stage-schedulable
 * (scalar / per-stage vector / file).
 */
struct Fuel
{
  /// Canonical class-name constant used in LP row labels and config
  /// fields like `VariableScale::class_name`.  Single source of truth —
  /// `FuelLP` exposes no separate `ClassName` member; callers reach the
  /// constant via `Fuel::class_name` directly (or
  /// `FuelLP::Element::class_name` in generic contexts).
  static constexpr LPClassName class_name {"Fuel"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name
  OptActive active {};  ///< Activation status

  /// Fuel price `[$/<fuel_unit>]`, stage-schedulable.  The fuel-unit is
  /// the user's choice (tonne, MMBtu, Nm³, GJ, …) and must match
  /// `heat_rate`, `combustion_emission_factor`,
  /// `upstream_emission_factor`, and `heat_content` for this fuel.
  OptTRealFieldSched price {};

  /// Heat content `[GJ/<fuel_unit>]`, stage-schedulable.  Optional —
  /// when set, enables physical/energy fuel-consumption reporting in
  /// the output parquets.  When unset, only physical-unit
  /// consumption is emitted.  Mirrors PLEXOS `Fuel.Heat Content` and
  /// SDDP `Poder Calorífico`.  Recommended: set explicitly for any
  /// fuel whose price/EF are NOT already in GJ-basis so downstream
  /// reporting can convert correctly.
  OptTRealFieldSched heat_content {};

  /// Combustion ("stack" / direct / tank-to-stack) emission factor
  /// `[tCO₂/<fuel_unit>]`, stage-schedulable.
  OptTRealFieldSched combustion_emission_factor {};

  /// Upstream ("well-to-tank" / pre-combustion / fuel-cycle) emission
  /// factor `[tCO₂/<fuel_unit>]`, stage-schedulable.
  OptTRealFieldSched upstream_emission_factor {};
};

}  // namespace gtopt
