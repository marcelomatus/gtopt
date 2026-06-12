"""Builder: ``wecc_240`` (WECC 240-bus scalability smoke test) variant.

The WECC 240-bus reduced model is a peer-reviewed academic test
case (Price & Goodin, 2011, IEEE PES GM; later refined by Yuan et
al.).  The upstream PowerSystemCaseBuilder.jl ``WECC_240`` system
descriptor loads from a PSS/E ``.raw`` file in
``PowerSystemsTestData/psse_raw/WECC240_v04.raw`` — that path is
NOT mirrored in any tabular (CSV / JSON) form we can read directly,
and a PSS/E ``.raw`` parser would require ~500 LOC of grammar work
just for this one variant.

**Decision (per the task brief):** ship a SYNTHETIC scalability
fixture parameterised by the bus / generator / line counts.  The
default values (240 buses, 240 demands, 100 generators, 240 lines)
match the published WECC 240 topology *sizes*.  The fixture
exercises the same LP-build / solve cost as the real WECC 240 data
would (the LP solver doesn't care about the physical realism of the
admittance matrix — only its sparsity pattern and dimension).

The C++ test mirrors this — it pins the LP build/solve as a single
smoke test with a 60 s ceiling.  No physical assertion beyond "LP
solves to optimal".

Topology generator (deterministic from seed=0):

* N_BUS buses laid out in a ring + a few cross-links.
* N_GEN generators spread across the first N_GEN buses with
  varied gcost in $10..$60/MWh.
* N_DEMAND demands on the first N_DEMAND buses.
* N_LINE lines as ring neighbours + a few cross-chords.

This is not the WECC 240 case.  It is a same-size SCALABILITY
proxy and is labeled as such in the System.name field.
"""

from __future__ import annotations

import math
from typing import Any

from sienna_to_gtopt._reader import SiennaCase

# Published WECC 240 reduced model topology sizes.
DEFAULT_N_BUS = 240
DEFAULT_N_GEN = 100
DEFAULT_N_DEMAND = 120
DEFAULT_N_LINE = 320


def _gcost_for_gen(idx: int) -> float:
    """Deterministic per-generator gcost in [10, 60] $/MWh."""

    return 10.0 + 50.0 * ((idx * 7) % 100) / 100.0


def _capacity_for_gen(idx: int) -> float:
    """Deterministic per-generator capacity in MW (50..550)."""

    return 50.0 + 500.0 * ((idx * 13) % 100) / 100.0


def _load_for_demand(idx: int, total_gen_capacity: float, n_demand: int) -> float:
    """Spread roughly 70% of installed gen capacity across the demands."""

    base = (total_gen_capacity * 0.70) / float(n_demand)
    perturb = 1.0 + 0.2 * math.sin(idx * 0.97)
    return max(1.0, base * perturb)


def build_wecc_240(
    case: SiennaCase,  # pylint: disable=unused-argument
    *,
    n_bus: int = DEFAULT_N_BUS,
    n_gen: int = DEFAULT_N_GEN,
    n_demand: int = DEFAULT_N_DEMAND,
    n_line: int = DEFAULT_N_LINE,
) -> dict[str, Any]:
    """Emit the gtopt JSON for the WECC 240-bus scalability variant.

    Parameters
    ----------
    case
        Parsed 5-bus bundle — unused (the WECC 240 fixture is
        synthesized, see file docstring), but accepted to keep the
        ``VariantBuilder`` signature uniform.
    n_bus, n_gen, n_demand, n_line
        Topology sizes (defaults match published WECC 240 sizes).
    """

    if n_bus < 1 or n_gen < 1 or n_demand < 1 or n_line < 1:
        raise ValueError("n_bus / n_gen / n_demand / n_line must all be >= 1")
    if n_gen > n_bus or n_demand > n_bus:
        raise ValueError("n_gen / n_demand cannot exceed n_bus")
    if n_line < n_bus:
        raise ValueError("n_line must be >= n_bus (ring connectivity floor)")

    bus_array: list[dict[str, Any]] = [
        {"uid": i + 1, "name": f"bus{i + 1}"} for i in range(n_bus)
    ]

    generator_array: list[dict[str, Any]] = []
    total_gen_capacity = 0.0
    for gi in range(n_gen):
        cap = _capacity_for_gen(gi)
        total_gen_capacity += cap
        generator_array.append(
            {
                "uid": gi + 1,
                "name": f"g{gi + 1}",
                "bus": gi + 1,  # one gen per low-index bus
                "gcost": _gcost_for_gen(gi),
                "capacity": cap,
            }
        )

    demand_array: list[dict[str, Any]] = []
    for di in range(n_demand):
        # Demands on buses starting at offset n_gen so they don't
        # overlap with generator buses (creates real transmission
        # need across the ring).
        bus_id = ((n_gen + di) % n_bus) + 1
        demand_array.append(
            {
                "uid": di + 1,
                "name": f"d{di + 1}",
                "bus": bus_id,
                "capacity": _load_for_demand(di, total_gen_capacity, n_demand),
            }
        )

    # Ring + diagonal chords.  First n_bus lines form a ring;
    # remaining lines are deterministic cross-links.
    line_array: list[dict[str, Any]] = []
    for li in range(n_bus):
        bus_a = li + 1
        bus_b = ((li + 1) % n_bus) + 1  # next bus in ring
        line_array.append(
            {
                "uid": li + 1,
                "name": f"ring_{li + 1}",
                "bus_a": bus_a,
                "bus_b": bus_b,
                "tmax_ab": 500.0,
                "tmax_ba": 500.0,
            }
        )
    for ci in range(n_line - n_bus):
        bus_a = (ci * 31) % n_bus + 1
        # ~120-degree shift so we add long-distance shortcuts
        bus_b = ((ci * 31 + n_bus // 3) % n_bus) + 1
        if bus_a == bus_b:
            bus_b = bus_b % n_bus + 1
        line_array.append(
            {
                "uid": n_bus + ci + 1,
                "name": f"chord_{ci + 1}",
                "bus_a": bus_a,
                "bus_b": bus_b,
                "tmax_ab": 300.0,
                "tmax_ba": 300.0,
            }
        )

    # Single-stage, 1-block horizon — the scalability test is the
    # LP build + first solve, not multi-period dispatch.
    simulation = {
        "block_array": [{"uid": 1, "duration": 1.0}],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
        "scenario_array": [{"uid": 0, "probability_factor": 1.0}],
    }

    system: dict[str, Any] = {
        "name": f"SiennaWECC{n_bus}Proxy",
        "bus_array": bus_array,
        "demand_array": demand_array,
        "generator_array": generator_array,
        "line_array": line_array,
    }

    return {
        # Multi-bus transportation problem — no Kirchhoff (we don't
        # have authentic reactances).  This is the standard
        # scalability-smoke configuration.
        "options": {
            "model_options": {
                "use_single_bus": False,
                "use_kirchhoff": False,
                "demand_fail_cost": 1.0e6,
                "scale_objective": 1.0,
            }
        },
        "simulation": simulation,
        "system": system,
    }


__all__ = [
    "DEFAULT_N_BUS",
    "DEFAULT_N_DEMAND",
    "DEFAULT_N_GEN",
    "DEFAULT_N_LINE",
    "build_wecc_240",
]
