# -*- coding: utf-8 -*-

"""Re-interpret hydro generator ``pmin`` as a downstream FlowRight obligation.

For some hydro centrals in the Chilean SIC, ``plpcnfce.dat`` ``pmin`` does
not encode a true minimum power output — it actually encodes a downstream
flow obligation (environmental / irrigation minimum flow).  When mapped
naively to a Generator's ``pmin``, gtopt forces ``g >= pmin`` on every
block, which can drain upstream reservoirs at low-inflow stages and cause
SDDP cascade infeasibilities (juan/gtopt_iplp p27/p28 confirmed this).

This module mutates a planning JSON in place:

1. Looks up each enabled central in :data:`DEFAULT_PMIN_CSV` against the
   planning's ``generator_array``, ``turbine_array`` and ``waterway_array``.
2. Captures the generator's current ``pmin`` value.
3. Sets the generator's ``pmin = 0.0``.
4. Appends a new ``FlowRight`` to ``system.flow_right_array`` representing
   the soft minimum-flow obligation, with ``discharge = pmin / rendi`` and
   no ``fmax`` (fixed-mode).  The right is bound to the **downstream
   junction** of the gen waterway (``junction_b``) with ``direction = -1``
   so the obligation is subtracted from that junction's physical balance,
   coupling it to the actual gen-waterway flow.

The FlowRight is a soft constraint — unmet flow incurs the system-wide
``hydro_fail_cost``, so the LP can choose to dispatch less when reservoir
headroom is not available.

Design notes
------------

* FlowRight does not have a ``waterway`` field — it can only bind to a
  ``junction`` (``OptSingleId``).  We bind to ``junction_b`` of the gen
  waterway so that the right's flow column is subtracted from the same
  junction the turbine discharges into; the only inflow to that junction
  is typically the gen waterway, making the obligation operationally
  equivalent to a "minimum flow on the gen waterway".
* When the generator's ``pmin`` is a parquet string reference (e.g.
  ``"pmin"`` → ``Generator/pmin.parquet``), gtopt_expand cannot read the
  parquet to divide by ``rendi``, so the FlowRight ``discharge`` is
  emitted as a string reference to a NEW parquet column whose name is
  the FlowRight's name; a TODO/warning is logged because plp2gtopt has
  to write that companion column for the conversion to be complete.
"""

from __future__ import annotations

import csv
import json
import logging
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable

_logger = logging.getLogger(__name__)

#: Required CSV columns for the pmin-as-FlowRight whitelist.
_REQUIRED_COLUMNS: tuple[str, ...] = ("name", "enabled")

#: Truthy values for the ``enabled`` column.
_TRUTHY: frozenset[str] = frozenset({"1", "true", "t", "yes", "y", "on"})

#: Path to the bundled default ``pmin_as_flowright.csv`` template that
#: ships with the ``gtopt_expand`` package.
DEFAULT_PMIN_CSV: Path = (
    Path(__file__).resolve().parent / "templates" / "pmin_as_flowright.csv"
)

#: Starting UID for FlowRights emitted by this transform.  Chosen well
#: above ``laja`` (2000) and ``maule`` (1000) so the three transforms
#: never collide when merged into the same system.
DEFAULT_UID_START: int = 5000

#: Suffix appended to the central name to derive the FlowRight name (and
#: the parquet-column name in the string-reference path).
_FLOW_RIGHT_SUFFIX: str = "_pmin_as_flow_right"

#: ``purpose`` field used on the emitted FlowRight (metadata only).
_FLOW_RIGHT_PURPOSE: str = "environmental"

#: ``direction`` for a consumptive (downstream) flow obligation.
_FLOW_RIGHT_DIRECTION: int = -1


# ---------------------------------------------------------------------------
# CSV parsing
# ---------------------------------------------------------------------------
@dataclass(frozen=True, slots=True)
class PminFlowRightSpec:
    """Per-central pmin-as-FlowRight whitelist entry.

    Attributes:
        name: Central name (matches Generator/Turbine ``name`` exactly).
        description: Free-form comment from the CSV (ignored, kept for
            documentation parity with the source file).
    """

    name: str
    description: str = ""


