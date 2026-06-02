# -*- coding: utf-8 -*-
# SPDX-License-Identifier: BSD-3-Clause
"""Fill in missing per-fuel CO2 emission factors on a gtopt planning dict.

Shared by ``plp2gtopt --emissions`` and ``plexos2gtopt --emissions``.
Driven by an optional ``--emissions-file PATH`` (default: the bundled
IPCC-2006 defaults at ``gtopt_shared/data/ipcc_emission_factors.json``).

Why this exists
---------------

* PLP has **no** fuel / emission information — its generator cost is a
  flat ``gcost`` ($/MWh).
* PLEXOS XML *sometimes* ships a per-fuel CO2 factor on the Fuel
  object (``CO2 Production Rate`` / ``Emission → Fuel: Production Rate``),
  but many CEN PCP bundles omit it.
* gtopt's emission accounting requires
  ``Fuel.emission_factors[{emission: "co2", combustion: …}]`` PLUS a
  matching ``emission_array[{"name": "co2"}]`` pollutant definition.

This module runs **after** the converter has emitted its own Fuel
array (and, for plp2gtopt, after the PLEXOS overlay) so any factor the
source did ship wins.  For every Fuel still missing a CO2 row, the
IPCC default for the matching fuel name (or alias) is injected; the
``emission_array`` row for ``"co2"`` is synthesized once if any Fuel
now carries a factor.

The bundled defaults file is curated for the Chilean PLP / PLEXOS fuel
list (diesel, fuel oil, natural gas, LNG, coal {bituminous,
sub-bituminous, anthracite, lignite, coking}, petcoke, LPG, kerosene,
naphtha, crude oil, biomass, biogas, geothermal) and follows IPCC 2006
Volume 2 Chapter 1 Table 1.4.  Aliases use an aggressive normalization
(case fold + strip space / underscore / dash), so e.g.
``"GAS_NATURAL"`` / ``"Gas Natural"`` / ``"gas-natural"`` all hit the
same entry.

The fill-in is non-destructive: a Fuel that already carries a CO2
factor (combustion > 0 or upstream > 0) is left untouched even if the
defaults file has a different value — PLEXOS / project data is always
authoritative.
"""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)


#: Bundled IPCC-2006 defaults shipped with the ``gtopt_shared`` package.
#: Resolved via ``Path(__file__).parent`` so it stays correct under
#: wheel install, src/ checkout, and pytest's site-packages hardlink.
DEFAULT_EMISSIONS_FILE: Path = (
    Path(__file__).parent / "data" / "ipcc_emission_factors.json"
)

#: Pollutant name used by gtopt to match ``emission_factors[].emission``
#: against ``emission_array[].name``.  Lower-case is the project
#: convention (see ``plexos2gtopt.gtopt_writer.build_fuel_array``).
_CO2_POLLUTANT: str = "co2"

#: Default :class:`EmissionZone` name synthesized when the converter
#: produces emission_factors but no zone yet covers them.  Inert
#: (no ``cap``, no ``price``, no ``allowance_pool``) so it does not
#: change the LP cost / dispatch by itself — its job is purely to make
#: gtopt's ``System::expand_fuel_emission_sources`` happy (it returns
#: early if ``emission_zone_array`` is empty), so the per-generator
#: ``EmissionSource`` rows actually land in the LP.  Users who want a
#: live carbon cap / price overlay this zone via a standard JSON merge
#: (adding ``cap``, ``cap_cost``, ``price`` fields) without re-running
#: the converter.
_DEFAULT_CO2_ZONE_NAME: str = "global_co2"


# ---------------------------------------------------------------------------
# Name normalization (inlined so this module has no external dep)
# ---------------------------------------------------------------------------


def _normalize(name: str) -> str:
    """Strip separators + upper-case for cross-source name matching.

    Identical to ``plp2gtopt._plexos_overlay._normalize`` — kept inline
    here so the shared emissions module has no plp2gtopt dependency.
    Strips all of ``" "`` / ``"_"`` / ``"-"`` and upper-cases so e.g.
    ``"Gas Natural"`` / ``"gas-natural"`` / ``"GAS_NATURAL"`` all
    collapse to ``"GASNATURAL"``.
    """
    out = name.strip().upper()
    for sep in (" ", "_", "-"):
        out = out.replace(sep, "")
    return out


