/**
 * @file      emission_source.hpp
 * @brief     Bridge entity — generator → emission zone with combustion +
 *            optional upstream (WTT) per-MWh rates.
 * @date      2026-05-18
 * @author    marcelo
 * @copyright BSD-3-Clause
 *
 * `EmissionSource` is the generator↔zone bridge for the emissions
 * model.  Each row says "this generator is an emission source for
 * pollutant `p` in zone `z` at this per-MWh rate".  Mirrors the
 * `InertiaProvision` / `ReserveProvision` bridge pattern.
 *
 * Terminology: an emission **source** (per IPCC AR6 / EPA AP-42 /
 * GHG Protocol) is the producing unit responsible for releasing a
 * pollutant.  PLEXOS calls this collection "Producers" on the Emission
 * object; we use the more universal scientific term.
 *
 * ## Combustion vs upstream (WTT) split
 *
 * The IPCC and GHG-Protocol distinguish two emission scopes for fuel-
 * burning units, both expressed in the same per-MWh unit and both
 * counted toward the zone's balance row:
 *
 *   - `rate`           — **combustion / tank-to-stack (TTW)** factor.
 *                        The CO₂ released at the burner per MWh
 *                        produced.  Synonyms: "stack", "direct",
 *                        "tank-to-wheel", "Scope 1".
 *
 *   - `upstream_rate`  — **upstream / well-to-tank (WTT)** factor.
 *                        Emissions from extracting, processing,
 *                        and transporting the fuel per MWh produced.
 *                        Synonyms: "fuel-cycle", "pre-combustion",
 *                        "well-to-tank", "Scope 3 (fuel-related)".
 *
 * Total (well-to-burner-tip / well-to-wheel / lifecycle) is the sum.
 * Set both, only `rate`, or only `upstream_rate` depending on which
 * scope(s) the zone is supposed to cap or price.  The LP balance row
 * sums both — there is no need to merge them on the user side.
 *
 * @see EmissionZone for the constraint owner and the GHG-basket weights
 * @see Generator.emissions[] for the inline form (parsed into this
 *      array by `System::expand_emission_sources()`)
 */

#pragma once

#include <gtopt/lp_class_name.hpp>
#include <gtopt/object.hpp>

namespace gtopt
{

struct EmissionSource
{
  /// Canonical class-name constant used in LP row labels.
  static constexpr LPClassName class_name {"EmissionSource"};

  Uid uid {unknown_uid};  ///< Unique identifier
  Name name {};  ///< Human-readable name (often auto-generated:
                 ///< `{generator}_to_{zone}_{emission}`).
  OptActive active {};  ///< Activation status

  /// FK to the `Generator` that emits.  Optional in JSON because the
  /// inline form on `Generator.emissions[]` omits it; set
  /// automatically by `System::expand_emission_sources()` to the
  /// parent generator's `SingleId`.
  OptSingleId generator {};

  /// FK to the `EmissionZone` the source contributes to.
  SingleId zone {unknown_uid};

  /// FK to the `Emission` pollutant kind.  Required when the zone is
  /// multi-pollutant (so the LP knows which GWP weight to pull from
  /// the zone's `emissions[]` table).  For single-pollutant zones,
  /// must still be set to the zone's pollutant FK — the LP-side
  /// weight lookup rejects sources whose emission isn't listed in
  /// the zone's `emissions[]`.
  SingleId emission {unknown_uid};

  /// Combustion / tank-to-stack (TTW) emission rate `[t/MWh]`,
  /// stage-schedulable.  The CO₂ released at the burner per MWh of
  /// gross generation.  Adds `weight · rate · dur · gen` to the
  /// zone's balance row.
  OptTRealFieldSched rate {};

  /// Upstream / well-to-tank (WTT) emission rate `[t/MWh]`,
  /// stage-schedulable.  Emissions from extracting, processing and
  /// transporting the fuel per MWh of generation.  Adds
  /// `weight · upstream_rate · dur · gen` to the zone's balance row
  /// in addition to `rate`.  Set this when you want a full
  /// lifecycle (well-to-burner-tip) cap or tax; leave unset for
  /// stack-only (Scope 1) accounting.
  OptTRealFieldSched upstream_rate {};
};

}  // namespace gtopt
