"""Builder: ``c_sys5_hy_cascading_turbine_energy`` variant.

The PowerSimulations.jl test case wires three hydro units into a
chain (``HydroUnit1 → HydroUnit2 → HydroUnit3``) using a flat
upstream-adjacency table.  gtopt models this via four primitives:

* ``Junction``  — one per chain node, plus a terminal drain.
* ``Waterway`` — one per upstream→downstream arc.
* ``Reservoir`` — storage tank attached to the head junction of each
  hydro plant.
* ``Turbine``  — couples a waterway flow to a Generator's MW output
  via a production factor.

The bundled ``gen.csv`` lists three hydro plants (``HydroDispatch1``,
``HydroDispatch2``, ``HydroDispatch3``) and the
``Hydro_Upstream_Input.csv`` rewrites those as ``HydroUnit{1..3}``
for the cascade variant.  We pair them by index: ``HydroUnit1`` →
``HydroDispatch1``, etc.  Each plant carries a small reservoir
(initial fill = half of MWh capacity) and a turbine with unit
production factor so the LP can route water either through the
turbine (producing power) or via a parallel spill waterway.
"""

from __future__ import annotations

from typing import Any

from sienna_to_gtopt._common import (
    is_hydro,
    make_bus_array,
    make_demand_array,
    make_planning_options,
    make_simulation,
    make_thermal_generator_array,
)
from sienna_to_gtopt._reader import SiennaCase

# Per-plant volumetric capacity (m³) and production factor (MWh/m³).
# Chosen so each plant's energy capacity ~ 8 hours × pmax — small
# enough that the LP visibly trades water storage vs. spill within
# the 24-block horizon.
DEFAULT_PROD_FACTOR = 1.0
DEFAULT_INFLOW_PER_BLOCK = 5.0  # m³/h baseline inflow at the head junction
DEFAULT_RESERVOIR_HOURS = 8.0  # storage = hours × pmax / pf
DEFAULT_SPILL_FMAX = 1.0e6  # spill waterway is unconstrained


def _sorted_hydro_chain(
    hydro_gens: list[Any],
    upstream: dict[str, list[str]],
) -> list[Any]:
    """Order hydro generators upstream→downstream using the adjacency table.

    The CSV maps synthetic names (``HydroUnit1..3``) to chain
    positions; we project that ordering onto the actual generator
    rows (``HydroDispatch1..3``) by index.
    """

    # Topological order of HydroUnit names: headwaters first.
    pending = list(upstream.keys())
    ordered: list[str] = []
    placed: set[str] = set()
    safety = 0
    while pending and safety < 100:
        safety += 1
        next_pending = []
        for unit in pending:
            if all(u in placed for u in upstream.get(unit, [])):
                ordered.append(unit)
                placed.add(unit)
            else:
                next_pending.append(unit)
        pending = next_pending

    # If the CSV ordering already matches a topological chain
    # (HydroUnit1, HydroUnit2, HydroUnit3) we end up with that exact
    # order.  Project chain index → hydro_gens index by sorting on the
    # numeric suffix of the unit name when present, else by input order.
    def idx_of(unit_name: str) -> int:
        digits = "".join(c for c in unit_name if c.isdigit())
        return int(digits) - 1 if digits else 0

    by_idx = sorted(
        range(len(hydro_gens)),
        key=lambda gi: idx_of(ordered[gi]) if gi < len(ordered) else gi,
    )
    return [hydro_gens[i] for i in by_idx]


