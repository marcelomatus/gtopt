# -*- coding: utf-8 -*-
# SPDX-License-Identifier: BSD-3-Clause
"""Overlay heat-rate / fuel data from a plexos2gtopt JSON onto a plp2gtopt case.

Driven by ``plp2gtopt --plexos-overlay PATH``.  ``PATH`` is either the
gtopt JSON file emitted by plexos2gtopt or the directory containing it
(in which case the JSON file is auto-located).

Why this exists:

* PLP carries a single ``gcost`` per generator (marginal cost in $/MWh).
* PLEXOS carries ``heat_rate`` (scalar or piecewise), a ``Fuel`` FK with
  ``price``, plus auxiliary derate and CO₂ plumbing.

When the plp2gtopt long-term case is fed into gtopt SDDP, the per-fuel
budget / emission accounting and the dispatch-vs-fuel-burn coupling need
the PLEXOS shape.  This module merges those fields onto the PLP-derived
Generators (and synthesizes the referenced Fuel elements) without
introducing any integer / commitment primitive.

Field allowlist
---------------

Per :class:`Generator` (continuous only):

* ``heat_rate`` (scalar)
* ``heat_rate_segments`` + ``pmax_segments`` (piecewise)
* ``fuel`` (FK by name)
* ``lossfactor`` (PLEXOS Auxiliary Use)
* ``gcost`` (scalar VO&M-only) — replaced only when BOTH the PLP value
  and the PLEXOS value are scalars; a PLP per-stage profile is preserved.

Per :class:`Fuel`:

* ``price``, ``heat_content``
* ``emission_factors`` (CO₂ combustion / upstream rates)
* ``max_offtake`` + ``max_offtake_cost`` (continuous soft cap)
* ``min_offtake`` + ``min_offtake_cost`` (only if present in source —
  per the ``no-invented-min-offtake`` invariant: we never derive a floor
  the source does not ship).

The forbidden-key invariant from :mod:`tests.test_no_integer_variables`
is enforced defensively: any commitment / startup / min-up-down attribute
is stripped from the overlaid Generator even if it leaks through.

Conflict policy: **PLEXOS wins** for the allowlisted fields.  Unmatched
PLP-only and PLEXOS-only Generators are preserved as-is and reported.
"""

from __future__ import annotations

import json
import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

logger = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Allowlists / forbidden lists
# ---------------------------------------------------------------------------

#: Continuous Generator fields ported from the PLEXOS overlay.
_GENERATOR_OVERLAY_FIELDS: tuple[str, ...] = (
    "heat_rate",
    "heat_rate_segments",
    "pmax_segments",
    "fuel",
    "lossfactor",
    "gcost",
)

#: Continuous Fuel fields ported from the PLEXOS overlay.
_FUEL_OVERLAY_FIELDS: tuple[str, ...] = (
    "price",
    "heat_content",
    "emission_factors",
    "max_offtake",
    "max_offtake_cost",
    "min_offtake",
    "min_offtake_cost",
)

#: Fields that MUST NEVER appear on an overlaid Generator — these
#: turn a continuous LP into a MIP (commitment / startup / UC state).
#: Mirrors ``_FORBIDDEN_NESTED_KEYS`` in
#: ``tests/test_no_integer_variables.py``.
_FORBIDDEN_FIELDS: frozenset[str] = frozenset(
    {
        "commitment",
        "integer_expmod",
        "startup_cost",
        "shutdown_cost",
        "max_starts",
        "min_starts",
        "max_starts_window",
        "min_uptime",
        "min_downtime",
        "uini",
        "initial_units",
    }
)


# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class OverlayReport:
    """Summary of what the overlay touched.

    Written to ``--plexos-overlay-report`` (default
    ``<output-dir>/plexos_overlay_report.json``) so the user can audit
    which PLP generators picked up PLEXOS heat-rate / fuel data and which
    were left alone.
    """

    source_path: str
    matched: tuple[str, ...] = ()
    unmatched_plp_only: tuple[str, ...] = ()
    unmatched_plexos_only: tuple[str, ...] = ()
    fuels_added: tuple[str, ...] = ()
    fuels_reused: tuple[str, ...] = ()
    skipped_fields: dict[str, list[str]] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        """Return a JSON-serializable dict view of the report."""
        return {
            "source_path": self.source_path,
            "summary": {
                "matched": len(self.matched),
                "unmatched_plp_only": len(self.unmatched_plp_only),
                "unmatched_plexos_only": len(self.unmatched_plexos_only),
                "fuels_added": len(self.fuels_added),
                "fuels_reused": len(self.fuels_reused),
                "skipped_field_entries": sum(
                    len(v) for v in self.skipped_fields.values()
                ),
            },
            "matched": list(self.matched),
            "unmatched_plp_only": list(self.unmatched_plp_only),
            "unmatched_plexos_only": list(self.unmatched_plexos_only),
            "fuels_added": list(self.fuels_added),
            "fuels_reused": list(self.fuels_reused),
            "skipped_fields": {
                name: list(fields) for name, fields in self.skipped_fields.items()
            },
        }


# ---------------------------------------------------------------------------
# Name normalization
# ---------------------------------------------------------------------------


def _normalize(name: str) -> str:
    """Normalize a generator / fuel name for cross-source matching.

    PLEXOS and PLP do not agree on capitalization, spacing, or any of
    the separators ``" "`` / ``"_"`` / ``"-"``.  Strip ALL of them and
    upper-case so e.g. ``"Ralco U1"`` (PLEXOS), ``"ralco-u1"`` (PLP),
    and ``"RALCO_U1"`` all collapse to ``"RALCOU1"``.

    Caveat: this is an aggressive normalization — two genuinely
    distinct names that differ only by separator placement (e.g.
    ``"RALCOU1"`` vs ``"RALCO_U1"``) collide.  In the CEN PCP case the
    PLEXOS-side names are per-unit (``RALCO_U1``) and PLP-side names
    are aggregate (``RALCO``), so per-unit→aggregate mapping is a
    separate problem that the report surfaces as
    ``unmatched_plexos_only``.  Users who need a richer name map should
    pre-aggregate the PLEXOS source before pointing
    ``--plexos-overlay`` at it.
    """
    out = name.strip().upper()
    for sep in (" ", "_", "-"):
        out = out.replace(sep, "")
    return out


# ---------------------------------------------------------------------------
# Path resolution
# ---------------------------------------------------------------------------


#: Cache registry of recent plexos2gtopt runs (mirror of
#: ``plexos2gtopt.plexos2gtopt.PLEXOS_RUN_REGISTRY``).  Kept in two
#: places — instead of importing from plexos2gtopt — so plp2gtopt has
#: no dependency on plexos2gtopt at all (the registry is a
#: filesystem-only contract).  If the location ever changes, update
#: BOTH constants in lockstep.
_PLEXOS_RUN_REGISTRY: Path = (
    Path.home() / ".cache" / "gtopt" / "plexos2gtopt" / "runs.jsonl"
)


