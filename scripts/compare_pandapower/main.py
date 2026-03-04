#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compare gtopt solver output against pandapower DC OPF reference.

Usage:
    compare_pandapower --case <name> --gtopt-output <dir> [options]

    compare_pandapower --case <name> --gtopt-output <dir> \\
        --pandapower-file <net.json>

    compare_pandapower --case <name> --save-pandapower-file <net.json>

Supported cases:
    s1b          1-bus dispatch (g1=$20/MWh 200 MW, g2=$40/MWh 300 MW, d1=250 MW)
    ieee_4b_ori  Grainger & Stevenson 4-bus OPF (g1=$20, g2=$35, 5 lines)
    ieee30b      IEEE 30-bus standard network with linear costs
    ieee_57b     IEEE 57-bus standard network with linear costs (large case, 1 block)
    bat_4b_24    Grainger & Stevenson 4-bus + solar + battery, 24 hourly blocks

For each case, reconstructs the equivalent pandapower network, runs DC OPF,
reads gtopt CSV results, and compares generation dispatch, cost, and (where
applicable) bus locational marginal prices.

For battery cases (bat_4b_24) the comparison reads the battery
charge/discharge schedule from the gtopt output and uses it to compute the
effective load per block before running pandapower.  This validates that the
conventional-generator dispatch is consistent with the DC power flow given the
battery dispatch that gtopt chose.

External pandapower files:
    Use --pandapower-file to load a pre-saved pandapower network from a JSON
    file produced by pandapower.to_json().  This allows using any external
    pandapower network instead of the built-in network builders, so the
    comparison is not restricted to the internally defined cases.

    For static cases (all except bat_4b_24) the file replaces the built-in
    network completely.  For bat_4b_24 the file provides the base 4-bus
    topology; battery net injections are still applied per-block.

    Use --save-pandapower-file to write the built network to a JSON file for
    later reuse.  Combine with --case to select which network to save:

        compare_pandapower --case s1b --save-pandapower-file cases/s1b/pandapower_net.json

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
# Pandapower network file I/O
# ---------------------------------------------------------------------------


def save_pandapower_net(net, file_path: Path) -> None:
    """Save a pandapower network to a JSON file using pandapower.to_json().

    Parameters
    ----------
    net:       pandapower network object to save.
    file_path: destination path for the JSON file.
    """
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    file_path = Path(file_path)
    file_path.parent.mkdir(parents=True, exist_ok=True)
    pp.to_json(net, str(file_path))


def load_pandapower_net(file_path: Path):
    """Load a pandapower network from a JSON file produced by to_json().

    Parameters
    ----------
    file_path: path to the JSON file saved by save_pandapower_net() or
               pandapower.to_json().

    Returns
    -------
    pandapower network object.

    Raises
    ------
    FileNotFoundError if *file_path* does not exist.
    """
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    file_path = Path(file_path)
    if not file_path.exists():
        raise FileNotFoundError(f"Pandapower network file not found: {file_path}")
    return pp.from_json(str(file_path))


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


def read_gtopt_battery_dispatch(output_dir: Path) -> tuple:
    """Return (fout, finp) lists from Battery/fout_sol.csv and finp_sol.csv.

    Each list has one float per block.  ``fout[b]`` is the discharge power
    (MW injected into the grid) and ``finp[b]`` is the charge power (MW drawn
    from the grid) at block *b* for battery uid:1.
    """

    def _read_battery_csv(path: Path) -> list:
        if not path.exists():
            raise FileNotFoundError(f"Not found: {path}")
        with open(path, newline="", encoding="utf-8") as fh:
            reader = csv.reader(fh)
            next(reader)  # skip header
            return [float(row[-1]) for row in reader]

    bat_dir = output_dir / "Battery"
    fout = _read_battery_csv(bat_dir / "fout_sol.csv")
    finp = _read_battery_csv(bat_dir / "finp_sol.csv")
    return fout, finp


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


def build_net_ieee_57b():
    """Load case57 with linear-only costs matching the gtopt conversion.

    Zeroes the quadratic cost term (cp2) so the OPF uses pure linear costs,
    matching the ``gcost`` field in ieee_57b.json (generated by pp2gtopt).
    The network has 57 buses, 7 generators (including ext_grid), and 80 branches.
    """
    import pandapower.networks as pn  # pylint: disable=import-outside-toplevel

    net = pn.case57()
    net.poly_cost["cp2_eur_per_mw2"] = 0.0
    return net