def build_cascading_hydro(case: SiennaCase) -> dict[str, Any]:
    """Emit the gtopt JSON for the cascading-hydro variant."""

    hydro_gens = [g for g in case.generators if is_hydro(g)]
    if not hydro_gens:
        raise ValueError("cascading_hydro variant requires HYDRO generators")
    hydro_chain = _sorted_hydro_chain(hydro_gens, case.hydro_upstream)
    n_chain = len(hydro_chain)

    # Thermal generators first; their UIDs occupy [1, n_thermal].
    gen_array, gen_uid = make_thermal_generator_array(case.generators)

    # Per-plant hydro generators after, on the same bus as the plant.
    junction_uid = 1
    waterway_uid = 1
    reservoir_uid = 1
    turbine_uid = 1
    flow_uid = 1
    hydro_gen_uids: list[int] = []
    junction_array: list[dict[str, Any]] = []
    waterway_array: list[dict[str, Any]] = []
    reservoir_array: list[dict[str, Any]] = []
    turbine_array: list[dict[str, Any]] = []
    flow_array: list[dict[str, Any]] = []

    # Build chain junctions: 1 per plant + 1 terminal drain.
    plant_junction_uid: dict[str, int] = {}
    for plant in hydro_chain:
        junction_array.append({"uid": junction_uid, "name": f"j_{plant.name}"})
        plant_junction_uid[plant.name] = junction_uid
        junction_uid += 1
    drain_uid = junction_uid
    junction_array.append({"uid": drain_uid, "name": "j_ocean", "drain": True})
    junction_uid += 1

    # Inflow at the head junction (first plant in the chain).  Simple
    # baseline inflow so the LP isn't trivially water-limited.
    flow_array.append(
        {
            "uid": flow_uid,
            "name": f"inflow_{hydro_chain[0].name}",
            "direction": 1,
            "junction": plant_junction_uid[hydro_chain[0].name],
            "discharge": float(DEFAULT_INFLOW_PER_BLOCK),
        }
    )
    flow_uid += 1

    # Per-plant: reservoir + turbine waterway (j_plant → j_next) +
    # parallel spill waterway (same junctions, no turbine).
    for idx, plant in enumerate(hydro_chain):
        j_head = plant_junction_uid[plant.name]
        if idx + 1 < n_chain:
            j_tail = plant_junction_uid[hydro_chain[idx + 1].name]
        else:
            j_tail = drain_uid

        cap_mwh = float(plant.pmax_mw) * DEFAULT_RESERVOIR_HOURS
        reservoir_array.append(
            {
                "uid": reservoir_uid,
                "name": f"rsv_{plant.name}",
                "junction": j_head,
                "capacity": cap_mwh,
                "emin": 0.0,
                "emax": cap_mwh,
                "eini": cap_mwh / 2.0,
            }
        )
        reservoir_uid += 1

        # Turbine waterway carries flow at MWh/m³ = production_factor.
        # Its fmax is the volumetric equivalent of pmax (so the turbine
        # can run at full output).
        waterway_array.append(
            {
                "uid": waterway_uid,
                "name": f"ww_turb_{plant.name}",
                "junction_a": j_head,
                "junction_b": j_tail,
                "fmin": 0.0,
                "fmax": float(plant.pmax_mw) / DEFAULT_PROD_FACTOR,
            }
        )
        turb_ww_uid = waterway_uid
        waterway_uid += 1

        # Parallel spill waterway — unconstrained, no turbine.
        waterway_array.append(
            {
                "uid": waterway_uid,
                "name": f"ww_spill_{plant.name}",
                "junction_a": j_head,
                "junction_b": j_tail,
                "fmin": 0.0,
                "fmax": DEFAULT_SPILL_FMAX,
            }
        )
        waterway_uid += 1

        # Hydro generator attached to this plant.
        gen_array.append(
            {
                "uid": gen_uid,
                "name": plant.name,
                "bus": int(plant.bus_id),
                "gcost": 0.0,  # hydro is free at the meter; storage is the cost
                "capacity": float(plant.pmax_mw),
            }
        )
        hydro_gen_uids.append(gen_uid)
        gen_uid += 1

        turbine_array.append(
            {
                "uid": turbine_uid,
                "name": f"t_{plant.name}",
                "waterway": turb_ww_uid,
                "generator": hydro_gen_uids[-1],
                "production_factor": DEFAULT_PROD_FACTOR,
            }
        )
        turbine_uid += 1

    system: dict[str, Any] = {
        "name": "SiennaC5HyCascade",
        "bus_array": make_bus_array(case.buses),
        "demand_array": make_demand_array(case.buses),
        "generator_array": gen_array,
        "junction_array": junction_array,
        "waterway_array": waterway_array,
        "flow_array": flow_array,
        "reservoir_array": reservoir_array,
        "turbine_array": turbine_array,
    }

    return {
        "options": make_planning_options(use_kirchhoff=False),
        "simulation": make_simulation(),
        "system": system,
    }
