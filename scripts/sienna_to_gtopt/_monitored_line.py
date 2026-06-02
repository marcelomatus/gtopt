"""Builder: ``c_sys5_ml`` (monitored-line) variant.

In PowerSimulations.jl ``MonitoredLine`` marks a single branch whose
thermal limit is enforced; sibling branches are modelled as
``Line`` (no flow cap).  gtopt mirrors this knob via
``Line.enforce_level``:

* ``2`` (default) — hard cap binds.
* ``1`` — voltage-conditional (treated as hard cap in our LP).
* ``0`` — cap not enforced; ``tmax_*`` is kept only for loss-segment
  discretization.

The bundle ships 7 branches.  By convention we pick the FIRST branch
in the CSV (``branch4`` = bus 2 → bus 3, rated 80 MW) as the
monitored line; all 6 others are emitted with ``enforce_level = 0``.
With ``use_kirchhoff = False`` the LP is a pure transportation
problem, so the test verifies the cap-binding pattern under a
controlled flow split.
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

# Tag value used by gtopt's Line.enforce_level field.
ENFORCE_HARD_CAP = 2
ENFORCE_NEVER = 0


def _line_dict(
    branch: SiennaBranch,
    uid: int,
    enforce_level: int,
    include_reactance: bool = True,
) -> dict[str, Any]:
    line: dict[str, Any] = {
        "uid": uid,
        "name": branch.uid,
        "bus_a": int(branch.from_bus),
        "bus_b": int(branch.to_bus),
        "tmax_ab": float(branch.rate),
        "tmax_ba": float(branch.rate),
        "enforce_level": int(enforce_level),
    }
    if include_reactance and branch.x > 0.0:
        # Per-unit reactance — ``voltage`` omitted so gtopt treats it
        # as already-pu (matches the SDDP/IEEE benchmark convention
        # used elsewhere in the test suite).
        line["reactance"] = float(branch.x)
    return line


def build_monitored_line(
    case: SiennaCase, monitored_branch_uid: str | None = None
) -> dict[str, Any]:
    """Emit the gtopt JSON for the monitored-line variant.

    Parameters
    ----------
    case
        Parsed 5-bus case bundle.
    monitored_branch_uid
        Optional CSV ``UID`` of the branch to monitor.  Defaults to
        the first row of ``branch.csv`` (``branch4`` in the
        upstream bundle).
    """

    if not case.branches:
        raise ValueError("monitored_line variant requires at least one branch")
    if monitored_branch_uid is None:
        monitored_branch_uid = case.branches[0].uid

    gen_array, _ = make_thermal_generator_array(case.generators)

    line_array: list[dict[str, Any]] = []
    uid = 1
    found_monitored = False
    for branch in case.branches:
        enforce = (
            ENFORCE_HARD_CAP if branch.uid == monitored_branch_uid else ENFORCE_NEVER
        )
        if branch.uid == monitored_branch_uid:
            found_monitored = True
        # We disable Kirchhoff for the monitored-line variant so the
        # LP is a pure transportation problem; reactances are
        # therefore irrelevant and we drop them to keep the JSON
        # lean.  (The DC variant exercises reactance presence /
        # absence explicitly.)
        line_array.append(_line_dict(branch, uid, enforce, include_reactance=False))
        uid += 1

    if not found_monitored:
        raise ValueError(
            f"monitored branch UID {monitored_branch_uid!r} not found in case"
        )

    system: dict[str, Any] = {
        "name": "SiennaC5Ml",
        "bus_array": make_bus_array(case.buses),
        "demand_array": make_demand_array(case.buses),
        "generator_array": gen_array,
        "line_array": line_array,
    }
    return {
        "options": make_planning_options(use_kirchhoff=False),
        "simulation": make_simulation(),
        "system": system,
    }
