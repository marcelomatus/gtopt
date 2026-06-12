# -*- coding: utf-8 -*-

"""RoR-as-reservoirs equivalence CSV parsing and daily-cycle expansion.

This module provides the reusable core for promoting run-of-river (pasada /
serie) centrals to daily-cycle reservoirs:

* :class:`RorSpec` — per-central parameters (vmax, production factor).
* :func:`parse_ror_equivalence_file` — read and validate the CSV whitelist.
* :func:`parse_ror_selection` — parse the CLI ``--ror-as-reservoirs`` value.

The ``plp2gtopt`` pipeline imports these and adds PLP-specific logic
(:func:`~plp2gtopt.ror_equivalence_parser.resolve_ror_reservoir_spec`,
:func:`~plp2gtopt.ror_equivalence_parser.pasada_unscale_map`) that depends on
the full parsed PLP central data.

CSV schema
----------

Required columns:

* ``name`` — central name (exact case-sensitive match).
* ``vmax_hm3`` — daily-cycle capacity in hm³ (strictly positive).
* ``production_factor`` — turbine production factor in MW/(m³/s)
  (strictly positive).

Optional columns:

* ``enabled`` — ``true`` / ``false`` (default ``true``).
* ``pmax_mw`` — expected installed capacity in MW (sanity cross-check).
* ``description`` / ``comment`` — free-form annotation (ignored).
"""

from __future__ import annotations

import csv
import json
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable

_REQUIRED_COLUMNS: tuple[str, ...] = ("name", "vmax_hm3", "production_factor")
_TRUTHY: frozenset[str] = frozenset({"1", "true", "t", "yes", "y", "on"})

#: Relative tolerance for the optional ``pmax_mw`` cross-check.
PMAX_MATCH_TOL: float = 0.05

#: Path to the bundled default ``ror_equivalence.csv`` template that ships
#: with the ``gtopt_expand`` package.
DEFAULT_ROR_CSV: Path = (
    Path(__file__).resolve().parent / "templates" / "ror_equivalence.csv"
)


@dataclass(frozen=True, slots=True)
class RorSpec:
    """Per-central RoR-as-reservoir parameters parsed from the CSV.

    Attributes:
        vmax_hm3: Daily-cycle reservoir capacity in hm³.
        production_factor: Turbine production factor in MW/(m³/s).
        pmax_mw: Optional expected turbine installed capacity in MW
            (sanity cross-check only).
    """

    vmax_hm3: float
    production_factor: float
    pmax_mw: float | None = None


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------
def _is_enabled(raw: str | None) -> bool:
    """Return True when *raw* names a truthy boolean value.

    ``None`` / empty string default to True so the ``enabled`` column is
    truly optional.
    """
    if raw is None:
        return True
    value = raw.strip().lower()
    if not value:
        return True
    return value in _TRUTHY


def _parse_positive_float(raw: str, *, name: str, line: int, field: str) -> float:
    """Coerce *raw* to a strictly positive ``float``."""
    text = (raw or "").strip()
    if not text:
        raise ValueError(f"line {line}: missing {field} for central '{name}'")
    try:
        value = float(text)
    except ValueError as exc:
        raise ValueError(
            f"line {line}: non-numeric {field} '{raw}' for central '{name}'"
        ) from exc
    if value <= 0.0:
        raise ValueError(
            f"line {line}: {field} must be > 0 for central '{name}' (got {value})"
        )
    return value


def _parse_optional_positive_float(
    raw: str | None, *, name: str, line: int, field: str
) -> float | None:
    """Coerce *raw* to a strictly positive ``float`` or ``None`` if blank."""
    if raw is None:
        return None
    text = raw.strip()
    if not text:
        return None
    try:
        value = float(text)
    except ValueError as exc:
        raise ValueError(
            f"line {line}: non-numeric {field} '{raw}' for central '{name}'"
        ) from exc
    if value <= 0.0:
        raise ValueError(
            f"line {line}: {field} must be > 0 for central '{name}' (got {value})"
        )
    return value


