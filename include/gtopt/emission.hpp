/**
 * @file      emission.hpp
 * @brief     Defines the Emission pollutant entity (price + cap)
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * An `Emission` is the first-class representation of a single pollutant
 * (e.g. CO₂, SO₂, NOₓ, CH₄-equivalent).  It carries the optional
 * stage-schedulable **price** ($/ton — added to the dispatch cost stack
 * via fuel-burn × per-fuel-factor × price) and **cap** (tons / stage —
 * a soft upper bound with `cap_cost` as the slack penalty).
 *
 * The mapping from fuel burn to emitted mass is **not** stored here;
 * it lives on the `Fuel` side as a (pollutant, factor) row in the
 * planned `Fuel.emission_factors[]` table (Commit 2).  Generators
 * inherit emissions from their `fuel` field automatically — there is
 * no per-generator emission factor.  Optional `EmissionCapture` rows on
 * the Generator can scale the inherited factor (CCS / abatement) in a
 * future commit (Layer 3).
 *
 * ## Naming conventions
 *
 * - `name` is the pollutant identifier (e.g. `"co2"`, `"so2"`,
 *   `"nox"`).  Used as the per-pollutant prefix in output parquet
 *   filenames: `Generator/co2_emissions_sol.parquet`,
 *   `Generator/co2_emission_rate_sol.parquet`, …
 *   Mirrors PSR SDDP `emissao_<pollutant>.csv` and PLEXOS
 *   `Generator.<Pollutant> Production` / `<Pollutant> Production Rate`.
 *
 * - `price` is the per-ton tax / permit price.  In PLEXOS this is
 *   `Emission.Price`; in PSR SDDP this is implicit through the
 *   emission-tax constraint.  Setting this adds a per-segment cost
 *   coefficient `price × ef × fuel_burn_per_MWh` to every
 *   thermal-generator dispatch column.
 *
 * - `cap` is the per-stage maximum total mass (across all generators
 *   and scenarios).  In PLEXOS this is `Emission.Max Production`; in
 *   PSR SDDP this is a per-stage emission cap.  Implemented as a soft
 *   constraint with `cap_cost` slack.
 *
 * ## Output (when this entity is fully wired in Commit 3+)
 *
 * - `Emission/cap_dual.parquet` — shadow price of the cap constraint
 *   per stage (marginal abatement cost).
 * - `Emission/slack_sol.parquet` — cap overshoot in tons per stage
 *   (zero when the cap is non-binding or slack-free).
 *
 * @see Fuel — currently still carries scalar `combustion_emission_factor`
 *      / `upstream_emission_factor` (single-pollutant CO₂); Commit 2
 *      refactors that into `Fuel.emission_factors[]` keyed by
 *      `SingleId{Emission}` and folds the legacy fields into a
 *      synthetic `Emission{name="co2"}` row at parse time.
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct Emission
 * @brief Pollutant entity — name, optional price, optional cap.
 *
 * Passive in Commit 1 — no LP rows / variables.  Cap and price wiring
 * land in Commit 4 once per-fuel per-pollutant factors are in place.
 */
struct Emission
{
  /// Canonical class-name constant used in LP row labels and per-element
  /// output parquet directories.  Single source of truth —
  /// `EmissionLP` exposes no separate `ClassName` member; callers reach
  /// the constant via `Emission::class_name` directly.
  static constexpr LPClassName class_name {"Emission"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Pollutant short name (`"co2"`, `"so2"`, `"nox"`, …)
  OptActive active {};  ///< Activation status

  /// Per-ton tax / permit price `[$/ton]`, stage-schedulable.  When
  /// set, adds `price × ε × fuel_burn` to each thermal-generator
  /// dispatch column's cost coefficient (Commit 4 wiring).
  OptTRealFieldSched price {};

  /// Per-stage emission cap `[tons]`.  When set, a soft constraint
  /// `Σ_g emission_g,s ≤ cap_s + slack_s` is added with `cap_cost` on
  /// the slack (Commit 4 wiring).
  OptTRealFieldSched cap {};

  /// Per-ton penalty applied to cap overshoot `[$/ton]`,
  /// stage-schedulable.  Mirrors gtopt's existing soft-constraint
  /// idioms (`hydro_fail_cost`, `soft_emin_cost`).
  OptTRealFieldSched cap_cost {};
};

}  // namespace gtopt
