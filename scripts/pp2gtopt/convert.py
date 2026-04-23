#!/usr/bin/env python3
"""Convert pandapower networks to gtopt JSON format.

The default network is the IEEE 30-bus Washington test system from MATPOWER,
available as pandapower.networks.case_ieee30(). Arbitrary pandapower networks
can be loaded from files via ``load_network(path)`` (JSON, Excel, MATPOWER).
The conversion:
  - Converts line/trafo reactances from physical (Ohm) to per-unit (p.u.)
  - Linearises quadratic generator costs to their cp1 coefficient ($/MWh)
  - Models lossless transformers as lines with x = vk_percent / 100 p.u.
  - Disables network thermal limits (max_i_ka = inf in the pandapower case)

For CLI usage run ``pp2gtopt --help``.
"""

import json
import math
from pathlib import Path
from typing import Any

import pandapower as pp
import pandapower.networks as pn

_BASE_MVA = 100.0  # system base (MVA) used for per-unit conversions
_TMAX_UNLIMITED: float = 9999  # sentinel for unconstrained thermal limit

# File extensions recognised by load_network()
_FORMAT_JSON = ".json"
_FORMAT_EXCEL = {".xlsx", ".xls"}
_FORMAT_MATPOWER = ".m"
_SUPPORTED_FORMATS = f"{_FORMAT_JSON}, .xlsx/.xls, {_FORMAT_MATPOWER}"


def load_network(path: Path) -> pp.pandapowerNet:
    """Load a pandapower network from a file.

    Supported formats
    -----------------
    ``.json``
        pandapower JSON (written by ``pandapower.to_json()``).
    ``.xlsx`` / ``.xls``
        pandapower Excel workbook (written by ``pandapower.to_excel()``).
    ``.m``
        MATPOWER case file (converted via ``pandapower.converter.from_mpc()``).

    Parameters
    ----------
    path:
        Path to the network file.

    Returns
    -------
    pp.pandapowerNet
        The loaded network.

    Raises
    ------
    FileNotFoundError
        If *path* does not exist.
    ValueError
        If the file extension is not recognised.
    """
    if not path.exists():
        raise FileNotFoundError(f"Network file not found: {path}")
    suffix = path.suffix.lower()
    if suffix == _FORMAT_JSON:
        return pp.from_json(str(path))
    if suffix in _FORMAT_EXCEL:
        return pp.from_excel(str(path))
    if suffix == _FORMAT_MATPOWER:
        from pandapower.converter.matpower.from_mpc import (  # pylint: disable=import-outside-toplevel
            from_mpc,
        )

        return from_mpc(str(path))
    raise ValueError(
        f"Unsupported file format {suffix!r} for {path.name}. "
        f"Supported extensions: {_SUPPORTED_FORMATS}"
    )


def get_bus_base_kv(net: pp.pandapowerNet, bus_idx: int) -> float:
    """Return nominal voltage (kV) of a bus by its integer index."""
    return float(net.bus.loc[bus_idx, "vn_kv"])


def ohm_to_pu(ohm: float, base_kv: float, base_mva: float = 100.0) -> float:
    """Convert impedance in Ohm to per-unit on (base_kv, base_mva) base."""
    z_base = base_kv**2 / base_mva
    return ohm / z_base


def _get_poly_cost(
    net: pp.pandapowerNet, et: str, element: int, default: float
) -> float:
    """Return cp1 linear cost coefficient for an element, or *default*."""
    mask = (net.poly_cost["et"] == et) & (net.poly_cost["element"] == element)
    row = net.poly_cost[mask]
    return float(row["cp1_eur_per_mw"].iloc[0]) if not row.empty else default


def _build_buses(net: pp.pandapowerNet) -> list[dict[str, Any]]:
    """Build bus array; fix the ext_grid bus as the DC OPF reference (theta=0)."""
    slack_bus = int(net.ext_grid.iloc[0]["bus"]) + 1  # 1-indexed
    bus_array: list[dict[str, Any]] = []
    for idx, _row in net.bus.iterrows():
        bus_uid = int(idx) + 1
        entry: dict[str, Any] = {"uid": bus_uid, "name": f"b{bus_uid}"}
        if bus_uid == slack_bus:
            entry["reference_theta"] = 0  # fix slack angle to 0 rad
        bus_array.append(entry)
    return bus_array


def _build_ext_grid_gen(net: pp.pandapowerNet) -> dict[str, Any]:
    """Return the gtopt generator entry for the ext_grid (slack generator)."""
    eg = net.ext_grid.iloc[0]
    eg_bus = int(eg["bus"])
    gcost = _get_poly_cost(net, "ext_grid", 0, default=20.0)
    pmax = float(eg.get("max_p_mw", 360.2))
    return {
        "uid": 1,
        "name": "g1",
        "bus": eg_bus + 1,
        "pmin": 0,
        "pmax": pmax,
        "gcost": gcost,
        "capacity": pmax,
    }


