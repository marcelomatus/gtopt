#!/usr/bin/env python3
"""Compare gtopt ieee30b solution with pandapower DC OPF.

Usage:
    python3 compare_pandapower.py --gtopt-output <dir> [--tol <mw>]

Loads pn.case_ieee30(), sets linear-only costs (cp2=0), runs DC OPF,
then validates total cost and bus LMPs against gtopt CSV results.

Expected result (uncongested, g1 cheapest):
  g1 = 283.4 MW (ext_grid serves all load)
  cost = 5668 $/h  (283.4 MW * 20 $/MWh, stored as 5.668 in gtopt scale_objective=1000)
  all bus LMPs = 20 $/MWh
"""

import argparse
import csv
import sys
from pathlib import Path

import pandapower as pp
import pandapower.networks as pn


def build_net() -> pp.pandapowerNet:
    """Load case_ieee30 with linear-only costs matching the gtopt conversion."""
    net = pn.case_ieee30()
    # Use linear costs only (cp2 removed) to match gtopt's gcost model
    net.poly_cost["cp2_eur_per_mw2"] = 0.0
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

    pp_ext = float(net.res_ext_grid["p_mw"].sum())
    pp_gen = list(net.res_gen["p_mw"].values)
    pp_all = [pp_ext] + pp_gen   # [ext_grid, gen0..gen4]
    pp_cost = net.res_cost
    pp_lmps = list(net.res_bus["lam_p"].values)

    gtopt_gen = read_gtopt_generation(output_dir)
    gtopt_cost = read_gtopt_cost(output_dir)
    gtopt_lmps = read_gtopt_lmps(output_dir)

    passed = True

    # --- total generation / total cost ---
    pp_total = sum(pp_all)
    gt_total = sum(gtopt_gen)
    diff_total = abs(pp_total - gt_total)
    status_total = "PASS" if diff_total <= tol_mw else "FAIL"
    if diff_total > tol_mw:
        passed = False
    print(f"Total generation: pandapower={pp_total:.4f}  gtopt={gt_total:.4f}  "
          f"diff={diff_total:.4f}  [{status_total}]")

    cost_diff = abs(pp_cost - gtopt_cost)
    cost_tol = max(1.0, abs(gtopt_cost) * 1e-3)
    cost_status = "PASS" if cost_diff <= cost_tol else "FAIL"
    if cost_diff > cost_tol:
        passed = False
    print(f"Cost: pandapower={pp_cost:.2f}  gtopt={gtopt_cost:.2f}  "
          f"diff={cost_diff:.4f}  [{cost_status}]")

    # --- bus LMPs ---
    print("Bus LMP comparison ($/MWh):")
    for i, (pp_val, gt_val) in enumerate(zip(pp_lmps, gtopt_lmps)):
        diff = abs(pp_val - gt_val)
        status = "PASS" if diff <= tol_lmp else "FAIL"
        if diff > tol_lmp:
            passed = False
        print(f"  b{i+1}: pandapower={pp_val:.4f}  gtopt={gt_val:.4f}  "
              f"diff={diff:.4f}  [{status}]")

    return passed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gtopt-output", required=True, type=Path)
    parser.add_argument("--tol", type=float, default=1.0)
    parser.add_argument("--tol-lmp", type=float, default=0.1)
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
