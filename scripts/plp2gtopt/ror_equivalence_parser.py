# -*- coding: utf-8 -*-

"""Parser for the RoR-as-reservoirs equivalence CSV file.

The ``--ror-as-reservoirs-file`` option accepts a CSV with a per-central
vmax + production-factor override that turns selected ``pasada`` / ``serie``
centrals into **daily-cycle reservoirs** (mirroring the ESS DCMod=2
regulation-tank pattern used by ``battery_writer.get_regulation_reservoirs``).

CSV schema
----------

Required columns:

* ``name`` — central name as it appears in ``plpcnfce.dat`` (exact
  case-sensitive match).
* ``vmax_hm3`` — daily-cycle capacity in hm³.  Must be strictly positive.
  Only centrals whose vmax is known are listed here — that is the whole
  point of the equivalence file.
* ``production_factor`` — turbine production factor in MW/(m³/s).  Must
  be strictly positive.  Overrides the PLP ``plpcnfce.dat`` efficiency
  value for this central — necessary because many pasada centrals carry
  a ``1.0`` placeholder in PLP that is not physically meaningful.  The
  override is applied to the emitted ``Turbine.production_factor``.

Optional columns:

* ``enabled`` — ``true`` (default) / ``false``.  A row with
  ``enabled=false`` is silently skipped, letting callers maintain a
  master list while disabling specific centrals without deleting
  rows.  Accepted truthy values: ``1``, ``true``, ``t``, ``yes``,
  ``y``, ``on`` (case-insensitive).  Everything else is falsy.
* ``pmax_mw`` — expected turbine installed capacity in MW.  Purely a
  sanity cross-check: :func:`resolve_ror_reservoir_spec` compares
  this against the PLP ``plpcnfce.dat`` ``pmax`` of the matched
  central and logs a **warning** (not an error) when the relative
  gap exceeds 5%.  A blank / missing value disables the cross-check
  for that row.  Common trigger: the operator filling the CSV was
  looking at a different central because of a name collision or a
  stale worksheet.
* ``description`` — free-form annotation, ignored by the parser.
  Conventionally used to cite the source of the ``vmax_hm3`` value
  (operator ficha técnica URL, SEIA study reference, internal
  communication, etc.) plus any relevant notes.  ``comment`` is
  also accepted as a legacy alias.

Any additional columns are silently ignored so the file can double as
a worksheet for analysts.

Usage::

    from plp2gtopt.ror_equivalence_parser import parse_ror_equivalence_file
    spec = parse_ror_equivalence_file(Path("ror_equivalence.csv"))
    # -> {"CentralA": RorSpec(vmax_hm3=1.23, production_factor=0.85), ...}

The function raises ``FileNotFoundError`` when the file is missing and
``ValueError`` for schema / validation errors (missing column, empty
name, non-positive / non-numeric ``vmax_hm3`` or ``production_factor``,
duplicate name).  Error messages include the 1-based line number to
make broken rows easy to find.
"""

from __future__ import annotations

import csv
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, Iterable, List, Mapping


_REQUIRED_COLUMNS: tuple[str, ...] = ("name", "vmax_hm3", "production_factor")
_TRUTHY: frozenset[str] = frozenset({"1", "true", "t", "yes", "y", "on"})
_ELIGIBLE_TYPES: frozenset[str] = frozenset({"pasada", "serie"})

#: Relative tolerance for the optional ``pmax_mw`` cross-check.  When a
#: row carries a ``pmax_mw`` column, the resolver warns (does not raise)
#: if its value differs from the PLP ``plpcnfce.dat`` pmax by more than
#: this fraction — a common signal that the operator who filled the CSV
#: was looking at a different central (name collision, renamed unit,
#: stale worksheet).
_PMAX_MATCH_TOL: float = 0.05

_logger = logging.getLogger(__name__)


@dataclass(frozen=True, slots=True)
class RorSpec:
    """Per-central RoR-as-reservoir parameters parsed from the CSV.

    Attributes:
        vmax_hm3: Daily-cycle reservoir capacity in hm³.  Applied to the
            emitted reservoir's ``emin`` / ``emax`` / ``capacity`` fields.
        production_factor: Turbine production factor in MW/(m³/s).
            Overrides the PLP ``plpcnfce.dat`` efficiency when the
            central is promoted.
        pmax_mw: Optional expected turbine installed capacity in MW,
            used only as a sanity cross-check against the PLP
            ``plpcnfce.dat`` ``pmax`` of the matched central.  When
            present and the relative gap exceeds ``_PMAX_MATCH_TOL``,
            :func:`resolve_ror_reservoir_spec` emits a warning — a
            common signal that the CSV row has drifted from the PLP
            case (renamed central, name collision, stale worksheet).
            ``None`` disables the cross-check for that row.
    """

    vmax_hm3: float
    production_factor: float
    pmax_mw: float | None = None


