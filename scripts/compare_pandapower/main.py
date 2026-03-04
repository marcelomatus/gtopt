#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compare gtopt solver output against pandapower DC OPF reference.

Usage:
    compare_pandapower --case <name> --gtopt-output <dir> [--tol <MW>] [--tol-lmp <$/MWh>]

Supported cases:
    s1b          1-bus dispatch (g1=$20/MWh 200 MW, g2=$40/MWh 300 MW, d1=250 MW)
    ieee_4b_ori  Grainger & Stevenson 4-bus OPF (g1=$20, g2=$35, 5 lines)
    ieee30b      IEEE 30-bus standard network with linear costs

For each case, reconstructs the equivalent pandapower network, runs DC OPF,
reads gtopt CSV results, and compares generation dispatch, cost, and (where
applicable) bus locational marginal prices.

Exit codes:
    0  PASS — pandapower and gtopt agree within tolerance
    1  FAIL — numeric mismatch detected
    2  ERROR — missing output file or unknown case
"""

import argparse
import csv
import math
import sys
from pathlib import Path

_SCALE_OBJECTIVE = 1000.0  # gtopt scale_objective used in all supported cases


# ---------------------------------------------------------------------------
# Shared I/O helpers
# ---------------------------------------------------------------------------


def read_gtopt_generation(output_dir: Path) -> list:
    """Return per-generator dispatch (MW) from Generator/generation_sol.csv.

    The CSV has a header row whose uid columns start with ``uid:``.
    Only the first data row (single block/stage/scenario) is read.
    """
    gen_file = output_dir / "Generator" / "generation_sol.csv"
    if not gen_file.exists():
        raise FileNotFoundError(f"Not found: {gen_file}")
    with open(gen_file, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        header = next(reader)
        row = next(reader)
    uid_start = next(i for i, h in enumerate(header) if h.startswith("uid:"))
    return [float(row[i]) for i in range(uid_start, len(row))]


def read_gtopt_lmps(output_dir: Path) -> list:
    """Return bus LMPs ($/MWh) from Bus/balance_dual.csv.

    Reads only the first data row (single block/stage/scenario).
    """
    lmp_file = output_dir / "Bus" / "balance_dual.csv"
    if not lmp_file.exists():
        raise FileNotFoundError(f"Not found: {lmp_file}")
    with open(lmp_file, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        header = next(reader)
        row = next(reader)
    uid_start = next(i for i, h in enumerate(header) if h.startswith("uid:"))
    return [float(row[i]) for i in range(uid_start, len(row))]


def read_gtopt_cost(output_dir: Path, scale: float = _SCALE_OBJECTIVE) -> float:
    """Return the objective value from solution.csv, scaled by *scale*.

    gtopt stores ``obj_value / scale_objective`` in solution.csv; multiplying
    by *scale* (default 1000) recovers the original cost in $/h.
    """
    sol_file = output_dir / "solution.csv"
    if not sol_file.exists():
        raise FileNotFoundError(f"Not found: {sol_file}")
    with open(sol_file, newline="", encoding="utf-8") as fh:
        for line in fh:
            key, _, val = line.strip().partition(",")
            if key.strip() == "obj_value":
                return float(val.strip()) * scale
    raise ValueError("obj_value not found in solution.csv")


# ---------------------------------------------------------------------------
# Network builders
# ---------------------------------------------------------------------------

_Z_BASE_4B = 132.0**2 / 100.0  # Ω  (132 kV, 100 MVA system base)


def build_net_s1b():
    """Construct the 1-bus pandapower network matching s1b.json.

    Single bus, two generators (g1 cheap, g2 expensive), one load.
    Expected optimal: g1=200 MW, g2=50 MW, cost=6000 $/h.
    """
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    net = pp.create_empty_network()
    b1 = pp.create_bus(net, vn_kv=132, name="b1")
    pp.create_ext_grid(net, bus=b1, min_p_mw=0, max_p_mw=0)
    pp.create_gen(net, bus=b1, p_mw=0, name="g1", min_p_mw=0, max_p_mw=200)
    pp.create_gen(net, bus=b1, p_mw=0, name="g2", min_p_mw=0, max_p_mw=300)
    pp.create_load(net, bus=b1, p_mw=250, name="d1")
    pp.create_poly_cost(net, element=0, et="ext_grid", cp1_eur_per_mw=1e6)
    pp.create_poly_cost(net, element=0, et="gen", cp1_eur_per_mw=20.0)
    pp.create_poly_cost(net, element=1, et="gen", cp1_eur_per_mw=40.0)
    return net


def build_net_ieee_4b_ori():
    """Construct the 4-bus Grainger & Stevenson network matching ieee_4b_ori.json.

    4 buses, 2 generators (g1@b1 $20, g2@b2 $35), 2 loads, 5 lines.
    Expected optimal: g1=250 MW, g2=0 MW, cost=5000 $/h, all LMPs=$20/MWh.
    """
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    net = pp.create_empty_network()
    buses = [pp.create_bus(net, vn_kv=132, name=f"b{i}") for i in range(1, 5)]
    b1, b2, b3, b4 = buses

    pp.create_ext_grid(net, bus=b1, min_p_mw=0, max_p_mw=0)
    pp.create_gen(net, bus=b1, p_mw=0, name="g1", min_p_mw=0, max_p_mw=300)
    pp.create_gen(net, bus=b2, p_mw=0, name="g2", min_p_mw=0, max_p_mw=200)

    pp.create_load(net, bus=b3, p_mw=150, name="d3")
    pp.create_load(net, bus=b4, p_mw=100, name="d4")

    def _add_line(from_b, to_b, x_pu: float, tmax_mw: float) -> None:
        pp.create_line_from_parameters(
            net,
            from_bus=from_b,
            to_bus=to_b,
            length_km=1,
            r_ohm_per_km=0,
            x_ohm_per_km=x_pu * _Z_BASE_4B,
            c_nf_per_km=0,
            max_i_ka=tmax_mw / (132.0 * math.sqrt(3)),
        )

    _add_line(b1, b2, 0.02, 300)
    _add_line(b1, b3, 0.02, 300)
    _add_line(b2, b3, 0.03, 200)
    _add_line(b2, b4, 0.02, 200)
    _add_line(b3, b4, 0.03, 150)

    pp.create_poly_cost(net, element=0, et="ext_grid", cp1_eur_per_mw=1e6)
    pp.create_poly_cost(net, element=0, et="gen", cp1_eur_per_mw=20.0)  # g1
    pp.create_poly_cost(net, element=1, et="gen", cp1_eur_per_mw=35.0)  # g2
    return net


def build_net_ieee30b():
    """Load case_ieee30 with linear-only costs matching the gtopt conversion.

    Zeroes the quadratic cost term (cp2) so the OPF uses pure linear costs,
    matching the ``gcost`` field in ieee30b.json.
    Expected: ext_grid serves all 283.4 MW at $20/MWh, cost ≈ 5668 $/h.
    """
    import pandapower.networks as pn  # pylint: disable=import-outside-toplevel

    net = pn.case_ieee30()
    net.poly_cost["cp2_eur_per_mw2"] = 0.0
    return net


# ---------------------------------------------------------------------------
# Per-case comparison functions
# ---------------------------------------------------------------------------


def _compare_s1b(output_dir: Path, tol_mw: float, tol_lmp: float) -> bool:
    """Compare s1b generation and cost; no bus LMPs for this 1-bus case."""
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    net = build_net_s1b()
    pp.rundcopp(net, verbose=False)

    pp_gen = list(net.res_gen["p_mw"].values)
    pp_cost = net.res_cost

    gtopt_gen = read_gtopt_generation(output_dir)
    gtopt_cost = read_gtopt_cost(output_dir)

    passed = True

    print("Generation comparison (MW):")
    for i, (pp_val, gt_val) in enumerate(zip(pp_gen, gtopt_gen)):
        diff = abs(pp_val - gt_val)
        status = "PASS" if diff <= tol_mw else "FAIL"
        if diff > tol_mw:
            passed = False
        print(
            f"  g{i + 1}: pandapower={pp_val:.4f}  gtopt={gt_val:.4f}"
            f"  diff={diff:.4f}  [{status}]"
        )

    cost_diff = abs(pp_cost - gtopt_cost)
    cost_tol = max(1.0, abs(gtopt_cost) * 1e-3)
    cost_status = "PASS" if cost_diff <= cost_tol else "FAIL"
    if cost_diff > cost_tol:
        passed = False
    print(
        f"Cost: pandapower={pp_cost:.2f}  gtopt={gtopt_cost:.2f}"
        f"  diff={cost_diff:.4f}  [{cost_status}]"
    )

    return passed


def _compare_ieee_4b_ori(output_dir: Path, tol_mw: float, tol_lmp: float) -> bool:
    """Compare ieee_4b_ori generation, cost, and bus LMPs."""
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    net = build_net_ieee_4b_ori()
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
        print(
            f"  g{i + 1}: pandapower={pp_val:.4f}  gtopt={gt_val:.4f}"
            f"  diff={diff:.4f}  [{status}]"
        )

    cost_diff = abs(pp_cost - gtopt_cost)
    cost_tol = max(1.0, abs(gtopt_cost) * 1e-3)
    cost_status = "PASS" if cost_diff <= cost_tol else "FAIL"
    if cost_diff > cost_tol:
        passed = False
    print(
        f"Cost: pandapower={pp_cost:.2f}  gtopt={gtopt_cost:.2f}"
        f"  diff={cost_diff:.4f}  [{cost_status}]"
    )

    print("Bus LMP comparison ($/MWh):")
    bus_names = [f"b{i + 1}" for i in range(len(pp_lmps))]
    for name, pp_val, gt_val in zip(bus_names, pp_lmps, gtopt_lmps):
        diff = abs(pp_val - gt_val)
        status = "PASS" if diff <= tol_lmp else "FAIL"
        if diff > tol_lmp:
            passed = False
        print(
            f"  {name}: pandapower={pp_val:.4f}  gtopt={gt_val:.4f}"
            f"  diff={diff:.4f}  [{status}]"
        )

    return passed


def _compare_ieee30b(output_dir: Path, tol_mw: float, tol_lmp: float) -> bool:
    """Compare ieee30b total generation, cost, and bus LMPs.

    The ext_grid generation is summed separately because gtopt models
    it as a regular generator while pandapower uses res_ext_grid.
    """
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    net = build_net_ieee30b()
    pp.rundcopp(net, verbose=False)

    pp_ext = float(net.res_ext_grid["p_mw"].sum())
    pp_gen = list(net.res_gen["p_mw"].values)
    pp_all = [pp_ext] + pp_gen
    pp_cost = net.res_cost
    pp_lmps = list(net.res_bus["lam_p"].values)

    gtopt_gen = read_gtopt_generation(output_dir)
    gtopt_cost = read_gtopt_cost(output_dir)
    gtopt_lmps = read_gtopt_lmps(output_dir)

    passed = True

    pp_total = sum(pp_all)
    gt_total = sum(gtopt_gen)
    diff_total = abs(pp_total - gt_total)
    status_total = "PASS" if diff_total <= tol_mw else "FAIL"
    if diff_total > tol_mw:
        passed = False
    print(
        f"Total generation: pandapower={pp_total:.4f}  gtopt={gt_total:.4f}"
        f"  diff={diff_total:.4f}  [{status_total}]"
    )

    cost_diff = abs(pp_cost - gtopt_cost)
    cost_tol = max(1.0, abs(gtopt_cost) * 1e-3)
    cost_status = "PASS" if cost_diff <= cost_tol else "FAIL"
    if cost_diff > cost_tol:
        passed = False
    print(
        f"Cost: pandapower={pp_cost:.2f}  gtopt={gtopt_cost:.2f}"
        f"  diff={cost_diff:.4f}  [{cost_status}]"
    )

    print("Bus LMP comparison ($/MWh):")
    for i, (pp_val, gt_val) in enumerate(zip(pp_lmps, gtopt_lmps)):
        diff = abs(pp_val - gt_val)
        status = "PASS" if diff <= tol_lmp else "FAIL"
        if diff > tol_lmp:
            passed = False
        print(
            f"  b{i + 1}: pandapower={pp_val:.4f}  gtopt={gt_val:.4f}"
            f"  diff={diff:.4f}  [{status}]"
        )

    return passed


# ---------------------------------------------------------------------------
# Dispatch table
# ---------------------------------------------------------------------------

_CASES = {
    "s1b": _compare_s1b,
    "ieee_4b_ori": _compare_ieee_4b_ori,
    "ieee30b": _compare_ieee30b,
}


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def main() -> None:
    """Parse CLI arguments and run the selected case comparison."""
    parser = argparse.ArgumentParser(
        prog="compare_pandapower",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--case",
        required=True,
        choices=sorted(_CASES),
        help="Test case name (selects network topology and comparison logic).",
    )
    parser.add_argument(
        "--gtopt-output",
        required=True,
        type=Path,
        metavar="DIR",
        help="Directory containing gtopt CSV output files.",
    )
    parser.add_argument(
        "--tol",
        type=float,
        default=1.0,
        metavar="MW",
        help="Generation / total-power tolerance in MW (default: 1.0).",
    )
    parser.add_argument(
        "--tol-lmp",
        type=float,
        default=0.1,
        metavar="$/MWh",
        help="Bus LMP tolerance in $/MWh (default: 0.1).",
    )
    args = parser.parse_args()

    try:
        compare_fn = _CASES[args.case]
        ok = compare_fn(args.gtopt_output, tol_mw=args.tol, tol_lmp=args.tol_lmp)
    except FileNotFoundError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(2)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(2)

    if ok:
        print("RESULT: PASS — pandapower and gtopt agree")
        sys.exit(0)
    else:
        print("RESULT: FAIL — mismatch detected")
        sys.exit(1)


if __name__ == "__main__":
    main()