def _rows_to_spec(
    reader: Iterable[Dict[str, str]], *, source: str
) -> Dict[str, RorSpec]:
    """Drain *reader* into a validated ``{name: RorSpec}`` dict."""
    spec: Dict[str, RorSpec] = {}
    for offset, row in enumerate(reader, start=2):
        name = (row.get("name") or "").strip()
        if not name:
            raise ValueError(f"{source}: line {offset}: empty name column")
        if not _is_enabled(row.get("enabled")):
            continue
        if name in spec:
            raise ValueError(
                f"{source}: line {offset}: duplicate central name '{name}'"
            )
        vmax = _parse_positive_float(
            row.get("vmax_hm3", ""), name=name, line=offset, field="vmax_hm3"
        )
        prod = _parse_positive_float(
            row.get("production_factor", ""),
            name=name,
            line=offset,
            field="production_factor",
        )
        pmax = _parse_optional_positive_float(
            row.get("pmax_mw"),
            name=name,
            line=offset,
            field="pmax_mw",
        )
        spec[name] = RorSpec(
            vmax_hm3=vmax,
            production_factor=prod,
            pmax_mw=pmax,
        )
    return spec


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------
def parse_ror_equivalence_file(path: str | Path) -> Dict[str, RorSpec]:
    """Parse a RoR equivalence CSV into a ``{name: RorSpec}`` dict.

    Args:
        path: Path to the CSV file.

    Returns:
        Ordered mapping from central name to its :class:`RorSpec`.
        Rows with ``enabled=false`` are omitted.

    Raises:
        FileNotFoundError: If the file does not exist.
        ValueError: On any schema / validation failure.
    """
    p = Path(path)
    if not p.is_file():
        raise FileNotFoundError(f"ROR equivalence file not found: {p}")

    with p.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        fieldnames = reader.fieldnames or []
        missing = [c for c in _REQUIRED_COLUMNS if c not in fieldnames]
        if missing:
            raise ValueError(
                f"{p}: missing required column(s) {missing}; got {list(fieldnames)}"
            )
        return _rows_to_spec(reader, source=str(p))


def parse_ror_selection(raw: str | None) -> frozenset[str] | None:
    """Parse the ``--ror-as-reservoirs`` CLI value.

    Returns:
        * ``None`` — feature disabled.
        * empty ``frozenset`` — sentinel for ``"all"``.
        * non-empty ``frozenset[str]`` — explicit list of central names.
    """
    if raw is None:
        return None
    text = raw.strip()
    if not text or text.lower() == "none":
        return None
    if text.lower() == "all":
        return frozenset()
    names = {tok.strip() for tok in text.split(",") if tok.strip()}
    if not names:
        raise ValueError(
            f"--ror-as-reservoirs: empty selection (got {raw!r}); "
            f"use 'all', 'none', or a comma-separated list of names"
        )
    return frozenset(names)


def expand_ror_from_file(
    csv_path: str | Path,
    planning_path: str | Path,
    selection: str | None = "all",
) -> dict[str, Any]:
    """Expand RoR equivalence CSV against a planning JSON.

    Reads the planning JSON to discover eligible serie/pasada centrals,
    matches them against the CSV whitelist, and emits a summary of
    which centrals would be promoted and their reservoir parameters.

    Parameters
    ----------
    csv_path:
        Path to the RoR equivalence CSV.
    planning_path:
        Path to the gtopt planning JSON (must contain
        ``system.generator_array``).
    selection:
        ``"all"``, ``"none"``, or comma-separated central names.

    Returns
    -------
    dict
        ``{"promoted": [{name, vmax_hm3, production_factor, pmax_mw}, ...]}``
    """
    sel = parse_ror_selection(selection)
    if sel is None:
        return {"promoted": []}

    whitelist = parse_ror_equivalence_file(Path(csv_path))

    if sel:
        unknown = sorted(sel - whitelist.keys())
        if unknown:
            raise ValueError(f"central(s) not in whitelist CSV: {unknown}")
        requested = {name: whitelist[name] for name in sel}
    else:
        requested = dict(whitelist)

    # Read the planning JSON to identify eligible centrals
    with open(planning_path, "r", encoding="utf-8") as fh:
        planning = json.load(fh)

    system = planning.get("system", {})
    generators = system.get("generator_array", [])
    gen_names = {g.get("name", "") for g in generators}

    promoted: list[dict[str, Any]] = []
    for name, spec in requested.items():
        if name not in gen_names:
            continue
        entry: dict[str, Any] = {
            "name": name,
            "vmax_hm3": spec.vmax_hm3,
            "production_factor": spec.production_factor,
        }
        if spec.pmax_mw is not None:
            entry["pmax_mw"] = spec.pmax_mw
        promoted.append(entry)

    return {"promoted": promoted}
