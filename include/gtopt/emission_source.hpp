/**
 * @file      emission_source.hpp
 * @brief     Bridge entity — generator → emission zone with a per-MWh rate
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `EmissionSource` is the generator↔zone bridge for the emissions
 * model.  Each row says "this generator is an emission source for that
 * zone, at this per-MWh rate".  Mirrors the
 * `InertiaProvision` / `ReserveProvision` bridge pattern.
 *
 * Terminology: an emission **source** (per IPCC AR6 / EPA AP-42 /
 * GHG Protocol) is the producing unit responsible for releasing a
 * pollutant.  PLEXOS calls this collection "Producers" on the Emission
 * object; we use the more universal scientific term.
 *
 * ## JSON shapes — top-level vs inline
 *
 * Canonical top-level form:
 * ```json
 * "emission_source_array": [
 *   {"generator": "ngcc_la", "zone": "global_co2", "rate": 0.4}
 * ]
 * ```
 *
 * Inline-on-generator shorthand (parse-time expansion in
 * `System::expand_emission_sources()`):
 * ```json
 * "generator_array": [
 *   {"name": "ngcc_la",
 *    "emissions": [
 *      {"zone": "global_co2", "rate": 0.4},
 *      {"zone": "la_nox",     "rate": 0.05}
 *    ]}
 * ]
 * ```
 *
 * The inline form copies each entry into `emission_source_array` and
 * sets the `generator` FK from the parent — same idiom used for
 * `Reservoir.seepage[]` → `reservoir_seepage_array`.
 *
 * ## Passive in Commit 2
 *
 * In Commit 2 the entity is purely data — `EmissionSourceLP` is a
 * passive parameter carrier.  In Commit 3 the LP-wiring lands: each
 * source contributes a coefficient `rate · gen · dur` to its zone's
 * balance row.
 *
 * @see Emission for the pollutant tag
 * @see EmissionZone for the constraint owner
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

/**
 * @struct EmissionSource
 * @brief One row per (generator, emission zone) — the per-MWh emission
 *        contribution from the generator into the zone's balance.
 *
 * The zone's `emission` FK determines the pollutant; this struct does
 * not carry the emission FK again (denormalized via the zone) so that
 * mismatched (generator, emission, zone) triples are unrepresentable
 * by construction.
 */
struct EmissionSource
{
  /// Canonical class-name constant used in LP row labels.
  static constexpr LPClassName class_name {"EmissionSource"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name (often auto-generated:
                 ///< `{generator}_to_{zone}`).
  OptActive active {};  ///< Activation status

  /// FK to the `Generator` that emits.  Optional in JSON because the
  /// inline form on `Generator.emissions[]` omits it; set
  /// automatically by `System::expand_emission_sources()` to the
  /// parent generator's `SingleId`.  Validated at LP-build time —
  /// a row reaching the LP layer with `!generator.has_value()` is an
  /// author error.
  OptSingleId generator {};

  /// FK to the `EmissionZone` the source contributes to.
  SingleId zone {unknown_uid};

  /// Per-MWh emission rate `[tons / MWh]`, stage-schedulable.  This
  /// is the DIRECT contribution path — independent of any fuel
  /// emission factor.  The Fuel-derived path (Generator.fuel ×
  /// Fuel.emission_factors × Generator.heat_rate) adds in parallel
  /// when both are configured for the same (zone, pollutant).
  OptTRealFieldSched rate {};
};

}  // namespace gtopt
