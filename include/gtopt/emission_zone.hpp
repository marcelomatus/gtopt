/**
 * @file      emission_zone.hpp
 * @brief     Pollutant constraint zone — balance row, optional cap / price
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `EmissionZone` is the LP-active constraint owner for one or more
 * pollutants within a regulatory / geographic scope.  Mirrors the
 * `InertiaZone` / `ReserveZone` pattern: the zone owns the
 * per-(scenario, stage, block) **balance row** plus the optional
 * **cap row** (hard or soft) and per-ton **price coefficient**.
 *
 * ## Multi-pollutant aggregation (GHG basket / CO₂-equivalent)
 *
 * A zone may cover several pollutants by listing them in
 * `emissions[]`, each with a `weight` (GWP — Global Warming
 * Potential, per IPCC AR6 100-year convention).  The zone's
 * production column carries CO₂-equivalent tonnage; the balance row
 * sums `weight_p · rate_{s,p} · gen_s · dur_b` across every
 * `EmissionSource` pointing at this zone.
 *
 * Common GWP-100 (IPCC AR6, AR5 in parentheses):
 *   - CO₂      :    1
 *   - CH₄      :   27.9  ( 28)
 *   - N₂O      :  273   (265)
 *   - HFC-134a : 1530   (1300)
 *   - SF₆      :22 800  (23500)
 *
 * For single-pollutant zones (the common case — a pure CO₂ cap),
 * provide a one-element `emissions = [{emission: "co2", weight: 1.0}]`
 * or use the JSON-only legacy shortcut `"emission": "co2"`.
 *
 * ## Constraint flavors
 *
 * | Configuration                  | Effect                            |
 * |--------------------------------|-----------------------------------|
 * | `cap` unset, `price` unset     | Pure reporting — balance row only |
 * | `cap` set, `cap_cost` unset    | Hard cap (no slack)               |
 * | `cap` set, `cap_cost` set      | Soft cap (slack with penalty)     |
 * | `price` set                    | Per-ton tax on the production col |
 *
 * Cap and price may both be set; they wire independently.  In a
 * multi-pollutant zone the cap and tax are both expressed in the
 * weighted (CO₂-eq) unit.
 *
 * @see Emission for the pollutant type registry
 * @see EmissionSource — the generator → zone bridge carries its own
 *      `emission` FK to disambiguate which pollutant in a
 *      multi-pollutant zone the rate is for.
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct EmissionZoneFactor
 * @brief One row of an `EmissionZone.emissions[]` table: the
 *        pollutant kind plus its CO₂-equivalent weight (GWP).
 *
 * `weight` defaults to 1.0 — the correct value for CO₂ itself and
 * for any single-pollutant zone where the cap unit equals the
 * pollutant unit.
 *
 * Naming aligns with IPCC AR6 / PLEXOS "Emissions Constraint with
 * weights" / PSR SDDP's per-emission GWP column.
 */
struct EmissionZoneFactor
{
  /// FK to the `Emission` pollutant kind (e.g. `"co2"`, `"ch4"`).
  SingleId emission {unknown_uid};

  /// CO₂-equivalent weight (GWP-100 per IPCC).  Defaults to 1.0.
  /// Multiplied into the zone's balance row coefficient on every
  /// `EmissionSource` pointing at this zone for this pollutant.
  OptReal weight {};
};

/**
 * @struct EmissionZone
 * @brief A pollutant balance / cap / price constraint scope, possibly
 *        covering multiple pollutants (GHG basket).
 */
struct EmissionZone
{
  /// Canonical class-name constant used in LP row labels and the
  /// per-element output directory `Output/EmissionZone/*.parquet`.
  static constexpr LPClassName class_name {"EmissionZone"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name (e.g. "global_co2", "ghg_basket")
  OptActive active {};  ///< Activation status

  /// List of pollutants covered by this zone, each with a GWP weight.
  /// One pollutant per `EmissionZoneFactor` row.  The balance row
  /// aggregates `Σ_p weight_p · production_p` into the (CO₂-eq)
  /// production column.  Legacy JSON `"emission": "<name>"`
  /// (singular) is auto-folded into a single-element list with
  /// weight 1.0 by the JSON constructor.
  Array<EmissionZoneFactor> emissions {};

  /// Optional cap on total emissions across all sources in this zone,
  /// per stage `[CO₂-eq tons / stage]`.  When set, a per-stage cap row
  /// is built that aggregates the per-block production over all
  /// blocks.  Hard by default; soft if `cap_cost` is also set.
  OptTRealFieldSched cap {};

  /// Optional penalty `[$/CO₂-eq ton]` on slack above `cap` — converts
  /// the cap from hard to soft.  Stage-schedulable.
  OptTRealFieldSched cap_cost {};

  /// Optional per-ton tax / permit price `[$/CO₂-eq ton]`,
  /// stage-schedulable.  Adds `price · production_b` to the objective
  /// for every block.
  OptTRealFieldSched price {};
};

}  // namespace gtopt
