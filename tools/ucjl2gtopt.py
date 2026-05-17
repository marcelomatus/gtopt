#!/usr/bin/env python3
"""Convert UnitCommitment.jl benchmark JSON to gtopt system JSON."""

import argparse
import json
import os
import sys


def compute_gen_cost(curve_mw, curve_cost):
    total_cost_increase = curve_cost[-1] - curve_cost[0]
    total_power_increase = curve_mw[-1] - curve_mw[0]
    if total_power_increase <= 0:
        return 0.0
    return total_cost_increase / total_power_increase


def convert(ucjl_path, output_path=None):
    with open(ucjl_path) as f:
        data = json.load(f)

    params = data.get("Parameters", {})
    t_h = params.get("Time horizon (h)", 24)
    t_min = params.get("Time horizon (min)")
    if t_min is not None:
        t_h = t_min / 60
    T = int(t_h)

    uc_buses = data.get("Buses", {})
    name_to_uid = {}
    bus_array = []
    bus_has_load = {}

    for i, (bname, bdata) in enumerate(uc_buses.items()):
        uid = i + 1
        name_to_uid[bname] = uid
        load_mw = bdata.get("Load (MW)", 0.0)
        if isinstance(load_mw, list):
            load_mw = load_mw[0] if load_mw else 0.0
        entry = {"uid": uid, "name": f"b{uid}"}
        bus_array.append(entry)
        bus_has_load[bname] = float(load_mw) if load_mw and float(load_mw) > 0 else 0.0

    demand_array = []
    demand_uid = 0
    for bname, load_mw in bus_has_load.items():
        if load_mw > 0:
            demand_uid += 1
            bus_uid = name_to_uid[bname]
            demand_array.append(
                {
                    "uid": demand_uid,
                    "name": f"d{demand_uid}",
                    "bus": bus_uid,
                    "lmax": [[load_mw]],
                }
            )

    uc_gens = data.get("Generators", {})
    generator_array = []
    gen_uid = 0
    for gname, gdata in uc_gens.items():
        gtype = gdata.get("Type", "thermal")
        bus_name = gdata.get("Bus")
        if bus_name not in name_to_uid:
            continue
        bus_uid = name_to_uid[bus_name]
        curve_mw = gdata.get("Production cost curve (MW)", [0, 100])
        curve_cost = gdata.get("Production cost curve ($)", [0, 1000])
        pmin = curve_mw[0]
        pmax = curve_mw[-1]
        capacity = pmax
        gcost = compute_gen_cost(curve_mw, curve_cost)

        gen_uid += 1
        entry = {
            "uid": gen_uid,
            "name": gname,
            "bus": bus_uid,
            "pmin": round(pmin, 6),
            "pmax": round(pmax, 6),
            "gcost": round(gcost, 6),
            "capacity": round(capacity, 6),
        }
        if gtype.lower() != "thermal":
            entry["type"] = gtype.lower()
        generator_array.append(entry)

    uc_lines = data.get("Transmission lines", {})
    line_array = []
    line_uid = 0
    for lname, ldata in uc_lines.items():
        src = ldata.get("Source bus")
        tgt = ldata.get("Target bus")
        if src not in name_to_uid or tgt not in name_to_uid:
            continue
        x = ldata.get("Reactance (ohms)", 0.0)
        if x is None or float(x) <= 0:
            continue
        x = float(x)

        tmax = ldata.get("Normal flow limit (MW)")
        if tmax is not None:
            if isinstance(tmax, list):
                tmax = tmax[0]
            tmax = min(float(tmax), 99999.0)
        else:
            tmax = 99999.0

        line_uid += 1
        entry = {
            "uid": line_uid,
            "name": lname,
            "bus_a": name_to_uid[src],
            "bus_b": name_to_uid[tgt],
            "reactance": round(x, 6),
            "voltage": 10,
            "tmax_ab": round(tmax, 1),
            "tmax_ba": round(tmax, 1),
        }
        line_array.append(entry)

    output = {
        "options": {
            "method": "monolithic",
            "annual_discount_rate": 0.0,
            "output_format": "csv",
            "output_compression": "uncompressed",
            "use_single_bus": False,
            "use_kirchhoff": True,
            "demand_fail_cost": 1000.0,
            "scale_objective": 1000.0,
        },
        "simulation": {
            "block_array": [{"uid": 1, "duration": T}],
            "stage_array": [
                {
                    "uid": 1,
                    "first_block": 0,
                    "count_block": 1,
                    "active": 1,
                }
            ],
            "scenario_array": [
                {"uid": 1, "probability_factor": 1.0}
            ],
        },
        "system": {
            "name": os.path.splitext(os.path.basename(ucjl_path))[0],
            "bus_array": bus_array,
            "generator_array": generator_array,
            "demand_array": demand_array,
            "line_array": line_array,
        },
    }

    if output_path:
        with open(output_path, "w") as f:
            json.dump(output, f, indent=2)
        print(f"Wrote {output_path}")
        print(f"  Buses: {len(bus_array)}")
        print(f"  Demands: {len(demand_array)}")
        print(f"  Generators: {len(generator_array)}")
        print(f"  Lines: {len(line_array)}")
        print(f"  Time horizon: {T} hours")
    else:
        return output


def main():
    parser = argparse.ArgumentParser(
        description="Convert UnitCommitment.jl JSON to gtopt JSON"
    )
    parser.add_argument("input", help="UC.jl JSON file path")
    parser.add_argument("-o", "--output", help="Output gtopt JSON path")
    args = parser.parse_args()

    convert(args.input, args.output)


if __name__ == "__main__":
    main()
