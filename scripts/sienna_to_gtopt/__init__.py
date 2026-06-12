"""sienna_to_gtopt — port NREL-Sienna 5-bus PowerSimulations.jl cases to gtopt.

This converter reads three NREL-Sienna test-data variants packaged as
``integration_test/data/sienna_5bus.tar.zst`` and emits a gtopt
planning JSON capturing the matching topology + dispatch fixture.

The three variants intentionally exercise **NON-EMISSION** features
that don't have peer-reviewed integration-test coverage today:

* ``cascading_hydro`` — Junction + Waterway + Turbine + Reservoir
  cascade chain (gtopt's unique hydro topology vs. PowerSystems.jl's
  flat ``HydroEnergyReservoir`` adjacency table).
* ``monitored_line``  — exactly one transmission line carries an
  enforced thermal limit; the rest are unconstrained
  (``Line.enforce_level = 0``).  Mirrors PowerSimulations.jl
  ``MonitoredLine``.
* ``hvdc``            — one branch is converted to an HVDC link by
  dropping its ``reactance``; the LP omits the Kirchhoff KVL row
  for the DC line.

Use as a module:

    python -m sienna_to_gtopt cascading_hydro -o cascading.json
    python -m sienna_to_gtopt monitored_line  -o mline.json
    python -m sienna_to_gtopt hvdc            -o hvdc.json
"""

from typing import Any, Callable

from sienna_to_gtopt._battery_degeneracy import build_battery_degeneracy
from sienna_to_gtopt._bundle import BUNDLES, available_bundles, extract_bundle
from sienna_to_gtopt._cascading_hydro import build_cascading_hydro
from sienna_to_gtopt._fuel_cost_ts import build_fuel_cost_ts
from sienna_to_gtopt._hvdc import build_hvdc
from sienna_to_gtopt._interruptible_load import build_interruptible_load
from sienna_to_gtopt._monitored_line import build_monitored_line
from sienna_to_gtopt._pumped_storage import build_pumped_storage
from sienna_to_gtopt._reader import (
    SiennaBranch,
    SiennaBus,
    SiennaCase,
    SiennaGen,
    parse_branches,
    parse_buses,
    parse_generators,
    parse_hydro_upstream,
    parse_user_descriptors,
)
from sienna_to_gtopt._wecc_240 import build_wecc_240

VariantBuilder = Callable[..., dict[str, Any]]
VARIANTS: dict[str, VariantBuilder] = {
    "cascading_hydro": build_cascading_hydro,
    "monitored_line": build_monitored_line,
    "hvdc": build_hvdc,
    "pumped_storage": build_pumped_storage,
    "battery_degeneracy": build_battery_degeneracy,
    "interruptible_load": build_interruptible_load,
    "fuel_cost_ts": build_fuel_cost_ts,
    "wecc_240": build_wecc_240,
}

__all__ = [
    "BUNDLES",
    "SiennaBranch",
    "SiennaBus",
    "SiennaCase",
    "SiennaGen",
    "VARIANTS",
    "VariantBuilder",
    "available_bundles",
    "build_battery_degeneracy",
    "build_cascading_hydro",
    "build_fuel_cost_ts",
    "build_hvdc",
    "build_interruptible_load",
    "build_monitored_line",
    "build_pumped_storage",
    "build_wecc_240",
    "extract_bundle",
    "parse_branches",
    "parse_buses",
    "parse_generators",
    "parse_hydro_upstream",
    "parse_user_descriptors",
]
