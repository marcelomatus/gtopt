# SPDX-License-Identifier: BSD-3-Clause
"""Shared data model and helpers for validation checks.

This module contains the :class:`Finding` and :class:`Severity` data
model plus internal helper functions used by the domain-specific check
modules.
"""

from collections import defaultdict
from dataclasses import dataclass
from enum import Enum
from typing import Any


# ---------------------------------------------------------------------------
# Finding data model
# ---------------------------------------------------------------------------


class Severity(Enum):
    """Severity levels for check findings."""

    CRITICAL = "CRITICAL"
    WARNING = "WARNING"
    NOTE = "NOTE"


@dataclass
class Finding:
    """A single check finding."""

    check_id: str
    severity: Severity
    message: str
    action: str = ""


# ---------------------------------------------------------------------------
# Element class helpers
# ---------------------------------------------------------------------------

# Maps each array name to a short human-readable label.
_ELEMENT_ARRAYS: dict[str, str] = {
    "bus_array": "Bus",
    "generator_array": "Generator",
    "demand_array": "Demand",
    "line_array": "Line",
    "battery_array": "Battery",
    "converter_array": "Converter",
    "reserve_zone_array": "ReserveZone",
    "reserve_provision_array": "ReserveProvision",
    "junction_array": "Junction",
    "waterway_array": "Waterway",
    "flow_array": "Flow",
    "reservoir_array": "Reservoir",
    "reservoir_seepage_array": "ReservoirSeepage",
    "reservoir_discharge_limit_array": "ReservoirDischargeLimit",
    "reservoir_production_factor_array": "ReservoirProductionFactor",
    "turbine_array": "Turbine",
    "generator_profile_array": "GeneratorProfile",
    "demand_profile_array": "DemandProfile",
}


def _get_uid_set(sys: dict[str, Any], array_key: str) -> dict[Any, list[int]]:
    """Return a mapping of uid -> list of indices for an element array."""
    result: dict[Any, list[int]] = defaultdict(list)
    for idx, elem in enumerate(sys.get(array_key, [])):
        uid = elem.get("uid")
        if uid is not None:
            result[uid].append(idx)
    return result


def _get_name_set(sys: dict[str, Any], array_key: str) -> dict[str, list[int]]:
    """Return a mapping of name -> list of indices for an element array."""
    result: dict[str, list[int]] = defaultdict(list)
    for idx, elem in enumerate(sys.get(array_key, [])):
        name = elem.get("name")
        if name:
            result[name].append(idx)
    return result


def _resolve_bus_lookup(
    sys: dict[str, Any],
) -> tuple[set[Any], dict[str, Any]]:
    """Return (uid_set, name_to_uid) for buses."""
    uid_set: set[Any] = set()
    name_to_uid: dict[str, Any] = {}
    for bus in sys.get("bus_array", []):
        uid = bus.get("uid")
        name = bus.get("name", "")
        if uid is not None:
            uid_set.add(uid)
        if name:
            name_to_uid[name] = uid
    return uid_set, name_to_uid


def _resolve_bus_ref(
    ref: Any,
    bus_uids: set[Any],
    bus_name_to_uid: dict[str, Any],
) -> bool:
    """Return True if *ref* resolves to an existing bus (by uid or name)."""
    if ref is None:
        return True  # optional reference
    if ref in bus_uids:
        return True
    if isinstance(ref, str) and ref in bus_name_to_uid:
        return True
    return False


def _resolve_uid_ref(
    ref: Any,
    uid_set: set[Any],
    name_map: dict[str, Any],
) -> bool:
    """Return True if *ref* resolves to an existing element."""
    if ref is None:
        return True
    if ref in uid_set:
        return True
    if isinstance(ref, str) and ref in name_map:
        return True
    return False


def _build_uid_name_maps(
    sys: dict[str, Any], array_key: str
) -> tuple[set[Any], dict[str, Any]]:
    """Return (uid_set, name_to_uid) for an element array."""
    uid_set: set[Any] = set()
    name_map: dict[str, Any] = {}
    for elem in sys.get(array_key, []):
        uid = elem.get("uid")
        name = elem.get("name", "")
        if uid is not None:
            uid_set.add(uid)
        if name:
            name_map[name] = uid
    return uid_set, name_map


def _extract_scalar_values(field: Any) -> list[float]:
    """Extract numeric values from a FieldSched-style field.

    Handles: scalar, list of scalars, nested lists, or returns []
    for string (file reference) or None.
    """
    if field is None:
        return []
    if isinstance(field, (int, float)):
        return [float(field)]
    if isinstance(field, str):
        return []  # file reference
    if isinstance(field, list):
        values: list[float] = []
        for item in field:
            if isinstance(item, (int, float)):
                values.append(float(item))
            elif isinstance(item, list):
                for sub in item:
                    if isinstance(sub, (int, float)):
                        values.append(float(sub))
        return values
    return []