# Solar generation profile for bat_4b_24 (same shape as ieee_9b)
_SOLAR_PROFILE_4B = [
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.0,
    0.05,
    0.15,
    0.35,
    0.55,
    0.75,
    0.9,
    1.0,
    0.95,
    0.85,
    0.7,
    0.5,
    0.3,
    0.1,
    0.02,
    0.0,
    0.0,
    0.0,
    0.0,
]

# Demand profiles for bat_4b_24 (loads at b3 and b4)
_DEMAND_4B_D3 = [
    30.0,
    28.0,
    27.0,
    27.0,
    28.0,
    32.0,
    40.0,
    55.0,
    70.0,
    80.0,
    85.0,
    88.0,
    90.0,
    88.0,
    84.0,
    80.0,
    82.0,
    88.0,
    100.0,
    110.0,
    105.0,
    95.0,
    75.0,
    50.0,
]
_DEMAND_4B_D4 = [
    20.0,
    18.0,
    17.0,
    17.0,
    18.0,
    22.0,
    28.0,
    38.0,
    48.0,
    55.0,
    58.0,
    60.0,
    62.0,
    60.0,
    57.0,
    55.0,
    56.0,
    60.0,
    68.0,
    75.0,
    72.0,
    65.0,
    50.0,
    32.0,
]


def build_net_bat_4b_24(block: int, bat_fout: float, bat_finp: float):
    """Build the 4-bus network for a single block of bat_4b_24.

    The battery is connected to bus b3.  Its net effect on the power balance is
    modelled as:

    * Discharge (``bat_fout``): extra generation at b3 (reduces g1/g2 burden).
    * Charge   (``bat_finp``): extra demand at b3 (increases g1/g2 burden).

    The net battery injection ``bat_fout - bat_finp`` is added as a fixed-output
    generator (if positive) or a fixed load (if negative) at bus b3.

    Parameters
    ----------
    block:    0-based block index (0..23).
    bat_fout: Battery discharge power (MW) at this block.
    bat_finp: Battery charge power (MW) at this block.
    """
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    net = pp.create_empty_network()

    buses = [pp.create_bus(net, vn_kv=132, name=f"b{i}") for i in range(1, 5)]
    b1, b2, b3, b4 = buses

    pp.create_ext_grid(net, bus=b1, min_p_mw=0, max_p_mw=0)

    pp.create_gen(net, bus=b1, p_mw=0, name="g1", min_p_mw=0, max_p_mw=250)
    pp.create_gen(net, bus=b2, p_mw=0, name="g2", min_p_mw=0, max_p_mw=150)
    solar_cap = 90.0 * _SOLAR_PROFILE_4B[block]
    pp.create_gen(net, bus=b1, p_mw=0, name="g_solar", min_p_mw=0, max_p_mw=solar_cap)

    pp.create_load(net, bus=b3, p_mw=_DEMAND_4B_D3[block], name="d3")
    pp.create_load(net, bus=b4, p_mw=_DEMAND_4B_D4[block], name="d4")

    bat_net = bat_fout - bat_finp
    if bat_net > 1e-6:
        pp.create_gen(
            net,
            bus=b3,
            p_mw=bat_net,
            name="bat_net",
            min_p_mw=bat_net,
            max_p_mw=bat_net,
        )
    elif bat_net < -1e-6:
        pp.create_load(net, bus=b3, p_mw=-bat_net, name="bat_net")

    line_params = [
        (b1, b2, 0.02, 300),
        (b1, b3, 0.02, 300),
        (b2, b3, 0.03, 200),
        (b2, b4, 0.02, 200),
        (b3, b4, 0.03, 150),
    ]
    z_base = 132.0**2 / 100.0  # Ω (132 kV, 100 MVA base)
    for from_b, to_b, x_pu, tmax in line_params:
        pp.create_line_from_parameters(
            net,
            from_bus=from_b,
            to_bus=to_b,
            length_km=1,
            r_ohm_per_km=0,
            x_ohm_per_km=x_pu * z_base,
            c_nf_per_km=0,
            max_i_ka=tmax / (132.0 * math.sqrt(3)),
        )

    pp.create_poly_cost(net, element=0, et="ext_grid", cp1_eur_per_mw=1e6)
    pp.create_poly_cost(net, element=0, et="gen", cp1_eur_per_mw=20.0)  # g1
    pp.create_poly_cost(net, element=1, et="gen", cp1_eur_per_mw=40.0)  # g2
    pp.create_poly_cost(net, element=2, et="gen", cp1_eur_per_mw=0.0)  # g_solar
    if bat_net > 1e-6:
        pp.create_poly_cost(net, element=3, et="gen", cp1_eur_per_mw=0.0)

    return net


