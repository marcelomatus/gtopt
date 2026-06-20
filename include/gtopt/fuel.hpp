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
 *                     + Generator.emission_rate
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
 * @struct FuelEmissionFactor
 * @brief Per-pollutant emission factors of a `Fuel` (tons / fuel-unit).
 *
 * A single fuel emits multiple pollutants — CO₂, SO₂, NOₓ, CH₄, …
 * Each pollutant has its own combustion (tank-to-stack) and optional
 * upstream (well-to-tank) factor in the same per-fuel-unit basis that
 * `Fuel.price` and `Fuel.heat_content` use.  Naming follows IPCC
 * AR6 / EPA AP-42 / GHG Protocol terminology.
 *
 * When a `Generator.fuel` reference + a non-null `Generator.heat_rate`
 * are both set, the fuel-derived per-MWh emission rate becomes
 *   `heat_rate · combustion`  [t/MWh] — combustion path
 *   `heat_rate · upstream`    [t/MWh] — upstream path (optional)
 * and is added to the matching `EmissionSource`'s `rate` /
 * `upstream_rate` at LP-build time (additive, NOT replacing).
 */
struct FuelEmissionFactor
{
  /// FK to the `Emission` pollutant this factor describes.
  SingleId emission {unknown_uid};

  /// Combustion / tank-to-stack factor `[t/<fuel_unit>]`,
  /// stage-schedulable.  The amount released at the burner per unit
  /// of fuel.  PLEXOS calls this `Emission Production Rate`; SDDP
  /// `Coeficiente de Emissão`; IPCC `combustion emission factor`.
  OptTRealFieldSched combustion {};