def _build_generators(net: pp.pandapowerNet) -> list[dict[str, Any]]:
    """Build generator array: ext_grid first, then PV generators."""
    generators = [_build_ext_grid_gen(net)]
    for gen_uid, (idx, row) in enumerate(net.gen.iterrows(), start=2):
        pmax = float(row["max_p_mw"])
        pmin = float(row.get("min_p_mw", 0.0))
        gcost = _get_poly_cost(net, "gen", int(idx), default=40.0)
        generators.append(
            {
                "uid": gen_uid,
                "name": f"g{gen_uid}",
                "bus": int(row["bus"]) + 1,
                "pmin": pmin,
                "pmax": pmax,
                "gcost": gcost,
                "capacity": pmax,
            }
        )
    return generators


def _build_demands(net: pp.pandapowerNet) -> list[dict[str, Any]]:
    """Build demand array from net.load, skipping zero-load entries."""
    demand_array: list[dict[str, Any]] = []
    uid = 1
    for _idx, row in net.load.iterrows():
        p_mw = float(row["p_mw"])
        if p_mw <= 0.0:
            continue
        demand_array.append(
            {
                "uid": uid,
                "name": f"d{uid}",
                "bus": int(row["bus"]) + 1,
                "lmax": [[p_mw]],
            }
        )
        uid += 1
    return demand_array


def _line_tmax(max_i_ka: float, base_kv: float) -> float:
    """Return thermal limit in MW; 9999 if the pandapower value is unconstrained."""
    if math.isinf(max_i_ka) or max_i_ka >= _TMAX_UNLIMITED:
        return _TMAX_UNLIMITED
    return round(max_i_ka * base_kv * math.sqrt(3), 1)


def _build_physical_lines(
    net: pp.pandapowerNet, base_mva: float
) -> list[dict[str, Any]]:
    """Build line entries for physical lines (not transformers)."""
    lines: list[dict[str, Any]] = []
    for _idx, row in net.line.iterrows():
        fb = int(row["from_bus"])
        tb = int(row["to_bus"])
        base_kv = get_bus_base_kv(net, fb)
        x_pu = ohm_to_pu(
            float(row["x_ohm_per_km"]) * float(row["length_km"]), base_kv, base_mva
        )
        if x_pu < 1e-6:
            continue
        tmax = _line_tmax(float(row.get("max_i_ka", float("inf"))), base_kv)
        lines.append(
            {
                "name": f"l{fb + 1}_{tb + 1}",
                "bus_a": fb + 1,
                "bus_b": tb + 1,
                "reactance": round(x_pu, 6),
                "voltage": 10,
                "tmax_ab": tmax,
                "tmax_ba": tmax,
            }
        )
    return lines


def _get_tap_ratio(row: Any) -> float:
    """Return the off-nominal tap ratio for a pandapower transformer row.

    The off-nominal tap ratio is computed as::

        tap = 1 + (tap_pos - tap_neutral) * tap_step_percent / 100

    When ``tap_pos`` equals ``tap_neutral`` (or tap data is missing / NaN)
    the function returns 1.0 (nominal, no correction).
    """
    try:
        neutral = row.get("tap_neutral")
        tap_neutral = 0.0 if neutral is None else float(neutral)

        pos = row.get("tap_pos")
        tap_pos = tap_neutral if pos is None else float(pos)

        step = row.get("tap_step_percent")
        tap_step_percent = 0.0 if step is None else float(step)
    except (TypeError, ValueError):
        return 1.0
    deviation = (tap_pos - tap_neutral) * tap_step_percent / 100.0
    return round(1.0 + deviation, 6)


def _build_transformers(net: pp.pandapowerNet, base_mva: float) -> list[dict[str, Any]]:
    """Build line entries for transformers (modelled as branches with tap support).

    Each pandapower ``trafo`` row is converted to a gtopt line entry with:

    * ``type = "transformer"`` for identification.
    * Reactance computed from the short-circuit voltage:
      ``x_pu = (vk_percent / 100) * (base_mva / sn_mva)``.
    * ``tap_ratio`` set to the off-nominal tap ratio (1.0 when at nominal).
    * ``phase_shift_deg`` set for phase-shifting transformers
      (from ``shift_degree``).
    * Thermal limit in MW derived from ``sn_mva`` when ``max_loading_percent``
      is available, otherwise unconstrained (9999 MW).
    """
    trafos: list[dict[str, Any]] = []
    for _idx, row in net.trafo.iterrows():
        hv = int(row["hv_bus"])
        lv = int(row["lv_bus"])
        sn_mva = float(row["sn_mva"])
        x_pu = (float(row["vk_percent"]) / 100.0) * (base_mva / sn_mva)
        if x_pu < 1e-6:
            continue

        entry: dict[str, Any] = {
            "name": f"t{hv + 1}_{lv + 1}",
            "bus_a": hv + 1,
            "bus_b": lv + 1,
            "reactance": round(x_pu, 6),
            "voltage": 10,
            "type": "transformer",
        }

        # Thermal limit: derive from sn_mva when loading percentage is given,
        # otherwise leave unconstrained.
        max_loading = float(row.get("max_loading_percent", 100) or 100)
        tmax_mw = round(sn_mva * max_loading / 100.0, 1)
        if tmax_mw < _TMAX_UNLIMITED:
            entry["tmax_ab"] = tmax_mw
            entry["tmax_ba"] = tmax_mw
        else:
            entry["tmax_ab"] = _TMAX_UNLIMITED
            entry["tmax_ba"] = _TMAX_UNLIMITED

        # Off-nominal tap ratio
        tau = _get_tap_ratio(row)
        if abs(tau - 1.0) > 1e-6:
            entry["tap_ratio"] = tau

        # Phase-shift angle for PSTs
        shift_deg = float(row.get("shift_degree", 0) or 0)
        if abs(shift_deg) > 1e-6:
            entry["phase_shift_deg"] = round(shift_deg, 6)

        trafos.append(entry)
    return trafos


