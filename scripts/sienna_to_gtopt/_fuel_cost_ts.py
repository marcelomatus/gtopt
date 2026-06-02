"""Builder: ``c_sys5_re_fuel_cost`` (time-varying Fuel.price) variant.

The PowerSimulations.jl ``c_sys5_re_fuel_cost`` fixture demonstrates
TIME-SERIES fuel prices: each fueled generator's per-MWh cost varies
block-by-block as the upstream fuel price moves.  PSY models this via
the ``Generator.fuel_cost`` ``TimeSeriesData`` attribute on the
``ThermalGen`` instance.

gtopt mirrors this with the ``Fuel.price`` field, which is an
``OptTRealFieldSched`` — accepts a scalar (broadcast across stages),
a per-stage 1-D vector, or a Parquet/CSV schedule file.  For an
LP-only test we exercise the per-stage shape: a single stage with a
multi-block horizon and a per-(stage, block) fuel.price schedule
implies a per-block effective generator cost = ``fuel.price[block]
× heat_rate``.

Note on the per-block axis: ``Fuel.price`` is currently
``OptTRealFieldSched`` (stage-only, NOT per-block).  To get a
per-block effective cost we instead price the variation directly via
``Generator.gcost`` (which IS per-(stage, block) via
``OptTBRealFieldSched``).  This is the same LP shape as a per-block
fuel.price — the difference is just where the time-series lives in
the JSON.  Sienna's ``fuel_cost`` ts is functionally identical to a
``gcost`` ts (price × heat_rate = $/MWh, both feed the generator's
linear cost coefficient).

To make the test self-documenting we ALSO emit the per-stage
``Fuel.price`` for the cheapest fuel — exercising the resolver path
on the Fuel side — and the per-block ``gcost`` for the cost shape.

Topology:

* 5-bus stack from the bundle's gen.csv.
* Add a ``Fuel`` element (``"gas"``) with a non-zero price.
* Attach one of the thermal units to that Fuel via
  ``Generator.fuel`` + ``Generator.heat_rate``.
* Set a 4-block horizon with a DELIBERATELY varying ``gcost`` on
  the marginal unit so the LP merit order rotates.
"""

from __future__ import annotations

from typing import Any

from sienna_to_gtopt._common import (
    make_bus_array,
    make_demand_array,
    make_planning_options,
)
from sienna_to_gtopt._reader import SiennaCase

# 4-block horizon — block 0 has cheap gas, block 2 has expensive gas.
HORIZON_BLOCKS = 4
DEFAULT_HEAT_RATE = 8.0  # GJ/MWh — typical CCGT

# Per-block fuel price schedule [$/GJ].  The variation across blocks
# is intentional: blocks 0/3 cheap, block 2 expensive — so the test
# can assert that the LP dispatches MORE of the variable-cost unit in
# the cheap blocks than in the expensive blocks.
DEFAULT_FUEL_PRICE_PER_BLOCK = (3.0, 5.0, 12.0, 4.0)


def build_fuel_cost_ts(
    case: SiennaCase,
    *,
    heat_rate: float = DEFAULT_HEAT_RATE,
    fuel_price_per_block: tuple[float, ...] = DEFAULT_FUEL_PRICE_PER_BLOCK,
) -> dict[str, Any]:
    """Emit the gtopt JSON for the fuel-cost-ts variant.

    Parameters
    ----------
    case
        Parsed 5-bus case bundle.
    heat_rate
        Heat rate [GJ/MWh] applied to the fueled generator.
    fuel_price_per_block
        Per-block fuel price [$/GJ] — len(...) defines the horizon.
    """

    if not case.buses:
        raise ValueError("fuel_cost_ts variant requires a non-empty case")
    if not fuel_price_per_block:
        raise ValueError("fuel_price_per_block must be non-empty")
    n_blocks = len(fuel_price_per_block)

    # Per-block gcost = fuel_price × heat_rate.  We model the LP via
    # the (stage, block) gcost schedule directly — see file docstring
    # for why this is functionally identical to a per-block
    # Fuel.price.
    per_block_gcost = [
        round(float(p) * float(heat_rate), 6) for p in fuel_price_per_block
    ]

    # Use a stage-AVG fuel price for the Fuel element (so the resolver
    # path is exercised).  The marginal Generator carries the per-
    # block gcost schedule.
    stage_avg_price = sum(fuel_price_per_block) / float(len(fuel_price_per_block))

    bus_array = make_bus_array(case.buses)
    demand_array = make_demand_array(case.buses)

    # Generator stack: one fixed-cost baseload + the time-varying
    # marginal unit.  We deliberately make the marginal unit
    # *cheaper than baseload* in some blocks and *more expensive* in
    # others, so the dispatch ratio rotates across blocks.
    generator_array: list[dict[str, Any]] = [
        {
            "uid": 1,
            "name": "baseload",
            "bus": int(case.buses[0].bus_id),
            "gcost": 35.0,  # constant — somewhere in the middle of
            # the per-block gcost range (24..96)
            "capacity": 60.0,
        },
        {
            "uid": 2,
            "name": "gas_ts",
            "bus": int(case.buses[0].bus_id),
            # Per-(stage, block) gcost schedule.  daw::json expects a
            # nested list ``[[block0, block1, ...]]`` for the 1-stage
            # horizon shape.
            "gcost": [per_block_gcost],
            "fuel": 1,
            "heat_rate": float(heat_rate),
            "capacity": 100.0,
        },
    ]

    fuel_array: list[dict[str, Any]] = [
        {
            "uid": 1,
            "name": "gas",
            "price": stage_avg_price,  # stage-scalar — Fuel.price is
            # currently per-stage only
        }
    ]

    simulation = {
        "block_array": [{"uid": i + 1, "duration": 1.0} for i in range(n_blocks)],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": n_blocks}],
        "scenario_array": [{"uid": 0, "probability_factor": 1.0}],
    }
    system: dict[str, Any] = {
        "name": "SiennaC5ReFuelCostTs",
        "bus_array": bus_array,
        "demand_array": demand_array,
        "generator_array": generator_array,
        "fuel_array": fuel_array,
    }

    options = make_planning_options(use_kirchhoff=False, demand_fail_cost=1000.0)
    options["model_options"]["use_single_bus"] = True

    return {
        "options": options,
        "simulation": simulation,
        "system": system,
    }


__all__ = ["build_fuel_cost_ts", "DEFAULT_FUEL_PRICE_PER_BLOCK", "DEFAULT_HEAT_RATE"]