# ---------------------------------------------------------------------------
# Data classes
# ---------------------------------------------------------------------------


#: Ordered tuple of GHG pollutants the IPCC defaults file may carry.
#: Each entry maps to the per-pollutant attribute pair on
#: :class:`EmissionFactor` (e.g. ``"ch4"`` ↔ ``ch4_combustion`` /
#: ``ch4_upstream``).  Add new pollutants here AND on the dataclass +
#: the loader to extend coverage.  Order matters only for stable
#: dict-key ordering in the synthesized ``Fuel.emission_factors[]``
#: output (deterministic JSON for round-trip tests).
_POLLUTANTS: tuple[str, ...] = ("co2", "ch4", "n2o")


@dataclass(frozen=True)
class EmissionFactor:
    """One per-fuel IPCC entry covering multiple GHGs.

    All factors are tonnes_of_pollutant / GJ on a net-calorific-value
    (NCV / LHV) basis to match gtopt's expected unit (multiplies
    ``Generator.heat_rate`` [GJ/MWh] to recover tonnes_pollutant/MWh).

    Currently carries the IPCC 2006 stationary-combustion GHG triplet:

    * **CO2** — Table 1.4 (Energy industries, default factors)
    * **CH4** — Table 2.2 (Default emission factors for stationary
      combustion in the energy industries)
    * **N2O** — Table 2.2 (same source)

    Extend with non-GHG air pollutants (NOx, SO2, particulates, VOCs)
    by sourcing from EPA AP-42 / EMEP-EEA Guidebook and adding new
    field pairs.  CH4 / N2O factors are ~10⁵ smaller than CO2 — keep
    them in the same tCO2/GJ basis (no GWP weighting at the factor
    level; weighting belongs in the EmissionZone basket).
    """

    name: str
    aliases: tuple[str, ...]
    co2_combustion: float
    co2_upstream: float
    ch4_combustion: float = 0.0
    ch4_upstream: float = 0.0
    n2o_combustion: float = 0.0
    n2o_upstream: float = 0.0
    heat_content: float = 0.0
    ipcc_reference: str = ""

    def rates(self, pollutant: str) -> tuple[float, float]:
        """Return ``(combustion, upstream)`` rates for ``pollutant``.

        Unknown pollutant names return ``(0.0, 0.0)`` — callers can
        iterate :data:`_POLLUTANTS` safely without explicit
        ``hasattr`` guards.
        """
        comb = getattr(self, f"{pollutant}_combustion", 0.0)
        upst = getattr(self, f"{pollutant}_upstream", 0.0)
        return (comb, upst)

    @property
    def has_factor(self) -> bool:
        """Return True when ANY pollutant has a non-zero rate.

        Used by :func:`apply_emission_defaults` to decide whether a
        fuel should land in ``fuels_factor_added`` (something to do)
        vs ``fuels_unknown`` (skip — all zero).  Biomass / biogas
        with biogenic-zero CO2 but non-zero CH4 / N2O correctly hit
        the "has_factor" branch and contribute CH4 / N2O rows.
        """
        return any(
            getattr(self, f"{p}_combustion", 0.0) != 0.0
            or getattr(self, f"{p}_upstream", 0.0) != 0.0
            for p in _POLLUTANTS
        )


