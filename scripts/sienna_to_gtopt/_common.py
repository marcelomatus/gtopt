"""Shared helpers across the three Sienna variant builders.

Each builder turns a parsed ``SiennaCase`` (or a subset thereof) into
a gtopt planning JSON dict.  The simulation skeleton (1 stage / 24-h
block / 1 scenario) and the bus/demand arrays are identical across
the three variants, so we centralise them here.
"""

from __future__ import annotations

from typing import Any

from sienna_to_gtopt._reader import SiennaBus, SiennaGen


# Single 24-block, 1-stage, 1-scenario simulation skeleton.  All three
# variants are LP-relaxation dispatch tests; 24 blocks gives us a
# day-ahead horizon without the per-block overhead of a full year.
DEFAULT_HORIZON_HOURS = 24


def make_simulation(hours: int = DEFAULT_HORIZON_HOURS) -> dict[str, Any]:
    return {
        "block_array": [{"uid": h + 1, "duration": 1.0} for h in range(hours)],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": hours}],
        "scenario_array": [{"uid": 0, "probability_factor": 1.0}],
    }


def make_bus_array(buses: list[SiennaBus]) -> list[dict[str, Any]]:
    return [{"uid": int(b.bus_id), "name": b.name or f"bus{b.bus_id}"} for b in buses]


def make_demand_array(buses: list[SiennaBus]) -> list[dict[str, Any]]:
    """One Demand per load-bearing bus, capacity = MW Load."""

    out: list[dict[str, Any]] = []
    uid = 1
    for b in buses:
        if b.mw_load <= 0.0:
            continue
        out.append(
            {
                "uid": uid,
                "name": f"load_{b.name or b.bus_id}",
                "bus": int(b.bus_id),
                "capacity": float(b.mw_load),
            }
        )
        uid += 1
    return out


def is_hydro(gen: SiennaGen) -> bool:
    """``True`` for any HYDRO-fuelled unit (ROR / dispatch / reservoir)."""

    return gen.fuel.upper() == "HYDRO"


def make_thermal_generator_array(
    generators: list[SiennaGen],
    skip_hydro: bool = True,
    next_uid_start: int = 1,
) -> tuple[list[dict[str, Any]], int]:
    """Build the gtopt generator_array for the thermal subset.

    Returns ``(generator_array, next_uid_after)`` so the caller can
    continue numbering hydro/aggregate generators without colliding.
    """

    out: list[dict[str, Any]] = []
    uid = next_uid_start
    for gen in generators:
        if skip_hydro and is_hydro(gen):
            continue
        out.append(
            {
                "uid": uid,
                "name": gen.name,
                "bus": int(gen.bus_id),
                "gcost": float(gen.var_cost or gen.vom),
                "capacity": float(gen.pmax_mw),
            }
        )
        uid += 1
    return out, uid


def make_planning_options(
    use_kirchhoff: bool, demand_fail_cost: float = 1000.0
) -> dict[str, Any]:
    return {
        "model_options": {
            "use_single_bus": False,
            "use_kirchhoff": use_kirchhoff,
            "demand_fail_cost": demand_fail_cost,
            "scale_objective": 1.0,
        }
    }
