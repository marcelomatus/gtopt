"""Parse the Sienna 5-bus CSVs / YAMLs into typed records.

Field names in the source files follow the PowerSystems.jl
``user_descriptors.yaml`` convention — ``custom_name`` is the CSV
column header, ``name`` is the canonical PSY field.  We use the YAML
purely as documentation; the converter pins its own column names
because the file ships uncomented in the bundle and may evolve
upstream.
"""

from __future__ import annotations

import csv
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any


def _strip(value: str) -> str:
    return value.strip().strip('"').strip("'")


def _float_or(value: str, default: float = 0.0) -> float:
    try:
        return float(_strip(value))
    except (TypeError, ValueError):
        return default


def _int_or(value: str, default: int = 0) -> int:
    try:
        return int(float(_strip(value)))
    except (TypeError, ValueError):
        return default


@dataclass
class SiennaBus:
    """One row from ``bus.csv``."""

    bus_id: int
    name: str
    base_kv: float
    bus_type: str  # "PV", "REF", "PQ"
    mw_load: float
    area: int


@dataclass
class SiennaBranch:
    """One row from ``branch.csv``."""

    uid: str
    from_bus: int
    to_bus: int
    r: float
    x: float
    b: float
    rate: float
    tap: float


@dataclass
class SiennaGen:
    """One row from ``gen.csv`` (column names verbatim from the bundle)."""

    name: str
    bus_id: int
    unit_type: str  # CT / CC / ST / ROR / HY
    fuel: str  # NATURAL_GAS / COAL / HYDRO
    category: str  # gas_ct / gas_cc / coal / hydro_ror / hydro_reservoir
    pmax_mw: float
    pmin_mw: float
    var_cost: float  # $/MWh — first-segment marginal cost (see parse_generators)
    vom: float


def parse_buses(path: Path) -> list[SiennaBus]:
    """Read ``bus.csv``."""

    out: list[SiennaBus] = []
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            if not row or not row.get("Bus ID"):
                continue
            out.append(
                SiennaBus(
                    bus_id=_int_or(row["Bus ID"]),
                    name=_strip(row.get("Bus Name", "")),
                    base_kv=_float_or(row.get("BaseKV", "")),
                    bus_type=_strip(row.get("Bus Type", "")),
                    mw_load=_float_or(row.get("MW Load", "")),
                    area=_int_or(row.get("Area", "1")),
                )
            )
    return out


def parse_branches(path: Path) -> list[SiennaBranch]:
    """Read ``branch.csv``."""

    out: list[SiennaBranch] = []
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            uid = _strip(row.get("UID", ""))
            if not uid:
                continue
            out.append(
                SiennaBranch(
                    uid=uid,
                    from_bus=_int_or(row.get("From Bus", "")),
                    to_bus=_int_or(row.get("To Bus", "")),
                    r=_float_or(row.get("R", "")),
                    x=_float_or(row.get("X", "")),
                    b=_float_or(row.get("B", "")),
                    rate=_float_or(row.get("Cont Rating", "")),
                    tap=_float_or(row.get("Tr Ratio", "")),
                )
            )
    return out


def parse_generators(path: Path) -> list[SiennaGen]:
    """Read ``gen.csv``."""

    out: list[SiennaGen] = []
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            name = _strip(row.get("GEN UID", ""))
            if not name:
                continue
            # The gen.csv carries a 3-knot piecewise linear cost
            # curve: var_cost_0 = no-load anchor ($/h), var_cost_1 =
            # marginal $/MWh on segment 1, var_cost_2 = marginal on
            # segment 2.  For LP-relaxation purposes we use the
            # first non-zero marginal (var_cost_1, else fall back
            # to var_cost_0 then VOM).
            vc1 = _float_or(row.get("var_cost_1", ""), default=-1.0)
            vc0 = _float_or(row.get("var_cost_0", ""), default=-1.0)
            vom = _float_or(row.get("VOM", ""))
            if vc1 > 0.0:
                var_cost = vc1
            elif vc0 > 0.0:
                var_cost = vc0
            else:
                var_cost = vom
            out.append(
                SiennaGen(
                    name=name,
                    bus_id=_int_or(row.get("Bus ID", "")),
                    unit_type=_strip(row.get("Unit Type", "")),
                    fuel=_strip(row.get("Fuel", "")),
                    category=_strip(row.get("Category", "")),
                    pmax_mw=_float_or(row.get("PMax MW", "")),
                    pmin_mw=_float_or(row.get("PMin MW", "")),
                    var_cost=var_cost,
                    vom=vom,
                )
            )
    return out