@dataclass(frozen=True)
class EmissionDefaults:
    """Lookup table of per-fuel emission factors keyed by canonical name and aliases."""

    source_path: str
    description: str
    units: dict[str, str]
    fuels: tuple[EmissionFactor, ...]
    #: Reverse lookup: normalized name / alias → EmissionFactor.  Built
    #: at construction time so :meth:`lookup` is O(1).
    _by_normalized: dict[str, EmissionFactor] = field(default_factory=dict, repr=False)

    def lookup(
        self,
        fuel_name: str,
        *,
        subtype_hint: str | None = None,
    ) -> EmissionFactor | None:
        """Find the factor entry for ``fuel_name`` by canonical name or alias.

        Returns ``None`` when no entry matches.

        Three-step match:

        0. **Subtype hint** (if given AND resolves).  Used by callers
           that already know the project-specific IPCC sub-grade —
           e.g. ``plexos2gtopt`` sets ``Fuel.subtype = "natural_gas"``
           on ``Gas_*_GN_*`` and ``"lng"`` on the rest of ``Gas_*``,
           and the emissions engine reads that field before falling
           back to name lookup.  Lets the same family name resolve to
           DIFFERENT sub-grades for different fuels in the same
           planning.
        1. **Exact normalized match** on the full name (case-fold +
           strip ``" "``/``"_"``/``"-"``).  This catches canonical
           names and aliases listed in the defaults file.
        2. **Prefix fallback** on the first underscore-separated token
           of the ORIGINAL name (before normalization).  Catches the
           ``<family>_<plant>`` naming convention shipped by CEN-Chile
           PLEXOS bundles (``Carbon_Andina`` → ``Carbon`` →
           ``coal_bituminous``; ``Biomasa_Celco_B1`` → ``Biomasa`` →
           ``biomass``; ``Gas_GNLQuintero_A`` → ``Gas`` →
           ``natural_gas`` via the ``"gas"`` alias).

        Without the prefix fallback the ~220 CEN PCP fuels (named
        ``Carbon_*``, ``Diesel_*``, ``FuelOil_*``, ``Biomasa_*``,
        ``Biogas_*``, ``Gas_*``, ``GLP_*``) all fall through as unknown
        — the IPCC factors land on nothing.
        """
        # Step 0: subtype hint wins when it resolves.
        if subtype_hint:
            hit = self._by_normalized.get(_normalize(subtype_hint))
            if hit is not None:
                return hit
        # Step 1: full-name match.
        hit = self._by_normalized.get(_normalize(fuel_name))
        if hit is not None:
            return hit
        # Step 2: first-token-of-original-name fallback.  Split BEFORE
        # normalization so the `_` is still visible as a separator.
        stripped = fuel_name.strip()
        if "_" in stripped:
            head = stripped.split("_", 1)[0]
            if head:
                return self._by_normalized.get(_normalize(head))
        return None


@dataclass(frozen=True)
class EmissionReport:
    """Summary of what the emissions pass touched.

    Written to ``--emissions-report`` (default
    ``<output-dir>/plexos_emissions_report.json``) so the user can
    audit which Fuels picked up IPCC defaults and which were left
    untouched (because they already had a factor or the fuel name was
    unknown to the defaults file).

    ``emission_zone_*`` fields track the default ``"global_co2"`` zone
    synthesis: required by gtopt's
    ``System::expand_fuel_emission_sources`` so the per-generator
    ``EmissionSource`` rows actually land in the LP.  See
    :data:`_DEFAULT_CO2_ZONE_NAME` for why the zone is inert by default.
    """

    source_path: str
    fuels_factor_added: tuple[str, ...] = ()
    fuels_factor_preserved: tuple[str, ...] = ()
    fuels_unknown: tuple[str, ...] = ()
    emission_array_created: bool = False
    emission_array_already_present: bool = False
    emission_zone_created: bool = False
    emission_zone_already_present: bool = False

    def to_dict(self) -> dict[str, Any]:
        """Return a JSON-serializable dict view of the report."""
        return {
            "source_path": self.source_path,
            "summary": {
                "factor_added": len(self.fuels_factor_added),
                "factor_preserved": len(self.fuels_factor_preserved),
                "unknown_fuels": len(self.fuels_unknown),
                "emission_array_created": self.emission_array_created,
                "emission_array_already_present": self.emission_array_already_present,
                "emission_zone_created": self.emission_zone_created,
                "emission_zone_already_present": self.emission_zone_already_present,
            },
            "fuels_factor_added": list(self.fuels_factor_added),
            "fuels_factor_preserved": list(self.fuels_factor_preserved),
            "fuels_unknown": list(self.fuels_unknown),
        }