# ---------------------------------------------------------------------------
# Per-case comparison functions
# ---------------------------------------------------------------------------


def _compare_s1b(
    output_dir: Path,
    tol_mw: float,
    tol_lmp: float,
    pandapower_file: Path | None = None,
) -> bool:
    """Compare s1b generation and cost; no bus LMPs for this 1-bus case."""
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    net = (
        load_pandapower_net(pandapower_file)
        if pandapower_file is not None
        else build_net_s1b()
    )
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


def _compare_ieee_4b_ori(
    output_dir: Path,
    tol_mw: float,
    tol_lmp: float,
    pandapower_file: Path | None = None,
) -> bool:
    """Compare ieee_4b_ori generation, cost, and bus LMPs."""
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    net = (
        load_pandapower_net(pandapower_file)
        if pandapower_file is not None
        else build_net_ieee_4b_ori()
    )
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


def _compare_ieee30b(
    output_dir: Path,
    tol_mw: float,
    tol_lmp: float,
    pandapower_file: Path | None = None,
) -> bool:
    """Compare ieee30b total generation, cost, and bus LMPs.

    The ext_grid generation is summed separately because gtopt models
    it as a regular generator while pandapower uses res_ext_grid.
    """
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    net = (
        load_pandapower_net(pandapower_file)
        if pandapower_file is not None
        else build_net_ieee30b()
    )
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


def _compare_ieee_57b(
    output_dir: Path,
    tol_mw: float,
    tol_lmp: float,
    pandapower_file: Path | None = None,
) -> bool:
    """Compare ieee_57b total generation, cost, and bus LMPs.

    Uses case57() with quadratic costs zeroed so it matches the linear-cost
    gtopt conversion produced by pp2gtopt.  The ext_grid generation is summed
    separately (as in the ieee30b case) because gtopt represents it as a
    regular generator while pandapower stores it in res_ext_grid.
    """
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    net = (
        load_pandapower_net(pandapower_file)
        if pandapower_file is not None
        else build_net_ieee_57b()
    )
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


