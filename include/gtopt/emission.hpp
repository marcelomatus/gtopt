/**
 * @file      emission.hpp
 * @brief     Pollutant TYPE entity (registry; no constraints)
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `Emission` is the lightweight registry of pollutant KINDS modeled in
 * a run — CO₂, SO₂, NOₓ, CH₄(-eq), HFC, SF₆, …  It carries the
 * pollutant's short name only; cap/price/cap_cost moved to
 * `EmissionZone` so the same pollutant can have different
 * regulatory treatments in different scopes (e.g. global CO₂ tax +
 * California-only cap).
 *
 * ### JSON
 *
 * ```json
 * {"uid": 1, "name": "co2"}
 * ```
 *
 * The `name` is the canonical pollutant identifier (lower-case
 * snake-case).  Used as the prefix in per-pollutant output parquet
 * filenames (e.g. `Generator/co2_emission_rate_sol.parquet`) and as
 * the FK target of `EmissionZone.emissions[].emission`,
 * `EmissionSource.emission`, and (planned) `Fuel.emission_factors[]`.
 *
 * Passive in the LP — no rows, no columns.  Constraint wiring lives
 * on `EmissionZone` (per-pollutant cap / price / slack).
 *
 * @see EmissionZone for the per-pollutant cap / price / slack
 * @see EmissionSource for the generator → zone bridge
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct Emission
 * @brief Pollutant type tag.  Just `uid` + `name`.
 */
struct Emission
{
  /// Canonical class-name constant used in LP row labels and
  /// `Output/Emission/*.parquet` paths.
  static constexpr LPClassName class_name {"Emission"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Pollutant short name (`"co2"`, `"so2"`, `"nox"`, …)
  OptActive active {};  ///< Activation status
};

}  // namespace gtopt