def _build_lines(net: pp.pandapowerNet, base_mva: float) -> list[dict[str, Any]]:
    """Build line array: physical lines followed by transformers, with sequential UIDs.

    Duplicate names (e.g. parallel transformers between the same buses) are
    made unique by appending ``_2``, ``_3``, … to later occurrences.
    """
    entries = _build_physical_lines(net, base_mva) + _build_transformers(net, base_mva)
    seen: dict[str, int] = {}
    for uid, entry in enumerate(entries, start=1):
        entry["uid"] = uid
        name = entry.get("name", "")
        if name in seen:
            seen[name] += 1
            entry["name"] = f"{name}_{seen[name]}"
        else:
            seen[name] = 1
    return entries


SUPPORTED_SOLVERS: tuple[str, ...] = ("sddp", "cascade")
DEFAULT_SOLVER: str = "cascade"


def _default_cascade_options() -> dict[str, Any]:
    """Build a 3-level default cascade configuration for OPF cases.

    Levels mirror the plp2gtopt defaults so behaviour is consistent:
    uninodal (single-bus) → transport (lines, no Kirchhoff/losses) →
    full network (Kirchhoff enabled).  Each transition inherits state
    targets and Benders cuts from the previous level.
    """
    transition: dict[str, Any] = {
        "inherit_targets": -1,
        "inherit_optimality_cuts": -1,
        "target_rtol": 0.05,
        "target_min_atol": 1.0,
        "target_penalty": 500.0,
    }
    return {
        "level_array": [
            {
                "uid": 1,
                "name": "uninodal",
                "model_options": {"use_single_bus": True},
            },
            {
                "uid": 2,
                "name": "transport",
                "model_options": {
                    "use_single_bus": False,
                    "use_kirchhoff": False,
                    "use_line_losses": False,
                },
                "transition": transition,
            },
            {
                "uid": 3,
                "name": "full_network",
                "model_options": {
                    "use_single_bus": False,
                    "use_kirchhoff": True,
                },
                "transition": transition,
            },
        ],
    }


def convert(
    output_path: Path | None = None,
    net: pp.pandapowerNet | None = None,
    name: str | None = None,
    solver_type: str = DEFAULT_SOLVER,
) -> dict[str, Any]:
    """Load a pandapower network and write the gtopt JSON file.

    Parameters
    ----------
    output_path:
        Destination for the JSON file.  When *None* the file is written as
        ``<name>.json`` in the same directory as this module.
    net:
        A pre-loaded ``pandapowerNet`` object.  When *None* ``case_ieee30()``
        is loaded automatically.
    name:
        System name to embed in the JSON.  Defaults to ``"ieee30b"``.
    solver_type:
        Planning method to embed in ``options.method``.  One of
        ``SUPPORTED_SOLVERS`` (``"sddp"`` or ``"cascade"``).  When
        ``"cascade"``, a 3-level default ``cascade_options`` block is
        also emitted.  Defaults to ``"cascade"``.

    Returns
    -------
    dict[str, Any]
        The generated gtopt planning dictionary.
    """
    if solver_type not in SUPPORTED_SOLVERS:
        raise ValueError(
            f"unsupported solver_type {solver_type!r}; "
            f"expected one of {SUPPORTED_SOLVERS}"
        )
    if net is None:
        net = pn.case_ieee30()
    if name is None:
        name = "ieee30b"
    options: dict[str, Any] = {
        "method": solver_type,
        "annual_discount_rate": 0.0,
        "output_format": "csv",
        "output_compression": "uncompressed",
        "use_single_bus": False,
        "demand_fail_cost": 1000,
        "scale_objective": 1000,
        "use_kirchhoff": True,
    }
    if solver_type == "cascade":
        options["cascade_options"] = _default_cascade_options()

    data: dict[str, Any] = {
        "options": options,
        "simulation": {
            "block_array": [{"uid": 1, "duration": 1}],
            "stage_array": [
                {"uid": 1, "first_block": 0, "count_block": 1, "active": 1}
            ],
            "scenario_array": [{"uid": 1, "probability_factor": 1}],
        },
        "system": {
            "name": name,
            "bus_array": _build_buses(net),
            "generator_array": _build_generators(net),
            "demand_array": _build_demands(net),
            "line_array": _build_lines(net, _BASE_MVA),
        },
    }

    if output_path is None:
        output_path = Path(__file__).parent / f"{name}.json"

    with open(output_path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
    print(f"Written: {output_path}")
    return data