  /// Upstream / well-to-tank factor `[t/<fuel_unit>]`,
  /// stage-schedulable.  Emissions released producing + transporting
  /// the fuel.  Optional — set for full lifecycle (well-to-burner-tip)
  /// accounting; leave unset for stack-only (Scope 1) accounting.
  OptTRealFieldSched upstream {};
};

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
  OptName type {};  ///< Optional element type/category tag
  /// Optional fine-grained sub-classification of `type` — e.g.
  /// `type="gas"` paired with `subtype="lng"`, `type="carbon"` with
  /// `subtype="sub_bituminous"`.  Carries the converter-side
  /// `Fuel.subtype` hint emitted by
  /// `plexos2gtopt.parsers._fuel_subtype_from_name` and consumed by
  /// the IPCC-emission-defaults overlay in
  /// `share/gtopt/emissions/cen_chile.json` (CEN-Chile sub-bituminous
  /// coal, LNG upstream chain).  Metadata only — not consumed by the
  /// LP build; round-trips through the JSON for downstream reporting
  /// and auditing.
  OptName subtype {};
  OptName description {};  ///< Optional free-text description (e.g. conversion
                           ///< provenance)

  /// Fuel price `[$/<fuel_unit>]`, **per-(stage, block)** schedulable.
  /// The fuel-unit is the user's choice (tonne, MMBtu, Nm³, GJ, …) and
  /// must match `heat_rate`, `combustion_emission_factor`,
  /// `upstream_emission_factor`, and `heat_content` for this fuel.
  ///
  /// Accepts the same value forms as `Generator.pmax`: a scalar
  /// constant, a `[stage]` 1-D array, a `[stage][block]` 2-D matrix, or
  /// a filename string referencing a Parquet/CSV schedule under
  /// `input_directory/Fuel/`.  Scalar / per-stage values broadcast to
  /// every block (back-compat with the legacy per-stage `price`).  This
  /// lets a single-stage PLEXOS conversion carry the per-period
  /// Fuel_Price profile across the horizon instead of collapsing it to
  /// one value.
  OptTBRealFieldSched price {};

  /// Heat content `[GJ/<fuel_unit>]`, stage-schedulable.  Optional —
  /// when set, enables physical/energy fuel-consumption reporting in
  /// the output parquets.  When unset, only physical-unit
  /// consumption is emitted.  Mirrors PLEXOS `Fuel.Heat Content` and
  /// SDDP `Poder Calorífico`.  Recommended: set explicitly for any
  /// fuel whose price/EF are NOT already in GJ-basis so downstream
  /// reporting can convert correctly.
  OptTRealFieldSched heat_content {};

  /// **Legacy single-pollutant** combustion CO₂ factor
  /// `[tCO₂/<fuel_unit>]`.  Auto-folded by
  /// `System::fold_legacy_fuel_emission_factors()` into
  /// `emission_factors[]` (matching emission "co2") at parse time
  /// and cleared.  Prefer the new `emission_factors[]` for new
  /// configs — it supports multi-pollutant.
  OptTRealFieldSched combustion_emission_factor {};

  /// **Legacy single-pollutant** upstream CO₂ factor — same fold path
  /// as `combustion_emission_factor`.
  OptTRealFieldSched upstream_emission_factor {};

  /// Multi-pollutant per-fuel emission factors.  One row per
  /// `Emission` kind, with optional combustion + upstream factors.
  /// Combines with `Generator.heat_rate` at LP-build to inject
  /// per-block coefficients into the matching pollutant's
  /// `EmissionZone/balance` row.
  Array<FuelEmissionFactor> emission_factors {};

  /// Maximum fuel consumption per stage `[<fuel_unit>/stage]`,
  /// stage-schedulable.  When set, FuelLP creates a per-(scenario,
  /// stage) constraint row enforcing
  ///
  ///   Σ_{g : Generator(g).fuel = this_fuel} (heat_rate_g(s, b) ·
  ///       generation_g(s, t, b) · duration_b)  ≤  max_offtake(s)
  ///
  /// summed across every active generator referencing this Fuel and
  /// every block in the stage.  Mirrors PLEXOS's
  /// `FueMaxOffWeek_<fuel>` / `FueMaxOffDay_<fuel>` Constraint
  /// pattern, where the natural stage period (week / day) bounds the
  /// total fuel offtake from a single contract band.
  ///
  /// Stage = the natural fuel-contract period (week in CEN PCP
  /// daily, month in long-term cases).  Unit-flexibility contract
  /// applies: `<fuel_unit>` must match `price`, `heat_rate`, etc.
  /// for this fuel.
  OptTRealFieldSched max_offtake {};

  /// Per-unit penalty for exceeding `max_offtake`
  /// `[$/<fuel_unit>]`, stage-schedulable.  When set and > 0 the cap
  /// becomes SOFT: a non-negative slack column with this cost is
  /// added to the cap row so the LP can over-consume at a per-unit
  /// price rather than going infeasible.  When unset, the cap is
  /// HARD.  Mirrors the `Reservoir.efin_cost` /
  /// `EmissionZone.cap_cost` convention.
  OptTRealFieldSched max_offtake_cost {};

  /// When `true`, enforce the offtake cap **per block** instead of
  /// summed over the stage.  The per-stage cap `max_offtake` is
  /// pro-rated by each block's share of stage duration:
  ///
  ///   block_cap_b = max_offtake(s) · duration_b / Σ_b duration_b
  ///
  /// and `FuelLP` creates one row per (scenario, stage, block):
  ///
  ///   Σ_g (heat_rate_g(s, b) · gen_g(s, t, b) · duration_b) ≤ block_cap_b
  ///
  /// Mirrors PLEXOS's per-period `FueMaxOffWeek_<fuel>` semantics:
  /// the weekly cap is enforced as a per-hour rate distributed
  /// uniformly across the week's hours, forcing the LP to spread
  /// offtake rather than front-loading it.  This is TIGHTER than
  /// the default per-stage sum — the per-stage sum would let the LP
  /// pile cheap-fuel dispatch into a few hours, the per-block view
  /// would not.
  ///
  /// When `false` / unset (default), the legacy per-stage sum
  /// behaviour applies: a single row per (scenario, stage) with the
  /// full `max_offtake(s)` on the right-hand side.  Soft-cap slack
  /// (`max_offtake_cost`) is supported in both modes — for per-block
  /// the slack is shared across the per-block rows via a single
  /// stage-scoped column.
  OptBool max_offtake_per_block {};

  /// Minimum fuel consumption per stage `[<fuel_unit>/stage]`,
  /// stage-schedulable.  When set, FuelLP creates a per-(scenario,
  /// stage) lower-bound row enforcing
  ///
  ///   Σ_{g : Generator(g).fuel = this_fuel} (heat_rate_g(s, b) ·
  ///       generation_g(s, t, b) · duration_b)  ≥  min_offtake(s)
  ///
  /// summed across every active generator referencing this Fuel and
  /// every block in the stage.  Mirrors PLEXOS's
  /// `Fuel.Min Offtake {Hour, Day, Week, Month, Year}` family
  /// (property IDs 595-600), used to model take-or-pay (ToP)
  /// contracts where the buyer commits to a minimum purchase
  /// quantity per period and pays a penalty for shortfall.  Mirrors
  /// PSR SDDP's per-period TOP minima (without the SDDP make-up bank
  /// — that is a future state-column extension).
  ///
  /// Canonical reference: Guerra, O.J. et al. "Analysis of a
  /// Multiple Year Gas Sales Agreement with Make-up, Carry-Forward
  /// and Indexation", Energy Economics 73 (2018), 358-374
  /// (DOI 10.1016/j.eneco.2018.03.013).
  ///
  /// Stage = the natural fuel-contract period.  Unit-flexibility
  /// contract applies: `<fuel_unit>` must match `price`, `heat_rate`,
  /// etc. for this fuel.  When both `min_offtake` and `max_offtake`
  /// are set on the same fuel, FuelLP emits two separate rows that
  /// share the per-block offtake DV (Y_f[b]) — same pattern as
  /// `Commitment.{min,max}_starts`.
  OptTRealFieldSched min_offtake {};

  /// Per-unit penalty for SHORTFALL below `min_offtake`
  /// `[$/<fuel_unit>]`, stage-schedulable.  When set and > 0 the
  /// floor becomes SOFT: a non-negative slack column with this cost
  /// is added to the floor row so the LP can under-consume at a
  /// per-unit price rather than going infeasible.  When unset, the
  /// floor is HARD (gtopt-native convention).
  ///
  /// PLEXOS asymmetry note: PLEXOS's `Fuel.Min Offtake Penalty`
  /// (property 602) defaults to **1000 $/fuel-unit** (NOT unset/hard
  /// like `Max Offtake Penalty`).  gtopt keeps the symmetric
  /// "unset = hard" convention for both directions; the converter
  /// `plexos2gtopt` is responsible for injecting `min_offtake_cost
  /// = 1000` when a PLEXOS bundle ships a Min Offtake without an
  /// explicit penalty, so PLEXOS-faithful behaviour is preserved at
  /// the conversion boundary rather than baked into the LP model.
  OptTRealFieldSched min_offtake_cost {};

  /// When `true`, enforce the floor **per block** instead of summed
  /// over the stage.  The per-stage floor `min_offtake` is pro-rated
  /// by each block's share of stage duration:
  ///
  ///   block_floor_b = min_offtake(s) · duration_b / Σ_b duration_b
  ///
  /// and `FuelLP` creates one lower-bound row per
  /// (scenario, stage, block):
  ///
  ///   Σ_g (heat_rate_g(s, b) · gen_g(s, t, b) · duration_b)
  ///       ≥ block_floor_b
  ///
  /// When `false` / unset (default), the per-stage sum applies (one
  /// row per scenario × stage).  Soft-floor slack (`min_offtake_cost`)
  /// is supported in both modes.
  ///
  /// `min_offtake_per_block` is independent of `max_offtake_per_block`:
  /// e.g., a fuel may have a per-block min (hourly take-or-pay floor)
  /// alongside a per-stage sum max (weekly delivery cap).
  OptBool min_offtake_per_block {};
};

}  // namespace gtopt
