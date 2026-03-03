#!/usr/bin/env python3
"""Compare gtopt s1b solution with pandapower DC OPF.

Usage:
    python3 compare_pandapower.py --gtopt-output <dir> [--tol <mw>]

Builds the identical 1-bus system (g1=$20 200MW, g2=$40 300MW, d1=250MW)
in pandapower, solves DC OPF, then compares generation dispatch and
total cost against the gtopt CSV results.

Expected result:
  g1 = 200 MW  (cheap generator, fully loaded)
  g2 =  50 MW  (expensive generator fills the gap)
  cost = 6000 $/h  (stored as 6 in gtopt with scale_objective=1000)
"""

import argparse
import csv
import sys
from pathlib import Path

import pandapower as pp


def build_net() -> pp.pandapowerNet:
    """Construct the 1-bus pandapower network matching s1b.json."""
    net = pp.create_empty_network()
    b1 = pp.create_bus(net, vn_kv=132, name="b1")
    # Slack with zero capacity — g1 and g2 are the only generators
    pp.create_ext_grid(net, bus=b1, min_p_mw=0, max_p_mw=0)
    pp.create_gen(net, bus=b1, p_mw=0, name="g1", min_p_mw=0, max_p_mw=200)
    pp.create_gen(net, bus=b1, p_mw=0, name="g2", min_p_mw=0, max_p_mw=300)
    pp.create_load(net, bus=b1, p_mw=250, name="d1")
    pp.create_poly_cost(net, element=0, et="ext_grid", cp1_eur_per_mw=1e6)
    pp.create_poly_cost(net, element=0, et="gen", cp1_eur_per_mw=20.0)
    pp.create_poly_cost(net, element=1, et="gen", cp1_eur_per_mw=40.0)
    return net


def read_gtopt_generation(output_dir: Path) -> list[float]:
    """Read per-generator dispatch from gtopt Generator/generation_sol.csv.

    Returns a list of generation values (MW) in generator uid order.
    """
    gen_file = output_dir / "Generator" / "generation_sol.csv"
    if not gen_file.exists():
        raise FileNotFoundError(f"Not found: {gen_file}")
    with open(gen_file, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        header = next(reader)
        row = next(reader)
    # Header: "scenario","stage","block","uid:1","uid:2",...
    # Data row: 1,1,1,200,50,...
    uid_start = next(i for i, h in enumerate(header) if h.startswith("uid:"))
    return [float(row[i]) for i in range(uid_start, len(row))]


def read_gtopt_cost(output_dir: Path) -> float:
    """Read objective value from gtopt solution.csv (scaled by scale_objective)."""
    sol_file = output_dir / "solution.csv"
    if not sol_file.exists():
        raise FileNotFoundError(f"Not found: {sol_file}")
    with open(sol_file, newline="", encoding="utf-8") as fh:
        for line in fh:
            key, _, val = line.strip().partition(",")
            if key.strip() == "obj_value":
                return float(val.strip())
    raise ValueError("obj_value not found in solution.csv")


def compare(output_dir: Path, tol_mw: float = 1.0, scale_objective: float = 1000.0) -> bool:
    """Run pandapower OPF, compare with gtopt results.  Returns True if pass."""
    net = build_net()
    pp.rundcopp(net, verbose=False)

    pp_gen = list(net.res_gen["p_mw"].values)   # [g1, g2]
    pp_cost = net.res_cost                        # $/h

    gtopt_gen = read_gtopt_generation(output_dir)
    gtopt_cost_scaled = read_gtopt_cost(output_dir)
    gtopt_cost = gtopt_cost_scaled * scale_objective

    passed = True

    # --- generation comparison ---
    print("Generation comparison (MW):")
    gen_names = [f"g{i+1}" for i in range(len(pp_gen))]
    for name, pp_val, gt_val in zip(gen_names, pp_gen, gtopt_gen):
        diff = abs(pp_val - gt_val)
        status = "PASS" if diff <= tol_mw else "FAIL"
        if diff > tol_mw:
            passed = False
        print(f"  {name}: pandapower={pp_val:.4f}  gtopt={gt_val:.4f}  diff={diff:.4f}  [{status}]")

    # --- cost comparison ---
    cost_diff = abs(pp_cost - gtopt_cost)
    cost_tol = max(1.0, abs(gtopt_cost) * 1e-3)
    cost_status = "PASS" if cost_diff <= cost_tol else "FAIL"
    if cost_diff > cost_tol:
        passed = False
    print(f"Cost: pandapower={pp_cost:.2f}  gtopt={gtopt_cost:.2f}  diff={cost_diff:.4f}  [{cost_status}]")

    return passed


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--gtopt-output", required=True, type=Path,
                        help="Directory containing gtopt CSV output files")
    parser.add_argument("--tol", type=float, default=1.0,
                        help="Tolerance for generation comparison (MW)")
    args = parser.parse_args()

    ok = compare(args.gtopt_output, tol_mw=args.tol)
    if ok:
        print("RESULT: PASS — pandapower and gtopt agree")
        sys.exit(0)
    else:
        print("RESULT: FAIL — mismatch detected")
        sys.exit(1)


if __name__ == "__main__":
    main()
