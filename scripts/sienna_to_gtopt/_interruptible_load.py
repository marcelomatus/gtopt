"""Builder: ``c_sys5_il`` (Interruptible Load) variant.

The PowerSimulations.jl ``c_sys5_il`` fixture adds a single
``InterruptibleLoad`` element to the 5-bus case.  In PSY terminology
an InterruptibleLoad behaves like a regular ``PowerLoad`` except that
it can be *shed* at a per-MWh penalty (``proportional_term``).  The
load is dispatched between ``[0, max_active_power]`` rather than
forced to ``max_active_power``: the LP picks how much to actually
serve and pays a fail penalty for the unserved portion.

gtopt's native mechanism for this is exactly the ``Demand.fcost``
field: when set (>0), the per-MWh curtailment cost replaces the
global ``demand_fail_cost``, letting the LP curtail a price-sensitive
load when the marginal generator cost exceeds the load's own
willingness-to-pay.

Topology:

* Reuse the 5-bus bundle's bus_array and the thermal stack (so the
  variant exercises a representative dispatch).
* Add ONE interruptible load on bus 3 (a load-bearing bus already)
  with ``fcost`` set deliberately BELOW the cheapest generator's
  marginal cost — so the LP MUST curtail it whenever a more
  expensive generator is the only available choice.

Assertion (verified C++-side):

* When marginal gen cost < fcost ⇒ load is fully served (no
  curtailment).
* When marginal gen cost > fcost ⇒ load is curtailed exactly to the
  point where marginal serving = fcost.
"""

from __future__ import annotations

from typing import Any

from sienna_to_gtopt._common import (
    make_bus_array,
    make_planning_options,
    make_simulation,
    make_thermal_generator_array,
)
from sienna_to_gtopt._reader import SiennaCase

# Interruptible-load economics: this is a price-sensitive load that
# will be SHED whenever serving it would cost > fcost $/MWh.  We pick
# 25 $/MWh — between the cheap thermal stack ($10-15) and the more
# expensive units ($30-40) so the variant tests both regimes.
DEFAULT_IL_FCOST = 25.0
DEFAULT_IL_CAPACITY_MW = 50.0  # interruptible portion of total load


def build_interruptible_load(
    case: SiennaCase,
    *,
    il_fcost: float = DEFAULT_IL_FCOST,
    il_capacity: float = DEFAULT_IL_CAPACITY_MW,
) -> dict[str, Any]:
    """Emit the gtopt JSON for the interruptible-load variant.

    Parameters
    ----------
    case
        Parsed 5-bus case bundle (uses bus_array + thermal stack).
    il_fcost
        Willingness-to-pay [$/MWh] for the interruptible portion
        (default 25 — between cheap/expensive thermal cost bands).
    il_capacity
        Interruptible-load capacity [MW] (default 50).
    """

    if not case.buses:
        raise ValueError("interruptible_load variant requires a non-empty case")

    bus_array = make_bus_array(case.buses)
    gen_array, _ = make_thermal_generator_array(case.generators)

    # Pick a load-bearing bus for the IL.  Bus 3 in the standard
    # 5-bus fixture carries a real (non-zero) load.
    target_bus = None
    for bus in case.buses:
        if bus.mw_load > 0.0:
            target_bus = int(bus.bus_id)
            break
    if target_bus is None:
        target_bus = int(case.buses[0].bus_id)

    # Two demands on the target bus:
    #   1. The original mandatory load (high fcost — must be served).
    #   2. The interruptible load (low fcost — LP picks).
    demand_array: list[dict[str, Any]] = []
    uid = 1
    for bus in case.buses:
        if bus.mw_load <= 0.0:
            continue
        demand_array.append(
            {
                "uid": uid,
                "name": f"load_{bus.name or bus.bus_id}",
                "bus": int(bus.bus_id),
                "capacity": float(bus.mw_load),
            }
        )
        uid += 1
    # Add the interruptible load — same bus, separate Demand row with
    # the lower fcost.  This is the row the LP will curtail first when
    # the marginal serving cost exceeds il_fcost.
    demand_array.append(
        {
            "uid": uid,
            "name": "il_load",
            "type": "interruptible",
            "bus": target_bus,
            "capacity": float(il_capacity),
            "fcost": float(il_fcost),
        }
    )

    system: dict[str, Any] = {
        "name": "SiennaC5Il",
        "bus_array": bus_array,
        "demand_array": demand_array,
        "generator_array": gen_array,
    }
    # use_single_bus collapses the network — the focus is the
    # demand-curtailment knob, not OPF.
    options = make_planning_options(use_kirchhoff=False, demand_fail_cost=1.0e6)
    options["model_options"]["use_single_bus"] = True

    return {
        "options": options,
        "simulation": make_simulation(),
        "system": system,
    }


__all__ = ["build_interruptible_load"]