def _compare_bat_4b_24(
    output_dir: Path,
    tol_mw: float,
    tol_lmp: float,
    pandapower_file: Path | None = None,
) -> bool:
    """Compare bat_4b_24 conventional-generator dispatch across 24 hourly blocks.

    bat_4b_24 extends the Grainger & Stevenson 4-bus network with a 24-hour
    solar profile and a 200 MWh battery connected to bus b3.  All demands are
    fully served in the reference gtopt solution (no load shedding), so the
    lmax values can be used directly as fixed loads in pandapower.

    For each block the battery's net injection at b3 (discharge - charge) is
    accounted for before running pandapower DC OPF.  The three conventional
    generators (g1/$20, g2/$40, g_solar/$0) are compared.

    LMP comparison is skipped for the same reason as in the battery case:
    battery coupling between blocks means the dual variables from a single-block
    pandapower OPF do not match gtopt's multi-block duals.

    When *pandapower_file* is provided it is ignored for bat_4b_24 because the
    network must be rebuilt per-block to apply the battery dispatch.  The
    built-in :func:`build_net_bat_4b_24` is always used for this case.
    """
    import pandapower as pp  # pylint: disable=import-outside-toplevel

    if pandapower_file is not None:
        print(
            "Note: --pandapower-file is ignored for bat_4b_24 "
            "(network is rebuilt per-block with battery dispatch)."
        )

    n_blocks = 24

    fout, finp = read_gtopt_battery_dispatch(output_dir)
    if len(fout) != n_blocks or len(finp) != n_blocks:
        raise ValueError(
            f"Expected {n_blocks} blocks in battery output; "
            f"got fout={len(fout)}, finp={len(finp)}"
        )

    gen_file = output_dir / "Generator" / "generation_sol.csv"
    if not gen_file.exists():
        raise FileNotFoundError(f"Not found: {gen_file}")

    gtopt_gen_all: list = []
    with open(gen_file, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        header = next(reader)
        uid_start = next(i for i, h in enumerate(header) if h.startswith("uid:"))
        for row in reader:
            gtopt_gen_all.append([float(row[i]) for i in range(uid_start, len(row))])

    passed = True

    print(f"bat_4b_24: comparing {n_blocks} blocks (g1, g2, g_solar vs pandapower)")
    for b in range(n_blocks):
        net = build_net_bat_4b_24(b, fout[b], finp[b])
        pp.rundcopp(net, verbose=False)

        pp_gen = list(net.res_gen["p_mw"].values)
        # pp_gen[0]=g1, pp_gen[1]=g2, pp_gen[2]=g_solar, (bat_net may add a 4th)
        gtopt_g1 = gtopt_gen_all[b][0]  # uid:1
        gtopt_g2 = gtopt_gen_all[b][1]  # uid:2
        gtopt_g_solar = gtopt_gen_all[b][2]  # uid:3

        for gi, (name, gt_val) in enumerate(
            [("g1", gtopt_g1), ("g2", gtopt_g2), ("g_solar", gtopt_g_solar)]
        ):
            if gi >= len(pp_gen):
                continue
            pp_val = pp_gen[gi]
            diff = abs(pp_val - gt_val)
            status = "PASS" if diff <= tol_mw else "FAIL"
            if diff > tol_mw:
                passed = False
            print(
                f"  block {b + 1:2d} {name}:"
                f" pandapower={pp_val:.3f}  gtopt={gt_val:.3f}"
                f"  diff={diff:.3f}  [{status}]"
            )

    return passed


# ---------------------------------------------------------------------------
# Dispatch table
# ---------------------------------------------------------------------------

# Maps case name → (build_net_fn, compare_fn).
# build_net_fn must accept no arguments and return a pandapower network.
# It is None for bat_4b_24 because that case builds the network per-block.
_CASES = {
    "s1b": _compare_s1b,
    "ieee_4b_ori": _compare_ieee_4b_ori,
    "ieee30b": _compare_ieee30b,
    "ieee_57b": _compare_ieee_57b,
    "bat_4b_24": _compare_bat_4b_24,
}

# Maps case name → network builder (for --save-pandapower-file).
# bat_4b_24 is excluded because its network is block-dependent.
_NET_BUILDERS = {
    "s1b": build_net_s1b,
    "ieee_4b_ori": build_net_ieee_4b_ori,
    "ieee30b": build_net_ieee30b,
    "ieee_57b": build_net_ieee_57b,
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
        type=Path,
        metavar="DIR",
        help=(
            "Directory containing gtopt CSV output files.  "
            "Required unless --save-pandapower-file is the only action."
        ),
    )
    parser.add_argument(
        "--pandapower-file",
        type=Path,
        metavar="FILE",
        default=None,
        help=(
            "Load the pandapower network from this JSON file instead of "
            "rebuilding it with the built-in network builder.  "
            "The file must have been saved with pandapower.to_json() (or "
            "--save-pandapower-file).  "
            "Ignored for bat_4b_24 (network is rebuilt per-block)."
        ),
    )
    parser.add_argument(
        "--save-pandapower-file",
        type=Path,
        metavar="FILE",
        default=None,
        help=(
            "Save the built pandapower network to this JSON file and exit "
            "(no gtopt comparison is performed).  "
            "Not supported for bat_4b_24."
        ),
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

    # --save-pandapower-file: build the network and write to JSON, then exit.
    if args.save_pandapower_file is not None:
        if args.case not in _NET_BUILDERS:
            print(
                f"ERROR: --save-pandapower-file is not supported for case '{args.case}'",
                file=sys.stderr,
            )
            sys.exit(2)
        try:
            net = _NET_BUILDERS[args.case]()
            save_pandapower_net(net, args.save_pandapower_file)
            print(f"Saved pandapower network to: {args.save_pandapower_file}")
        except Exception as exc:  # pylint: disable=broad-except
            print(f"ERROR saving pandapower network: {exc}", file=sys.stderr)
            sys.exit(2)
        # If --gtopt-output was not given, exit after saving.
        if args.gtopt_output is None:
            sys.exit(0)

    if args.gtopt_output is None:
        parser.error("--gtopt-output is required when not using --save-pandapower-file alone")

    try:
        compare_fn = _CASES[args.case]
        ok = compare_fn(
            args.gtopt_output,
            tol_mw=args.tol,
            tol_lmp=args.tol_lmp,
            pandapower_file=args.pandapower_file,
        )
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