def _is_enabled(raw: str | None) -> bool:
    """Return True when *raw* names a truthy boolean value.

    ``None`` / empty string default to True so the ``enabled`` column is
    truly optional and unset rows behave as enabled.
    """
    if raw is None:
        return True
    value = raw.strip().lower()
    if not value:
        return True
    return value in _TRUTHY


def _parse_positive_float(raw: str, *, name: str, line: int, field: str) -> float:
    """Coerce *raw* to a strictly positive ``float``.

    Raises ``ValueError`` with a descriptive message including the
    central name, field name, and 1-based line number when the value
    is missing, non-numeric, or non-positive.
    """
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
    """Coerce *raw* to a strictly positive ``float`` or ``None`` if blank.

    Unlike :func:`_parse_positive_float`, a missing / empty / ``None``
    value is accepted and returned as ``None`` (the column is optional).
    A present-but-unparseable or non-positive value is still an error —
    silent fallthrough would defeat the sanity check.
    """
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


def parse_ror_equivalence_file(path: Path) -> Dict[str, RorSpec]:
    """Parse a RoR equivalence CSV into a ``{name: RorSpec}`` dict.

    Args:
        path: Path to the CSV file.

    Returns:
        Ordered mapping from central name to its ``RorSpec`` (vmax_hm3
        and production_factor).  Rows with ``enabled=false`` are
        omitted.

    Raises:
        FileNotFoundError: If the file does not exist.
        ValueError: On any schema / validation failure.  The error
            message includes the 1-based CSV line number so broken rows
            are easy to locate.
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


def _rows_to_spec(
    reader: Iterable[Dict[str, str]], *, source: str
) -> Dict[str, RorSpec]:
    """Drain *reader* into a validated ``{name: RorSpec}`` dict.

    Split out from ``parse_ror_equivalence_file`` so tests that build a
    ``csv.DictReader`` over an ``io.StringIO`` can exercise the same
    validation path without writing to disk.
    """
    spec: Dict[str, RorSpec] = {}
    # Line 1 is the header, so data starts at line 2.
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


def resolve_ror_reservoir_spec(
    options: Mapping[str, Any],
    central_items: List[Dict[str, Any]],
) -> Dict[str, RorSpec]:
    """Resolve the ``--ror-as-reservoirs`` selection against the PLP case.

    Pure helper shared by ``JunctionWriter`` (to emit daily-cycle
    reservoirs) and ``AflceWriter`` (to un-scale the discharge inflow
    of promoted pasada centrals).  Keeping it here — next to the parser
    and the ``RorSpec`` dataclass — avoids a circular import between
    the two writers and matches the "resolve once, consume twice"
    layout of the gtopt writer pipeline.

    Args:
        options: The gtopt writer options dict.  Reads
            ``ror_as_reservoirs`` (selection string) and
            ``ror_as_reservoirs_file`` (CSV path).
        central_items: List of central dicts from the active PLP case
            (``{"name", "type", "bus", "efficiency", ...}``).  The
            resolver validates each promoted central against this list.

    Returns:
        ``{name: RorSpec}`` for every central that is
        (a) named in the selection,
        (b) present in the CSV whitelist,
        (c) of an eligible type (``pasada``/``serie``),
        (d) connected to a bus (``bus > 0``),
        (e) has a strictly positive efficiency.
        Returns an empty dict when the feature is disabled
        (no selection given).

    Raises:
        ValueError: on any validation failure — unknown name, missing
            CSV, type mismatch, ``bus<=0``, or ``efficiency<=0``.
            Error messages are actionable and name the offending central.
    """
    raw_selection = options.get("ror_as_reservoirs")
    selection = parse_ror_selection(raw_selection)
    csv_path = options.get("ror_as_reservoirs_file")

    if selection is None:
        # Feature disabled — the CSV default may still be populated by
        # the packaged template; silently ignore it.
        return {}

    if csv_path is None:
        # Programmatic callers (convert_plp_case(opts) with no argparse
        # defaults) may not populate the CSV path.  Fall back to the
        # packaged template that ships inside the plp2gtopt wheel —
        # matching the argparse default in _parsers.py so both CLI and
        # library entry-points behave identically.  Imported lazily to
        # avoid a top-level circular import with _parsers.
        from ._parsers import DEFAULT_ROR_RESERVOIRS_FILE

        csv_path = DEFAULT_ROR_RESERVOIRS_FILE

    whitelist = parse_ror_equivalence_file(Path(csv_path))

    # "all" (empty frozenset sentinel) means: every whitelist entry
    # that is eligible.  An explicit list must be fully resolvable.
    if selection:
        unknown = sorted(selection - whitelist.keys())
        if unknown:
            raise ValueError(
                f"--ror-as-reservoirs: central(s) not in whitelist "
                f"CSV '{csv_path}': {unknown}"
            )
        requested = {name: whitelist[name] for name in selection}
    else:
        requested = dict(whitelist)

    items_by_name = {c["name"]: c for c in central_items}
    resolved: Dict[str, RorSpec] = {}
    for name, spec in requested.items():
        central = items_by_name.get(name)
        if central is None:
            raise ValueError(
                f"--ror-as-reservoirs: central '{name}' listed in the "
                f"whitelist is not present in the current PLP case"
            )
        ctype = central.get("type", "")
        if ctype not in _ELIGIBLE_TYPES:
            raise ValueError(
                f"--ror-as-reservoirs: central '{name}' has type "
                f"'{ctype}', expected one of {sorted(_ELIGIBLE_TYPES)}"
            )
        if central.get("bus", 0) <= 0:
            raise ValueError(
                f"--ror-as-reservoirs: central '{name}' has bus<=0 "
                f"so no turbine would drain the reservoir"
            )
        if float(central.get("efficiency", 0.0) or 0.0) <= 0.0:
            raise ValueError(
                f"--ror-as-reservoirs: central '{name}' has "
                f"efficiency<=0; cannot drain a daily-cycle reservoir"
            )
        # Optional sanity cross-check: if the CSV carries a pmax_mw
        # column, warn when it drifts from the PLP pmax by more than
        # 5% — a common signal the row has become stale or the name
        # now resolves to a different central.
        if spec.pmax_mw is not None:
            plp_pmax = float(central.get("pmax", 0.0) or 0.0)
            if plp_pmax > 0.0:
                rel = abs(spec.pmax_mw - plp_pmax) / plp_pmax
                if rel > _PMAX_MATCH_TOL:
                    _logger.warning(
                        "--ror-as-reservoirs: pmax mismatch for '%s' — "
                        "CSV=%.3f MW vs PLP=%.3f MW (relative gap %.1f%% "
                        "> %.0f%% tolerance); check for name collision "
                        "or stale worksheet",
                        name,
                        spec.pmax_mw,
                        plp_pmax,
                        rel * 100.0,
                        _PMAX_MATCH_TOL * 100.0,
                    )
            else:
                _logger.warning(
                    "--ror-as-reservoirs: cannot cross-check pmax for "
                    "'%s' — PLP pmax is missing or zero; CSV says "
                    "pmax_mw=%.3f MW",
                    name,
                    spec.pmax_mw,
                )
        resolved[name] = spec

    _logger.info(
        "RoR-as-reservoirs: promoting %d central(s) to daily-cycle reservoirs: %s",
        len(resolved),
        sorted(resolved),
    )
    return resolved


def pasada_unscale_map(
    resolved: Mapping[str, RorSpec],
    central_items: List[Dict[str, Any]],
) -> Dict[str, float]:
    """Build the ``{name: 1/production_factor}`` map for promoted pasadas.

    PLP stores pasada discharge as ``physical_flow × real_production_factor``
    in MW-equivalent m³/s (with the plpcnfce ``efficiency`` column set to
    the placeholder ``1.0``).  To promote a pasada to a daily-cycle
    volume-reservoir, the aflce inflow must be divided by the real
    production factor so the reservoir balance is in physical m³/s,
    consistent with the ``vmax_hm3`` from the equivalence CSV.

    Serie centrals are **not** included: PLP stores serie efficiency as
    the real MW/(m³/s) and the aflce flow is already physical.

    Args:
        resolved: The ``{name: RorSpec}`` map from
            :func:`resolve_ror_reservoir_spec`.
        central_items: The PLP central dicts — used only to look up
            each promoted central's ``type``.

    Returns:
        ``{name: 1.0 / spec.production_factor}`` for each promoted
        pasada.  Empty when no pasadas are promoted.
    """
    if not resolved:
        return {}
    items_by_name = {c["name"]: c for c in central_items}
    scale: Dict[str, float] = {}
    for name, spec in resolved.items():
        central = items_by_name.get(name)
        if central is None:
            continue
        if central.get("type", "") != "pasada":
            continue
        scale[name] = 1.0 / spec.production_factor
    return scale


def parse_ror_selection(raw: str | None) -> frozenset[str] | None:
    """Parse the ``--ror-as-reservoirs`` CLI value.

    Returns:
        * ``None`` — feature disabled (caller left the flag unset or
          passed ``"none"``).
        * empty ``frozenset`` — sentinel for ``"all"`` (caller should
          promote every eligible central present in the CSV whitelist).
        * non-empty ``frozenset[str]`` — explicit list of central names.

    The parser is intentionally tolerant of whitespace and empty tokens
    (``"A, ,B"`` -> ``{"A", "B"}``) but rejects a fully empty selection
    (``""`` or ``","``) as an error — that almost always signals a shell
    quoting mistake.
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
