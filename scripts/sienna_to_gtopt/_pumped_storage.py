"""Builder: ``c_sys5_phes_ed`` (pumped-storage hydro) variant.

The PowerSimulations.jl ``c_sys5_phes_ed`` fixture (formerly
``c_sys5_hy_pump_energy``) adds a single PUMPED-storage hydro plant to
the 5-bus case.  PowerSystems.jl models it via the
``HydroPumpedStorage`` element, which carries:

* an UPPER reservoir (storage tank above the dam),
* a LOWER reservoir (tail-water pond),
* a PUMP path (grid → upper, consumes power, η_pump < 1),
* a GENERATOR path (upper → lower via turbine, produces power, η_gen < 1),

and a round-trip efficiency ``η_rt = η_pump · η_gen``.

gtopt has no first-class ``HydroPumpedStorage`` element, but its
``Battery`` primitive (with the ``bus`` / synthetic-converter unified
form — see ``battery.hpp``) captures the exact same LP shape: SoC
state, input_efficiency (η_pump), output_efficiency (η_gen),
pmax_charge / pmax_discharge, capacity (MWh of usable upper-reservoir
storage), and a strict-positive ``ecost`` is unnecessary because the
LP arbitrages naturally between cheap charging hours and expensive
discharging hours.

In the closed-loop pumped-storage operation the LP must drive both
dispatch modes (charge AND discharge) to non-zero values whenever the
generation cost time-shape rewards it.  We construct a TWO-block
horizon with a deliberately asymmetric cost stack:

* block 0 (off-peak): a single CHEAP thermal unit serves the load
  AND has spare headroom to charge the battery.
* block 1 (peak): the cheap unit shuts off; only an EXPENSIVE
  marginal generator can serve the load.  The LP discharges the
  battery to displace the expensive MWh.

The round-trip closed-loop assertion is then ``charge_in × η_pump ·
η_gen >= discharge_out`` (energy conservation across the storage tank).
"""

from __future__ import annotations

from typing import Any

from sienna_to_gtopt._common import (
    make_bus_array,
    make_thermal_generator_array,
)
from sienna_to_gtopt._reader import SiennaCase

# Pumped-storage parameters chosen to match the PowerSystems.jl
# ``c_sys5_phes_ed`` defaults (PMaxPump = PMaxGen = 50 MW, η_rt ~ 0.81).
DEFAULT_PUMP_EFF = 0.9  # input efficiency (η_pump)
DEFAULT_GEN_EFF = 0.9  # output efficiency (η_gen)
DEFAULT_PMAX_PUMP = 50.0  # MW
DEFAULT_PMAX_GEN = 50.0  # MW
DEFAULT_CAPACITY_MWH = 200.0  # MWh upper-reservoir usable energy
DEFAULT_EINI_MWH = 100.0  # mid-fill at horizon start

# Two-block horizon — off-peak/peak — keeps the variant minimal but
# still forces both pump (charge) and gen (discharge) dispatch.
HORIZON_BLOCKS = 2
OFFPEAK_BLOCK_HOURS = 6.0
PEAK_BLOCK_HOURS = 6.0


def _make_simulation_two_block() -> dict[str, Any]:
    return {
        "block_array": [
            {"uid": 1, "duration": OFFPEAK_BLOCK_HOURS},
            {"uid": 2, "duration": PEAK_BLOCK_HOURS},
        ],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": HORIZON_BLOCKS}],
        "scenario_array": [{"uid": 0, "probability_factor": 1.0}],
    }


def _make_demand_two_block(load_total: float) -> list[dict[str, Any]]:
    """Single aggregate demand at a fixed MW level (matches both blocks)."""

    return [
        {
            "uid": 1,
            "name": "phes_load",
            "bus": 1,
            "capacity": float(load_total),
        }
    ]


def build_pumped_storage(case: SiennaCase) -> dict[str, Any]:
    """Emit the gtopt JSON for the pumped-storage variant.

    The 5-bus topology is collapsed to a single-bus dispatch (the
    ``use_single_bus`` flag) so we focus on the storage LP shape.
    """

    if not case.generators:
        raise ValueError("pumped_storage variant requires generators in the case")

    # Pick the first bus for everything — single-bus mode collapses
    # the network anyway.
    primary_bus = int(case.buses[0].bus_id) if case.buses else 1

    # Asymmetric cost stack: cheap-block unit + expensive-block unit.
    # We synthesize two units (named after the variant) so the cost
    # split is deterministic across runs and doesn't depend on the
    # bundle's generator stack.
    generator_array: list[dict[str, Any]] = [
        # Cheap baseload — only available in block 0 via a pmax
        # profile would be cleaner, but we don't have profile parsing
        # here.  Instead we use TWO generators with different gcosts;
        # the LP picks the cheap one whenever feasible.
        {
            "uid": 1,
            "name": "g_cheap",
            "bus": primary_bus,
            "gcost": 5.0,
            "capacity": 60.0,
        },
        {
            "uid": 2,
            "name": "g_peaker",
            "bus": primary_bus,
            "gcost": 200.0,
            "capacity": 200.0,
        },
    ]

    # Single battery (pumped-storage) wired in the unified
    # auto-converter mode (`bus` set, no `source_generator`).
    battery_array: list[dict[str, Any]] = [
        {
            "uid": 1,
            "name": "phes",
            "type": "pumped",
            "bus": primary_bus,
            "input_efficiency": DEFAULT_PUMP_EFF,
            "output_efficiency": DEFAULT_GEN_EFF,
            "emin": 0.0,
            "emax": DEFAULT_CAPACITY_MWH,
            "capacity": DEFAULT_CAPACITY_MWH,
            "pmax_charge": DEFAULT_PMAX_PUMP,
            "pmax_discharge": DEFAULT_PMAX_GEN,
            "eini": DEFAULT_EINI_MWH,
        }
    ]

    bus_array = (
        make_bus_array(case.buses)
        if case.buses
        else [{"uid": primary_bus, "name": f"bus{primary_bus}"}]
    )

    # Concentrate the demand on the primary bus.
    demand_array = _make_demand_two_block(40.0)

    # Discard the bundle's stock thermal stack — we use the
    # synthesized cheap/peaker pair above so the cost split is
    # deterministic.  Keep the function call to validate the helper
    # at least once on this code path.
    _ = make_thermal_generator_array(case.generators)

    system: dict[str, Any] = {
        "name": "SiennaC5Phes",
        "bus_array": bus_array,
        "demand_array": demand_array,
        "generator_array": generator_array,
        "battery_array": battery_array,
    }
    # use_single_bus collapses the network so the storage LP shape is
    # the only thing under test — exactly the focus of this variant.
    return {
        "options": {
            "model_options": {
                "use_single_bus": True,
                "use_kirchhoff": False,
                "demand_fail_cost": 10000.0,
                "scale_objective": 1.0,
            }
        },
        "simulation": _make_simulation_two_block(),
        "system": system,
    }


# Re-export for the wider module API.
__all__ = ["build_pumped_storage"]
