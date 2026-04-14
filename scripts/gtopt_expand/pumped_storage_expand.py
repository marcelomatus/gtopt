# -*- coding: utf-8 -*-

"""Pumped-storage expansion.

Emits the gtopt entities for a generic reversible pumped-storage unit
between an upper and a lower reservoir.  The expansion is
plant-agnostic: every technical parameter (production factors, pump
factor, nameplates, efficiency) is supplied via a canonical JSON
config.  The template values shipped with :func:`default_config` come
from the HB Maule specification (pump.pdf §4), but any pumped-storage
plant with the same topology can be described by editing those
numbers.

Topology
--------

* Two directional waterways between the upper and lower junctions —
  one for generation (upper → lower), one for pumping
  (lower → upper).
* One ``Turbine`` on the generation waterway, with the production
  factor driven by a ``ReservoirProductionFactor`` on the upper
  reservoir (volume-dependent head curve, two-segment concave
  envelope anchored at ``vmin`` / ``vmax``).
* One ``Pump`` on the pumping waterway, with a constant
  ``pump_factor``.
* An auxiliary ``Generator`` that dispatches the turbine output to
  the electrical bus.
* An auxiliary ``Demand`` that drains bus energy to feed the pump.

Every emitted element name is parameterized by the unit ``name``:

* Generator: ``hydro_{name}``
* Demand: ``pump_load_{name}``
* Waterways: ``ww_{name}_gen``, ``ww_{name}_pump``
* Turbine: ``tur_{name}``
* Pump: ``pump_{name}``
* ReservoirProductionFactor: ``rpf_{name}``

Canonical input schema
----------------------

.. code-block:: json

   {
     "name": "hb_maule",
     "upper_reservoir": "COLBUN",
     "lower_reservoir": "MACHICURA",
     "vmin": 1000.0,
     "vmax": 10000.0,
     "bus": "colbun",
     "gen_nominal": {"pmax_mw": 70, "qmax_m3s": 49, "production_factor": 1.44},
     "gen_mintec":  {"pmin_mw": 34, "qmin_m3s": 25, "production_factor": 1.36},
     "pump":        {"pmax_mw": 75, "qmax_m3s": 40, "pump_factor": 1.88},
     "pump_efficiency": 0.85
   }

When ``name`` is absent, :func:`expand_pumped_storage_from_file` uses
the input filename stem (e.g. ``hb_maule.json`` → ``name="hb_maule"``).

``vmin`` / ``vmax`` anchor the ``ReservoirProductionFactor`` curve;
they fall back to the upper reservoir's ``emin`` / ``emax`` when not
supplied explicitly (or when left as ``0``).

Prerequisite: the ``lower_reservoir`` must be a reservoir in the
planning case (real embalse or RoR-promoted).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Mapping

# ---------------------------------------------------------------------------
# Template default values — pump.pdf §4, HB Maule "Datos técnicos" table.
# Only :func:`default_config` uses these; :func:`expand_pumped_storage`
# never silently substitutes them for missing config fields.
# ---------------------------------------------------------------------------

GEN_PNOM_MW: float = 70.0
GEN_QNOM_M3S: float = 49.0
GEN_PF_NOMINAL: float = 1.44

GEN_PMIN_MW: float = 34.0
GEN_QMIN_M3S: float = 25.0
GEN_PF_MINTEC: float = 1.36

PUMP_PMAX_MW: float = 75.0
PUMP_QMAX_M3S: float = 40.0
PUMP_FACTOR: float = 1.88

PUMP_EFFICIENCY: float = 0.85

DEFAULT_UID_START: int = 900_000


def default_config(
    *,
    name: str = "pumped_storage",
    upper_reservoir: str = "COLBUN",
    lower_reservoir: str = "MACHICURA",
    vmin: float = 0.0,
    vmax: float = 0.0,
) -> dict[str, Any]:
    """Build a canonical pumped-storage config template.

    Numeric fields are populated with HB Maule reference values — edit
    them to match the specific plant before passing to
    :func:`expand_pumped_storage`.
    """
    return {
        "name": name,
        "upper_reservoir": upper_reservoir,
        "lower_reservoir": lower_reservoir,
        "vmin": float(vmin),
        "vmax": float(vmax),
        "gen_nominal": {
            "pmax_mw": GEN_PNOM_MW,
            "qmax_m3s": GEN_QNOM_M3S,
            "production_factor": GEN_PF_NOMINAL,
        },
        "gen_mintec": {
            "pmin_mw": GEN_PMIN_MW,
            "qmin_m3s": GEN_QMIN_M3S,
            "production_factor": GEN_PF_MINTEC,
        },
        "pump": {
            "pmax_mw": PUMP_PMAX_MW,
            "qmax_m3s": PUMP_QMAX_M3S,
            "pump_factor": PUMP_FACTOR,
        },
        "pump_efficiency": PUMP_EFFICIENCY,
    }


def _production_factor_segments(
    vmin: float,
    vmax: float,
    pf_low: float,
    pf_high: float,
) -> list[dict[str, float]]:
    """Build the two-segment concave-envelope production-factor curve."""
    span = vmax - vmin
    if span <= 0.0:
        raise ValueError(
            f"pumped-storage upper reservoir has invalid capacity span: "
            f"vmin={vmin}, vmax={vmax}"
        )
    slope = (pf_high - pf_low) / span
    return [
        {"volume": float(vmin), "constant": float(pf_low), "slope": float(slope)},
        {"volume": float(vmax), "constant": float(pf_high), "slope": 0.0},
    ]


def _find_reservoir(
    reservoirs: list[dict[str, Any]],
    name: str,
) -> dict[str, Any] | None:
    """Return the first reservoir entry whose ``name`` matches ``name``."""
    for entry in reservoirs:
        if isinstance(entry, dict) and entry.get("name") == name:
            return entry
    return None


def _resolve_vmin_vmax(
    cfg: Mapping[str, Any],
    upper_name: str,
    reservoirs: list[dict[str, Any]],
    unit_name: str,
) -> tuple[float, float]:
    """Resolve the PF curve anchor from cfg or the upper reservoir entry.

    Zero is treated as a sentinel meaning "fall back", so a template
    emitted by :func:`default_config` can be saved with ``vmin=vmax=0``
    and still work after the caller patches in real values via
    ``reservoirs``.
    """
    cfg_vmin = cfg.get("vmin")
    cfg_vmax = cfg.get("vmax")
    if cfg_vmin is not None and cfg_vmax is not None and cfg_vmin and cfg_vmax:
        return float(cfg_vmin), float(cfg_vmax)

    if "emin" in cfg and "emax" in cfg:
        return float(cfg["emin"]), float(cfg["emax"])

    upper_entry = _find_reservoir(reservoirs, upper_name)
    if upper_entry is None:
        raise ValueError(
            f"pumped-storage '{unit_name}' needs the '{upper_name}' "
            f"reservoir entry (or explicit vmin/vmax in config) to "
            f"anchor the production factor curve"
        )
    return (
        float(upper_entry.get("emin", 0.0)),
        float(upper_entry.get("emax", upper_entry.get("capacity", 0.0))),
    )


def expand_pumped_storage(
    config: Mapping[str, Any],
    *,
    name: str | None = None,
    reservoirs: list[dict[str, Any]] | None = None,
    reservoir_names: set[str] | None = None,
    bus_name: str | None = None,
    uid_start: int = DEFAULT_UID_START,
) -> dict[str, Any]:
    """Build a pumped-storage system fragment from a canonical config."""
    if config is None:
        raise ValueError("expand_pumped_storage requires a config")
    cfg: dict[str, Any] = dict(config)

    unit_name = name if name is not None else cfg.get("name")
    if not unit_name:
        raise ValueError(
            "pumped-storage expansion requires a 'name' — pass name=... "
            'or include "name" in the config JSON'
        )
    unit_name = str(unit_name).strip()
    if not unit_name:
        raise ValueError("pumped-storage 'name' cannot be empty")

    upper_name = cfg.get("upper_reservoir")
    lower_name = cfg.get("lower_reservoir")
    if not upper_name or not lower_name:
        raise ValueError(
            f"pumped-storage '{unit_name}' requires 'upper_reservoir' "
            f"and 'lower_reservoir' names"
        )

    reservoirs_list = list(reservoirs) if reservoirs else []
    name_set = set(reservoir_names) if reservoir_names else set()
    name_set.update(
        r.get("name", "")
        for r in reservoirs_list
        if isinstance(r, dict) and r.get("name")
    )

    if lower_name not in name_set:
        raise ValueError(
            f"pumped-storage '{unit_name}' requires lower reservoir "
            f"'{lower_name}' to exist (real embalse or RoR-promoted); "
            f"found only {sorted(name_set)}"
        )

    vmin, vmax = _resolve_vmin_vmax(cfg, upper_name, reservoirs_list, unit_name)

    gen_nominal = cfg.get("gen_nominal")
    gen_mintec = cfg.get("gen_mintec")
    pump_spec = cfg.get("pump")
    if not (
        isinstance(gen_nominal, Mapping)
        and isinstance(gen_mintec, Mapping)
        and isinstance(pump_spec, Mapping)
    ):
        raise ValueError(
            f"pumped-storage '{unit_name}' config missing required "
            f"'gen_nominal' / 'gen_mintec' / 'pump' dicts"
        )

    pump_eff = float(cfg.get("pump_efficiency", 1.0))
    bus = cfg.get("bus", cfg.get("bus_name", bus_name or upper_name.lower()))

    pf_nominal = float(gen_nominal["production_factor"])
    pf_mintec = float(gen_mintec["production_factor"])
    gen_cap = float(gen_nominal["pmax_mw"])
    gen_fmax = float(gen_nominal["qmax_m3s"])
    pump_cap = float(pump_spec["pmax_mw"])
    pump_fmax = float(pump_spec["qmax_m3s"])
    pump_factor = float(pump_spec["pump_factor"])

    segments = _production_factor_segments(vmin, vmax, pf_mintec, pf_nominal)

    u = uid_start
    gen_element = f"hydro_{unit_name}"
    dem_element = f"pump_load_{unit_name}"
    ww_gen = f"ww_{unit_name}_gen"
    ww_pump = f"ww_{unit_name}_pump"
    tur_element = f"tur_{unit_name}"
    pump_element = f"pump_{unit_name}"
    rpf_element = f"rpf_{unit_name}"

    generator = {
        "uid": u,
        "name": gen_element,
        "bus": bus,
        "gcost": 0.0,
        "capacity": gen_cap,
    }
    demand = {
        "uid": u + 1,
        "name": dem_element,
        "bus": bus,
        "capacity": pump_cap,
    }
    waterway_gen = {
        "uid": u + 2,
        "name": ww_gen,
        "junction_a": upper_name,
        "junction_b": lower_name,
        "fmin": 0.0,
        "fmax": gen_fmax,
    }
    waterway_pump = {
        "uid": u + 3,
        "name": ww_pump,
        "junction_a": lower_name,
        "junction_b": upper_name,
        "fmin": 0.0,
        "fmax": pump_fmax,
    }
    turbine = {
        "uid": u + 4,
        "name": tur_element,
        "waterway": ww_gen,
        "generator": gen_element,
        "production_factor": pf_nominal,
        "main_reservoir": upper_name,
    }
    pump = {
        "uid": u + 5,
        "name": pump_element,
        "waterway": ww_pump,
        "demand": dem_element,
        "pump_factor": pump_factor,
        "efficiency": pump_eff,
        "capacity": pump_fmax,
    }
    rpf = {
        "uid": u + 6,
        "name": rpf_element,
        "turbine": tur_element,
        "reservoir": upper_name,
        "mean_production_factor": pf_nominal,
        "segments": segments,
    }

    return {
        "generator_array": [generator],
        "demand_array": [demand],
        "waterway_array": [waterway_gen, waterway_pump],
        "turbine_array": [turbine],
        "pump_array": [pump],
        "reservoir_production_factor_array": [rpf],
    }


def expand_pumped_storage_from_file(
    input_path: str | Path,
    *,
    name: str | None = None,
    reservoirs: list[dict[str, Any]] | None = None,
    reservoir_names: set[str] | None = None,
    bus_name: str | None = None,
    uid_start: int = DEFAULT_UID_START,
) -> dict[str, Any]:
    """Read a canonical pumped-storage config and expand it.

    If the config does not carry a ``name`` field (and ``name`` is not
    passed explicitly), the input filename stem is used as the unit
    identifier (e.g. ``hb_maule.json`` → ``name="hb_maule"``).
    """
    path = Path(input_path)
    with open(path, "r", encoding="utf-8") as fh:
        config = json.load(fh)
    if not isinstance(config, dict):
        raise ValueError(
            f"pumped-storage config {input_path}: expected a JSON object, "
            f"got {type(config).__name__}"
        )
    resolved_name = name or config.get("name") or path.stem
    return expand_pumped_storage(
        config,
        name=resolved_name,
        reservoirs=reservoirs,
        reservoir_names=reservoir_names,
        bus_name=bus_name,
        uid_start=uid_start,
    )