def parse_hydro_upstream(path: Path) -> dict[str, list[str]]:
    """Return ``{downstream: [upstream, ...]}`` from ``Hydro_Upstream_Input.csv``.

    Source CSV format::

        hydrounit,upstream
        HydroUnit1,
        HydroUnit2,(HydroUnit1)
        HydroUnit3,(HydroUnit2)

    The ``upstream`` cell is comma-separated, parenthesised; an empty
    cell means "headwater" (no upstream).
    """

    out: dict[str, list[str]] = {}
    with path.open(newline="") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            unit = _strip(row.get("hydrounit", ""))
            if not unit:
                continue
            up_raw = _strip(row.get("upstream", "")).strip("()")
            ups = (
                [_strip(piece) for piece in up_raw.split(",") if _strip(piece)]
                if up_raw
                else []
            )
            out[unit] = ups
    return out


def parse_user_descriptors(path: Path) -> dict[str, dict[str, str]]:
    """Return a minimal mapping of ``{section: {custom_name: name}}``.

    The full Sienna ``user_descriptors.yaml`` schema is rich (per-field
    units, unit systems, etc.).  For the converter we only need the
    name-to-canonical-PSY-field map so a future column rename in
    upstream doesn't silently break parsing.  We avoid pulling in
    PyYAML — a tiny hand-rolled parser handles the flat dash-bullet
    structure of the file.
    """

    sections: dict[str, dict[str, str]] = {}
    current: dict[str, str] | None = None
    with path.open() as fh:
        for raw_line in fh:
            line = raw_line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if not line.startswith(" ") and line.rstrip().endswith(":"):
                section_name = line.rstrip(":").strip()
                current = {}
                sections[section_name] = current
                continue
            if current is None:
                continue
            stripped = line.lstrip()
            if not stripped.startswith("- "):
                continue
            inner = stripped[2:].strip().lstrip("{").rstrip("}")
            entries: dict[str, str] = {}
            for kv in inner.split(","):
                if ":" not in kv:
                    continue
                k, v = kv.split(":", 1)
                entries[k.strip()] = v.strip().rstrip(",")
            custom = entries.get("custom_name")
            name = entries.get("name")
            if custom and name:
                current[custom] = name
    return sections


def to_any_dict(records: list[Any]) -> list[dict[str, Any]]:
    """Convenience: serialize a list of dataclass rows to plain dicts."""

    out: list[dict[str, Any]] = []
    for rec in records:
        if hasattr(rec, "__dataclass_fields__"):
            out.append({f: getattr(rec, f) for f in rec.__dataclass_fields__})
        else:
            out.append(dict(rec))
    return out


@dataclass
class SiennaCase:
    """Pre-parsed 5-bus case bundle."""

    buses: list[SiennaBus] = field(default_factory=list)
    branches: list[SiennaBranch] = field(default_factory=list)
    generators: list[SiennaGen] = field(default_factory=list)
    hydro_upstream: dict[str, list[str]] = field(default_factory=dict)


def load_case(case_dir: Path) -> SiennaCase:
    """Parse all CSVs in a Sienna case directory into a ``SiennaCase``."""

    return SiennaCase(
        buses=parse_buses(case_dir / "bus.csv"),
        branches=parse_branches(case_dir / "branch.csv"),
        generators=parse_generators(case_dir / "gen.csv"),
        hydro_upstream=parse_hydro_upstream(case_dir / "Hydro_Upstream_Input.csv"),
    )