# ---------------------------------------------------------------------------
# Loader
# ---------------------------------------------------------------------------


def load_emission_defaults(path: Path | None = None) -> EmissionDefaults:
    """Load an emissions JSON file.

    ``path`` defaults to :data:`DEFAULT_EMISSIONS_FILE` (the bundled
    IPCC-2006 defaults).  The file must follow the schema described in
    the bundled file's header — top-level ``"fuels"`` list of objects
    with ``name``, optional ``aliases``, ``co2_combustion``,
    ``co2_upstream``, ``heat_content``, ``ipcc_reference``.

    Raises:
        FileNotFoundError: ``path`` does not exist.
        ValueError: file contents are not a JSON object with a
            ``"fuels"`` list, or any fuel entry has an empty name.
    """
    if path is None:
        path = DEFAULT_EMISSIONS_FILE
    if not path.exists():
        raise FileNotFoundError(f"--emissions-file {path}: no such file")
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(
            f"--emissions-file {path}: expected a JSON object, "
            f"got {type(data).__name__}"
        )
    raw_fuels = data.get("fuels")
    if not isinstance(raw_fuels, list):
        raise ValueError(f"--emissions-file {path}: missing top-level 'fuels' list")
    parsed: list[EmissionFactor] = []
    by_norm: dict[str, EmissionFactor] = {}
    for entry in raw_fuels:
        if not isinstance(entry, dict):
            continue
        name = str(entry.get("name", "")).strip()
        if not name:
            raise ValueError(f"--emissions-file {path}: fuel entry without a 'name'")
        aliases = tuple(str(a) for a in entry.get("aliases", []) or [])
        factor = EmissionFactor(
            name=name,
            aliases=aliases,
            co2_combustion=float(entry.get("co2_combustion", 0.0) or 0.0),
            co2_upstream=float(entry.get("co2_upstream", 0.0) or 0.0),
            # CH4 / N2O are optional in the JSON schema (legacy / external
            # user-curated files may carry only CO2).  Default 0 means
            # "no row injected for this pollutant on this fuel" — the
            # apply step skips zero-rate pollutants entirely.
            ch4_combustion=float(entry.get("ch4_combustion", 0.0) or 0.0),
            ch4_upstream=float(entry.get("ch4_upstream", 0.0) or 0.0),
            n2o_combustion=float(entry.get("n2o_combustion", 0.0) or 0.0),
            n2o_upstream=float(entry.get("n2o_upstream", 0.0) or 0.0),
            heat_content=float(entry.get("heat_content", 0.0) or 0.0),
            ipcc_reference=str(entry.get("ipcc_reference", "")),
        )
        parsed.append(factor)
        # Index by canonical name + every alias.  Last writer wins on
        # collision — usually a sign of overlapping aliases that should
        # be cleaned up in the defaults file, so log it.
        for key in (name, *aliases):
            norm = _normalize(key)
            if norm and norm in by_norm and by_norm[norm].name != name:
                logger.warning(
                    "emissions defaults: alias '%s' collides between "
                    "'%s' and '%s' — '%s' wins",
                    key,
                    by_norm[norm].name,
                    name,
                    name,
                )
            by_norm[norm] = factor
    return EmissionDefaults(
        source_path=str(path),
        description=str(data.get("description", "")),
        units=dict(data.get("units", {}) or {}),
        fuels=tuple(parsed),
        _by_normalized=by_norm,
    )


# ---------------------------------------------------------------------------
# Apply step
# ---------------------------------------------------------------------------


def _fuel_pollutants_present(fuel: dict[str, Any]) -> frozenset[str]:
    """Return the set of pollutant tags this Fuel already carries with a
    non-trivial (non-all-zero) row.

    A row whose combustion + upstream are BOTH zero is treated as a
    placeholder — the apply step replaces it with the IPCC default.
    """
    present: set[str] = set()
    factors = fuel.get("emission_factors")
    if not isinstance(factors, list):
        return frozenset()
    for row in factors:
        if not isinstance(row, dict):
            continue
        tag = str(row.get("emission", "")).strip().lower()
        if not tag:
            continue
        comb = row.get("combustion", 0.0) or 0.0
        upst = row.get("upstream", 0.0) or 0.0
        if comb != 0.0 or upst != 0.0:
            present.add(tag)
    return frozenset(present)


