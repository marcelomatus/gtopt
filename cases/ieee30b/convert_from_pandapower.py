#!/usr/bin/env python3
"""Convert pandapower IEEE 30-bus case to gtopt JSON format.

Usage:
    python3 convert_from_pandapower.py [output_file]

Outputs ieee30b.json in the same directory by default.

The IEEE 30-bus case is the Washington 30-bus test system from MATPOWER,
available as pandapower.networks.case_ieee30(). This script:
  - Converts line/trafo reactances from physical (Ohm) to per-unit (p.u.)
  - Linearises quadratic generator costs to their cp1 coefficient ($/MWh)
  - Models lossless transformers as lines with x = vk_percent / 100 p.u.
  - Disables network thermal limits (max_i_ka = inf in the pandapower case)
"""

import json
import math
import sys
from pathlib import Path

import pandapower as pp
import pandapower.networks as pn


def get_bus_base_kv(net: pp.pandapowerNet, bus_idx: int) -> float:
    """Return nominal voltage (kV) of a bus by its integer index."""
    return float(net.bus.loc[bus_idx, "vn_kv"])


def ohm_to_pu(ohm: float, base_kv: float, base_mva: float = 100.0) -> float:
    """Convert impedance in Ohm to per-unit on (base_kv, base_mva) base."""
    z_base = base_kv**2 / base_mva
    return ohm / z_base


def convert(output_path: Path | None = None) -> None:
    """Load case_ieee30 and write the gtopt JSON file."""
    net = pn.case_ieee30()
    base_mva = 100.0  # IEEE 30-bus system base

    # ---- buses ---------------------------------------------------------------
    bus_array = []
    for idx, row in net.bus.iterrows():
        bus_array.append({"uid": int(idx) + 1, "name": f"b{int(idx) + 1}"})

    # ---- generators ----------------------------------------------------------
    # ext_grid (slack/reference generator) + PV generators
    generator_array = []
    gen_uid = 1

    # ext_grid at bus index 0 → bus uid 1
    eg = net.ext_grid.iloc[0]
    eg_bus = int(eg["bus"])
    # cost from poly_cost where et='ext_grid' and element=0
    eg_cost_row = net.poly_cost[
        (net.poly_cost["et"] == "ext_grid") & (net.poly_cost["element"] == 0)
    ]
    eg_gcost = float(eg_cost_row["cp1_eur_per_mw"].iloc[0]) if not eg_cost_row.empty else 20.0
    generator_array.append(
        {
            "uid": gen_uid,
            "name": "g1",
            "bus": eg_bus + 1,
            "pmin": 0,
            "pmax": float(eg.get("max_p_mw", 360.2)),
            "gcost": eg_gcost,
            "capacity": float(eg.get("max_p_mw", 360.2)),
        }
    )
    gen_uid += 1

    for i, (idx, row) in enumerate(net.gen.iterrows()):
        bus_idx = int(row["bus"])
        pmax = float(row["max_p_mw"])
        pmin = float(row.get("min_p_mw", 0.0))
        # find linear cost coefficient
        cost_row = net.poly_cost[
            (net.poly_cost["et"] == "gen") & (net.poly_cost["element"] == int(idx))
        ]
        gcost = float(cost_row["cp1_eur_per_mw"].iloc[0]) if not cost_row.empty else 40.0
        generator_array.append(
            {
                "uid": gen_uid,
                "name": f"g{gen_uid}",
                "bus": bus_idx + 1,
                "pmin": pmin,
                "pmax": pmax,
                "gcost": gcost,
                "capacity": pmax,
            }
        )
        gen_uid += 1

    # ---- demands -------------------------------------------------------------
    demand_array = []
    for i, (idx, row) in enumerate(net.load.iterrows()):
        bus_idx = int(row["bus"])
        p_mw = float(row["p_mw"])
        if p_mw <= 0.0:
            continue
        demand_array.append(
            {
                "uid": i + 1,
                "name": f"d{i + 1}",
                "bus": bus_idx + 1,
                "lmax": [[p_mw]],
            }
        )

    # ---- lines (physical lines, no transformers) ----------------------------
    line_array = []
    line_uid = 1

    for _idx, row in net.line.iterrows():
        fb = int(row["from_bus"])
        tb = int(row["to_bus"])
        x_ohm = float(row["x_ohm_per_km"]) * float(row["length_km"])
        r_ohm = float(row["r_ohm_per_km"]) * float(row["length_km"])

        # Use the from-bus voltage for per-unit conversion
        base_kv = get_bus_base_kv(net, fb)
        x_pu = ohm_to_pu(x_ohm, base_kv, base_mva)
        r_pu = ohm_to_pu(r_ohm, base_kv, base_mva)

        # Skip degenerate lines
        if x_pu < 1e-6:
            continue

        # Thermal limit: convert kA limit to MW, or use large default
        max_i_ka = float(row.get("max_i_ka", float("inf")))
        if math.isinf(max_i_ka) or max_i_ka >= 9999:
            # No thermal limit in pandapower case — use a large sentinel value
            tmax = 9999
        else:
            # Convert kA × kV × sqrt(3) → MVA ≈ MW (for DC OPF)
            tmax = round(max_i_ka * base_kv * math.sqrt(3), 1)

        entry = {
            "uid": line_uid,
            "name": f"l{fb + 1}_{tb + 1}",
            "bus_a": fb + 1,
            "bus_b": tb + 1,
            "reactance": round(x_pu, 6),
            "tmax_ab": tmax,
            "tmax_ba": tmax,
        }
        line_array.append(entry)
        line_uid += 1

    # ---- transformers (modelled as lossless lines) ---------------------------
    for _idx, row in net.trafo.iterrows():
        hv = int(row["hv_bus"])
        lv = int(row["lv_bus"])
        vk = float(row["vk_percent"])
        sn_mva = float(row["sn_mva"])
        # p.u. reactance on the system base
        x_pu = (vk / 100.0) * (base_mva / sn_mva)

        if x_pu < 1e-6:
            continue

        entry = {
            "uid": line_uid,
            "name": f"t{hv + 1}_{lv + 1}",
            "bus_a": hv + 1,
            "bus_b": lv + 1,
            "reactance": round(x_pu, 6),
            "tmax_ab": 9999,
            "tmax_ba": 9999,
        }
        line_array.append(entry)
        line_uid += 1

    # ---- assemble JSON -------------------------------------------------------
    data = {
        "options": {
            "annual_discount_rate": 0.0,
            "use_lp_names": True,
            "output_format": "csv",
            "output_compression": "uncompressed",
            "use_single_bus": False,
            "demand_fail_cost": 1000,
            "scale_objective": 1000,
            "use_kirchhoff": True,
        },
        "simulation": {
            "block_array": [{"uid": 1, "duration": 1}],
            "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1, "active": 1}],
            "scenario_array": [{"uid": 1, "probability_factor": 1}],
        },
        "system": {
            "name": "ieee30b",
            "bus_array": bus_array,
            "generator_array": generator_array,
            "demand_array": demand_array,
            "line_array": line_array,
        },
    }

    if output_path is None:
        output_path = Path(__file__).parent / "ieee30b.json"

    with open(output_path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
    print(f"Written: {output_path}")


if __name__ == "__main__":
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    convert(out)
