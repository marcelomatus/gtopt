# -*- coding: utf-8 -*-

"""HB Maule pumped-storage expansion.

Emits the gtopt entities for the reversible HB Maule unit between the
Colbún (upper) and Machicura (lower) reservoirs, as described in the
``Términos de Referencia Proyecto Bombeo`` specification (pump.pdf).

Topology (see pump.pdf §3, "Diagrama Hidráulico Colbún – HB Maule – Machicura"):

* Two directional waterways between the Colbún and Machicura junctions
  — one for generation (Colbún → Machicura), one for pumping
  (Machicura → Colbún).
* One ``Turbine`` on the generation waterway, with the production
  factor driven by a ``ReservoirProductionFactor`` on Colbún whose
  segments are derived from the pump.pdf table (1.36 at low head,
  1.44 at nominal head).
* One ``Pump`` on the pumping waterway, with a constant
  ``pump_factor`` (the single pump row of the table).
* An auxiliary ``Generator`` that dispatches the turbine output to the
  electrical bus (nameplate from the gen-nominal row).
* An auxiliary ``Demand`` that drains bus energy to feed the pump
  (nameplate from the pump row).

Canonical input (``hb_maule_dat.json``)
---------------------------------------

The Stage-1 parser builds a self-describing config dict mirroring the
pump.pdf table, so the Stage-2 transform is pure data-in → entities-out:

.. code-block:: json

   {
     "upper_reservoir": "COLBUN",
     "lower_reservoir": "MACHICURA",
     "vmin": 1000.0,
     "vmax": 10000.0,
     "gen_nominal": {"pmax_mw": 70, "qmax_m3s": 49, "production_factor": 1.44},
     "gen_mintec":  {"pmin_mw": 34, "qmin_m3s": 25, "production_factor": 1.36},
     "pump":        {"pmax_mw": 75, "qmax_m3s": 40, "pump_factor": 1.88},
     "pump_efficiency": 0.85,
     "bus_name": "colbun"
   }

The ``vmin`` / ``vmax`` entries anchor the ReservoirProductionFactor
curve; they are taken from Colbún's ``emin`` / ``emax`` in
``plpcnfce.dat``.  All other entries default to the pump.pdf values
via :func:`default_config`.

Prerequisite: **MACHICURA must be a reservoir** in the planning case
— either a real embalse or an RoR-promoted central surfaced in
``ror_promoted.json``.  Otherwise :func:`expand_hb_maule` raises
``ValueError``.

Reference: ``test/source/test_pump_production_factor.cpp`` encodes the
same topology as a C++ unit test fixture.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any, Mapping

# ---------------------------------------------------------------------------
# Default technical parameters — pump.pdf §4, table "Datos técnicos"
# ---------------------------------------------------------------------------

#: Gen nominal: 70 MW at 49 m³/s, production factor 1.44 MW·s/m³.
GEN_PNOM_MW: float = 70.0
GEN_QNOM_M3S: float = 49.0
GEN_PF_NOMINAL: float = 1.44

#: Gen min-tech: 34 MW at 25 m³/s, production factor 1.36 MW·s/m³.
#: Informational only — the LP does not enforce a minimum technical flow
#: in Phase 1; the ``1.36`` value anchors the low-head end of the
#: ``ReservoirProductionFactor`` curve.
GEN_PMIN_MW: float = 34.0
GEN_QMIN_M3S: float = 25.0
GEN_PF_MINTEC: float = 1.36

#: Bombeo: 75 MW at 40 m³/s, pump factor 1.88 MW·s/m³ (constant).
PUMP_PMAX_MW: float = 75.0
PUMP_QMAX_M3S: float = 40.0
PUMP_FACTOR: float = 1.88

#: Round-trip efficiency on the Pump side — ensures the LP strictly
#: prefers either gen OR pump, never both at once, without needing an
#: integer commitment variable.
PUMP_EFFICIENCY: float = 0.85

#: Default reservoir / junction names expected in the plp2gtopt output.
DEFAULT_UPPER_RESERVOIR: str = "COLBUN"
DEFAULT_LOWER_RESERVOIR: str = "MACHICURA"


def default_config(vmin: float, vmax: float) -> dict[str, Any]:
    """Build a canonical ``hb_maule_dat.json`` dict from the pump.pdf table.

    Parameters
    ----------
    vmin, vmax:
        Colbún reservoir minimum / maximum volume (hm³), taken from
        ``plpcnfce.dat`` via :class:`~plp2gtopt.central_parser.CentralParser`.

    Returns
    -------
    dict
        A self-describing config dict.  All numeric fields can be
        overridden by caller-supplied dicts before passing to
        :func:`expand_hb_maule`.
    """
    return {
        "upper_reservoir": DEFAULT_UPPER_RESERVOIR,
        "lower_reservoir": DEFAULT_LOWER_RESERVOIR,
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
    """Build the two-segment concave-envelope curve for the turbine.

    ``evaluate_production_factor`` in ``reservoir_production_factor.hpp``
    computes ``min_i { constant_i + slope_i × (V − volume_i) }``, so
    using two segments gives:

    * segment 1 (linear rise):
        ``pf_low + slope × (V − vmin)`` where
        ``slope = (pf_high − pf_low) / (vmax − vmin)``.
    * segment 2 (upper cap): ``pf_high`` for ``V ≥ vmax``.

    At ``V = vmin`` the minimum is ``pf_low``; at ``V = vmax`` both
    segments give ``pf_high``; for ``V > vmax`` the cap binds.
    """
    span = vmax - vmin
    if span <= 0.0:
        raise ValueError(
            f"Colbún reservoir has invalid capacity span: vmin={vmin}, vmax={vmax}"
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


def expand_hb_maule(
    config: Mapping[str, Any] | None = None,
    *,
    reservoirs: list[dict[str, Any]] | None = None,
    reservoir_names: set[str] | None = None,
    bus_name: str | None = None,
    uid_start: int = 900_000,
) -> dict[str, Any]:
    """Build the HB Maule system fragment.

    Accepts either a full canonical config (as produced by
    :func:`default_config`) or a partial override — missing keys fall
    back to the pump.pdf defaults.  ``vmin`` / ``vmax`` are the only
    keys that must either be provided directly, inferred from the
    Colbún entry in ``reservoirs`` (``emin`` / ``emax``), or passed
    through a full canonical dict.

    Parameters
    ----------
    config:
        Optional canonical ``hb_maule_dat`` dict (full or partial) —
        see module docstring for the schema.  When ``None`` (the CLI
        ``--planning`` path), the function falls back to
        :func:`default_config` with ``vmin`` / ``vmax`` inferred from
        ``reservoirs``.
    reservoirs:
        The planning system's ``reservoir_array``, consulted for the
        upper reservoir's ``emin`` / ``emax`` when ``config`` does not
        carry ``vmin`` / ``vmax`` explicitly.
    reservoir_names:
        Alternative to ``reservoirs`` when only the names are known
        (e.g. the lower reservoir was RoR-promoted and appears only in
        ``ror_promoted.json``).
    bus_name:
        Bus for the aux generator and demand.  ``config["bus_name"]``
        wins over this parameter, which in turn wins over the default
        (lower-cased upper-reservoir name).
    uid_start:
        First UID to assign.

    Returns
    -------
    dict
        System fragment with keys:
        ``generator_array``, ``demand_array``, ``waterway_array``,
        ``turbine_array``, ``pump_array``,
        ``reservoir_production_factor_array``.

    Raises
    ------
    ValueError
        If the lower reservoir (MACHICURA) is not in the combined set
        of ``reservoirs`` and ``reservoir_names``, or if
        ``vmin`` / ``vmax`` for the upper reservoir cannot be resolved.
    """
    cfg: dict[str, Any] = dict(config) if config else {}
    upper_name = cfg.get("upper_reservoir", DEFAULT_UPPER_RESERVOIR)
    lower_name = cfg.get("lower_reservoir", DEFAULT_LOWER_RESERVOIR)

    reservoirs = list(reservoirs) if reservoirs else []
    name_set = set(reservoir_names) if reservoir_names else set()
    name_set.update(
        r.get("name", "") for r in reservoirs if isinstance(r, dict) and r.get("name")
    )

    if lower_name not in name_set:
        raise ValueError(
            f"HB Maule expansion requires '{lower_name}' to be a "
            f"reservoir (real embalse or RoR-promoted); found only "
            f"{sorted(name_set)}"
        )

    # Resolve Colbún vmin/vmax for the PF curve.
    if "vmin" in cfg and "vmax" in cfg:
        vmin = float(cfg["vmin"])
        vmax = float(cfg["vmax"])
    elif "emin" in cfg and "emax" in cfg:
        vmin = float(cfg["emin"])
        vmax = float(cfg["emax"])
    else:
        upper_entry = _find_reservoir(reservoirs, upper_name)
        if upper_entry is None:
            raise ValueError(
                f"HB Maule expansion needs the '{upper_name}' reservoir "
                f"entry (or explicit vmin/vmax in config) to anchor the "
                f"production factor curve"
            )
        vmin = float(upper_entry.get("emin", 0.0))
        vmax = float(upper_entry.get("emax", upper_entry.get("capacity", 0.0)))

    # Merge in PDF defaults for any missing sub-dicts.
    defaults = default_config(vmin, vmax)
    gen_nominal = {**defaults["gen_nominal"], **cfg.get("gen_nominal", {})}
    gen_mintec = {**defaults["gen_mintec"], **cfg.get("gen_mintec", {})}
    pump_spec = {**defaults["pump"], **cfg.get("pump", {})}
    pump_eff = float(cfg.get("pump_efficiency", defaults["pump_efficiency"]))
    bus = cfg.get("bus_name", bus_name or upper_name.lower())

    pf_nominal = float(gen_nominal["production_factor"])
    pf_mintec = float(gen_mintec["production_factor"])
    gen_cap = float(gen_nominal["pmax_mw"])
    gen_fmax = float(gen_nominal["qmax_m3s"])
    pump_cap = float(pump_spec["pmax_mw"])
    pump_fmax = float(pump_spec["qmax_m3s"])
    pump_factor = float(pump_spec["pump_factor"])

    segments = _production_factor_segments(vmin, vmax, pf_mintec, pf_nominal)

    u = uid_start

    generator = {
        "uid": u,
        "name": "hydro_hb_maule",
        "bus": bus,
        "gcost": 0.0,
        "capacity": gen_cap,
    }
    demand = {
        "uid": u + 1,
        "name": "pump_load_hb_maule",
        "bus": bus,
        "capacity": pump_cap,
    }
    ww_gen = {
        "uid": u + 2,
        "name": "ww_hb_maule_gen",
        "junction_a": upper_name,
        "junction_b": lower_name,
        "fmin": 0.0,
        "fmax": gen_fmax,
    }
    ww_pump = {
        "uid": u + 3,
        "name": "ww_hb_maule_pump",
        "junction_a": lower_name,
        "junction_b": upper_name,
        "fmin": 0.0,
        "fmax": pump_fmax,
    }
    turbine = {
        "uid": u + 4,
        "name": "tur_hb_maule",
        "waterway": "ww_hb_maule_gen",
        "generator": "hydro_hb_maule",
        "production_factor": pf_nominal,
        "main_reservoir": upper_name,
    }
    pump = {
        "uid": u + 5,
        "name": "pump_hb_maule",
        "waterway": "ww_hb_maule_pump",
        "demand": "pump_load_hb_maule",
        "pump_factor": pump_factor,
        "efficiency": pump_eff,
        "capacity": pump_fmax,
    }
    rpf = {
        "uid": u + 6,
        "name": "rpf_hb_maule",
        "turbine": "tur_hb_maule",
        "reservoir": upper_name,
        "mean_production_factor": pf_nominal,
        "segments": segments,
    }

    return {
        "generator_array": [generator],
        "demand_array": [demand],
        "waterway_array": [ww_gen, ww_pump],
        "turbine_array": [turbine],
        "pump_array": [pump],
        "reservoir_production_factor_array": [rpf],
    }


def expand_hb_maule_from_file(
    input_path: str | Path,
    *,
    reservoirs: list[dict[str, Any]] | None = None,
    reservoir_names: set[str] | None = None,
    bus_name: str | None = None,
    uid_start: int = 900_000,
) -> dict[str, Any]:
    """Read a canonical ``hb_maule_dat.json`` and expand it.

    Mirrors :func:`~gtopt_expand.lng_expand.expand_lng_from_file`: the
    on-disk intermediate stays a convenience for the CLI; the
    in-process pipeline keeps the config in memory and never writes
    ``hb_maule_dat.json`` to disk.
    """
    with open(input_path, "r", encoding="utf-8") as fh:
        config = json.load(fh)
    return expand_hb_maule(
        config,
        reservoirs=reservoirs,
        reservoir_names=reservoir_names,
        bus_name=bus_name,
        uid_start=uid_start,
    )