def _fuel_has_co2_factor(fuel: dict[str, Any]) -> bool:
    """Back-compat alias: True iff the CO2 pollutant is already populated.

    The multi-pollutant apply step uses :func:`_fuel_pollutants_present`
    instead; this helper is kept so external callers (and the public
    API surface tests pin) don't break.
    """
    return _CO2_POLLUTANT in _fuel_pollutants_present(fuel)


def _inject_emission_rows(
    fuel: dict[str, Any],
    factor: EmissionFactor,
    *,
    skip_existing: frozenset[str] = frozenset(),
) -> set[str]:
    """Append one row per non-zero pollutant on ``fuel``.

    Iterates :data:`_POLLUTANTS` — for each pollutant whose combustion
    or upstream rate on ``factor`` is non-zero AND that is NOT already
    in ``skip_existing``, appends a fresh row.  Rows already on the
    fuel are preserved entirely (the source data — PLEXOS XML, user
    JSON merge — is authoritative; defaults only FILL GAPS).

    A zero-rate placeholder for a pollutant (combustion=0,
    upstream=0) is treated as "absent" by ``_fuel_pollutants_present``
    — callers compute ``skip_existing`` from that helper so
    placeholders get replaced cleanly.

    Returns the set of pollutant tags that ended up being injected
    — used by the caller to know which pollutants now need an
    ``emission_array`` entry + ``EmissionZone`` coverage.
    """
    factors = fuel.get("emission_factors")
    if not isinstance(factors, list):
        factors = []
    injected: set[str] = set()
    new_rows: list[dict[str, Any]] = []
    for pollutant in _POLLUTANTS:
        if pollutant in skip_existing:
            continue
        comb, upst = factor.rates(pollutant)
        if comb == 0.0 and upst == 0.0:
            continue
        row: dict[str, Any] = {"emission": pollutant}
        if comb != 0.0:
            row["combustion"] = comb
        if upst != 0.0:
            row["upstream"] = upst
        new_rows.append(row)
        injected.add(pollutant)
    # Strip placeholder rows for the pollutants we're about to inject
    # (zero-rate rows that ``_fuel_pollutants_present`` correctly
    # ignored).  Rows for OTHER pollutants and non-placeholder rows
    # for the pollutants in ``skip_existing`` are preserved.
    cleaned: list[dict[str, Any]] = []
    for row in factors:
        if not isinstance(row, dict):
            cleaned.append(row)
            continue
        tag = str(row.get("emission", "")).strip().lower()
        if tag in injected:
            # The only row with this tag still standing is a placeholder;
            # `_fuel_pollutants_present` would have added the tag to
            # skip_existing if it had carried a non-zero value.
            continue
        cleaned.append(row)
    fuel["emission_factors"] = cleaned + new_rows
    # Carry NCV if the Fuel does not already have one — informational.
    if factor.heat_content > 0.0 and not fuel.get("heat_content"):
        fuel["heat_content"] = factor.heat_content
    return injected


# Legacy alias retained so external callers / pinned tests continue to
# import it.  The new multi-pollutant ``_inject_emission_rows`` is the
# canonical entry point.
_inject_co2_row = _inject_emission_rows