def _is_enabled(raw: str | None) -> bool:
    """Return True when *raw* names a truthy boolean value."""
    if raw is None:
        return False
    value = raw.strip().lower()
    if not value:
        return False
    return value in _TRUTHY


def _rows_to_spec(
    reader: Iterable[dict[str, str]],
    *,
    source: str,
) -> dict[str, PminFlowRightSpec]:
    """Drain *reader* into a validated ``{name: PminFlowRightSpec}`` dict."""
    spec: dict[str, PminFlowRightSpec] = {}
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
        spec[name] = PminFlowRightSpec(
            name=name,
            description=(row.get("description") or "").strip(),
        )
    return spec


def parse_pmin_flowright_file(path: str | Path) -> dict[str, PminFlowRightSpec]:
    """Parse a pmin-as-FlowRight CSV into a ``{name: PminFlowRightSpec}`` dict.

    Args:
        path: Path to the CSV file.

    Returns:
        Ordered mapping from central name to its :class:`PminFlowRightSpec`.
        Rows with ``enabled=false`` are omitted.

    Raises:
        FileNotFoundError: If the file does not exist.
        ValueError: On any schema / validation failure.
    """
    p = Path(path)
    if not p.is_file():
        raise FileNotFoundError(f"pmin-as-flowright CSV not found: {p}")

    with p.open("r", encoding="utf-8", newline="") as fh:
        reader = csv.DictReader(fh)
        fieldnames = reader.fieldnames or []
        missing = [c for c in _REQUIRED_COLUMNS if c not in fieldnames]
        if missing:
            raise ValueError(
                f"{p}: missing required column(s) {missing}; got {list(fieldnames)}"
            )
        return _rows_to_spec(reader, source=str(p))


# ---------------------------------------------------------------------------
# Planning JSON helpers
# ---------------------------------------------------------------------------
def _find_by_name(
    array: list[dict[str, Any]],
    name: str,
) -> dict[str, Any] | None:
    """Return the first dict in *array* whose ``"name"`` equals *name*."""
    for entry in array:
        if isinstance(entry, dict) and entry.get("name") == name:
            return entry
    return None


def _scale_pmin(
    pmin: Any,
    *,
    rendi: float,
    central_name: str,
) -> tuple[Any, str | None]:
    """Convert a generator's ``pmin`` value to a FlowRight ``discharge``.

    Args:
        pmin: The generator's current ``pmin`` field.  May be a scalar
            (``int`` / ``float``), a string reference to a parquet column,
            or an inline ``[stage][block]`` 2D list.
        rendi: The turbine production factor [MW/(m³/s)].
        central_name: The central name (used to derive the parquet
            string-reference path on the file-backed branch).

    Returns:
        A ``(discharge, todo)`` pair.  ``discharge`` is the value to put
        into the new FlowRight; ``todo`` is a non-empty string when the
        caller still needs to ensure a companion parquet column exists,
        ``None`` otherwise.

    Raises:
        ValueError: If ``rendi`` is not strictly positive, or if ``pmin``
            has an unsupported shape.
    """
    if rendi <= 0.0:
        raise ValueError(
            f"central '{central_name}': production_factor must be > 0 (got {rendi})"
        )

    if pmin is None:
        # No pmin to convert — the caller decides whether to skip.
        return 0.0, None

    if isinstance(pmin, bool):
        # bool is a subclass of int; treat it as a misuse.
        raise ValueError(
            f"central '{central_name}': pmin must not be a boolean (got {pmin!r})"
        )

    if isinstance(pmin, (int, float)):
        return float(pmin) / rendi, None

    if isinstance(pmin, str):
        new_col = f"{central_name}{_FLOW_RIGHT_SUFFIX}"
        todo = (
            f"central '{central_name}': pmin is a parquet reference"
            f" ('{pmin}'); the FlowRight discharge is emitted as a"
            f" reference to a NEW parquet column '{new_col}' which must"
            f" be written by plp2gtopt (currently absent)."
        )
        return new_col, todo

    if isinstance(pmin, list):
        # Generator pmin is TBRealFieldSched: a 2D [stage][block] array.
        # FlowRight discharge is STBRealFieldSched (3D), so wrap with one
        # scenario layer.
        scaled_2d: list[list[float]] = []
        for stage_idx, stage_row in enumerate(pmin):
            if not isinstance(stage_row, list):
                raise ValueError(
                    f"central '{central_name}': pmin[{stage_idx}] is not a"
                    f" list (got {type(stage_row).__name__})"
                )
            scaled_row: list[float] = []
            for block_idx, val in enumerate(stage_row):
                if not isinstance(val, (int, float)) or isinstance(val, bool):
                    raise ValueError(
                        f"central '{central_name}': pmin[{stage_idx}]"
                        f"[{block_idx}] is not numeric (got {val!r})"
                    )
                scaled_row.append(float(val) / rendi)
            scaled_2d.append(scaled_row)
        return [scaled_2d], None

    raise ValueError(
        f"central '{central_name}': unsupported pmin shape {type(pmin).__name__}"
    )


