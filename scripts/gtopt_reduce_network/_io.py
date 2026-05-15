# SPDX-License-Identifier: BSD-3-Clause
"""Load / save gtopt JSON cases and canonicalise SingleId references.

A gtopt case is a JSON object with three top-level keys: ``options``,
``simulation``, ``system``. Inside ``system`` we care about ``bus_array``,
``line_array``, ``generator_array``, ``demand_array``, ``battery_array``,
``junction_array``, ``waterway_array``, ``turbine_array``, ``reservoir_array``
(any may be absent).

Element bus references can be either an ``int`` (uid) or a ``str`` (name); we
canonicalise to uid internally and only re-emit the original form on save.
"""

from __future__ import annotations

import copy
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from plp2gtopt.compressed_open import compressed_open

# Reference fields per element kind: (array_name, [bus-ref-field, ...]).
# Only fields that resolve to a *bus* uid go here; junction/waterway refs are
# handled by their own dedicated maps (see _components.py).
_ELEMENT_BUS_REFS: tuple[tuple[str, tuple[str, ...]], ...] = (
    ("generator_array", ("bus",)),
    ("demand_array", ("bus",)),
    ("battery_array", ("bus",)),
    ("turbine_array", ("bus",)),
    ("reserve_provision_array", ("bus",)),
)


@dataclass(slots=True)
class Case:
    """In-memory view of a gtopt JSON case with a precomputed name→uid map."""

    raw: dict[str, Any]
    bus_uid_by_name: dict[str, int] = field(default_factory=dict)
    bus_name_by_uid: dict[int, str] = field(default_factory=dict)

    @property
    def system(self) -> dict[str, Any]:
        return self.raw.setdefault("system", {})

    @property
    def options(self) -> dict[str, Any]:
        return self.raw.setdefault("options", {})

    @property
    def simulation(self) -> dict[str, Any]:
        return self.raw.setdefault("simulation", {})

    def array(self, name: str) -> list[dict[str, Any]]:
        return self.system.setdefault(name, [])

    def deepcopy(self) -> Case:
        return Case(
            raw=copy.deepcopy(self.raw),
            bus_uid_by_name=dict(self.bus_uid_by_name),
            bus_name_by_uid=dict(self.bus_name_by_uid),
        )


def load_case(path: str | Path) -> Case:
    """Load a gtopt case JSON (optionally compressed) into a Case."""
    with compressed_open(Path(path), encoding="utf-8", errors="strict") as f:
        raw = json.load(f)
    if not isinstance(raw, dict):
        raise ValueError(f"{path}: expected top-level JSON object, got {type(raw)}")
    case = Case(raw=raw)
    _index_buses(case)
    return case


def save_case(case: Case, path: str | Path, *, indent: int | None = 2) -> None:
    """Write a Case back to JSON. No automatic compression — pass a .json path."""
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    with p.open("w", encoding="utf-8") as f:
        json.dump(case.raw, f, indent=indent, ensure_ascii=False)


def _index_buses(case: Case) -> None:
    case.bus_uid_by_name.clear()
    case.bus_name_by_uid.clear()
    for bus in case.array("bus_array"):
        if "uid" not in bus:
            raise ValueError(f"bus missing uid: {bus}")
        uid = int(bus["uid"])
        case.bus_name_by_uid[uid] = str(bus.get("name", f"bus_{uid}"))
        if "name" in bus:
            name = str(bus["name"])
            if name in case.bus_uid_by_name and case.bus_uid_by_name[name] != uid:
                raise ValueError(
                    f"duplicate bus name {name!r} maps to uids "
                    f"{case.bus_uid_by_name[name]} and {uid}"
                )
            case.bus_uid_by_name[name] = uid


def resolve_bus_ref(case: Case, ref: int | str | None) -> int | None:
    """Resolve a SingleId-style bus reference (int or str) to a bus uid."""
    if ref is None:
        return None
    if isinstance(ref, bool):  # bool is int subclass; reject
        raise TypeError(f"bus ref must not be bool: {ref!r}")
    if isinstance(ref, int):
        if ref not in case.bus_name_by_uid:
            raise KeyError(f"bus uid {ref} not in bus_array")
        return ref
    if isinstance(ref, str):
        try:
            return case.bus_uid_by_name[ref]
        except KeyError as exc:
            raise KeyError(f"bus name {ref!r} not in bus_array") from exc
    raise TypeError(f"unsupported bus ref type: {type(ref)}")


def iter_bus_referencing_elements(
    case: Case,
) -> list[tuple[str, dict[str, Any], str]]:
    """Yield (array_name, element_dict, field_name) for every bus ref."""
    out: list[tuple[str, dict[str, Any], str]] = []
    for arr_name, fields in _ELEMENT_BUS_REFS:
        for elem in case.array(arr_name):
            for field_name in fields:
                if field_name in elem:
                    out.append((arr_name, elem, field_name))
    return out


# --- Schedule-field classification ----------------------------------------


def classify_schedule_field(value: Any) -> str:
    """Classify a gtopt schedule field as ``scalar``, ``array``, or ``file``.

    gtopt accepts three shapes for fields like ``pmax`` / ``lmax`` /
    ``tmax_ab``:

    * scalar number (int / float)
    * 2-D nested list ``[[values per block] per stage]``
    * filename string (resolved against ``input_directory``)

    Unknown shapes are reported as ``"unknown"``.
    """
    if isinstance(value, bool):
        return "unknown"
    if isinstance(value, (int, float)):
        return "scalar"
    if isinstance(value, str):
        return "file"
    if isinstance(value, list):
        return "array"
    return "unknown"


def aggregate_scalar(values: list[float], *, rule: str) -> float:
    """Combine a list of scalar field values according to ``rule``.

    Supported rules: ``"sum"`` (capacities), ``"max"`` (cost-like), ``"min"``
    (binding cap), ``"mean"`` (rate-like).
    """
    if not values:
        raise ValueError("cannot aggregate empty list")
    if rule == "sum":
        return float(sum(values))
    if rule == "max":
        return float(max(values))
    if rule == "min":
        return float(min(values))
    if rule == "mean":
        return float(sum(values) / len(values))
    raise ValueError(f"unknown aggregation rule: {rule!r}")