def _resolve_latest_run() -> Path:
    """Resolve the ``--plexos-overlay latest`` sentinel.

    Reads the LAST line of the plexos2gtopt run registry and returns
    the absolute output_file path of that run.  Each successful
    ``plexos2gtopt`` conversion appends one JSONL row to the registry
    (see ``plexos2gtopt.plexos2gtopt._record_plexos_run``).

    Raises FileNotFoundError with a self-explanatory message when:
      * the registry file does not exist (no plexos2gtopt run has
        happened on this machine yet — or all runs were under a
        different ``HOME``),
      * the registry is empty,
      * the most recent run's ``output_file`` no longer exists on
        disk (output dir deleted / scratch dir cleaned up).
    """
    if not _PLEXOS_RUN_REGISTRY.exists():
        raise FileNotFoundError(
            "--plexos-overlay latest: no plexos2gtopt run registry at "
            f"{_PLEXOS_RUN_REGISTRY}.  Run plexos2gtopt at least once "
            "first, or pass an explicit path."
        )
    last_row: dict[str, Any] | None = None
    with open(_PLEXOS_RUN_REGISTRY, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                last_row = json.loads(line)
            except json.JSONDecodeError:
                # Skip malformed rows; the registry is append-only and
                # a partially-written line at the tail (process killed
                # mid-write) shouldn't poison the whole resolver.
                continue
    if last_row is None:
        raise FileNotFoundError(
            f"--plexos-overlay latest: registry {_PLEXOS_RUN_REGISTRY} "
            "exists but has no valid entries."
        )
    out_file_str = last_row.get("output_file")
    if not isinstance(out_file_str, str):
        raise FileNotFoundError(
            "--plexos-overlay latest: most recent registry entry is "
            "missing the 'output_file' key."
        )
    out_file = Path(out_file_str)
    if not out_file.exists():
        raise FileNotFoundError(
            f"--plexos-overlay latest: most recent registry entry points "
            f"at {out_file}, which no longer exists.  Re-run plexos2gtopt "
            "or pass an explicit path."
        )
    return out_file


def _resolve_overlay_path(path: Path) -> Path:
    """Resolve --plexos-overlay PATH to the gtopt JSON file.

    Accepts:
      * a ``.json`` file directly,
      * a directory (prefers ``<dir>/<dir.name>.json``, the
        plexos2gtopt naming convention; falls back to any single
        top-level ``*.json``; raises on ambiguity),
      * the literal sentinel ``"latest"`` (resolves to the most
        recent plexos2gtopt run via :func:`_resolve_latest_run`).
    """
    # "latest" sentinel — the literal Path("latest") doesn't exist on
    # disk, so the regular file/dir branches would fall through to the
    # "no such file" error.  Catch it here BEFORE the disk checks.
    if str(path) == "latest":
        return _resolve_latest_run()
    if path.is_file():
        return path
    if path.is_dir():
        preferred = path / f"{path.name}.json"
        if preferred.exists():
            return preferred
        candidates = sorted(p for p in path.glob("*.json"))
        if not candidates:
            raise FileNotFoundError(
                f"--plexos-overlay {path}: no .json file in directory"
            )
        if len(candidates) == 1:
            return candidates[0]
        names = ", ".join(c.name for c in candidates)
        raise FileNotFoundError(
            f"--plexos-overlay {path}: multiple .json files; point to "
            f"one explicitly. Candidates: {names}"
        )
    raise FileNotFoundError(f"--plexos-overlay {path}: no such file or directory")


# ---------------------------------------------------------------------------
# Overlay engine
# ---------------------------------------------------------------------------


class PlexosOverlay:
    """In-memory view of a plexos2gtopt gtopt-JSON source ready to overlay.

    Indexes generators and fuels by their normalized name so a single
    :meth:`apply` pass can do O(1) lookups against the plp2gtopt
    planning dict.
    """

    def __init__(self, source: dict[str, Any], source_path: Path) -> None:
        self.source = source
        self.source_path = source_path
        plexos_sys = source.get("system", {}) or {}
        self._plexos_gens_by_name: dict[str, dict[str, Any]] = {
            _normalize(str(g.get("name", ""))): g
            for g in plexos_sys.get("generator_array", []) or []
            if g.get("name")
        }
        self._plexos_fuels_by_name: dict[str, dict[str, Any]] = {
            _normalize(str(f.get("name", ""))): f
            for f in plexos_sys.get("fuel_array", []) or []
            if f.get("name")
        }

    # ----- public ----------------------------------------------------------

    def apply(self, planning: dict[str, Any]) -> OverlayReport:
        """Overlay heat-rate / fuel data onto ``planning`` in place.

        Returns a :class:`OverlayReport` summarizing what was touched.
        """
        system = planning.setdefault("system", {})
        gens: list[dict[str, Any]] = system.setdefault("generator_array", [])
        fuels: list[dict[str, Any]] = system.setdefault("fuel_array", [])

        plp_by_norm: dict[str, dict[str, Any]] = {
            _normalize(str(g.get("name", ""))): g for g in gens if g.get("name")
        }

        matched: list[str] = []
        skipped_fields: dict[str, list[str]] = {}
        needed_fuel_names: set[str] = set()

        # 1) Generator overlay
        for plp_norm, gen in plp_by_norm.items():
            plexos_gen = self._plexos_gens_by_name.get(plp_norm)
            if plexos_gen is None:
                continue
            if self._overlay_one_generator(gen, plexos_gen, skipped_fields):
                matched.append(str(gen.get("name", plp_norm)))
            fuel_ref = gen.get("fuel")
            if isinstance(fuel_ref, str):
                needed_fuel_names.add(_normalize(fuel_ref))

        # 2) Fuel overlay / synthesis
        existing_fuels_by_norm: dict[str, dict[str, Any]] = {
            _normalize(str(f.get("name", ""))): f for f in fuels if f.get("name")
        }
        max_uid = max(
            (int(f.get("uid", 0) or 0) for f in fuels if isinstance(f, dict)),
            default=0,
        )
        fuels_added: list[str] = []
        fuels_reused: list[str] = []
        for need_norm in sorted(needed_fuel_names):
            plexos_fuel = self._plexos_fuels_by_name.get(need_norm)
            if plexos_fuel is None:
                # Generator points at a fuel name that's not in the
                # PLEXOS source: skip silently — the FK will resolve
                # against whatever the planning already has, or gtopt
                # will warn at LP-build time.
                continue
            plexos_fuel_name = str(plexos_fuel.get("name", ""))
            if need_norm in existing_fuels_by_norm:
                target = existing_fuels_by_norm[need_norm]
                self._overlay_one_fuel(target, plexos_fuel)
                fuels_reused.append(str(target.get("name", plexos_fuel_name)))
                continue
            max_uid += 1
            new_entry: dict[str, Any] = {"uid": max_uid, "name": plexos_fuel_name}
            self._overlay_one_fuel(new_entry, plexos_fuel)
            fuels.append(new_entry)
            existing_fuels_by_norm[need_norm] = new_entry
            fuels_added.append(plexos_fuel_name)

        # 3) Bookkeeping for the report
        unmatched_plp_norms = sorted(set(plp_by_norm) - set(self._plexos_gens_by_name))
        unmatched_plexos_norms = sorted(
            set(self._plexos_gens_by_name) - set(plp_by_norm)
        )
        unmatched_plp = tuple(
            str(plp_by_norm[n].get("name", n)) for n in unmatched_plp_norms
        )
        unmatched_plexos = tuple(
            str(self._plexos_gens_by_name[n].get("name", n))
            for n in unmatched_plexos_norms
        )

        return OverlayReport(
            source_path=str(self.source_path),
            matched=tuple(matched),
            unmatched_plp_only=unmatched_plp,
            unmatched_plexos_only=unmatched_plexos,
            fuels_added=tuple(fuels_added),
            fuels_reused=tuple(fuels_reused),
            skipped_fields=skipped_fields,
        )

    # ----- internal --------------------------------------------------------

    @staticmethod
    def _overlay_one_generator(
        plp_gen: dict[str, Any],
        plexos_gen: dict[str, Any],
        skipped: dict[str, list[str]],
    ) -> bool:
        """Apply the allowlisted fields from ``plexos_gen`` onto ``plp_gen``.

        Returns ``True`` when at least one field was overlaid.
        """
        name = str(plp_gen.get("name", "?"))
        applied = False

        has_segments = (
            "heat_rate_segments" in plexos_gen and "pmax_segments" in plexos_gen
        )
        if has_segments:
            plp_gen["heat_rate_segments"] = list(plexos_gen["heat_rate_segments"])
            plp_gen["pmax_segments"] = list(plexos_gen["pmax_segments"])
            plp_gen.pop("heat_rate", None)
            applied = True
        elif "heat_rate" in plexos_gen:
            hr = plexos_gen["heat_rate"]
            if isinstance(hr, (int, float)):
                plp_gen["heat_rate"] = float(hr)
                plp_gen.pop("heat_rate_segments", None)
                plp_gen.pop("pmax_segments", None)
                applied = True
            else:
                # Non-scalar (Parquet field-file reference): first pass
                # does not copy field files — record and skip.
                skipped.setdefault(name, []).append("heat_rate (non-scalar)")

        if "fuel" in plexos_gen:
            plp_gen["fuel"] = plexos_gen["fuel"]
            applied = True

        if "lossfactor" in plexos_gen:
            lf = plexos_gen["lossfactor"]
            if isinstance(lf, (int, float)):
                plp_gen["lossfactor"] = float(lf)
                applied = True
            else:
                skipped.setdefault(name, []).append("lossfactor (non-scalar)")

        if "gcost" in plexos_gen:
            gc = plexos_gen["gcost"]
            existing = plp_gen.get("gcost")
            if isinstance(gc, (int, float)) and isinstance(existing, (int, float)):
                plp_gen["gcost"] = float(gc)
                applied = True
            elif not isinstance(gc, (int, float)):
                # Non-scalar PLEXOS gcost is unusual (plexos2gtopt emits
                # scalar) — record and skip.
                skipped.setdefault(name, []).append("gcost (non-scalar)")
            else:
                # PLP side carries a per-stage profile; preserving it
                # wins over a scalar overlay.
                skipped.setdefault(name, []).append("gcost (plp profile preserved)")

        # Defensive guard: never let a commitment / UC attribute land on
        # a plp Generator — keeps the no-integer invariant.
        for forbidden in _FORBIDDEN_FIELDS:
            if forbidden in plp_gen:
                plp_gen.pop(forbidden, None)
                skipped.setdefault(name, []).append(f"{forbidden} (forbidden)")

        return applied

    @staticmethod
    def _overlay_one_fuel(target: dict[str, Any], plexos_fuel: dict[str, Any]) -> None:
        """Copy allowlisted Fuel fields from ``plexos_fuel`` onto ``target``."""
        for key in _FUEL_OVERLAY_FIELDS:
            if key in plexos_fuel:
                value = plexos_fuel[key]
                if isinstance(value, list):
                    # Defensive deep copy for nested structures (emission_factors).
                    target[key] = json.loads(json.dumps(value))
                else:
                    target[key] = value


# ---------------------------------------------------------------------------
# Public entry points
# ---------------------------------------------------------------------------


def load_plexos_overlay(path: Path) -> PlexosOverlay:
    """Load the plexos2gtopt gtopt-JSON case from ``path``.

    ``path`` may be the JSON file or a directory holding it (see
    :func:`_resolve_overlay_path` for the resolution rule).
    """
    json_path = _resolve_overlay_path(path)
    with open(json_path, encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ValueError(
            f"--plexos-overlay {json_path}: expected a JSON object, "
            f"got {type(data).__name__}"
        )
    return PlexosOverlay(data, json_path)


def apply_plexos_overlay(
    planning: dict[str, Any],
    source_path: Path,
    *,
    report_path: Path | None = None,
) -> OverlayReport:
    """Load + apply the PLEXOS overlay onto ``planning`` in place.

    Writes the report JSON to ``report_path`` when given.  Returns the
    in-memory :class:`OverlayReport` so callers can also log / inspect.
    """
    overlay = load_plexos_overlay(source_path)
    report = overlay.apply(planning)
    logger.info(
        "PLEXOS overlay: %d matched, %d plp-only, %d plexos-only, "
        "%d fuels added, %d fuels reused (source: %s)",
        len(report.matched),
        len(report.unmatched_plp_only),
        len(report.unmatched_plexos_only),
        len(report.fuels_added),
        len(report.fuels_reused),
        report.source_path,
    )
    if report_path is not None:
        report_path.parent.mkdir(parents=True, exist_ok=True)
        with open(report_path, "w", encoding="utf-8") as f:
            json.dump(report.to_dict(), f, indent=2, sort_keys=False)
    return report