# ---------------------------------------------------------------------------
# Public expansion entrypoint
# ---------------------------------------------------------------------------
def expand_pmin_flowright(
    planning: dict[str, Any],
    *,
    csv_path: str | Path | None = None,
    uid_start: int = DEFAULT_UID_START,
) -> int:
    """Re-interpret hydro generator ``pmin`` as a FlowRight obligation.

    Mutates *planning* in place:

    * Sets each affected generator's ``pmin`` to ``0.0``.
    * Appends one new entry to ``planning["system"]["flow_right_array"]``
      per converted central.

    Args:
        planning: Parsed planning JSON (the dict produced by
            ``json.load``).  Must contain ``system.generator_array``,
            ``system.turbine_array`` and ``system.waterway_array``.
        csv_path: Optional path to the whitelist CSV; defaults to
            :data:`DEFAULT_PMIN_CSV`.
        uid_start: First UID assigned to the new FlowRights.  Successive
            entries receive ``uid_start + 1``, ``uid_start + 2``, …

    Returns:
        The number of conversions performed (i.e. how many FlowRights
        were appended).

    Raises:
        FileNotFoundError: If the CSV does not exist.
        ValueError: On CSV / planning-shape validation failures.
    """
    csv_path = Path(csv_path) if csv_path is not None else DEFAULT_PMIN_CSV
    spec = parse_pmin_flowright_file(csv_path)
    if not spec:
        return 0

    system = planning.get("system")
    if not isinstance(system, dict):
        raise ValueError("planning JSON: missing 'system' object")

    generators = system.get("generator_array")
    turbines = system.get("turbine_array")
    waterways = system.get("waterway_array")
    if not isinstance(generators, list):
        raise ValueError("planning JSON: 'system.generator_array' must be a list")
    if not isinstance(turbines, list):
        raise ValueError("planning JSON: 'system.turbine_array' must be a list")
    if not isinstance(waterways, list):
        raise ValueError("planning JSON: 'system.waterway_array' must be a list")

    # The FlowRight array may not exist yet on legacy planning JSONs —
    # create it on first use (mirrors the ror_expand "missing arrays"
    # convention of being lenient).
    flow_rights = system.setdefault("flow_right_array", [])
    if not isinstance(flow_rights, list):
        raise ValueError("planning JSON: 'system.flow_right_array' must be a list")

    uid = uid_start
    converted = 0
    for central_name in spec:
        generator = _find_by_name(generators, central_name)
        if generator is None:
            _logger.warning(
                "pmin_as_flowright: generator '%s' not found in"
                " generator_array; skipping.",
                central_name,
            )
            continue

        turbine = _find_by_name(turbines, central_name)
        if turbine is None:
            _logger.warning(
                "pmin_as_flowright: turbine '%s' not found in turbine_array; skipping.",
                central_name,
            )
            continue

        gen_waterway_name = turbine.get("waterway")
        if not isinstance(gen_waterway_name, str) or not gen_waterway_name:
            _logger.warning(
                "pmin_as_flowright: turbine '%s' has no 'waterway' field; skipping.",
                central_name,
            )
            continue

        waterway = _find_by_name(waterways, gen_waterway_name)
        if waterway is None:
            _logger.warning(
                "pmin_as_flowright: gen waterway '%s' (turbine '%s') not"
                " found in waterway_array; skipping.",
                gen_waterway_name,
                central_name,
            )
            continue

        rendi_raw = turbine.get("production_factor")
        try:
            rendi = float(rendi_raw)  # type: ignore[arg-type]
        except (TypeError, ValueError):
            _logger.warning(
                "pmin_as_flowright: turbine '%s' has non-numeric"
                " production_factor (%r); skipping.",
                central_name,
                rendi_raw,
            )
            continue
        if rendi <= 0.0:
            _logger.warning(
                "pmin_as_flowright: turbine '%s' has non-positive"
                " production_factor (%g); skipping.",
                central_name,
                rendi,
            )
            continue

        # Bind the FlowRight to the *downstream* junction of the gen
        # waterway (junction_b).  See the module docstring for the
        # "no FlowRight.waterway field" design discussion.
        junction_b = waterway.get("junction_b")
        if not isinstance(junction_b, str) or not junction_b:
            _logger.warning(
                "pmin_as_flowright: gen waterway '%s' has no 'junction_b';"
                " skipping central '%s'.",
                gen_waterway_name,
                central_name,
            )
            continue

        pmin_value = generator.get("pmin")
        if pmin_value is None:
            _logger.info(
                "pmin_as_flowright: generator '%s' has no pmin set;"
                " skipping (nothing to convert).",
                central_name,
            )
            continue

        try:
            discharge, todo = _scale_pmin(
                pmin_value, rendi=rendi, central_name=central_name
            )
        except ValueError as exc:
            _logger.warning("pmin_as_flowright: %s; skipping.", exc)
            continue
        if todo is not None:
            _logger.warning("pmin_as_flowright: TODO: %s", todo)

        # Override the generator's pmin so the LP no longer enforces
        # ``g >= pmin`` on every block.
        generator["pmin"] = 0.0

        flow_right: dict[str, Any] = {
            "uid": uid,
            "name": f"{central_name}{_FLOW_RIGHT_SUFFIX}",
            "purpose": _FLOW_RIGHT_PURPOSE,
            "junction": junction_b,
            "direction": _FLOW_RIGHT_DIRECTION,
            "discharge": discharge,
        }
        flow_rights.append(flow_right)
        uid += 1
        converted += 1

    return converted


def expand_pmin_flowright_from_file(
    planning_path: str | Path,
    output_path: str | Path,
    *,
    csv_path: str | Path | None = None,
    uid_start: int = DEFAULT_UID_START,
) -> int:
    """Read a planning JSON, run :func:`expand_pmin_flowright`, write it back.

    Args:
        planning_path: Path to the input planning JSON.
        output_path: Path where the mutated planning JSON is written
            (may be the same as ``planning_path`` for in-place edits).
        csv_path: Optional whitelist CSV path (default: bundled).
        uid_start: First UID assigned to new FlowRights.

    Returns:
        The number of conversions performed.
    """
    with open(planning_path, "r", encoding="utf-8") as fh:
        planning = json.load(fh)

    converted = expand_pmin_flowright(planning, csv_path=csv_path, uid_start=uid_start)

    out = Path(output_path)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w", encoding="utf-8") as fh:
        json.dump(planning, fh, indent=2, sort_keys=False)
        fh.write("\n")

    return converted
