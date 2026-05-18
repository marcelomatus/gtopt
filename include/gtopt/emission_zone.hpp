/**
 * @file      emission_zone.hpp
 * @brief     Pollutant constraint zone — balance row, optional cap / price
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `EmissionZone` is the LP-active constraint owner for a single
 * pollutant kind within a regulatory or geographic scope.  Mirrors the
 * `InertiaZone` / `ReserveZone` pattern: the zone owns the
 * per-(scenario, stage, block) **balance row** plus the optional
 * **cap row** (hard or soft) and per-ton **price coefficient**.
 *
 * Multiple zones may target the same pollutant (e.g. a global CO₂
 * cap PLUS a state-level CO₂ cap stacked on the same generators); the
 * `EmissionSource` bridge entity (one row per generator-into-zone)
 * carries the per-generator contribution rate (tons / MWh).
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
 * Cap and price may both be set; they wire independently.
 *
 * ## Passive in Commit 2
 *
 * Like `Emission` in Commit 1, this entity is **passive** in Commit 2 —
 * the LP-active wiring (balance row + cap row + price coefficient)
 * lands in Commit 3.  See `EmissionZoneLP` for the wrapper.
 *
 * @see Emission for the pollutant type registry
 * @see EmissionSource for the generator → zone bridge entity
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct EmissionZone
 * @brief A pollutant balance / cap / price constraint scope.
 *
 * Each EmissionZone targets exactly ONE pollutant kind (via the
 * `emission` FK).  Multi-pollutant rollups (e.g. GHG basket with GWP
 * weights) are modeled as multiple zones plus a `UserConstraint` that
 * aggregates them.
 */
struct EmissionZone
{
  /// Canonical class-name constant used in LP row labels and the
  /// per-element output directory `Output/EmissionZone/*.parquet`.
  static constexpr LPClassName class_name {"EmissionZone"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name (e.g. "global_co2", "la_nox")
  OptActive active {};  ///< Activation status

  /// FK to the `Emission` pollutant this zone constrains.  One zone =
  /// one pollutant; multi-pollutant scopes use multiple zones.
  SingleId emission {unknown_uid};

  /// Optional cap on total emissions across all sources in this zone,
  /// per stage `[tons / stage]`.  When set, a per-stage cap row is
  /// built that aggregates the per-block production over all blocks.
  /// Hard by default; soft if `cap_cost` is also set.
  OptTRealFieldSched cap {};

  /// Optional penalty `[$/ton]` on slack above `cap` — converts the
  /// cap from hard to soft.  Stage-schedulable.
  OptTRealFieldSched cap_cost {};

  /// Optional per-ton tax / permit price `[$/ton]`, stage-schedulable.
  /// Adds `price · production_b` to the objective for every block.
  OptTRealFieldSched price {};
};

}  // namespace gtopt
