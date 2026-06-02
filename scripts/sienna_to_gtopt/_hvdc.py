"""Builder: ``c_sys5_dc`` (HVDC link) variant.

PowerSimulations.jl ships ``c_sys5_dc`` as the 5-bus case with one
branch upgraded to an ``HVDCLine`` (``TwoTerminalHVDCLine`` in newer
releases).  gtopt's mechanism is structural: a line with no
``reactance`` value is excluded from the Kirchhoff voltage-law row
assembly (see ``source/planning_lp.cpp`` line 132 — ``if
(!line.reactance.has_value()) continue;``) and is therefore a pure
flow-capacity element, exactly the DC-link contract.

For provenance we also set ``Line.type = "dc"`` on the marked branch
— this string is purely informational (see ``include/gtopt/line.hpp``
line 85: "Optional line type tag (e.g. 'ac', 'dc', 'transformer')")
but makes the JSON self-documenting.
"""

from __future__ import annotations

from typing import Any

from sienna_to_gtopt._common import (
    make_bus_array,
    make_demand_array,
    make_planning_options,
    make_simulation,
    make_thermal_generator_array,
)
from sienna_to_gtopt._reader import SiennaBranch, SiennaCase


def _line_dict_ac(branch: SiennaBranch, uid: int) -> dict[str, Any]:
    return {
        "uid": uid,
        "name": branch.uid,
        "type": "ac",
        "bus_a": int(branch.from_bus),
        "bus_b": int(branch.to_bus),
        "tmax_ab": float(branch.rate),
        "tmax_ba": float(branch.rate),
        # Per-unit reactance — voltage omitted (matches the IEEE
        # benchmark convention used elsewhere in this test suite).
        "reactance": float(branch.x),
    }


def _line_dict_dc(branch: SiennaBranch, uid: int) -> dict[str, Any]:
    return {
        "uid": uid,
        "name": branch.uid,
        "type": "dc",
        "bus_a": int(branch.from_bus),
        "bus_b": int(branch.to_bus),
        "tmax_ab": float(branch.rate),
        "tmax_ba": float(branch.rate),
        # No `reactance` — gtopt skips Kirchhoff row assembly for
        # this line (planning_lp.cpp:132).
    }


def build_hvdc(case: SiennaCase, dc_branch_uid: str | None = None) -> dict[str, Any]:
    """Emit the gtopt JSON for the HVDC-link variant.

    Parameters
    ----------
    case
        Parsed 5-bus case bundle.
    dc_branch_uid
        Optional CSV ``UID`` of the branch to convert to HVDC.
        Defaults to the first row of ``branch.csv``.
    """

    if not case.branches:
        raise ValueError("hvdc variant requires at least one branch")
    if dc_branch_uid is None:
        dc_branch_uid = case.branches[0].uid

    if not any(b.x > 0.0 for b in case.branches):
        raise ValueError(
            "hvdc variant requires at least one non-zero reactance "
            "(can't tell DC apart from AC otherwise)"
        )

    gen_array, _ = make_thermal_generator_array(case.generators)
    line_array: list[dict[str, Any]] = []
    uid = 1
    found_dc = False
    for branch in case.branches:
        if branch.uid == dc_branch_uid:
            line_array.append(_line_dict_dc(branch, uid))
            found_dc = True
        else:
            line_array.append(_line_dict_ac(branch, uid))
        uid += 1

    if not found_dc:
        raise ValueError(f"DC branch UID {dc_branch_uid!r} not found in case")

    system: dict[str, Any] = {
        "name": "SiennaC5Dc",
        "bus_array": make_bus_array(case.buses),
        "demand_array": make_demand_array(case.buses),
        "generator_array": gen_array,
        "line_array": line_array,
    }
    # The HVDC variant exists to exercise the KVL skip path, so we
    # enable Kirchhoff explicitly.
    return {
        "options": make_planning_options(use_kirchhoff=True),
        "simulation": make_simulation(),
        "system": system,
    }
