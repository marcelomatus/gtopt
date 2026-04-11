# -*- coding: utf-8 -*-

"""Parser for the RoR-as-reservoirs equivalence CSV file.

The ``--ror-as-reservoirs-file`` option accepts a CSV with a per-central
vmax override that turns selected ``pasada`` / ``serie`` centrals into
**daily-cycle reservoirs** (mirroring the ESS DCMod=2 regulation-tank
pattern used by ``battery_writer.get_regulation_reservoirs``).

CSV schema
----------

Required columns:

* ``name`` — central name as it appears in ``plpcnfce.dat`` (exact
  case-sensitive match).
* ``vmax_hm3`` — daily-cycle capacity in hm³.  Must be strictly positive.
  Only centrals whose vmax is known are listed here — that is the whole
  point of the equivalence file.

Optional columns:

* ``enabled`` — ``true`` (default) / ``false``.  A row with
  ``enabled=false`` is silently skipped, letting callers maintain a
  master list while disabling specific centrals without deleting
  rows.  Accepted truthy values: ``1``, ``true``, ``t``, ``yes``,
  ``y``, ``on`` (case-insensitive).  Everything else is falsy.
* ``comment`` — free-form annotation, ignored by the parser.

Any additional columns are silently ignored so the file can double as
a worksheet for analysts.

Usage::

    from plp2gtopt.ror_equivalence_parser import parse_ror_equivalence_file
    spec = parse_ror_equivalence_file(Path("ror_equivalence.csv"))
    # -> {"CentralA": 1.23, "CentralB": 0.45}

The function raises ``FileNotFoundError`` when the file is missing and
``ValueError`` for schema / validation errors (missing column, empty
name, non-positive / non-numeric ``vmax_hm3``, duplicate name).  Error
messages include the 1-based line number to make broken rows easy to
find.
"""

from __future__ import annotations

import csv
from pathlib import Path
from typing import Dict, Iterable


_REQUIRED_COLUMNS: tuple[str, ...] = ("name", "vmax_hm3")
_TRUTHY: frozenset[str] = frozenset({"1", "true", "t", "yes", "y", "on"})


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


def _parse_vmax(raw: str, *, name: str, line: int) -> float:
    """Coerce *raw* to a strictly positive ``float``.

    Raises ``ValueError`` with a descriptive message including the
    central name and 1-based line number when the value is missing,
    non-numeric, or non-positive.
    """
    text = (raw or "").strip()
    if not text:
        raise ValueError(f"line {line}: missing vmax_hm3 for central '{name}'")
    try:
        value = float(text)
    except ValueError as exc:
        raise ValueError(
            f"line {line}: non-numeric vmax_hm3 '{raw}' for central '{name}'"
        ) from exc
    if value <= 0.0:
        raise ValueError(
            f"line {line}: vmax_hm3 must be > 0 for central '{name}' (got {value})"
        )
    return value


def parse_ror_equivalence_file(path: Path) -> Dict[str, float]:
    """Parse a RoR equivalence CSV into a ``{name: vmax_hm3}`` dict.

    Args:
        path: Path to the CSV file.

    Returns:
        Ordered mapping from central name to daily-cycle vmax [hm³].
        Rows with ``enabled=false`` are omitted.

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


def _rows_to_spec(reader: Iterable[Dict[str, str]], *, source: str) -> Dict[str, float]:
    """Drain *reader* into a validated ``{name: vmax_hm3}`` dict.

    Split out from ``parse_ror_equivalence_file`` so tests that build a
    ``csv.DictReader`` over an ``io.StringIO`` can exercise the same
    validation path without writing to disk.
    """
    spec: Dict[str, float] = {}
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
        spec[name] = _parse_vmax(row.get("vmax_hm3", ""), name=name, line=offset)
    return spec


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
