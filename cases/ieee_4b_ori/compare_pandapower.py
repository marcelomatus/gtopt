#!/usr/bin/env python3
"""Compare gtopt ieee_4b_ori solution with pandapower DC OPF.

Usage:
    python3 compare_pandapower.py --gtopt-output <dir> [--tol <mw>]

Reconstructs the Grainger & Stevenson 4-bus system in pandapower
(identical topology to ieee_4b_ori.json) and runs DC OPF with the
same linear costs, then validates generation dispatch, cost, and LMPs.

Expected result (uncongested, g1 is cheapest and has sufficient capacity):
  g1 = 250 MW,  g2 = 0 MW
  cost = 5000 $/h
  all bus LMPs = 20 $/MWh
"""

import argparse
import csv
import math
import sys
from pathlib import Path

import pandapower as pp


_Z_BASE = 132.0**2 / 100.0  # Ohm  (132 kV, 100 MVA system base)


def build_net() -> pp.pandapowerNet:
    """Construct the 4-bus pandapower network matching ieee_4b_ori.json."""
    net = pp.create_empty_network()
    buses = [pp.create_bus(net, vn_kv=132, name=f"b{i}") for i in range(1, 5)]
    b1, b2, b3, b4 = buses

    # Slack with zero capacity; g1/g2 are the dispatchable generators
    pp.create_ext_grid(net, bus=b1, min_p_mw=0, max_p_mw=0)
    pp.create_gen(net, bus=b1, p_mw=0, name="g1", min_p_mw=0, max_p_mw=300)
    pp.create_gen(net, bus=b2, p_mw=0, name="g2", min_p_mw=0, max_p_mw=200)

    pp.create_load(net, bus=b3, p_mw=150, name="d3")
    pp.create_load(net, bus=b4, p_mw=100, name="d4")

    def add_line(from_b, to_b, x_pu: float, tmax_mw: float) -> None:
        pp.create_line_from_parameters(
            net,
            from_bus=from_b,
            to_bus=to_b,
            length_km=1,
            r_ohm_per_km=0,
            x_ohm_per_km=x_pu * _Z_BASE,
            c_nf_per_km=0,
            max_i_ka=tmax_mw / (132.0 * math.sqrt(3)),
        )

    add_line(b1, b2, 0.02, 300)
    add_line(b1, b3, 0.02, 300)
    add_line(b2, b3, 0.03, 200)
    add_line(b2, b4, 0.02, 200)
    add_line(b3, b4, 0.03, 150)

    pp.create_poly_cost(net, element=0, et="ext_grid", cp1_eur_per_mw=1e6)
    pp.create_poly_cost(net, element=0, et="gen", cp1_eur_per_mw=20.0)  # g1
    pp.create_poly_cost(net, element=1, et="gen", cp1_eur_per_mw=35.0)  # g2
    return net


def read_gtopt_generation(output_dir: Path) -> list[float]:
    gen_file = output_dir / "Generator" / "generation_sol.csv"
    if not gen_file.exists():
        raise FileNotFoundError(f"Not found: {gen_file}")
    with open(gen_file, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        header = next(reader)
        row = next(reader)
    uid_start = next(i for i, h in enumerate(header) if h.startswith("uid:"))
    return [float(row[i]) for i in range(uid_start, len(row))]


def read_gtopt_lmps(output_dir: Path) -> list[float]:
    lmp_file = output_dir / "Bus" / "balance_dual.csv"
    if not lmp_file.exists():
        raise FileNotFoundError(f"Not found: {lmp_file}")
    with open(lmp_file, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        header = next(reader)
        row = next(reader)
    uid_start = next(i for i, h in enumerate(header) if h.startswith("uid:"))
    return [float(row[i]) for i in range(uid_start, len(row))]


def read_gtopt_cost(output_dir: Path, scale: float = 1000.0) -> float:
    sol_file = output_dir / "solution.csv"
    if not sol_file.exists():
        raise FileNotFoundError(f"Not found: {sol_file}")
    with open(sol_file, newline="", encoding="utf-8") as fh:
        for line in fh:
            key, _, val = line.strip().partition(",")
            if key.strip() == "obj_value":
                return float(val.strip()) * scale
    raise ValueError("obj_value not found in solution.csv")


def compare(output_dir: Path, tol_mw: float = 1.0, tol_lmp: float = 0.1) -> bool:
    net = build_net()
    pp.rundcopp(net, verbose=False)

    pp_gen = list(net.res_gen["p_mw"].values)
    pp_cost = net.res_cost
    pp_lmps = list(net.res_bus["lam_p"].values)

    gtopt_gen = read_gtopt_generation(output_dir)
    gtopt_cost = read_gtopt_cost(output_dir)
    gtopt_lmps = read_gtopt_lmps(output_dir)

    passed = True

    print("Generation comparison (MW):")
    for i, (pp_val, gt_val) in enumerate(zip(pp_gen, gtopt_gen)):
        diff = abs(pp_val - gt_val)
        status = "PASS" if diff <= tol_mw else "FAIL"
        if diff > tol_mw:
            passed = False
        print(f"  g{i+1}: pandapower={pp_val:.4f}  gtopt={gt_val:.4f}  diff={diff:.4f}  [{status}]")

    cost_diff = abs(pp_cost - gtopt_cost)
    cost_tol = max(1.0, abs(gtopt_cost) * 1e-3)
    cost_status = "PASS" if cost_diff <= cost_tol else "FAIL"
    if cost_diff > cost_tol:
        passed = False
    print(f"Cost: pandapower={pp_cost:.2f}  gtopt={gtopt_cost:.2f}  diff={cost_diff:.4f}  [{cost_status}]")

    print("Bus LMP comparison ($/MWh):")
    bus_names = [f"b{i+1}" for i in range(len(pp_lmps))]
    for name, pp_val, gt_val in zip(bus_names, pp_lmps, gtopt_lmps):
        diff = abs(pp_val - gt_val)
        status = "PASS" if diff <= tol_lmp else "FAIL"
        if diff > tol_lmp:
            passed = False
        print(f"  {name}: pandapower={pp_val:.4f}  gtopt={gt_val:.4f}  diff={diff:.4f}  [{status}]")

    return passed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gtopt-output", required=True, type=Path)
    parser.add_argument("--tol", type=float, default=1.0, help="Generation tolerance (MW)")
    parser.add_argument("--tol-lmp", type=float, default=0.1, help="LMP tolerance ($/MWh)")
    args = parser.parse_args()

    ok = compare(args.gtopt_output, tol_mw=args.tol, tol_lmp=args.tol_lmp)
    if ok:
        print("RESULT: PASS — pandapower and gtopt agree")
        sys.exit(0)
    else:
        print("RESULT: FAIL — mismatch detected")
        sys.exit(1)


if __name__ == "__main__":
    main()