def apply_emission_defaults(
    planning: dict[str, Any],
    defaults: EmissionDefaults,
    *,
    report_path: Path | None = None,
) -> EmissionReport:
    """Fill in missing CO2 factors on ``planning['system']['fuel_array']``.

    Idempotent: a Fuel that already carries a non-zero CO2 row is left
    untouched and ends up in :attr:`EmissionReport.fuels_factor_preserved`.

    Synthesizes ``system['emission_array']`` with a ``{"uid": 1,
    "name": "co2"}`` row if (and only if) at least one Fuel now carries
    a factor AND the array did not already contain a ``"co2"`` entry.

    Synthesizes ``system['emission_zone_array']`` with a single inert
    ``"global_co2"`` zone (see :data:`_DEFAULT_CO2_ZONE_NAME`) under
    the same condition AND when no existing zone already covers
    ``"co2"``.  This is required by gtopt's
    ``System::expand_fuel_emission_sources``, which returns early on
    an empty zone array — without it the per-generator
    ``EmissionSource`` rows never land in the LP even when every
    other ingredient is in place.
    """
    system = planning.setdefault("system", {})
    fuels: list[dict[str, Any]] = system.setdefault("fuel_array", [])

    added: list[str] = []
    preserved: list[str] = []
    unknown: list[str] = []

    # All pollutants that ended up with rows on ANY fuel after the
    # apply pass — drives emission_array and EmissionZone synthesis.
    pollutants_in_use: set[str] = set()

    any_factor_present = False
    for fuel in fuels:
        if not isinstance(fuel, dict):
            continue
        name = str(fuel.get("name", "")).strip()
        if not name:
            continue
        existing = _fuel_pollutants_present(fuel)
        # "preserved" means EVERY pollutant the defaults would inject
        # is already populated by the source data.  In multi-pollutant
        # mode that means: even if CO2 is present, the defaults can
        # still ADD a missing CH4 / N2O row.
        # Honour the optional ``subtype`` hint on the Fuel JSON when
        # present — set by ``plexos2gtopt`` for the natural-gas /
        # LNG split so the same family name resolves to different
        # IPCC sub-grades for different fuels.  When absent, lookup
        # falls back to full-name + family-prefix matching.
        subtype_hint = str(fuel.get("subtype", "") or "").strip() or None
        # Strip the converter-side ``subtype`` hint after reading it.
        # gtopt's C++ JSON parser uses StrictParsePolicy and would
        # error on this unknown field (it's a converter-internal
        # routing knob, not part of the gtopt Fuel schema).  Removed
        # here so the final on-disk JSON stays gtopt-compatible
        # regardless of which converter populated it.
        if "subtype" in fuel:
            del fuel["subtype"]
        factor = defaults.lookup(name, subtype_hint=subtype_hint)
        if factor is None or not factor.has_factor:
            if existing:
                # Source data has factors the defaults don't know
                # about; leave it untouched and mark preserved.
                preserved.append(name)
                pollutants_in_use.update(existing)
                any_factor_present = True
            else:
                unknown.append(name)
            continue
        # Determine which pollutants on the factor are NEW (would be
        # injected) — if at least one is new, we touch the fuel.
        would_inject = {
            p
            for p in _POLLUTANTS
            if factor.rates(p)[0] != 0.0 or factor.rates(p)[1] != 0.0
        }
        truly_new = would_inject - existing
        if not truly_new:
            preserved.append(name)
            pollutants_in_use.update(existing)
            any_factor_present = True
            continue
        # Inject only the new pollutants; existing ones survive
        # untouched (skip_existing tells inject which tags to leave
        # alone — so PLEXOS-shipped CO2 with a custom value stays put
        # while the missing CH4 / N2O rows get filled).
        injected = _inject_emission_rows(
            fuel, factor, skip_existing=frozenset(existing)
        )
        # Track every pollutant now on the fuel (injected ∪ preserved)
        pollutants_in_use.update(injected)
        pollutants_in_use.update(existing)
        added.append(name)
        any_factor_present = True

    # Ensure emission_array has a row per pollutant in use.  Without it,
    # gtopt's LP build drops the per-fuel factor with an unresolved-name
    # warning.  Backward-compat: when only CO2 is in use, behaviour is
    # byte-for-byte identical to the legacy CO2-only path.
    emission_array: list[dict[str, Any]] = system.setdefault("emission_array", [])
    existing_pollutant_names: set[str] = {
        str(row.get("name", "")).strip().lower()
        for row in emission_array
        if isinstance(row, dict)
    }
    # Preserve the legacy "co2 pollutant" boolean for the EmissionReport
    # — many existing tests assert this specific flag.
    has_co2_pollutant = _CO2_POLLUTANT in existing_pollutant_names
    pollutants_to_add = sorted(pollutants_in_use - existing_pollutant_names)
    if pollutants_to_add and any_factor_present:
        max_uid = max(
            (
                int(row.get("uid", 0) or 0)
                for row in emission_array
                if isinstance(row, dict)
            ),
            default=0,
        )
        for pollutant in pollutants_to_add:
            max_uid += 1
            emission_array.append({"uid": max_uid, "name": pollutant})
    # Legacy boolean: True if a CO2 row was JUST created by this pass.
    emission_array_created = _CO2_POLLUTANT in pollutants_to_add and any_factor_present

    # Default inert EmissionZone covering EVERY pollutant in use.
    # Required by gtopt's ``System::expand_fuel_emission_sources``
    # (returns early when ``emission_zone_array`` is empty).  No cap /
    # price / pool — the zone is purely a hook so the per-generator
    # EmissionSource rows are synthesized.  Users overlay cap /
    # cap_cost / price via a standard JSON merge to make it bite.
    # The zone NAME is "global_ghg" when it covers multiple
    # pollutants, "global_co2" when CO2-only (back-compat).
    zone_array: list[dict[str, Any]] = system.setdefault("emission_zone_array", [])
    zone_already_covers: set[str] = set()
    for zone in zone_array:
        if not isinstance(zone, dict):
            continue
        for ef in zone.get("emissions", []) or []:
            if not isinstance(ef, dict):
                continue
            tag = str(ef.get("emission", "")).strip().lower()
            if tag:
                zone_already_covers.add(tag)
    has_co2_zone = _CO2_POLLUTANT in zone_already_covers
    uncovered_pollutants = sorted(pollutants_in_use - zone_already_covers)
    emission_zone_created = False
    if uncovered_pollutants and any_factor_present:
        max_zone_uid = max(
            (
                int(zone.get("uid", 0) or 0)
                for zone in zone_array
                if isinstance(zone, dict)
            ),
            default=0,
        )
        zone_name = (
            _DEFAULT_CO2_ZONE_NAME
            if uncovered_pollutants == [_CO2_POLLUTANT]
            else "global_ghg"
        )
        zone_array.append(
            {
                "uid": max_zone_uid + 1,
                "name": zone_name,
                "emissions": [
                    {"emission": p, "weight": 1.0} for p in uncovered_pollutants
                ],
            }
        )
        emission_zone_created = True

    report = EmissionReport(
        source_path=defaults.source_path,
        fuels_factor_added=tuple(added),
        fuels_factor_preserved=tuple(preserved),
        fuels_unknown=tuple(unknown),
        emission_array_created=emission_array_created,
        emission_array_already_present=has_co2_pollutant,
        emission_zone_created=emission_zone_created,
        emission_zone_already_present=has_co2_zone,
    )
    logger.info(
        "Emission defaults: %d added, %d preserved, %d unknown "
        "(co2 pollutant row: %s, co2 zone: %s) — source %s",
        len(added),
        len(preserved),
        len(unknown),
        "created"
        if emission_array_created
        else ("already present" if has_co2_pollutant else "absent (no factors)"),
        "created"
        if emission_zone_created
        else ("already present" if has_co2_zone else "absent (no factors)"),
        defaults.source_path,
    )
    if report_path is not None:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        with open(report_path, "w", encoding="utf-8") as f:
            json.dump(report.to_dict(), f, indent=2, sort_keys=False)
    return report


def apply_emission_defaults_from_file(
    planning: dict[str, Any],
    source_path: Path | None,
    *,
    report_path: Path | None = None,
) -> EmissionReport:
    """Load ``source_path`` and apply it in one call.

    Convenience for the CLI hooks in plp2gtopt / plexos2gtopt writers.
    """
    defaults = load_emission_defaults(source_path)
    return apply_emission_defaults(planning, defaults, report_path=report_path)
