#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""validate_network_reduction.py — pandapower DC-OPF validation of the
gtopt_reduce_network reduction.

Compares the original (full-network) gtopt case against its reduced
versions over a K-sweep and reports four physical-fidelity indices plus
two LP-side indicators.  Optionally compares the baseline reduction
against a transport-only + loss-uplift reduction side-by-side.

This is a **developer/agent tool**, not an end-user utility — it lives
under ``tools/`` for the same reason as ``get_gtopt_binary.py``.  It is
not pip-installed.

Usage
-----

    # Single-mode sweep (default reduction settings)
    python tools/validate_network_reduction.py \\
        cases/ieee_57b/ieee_57b.json --k 50 40 30 20 15 10 5

    # Side-by-side baseline vs transport+uplift
    python tools/validate_network_reduction.py \\
        cases/ieee_57b/ieee_57b.json --compare-transport-uplift \\
        --uplift-pct 3.0 --k 50 40 30 20 15 10 5

    # Single-mode with transport-only + uplift
    python tools/validate_network_reduction.py \\
        cases/ieee_57b/ieee_57b.json --transport-only \\
        --loss-mode uplift --uplift-pct 3.0 --k 50 40 30 20 15 10 5

Indices reported
----------------

* ``cost Δ%``  — pandapower res_cost relative error vs the original case.
  (Note: pandapower's DC-OPF has a zero-cost ext_grid slack, so this is
  insensitive to network constraints on academic cases; treat as a
  sanity signal only.)
* ``load Δ%`` — total served load relative change.  For ``--loss-mode
  uplift`` this should match the uplift percentage exactly.
* ``gen MAE`` — mean absolute per-generator dispatch error in MW.
  Subject to alternate-optima noise; usually not monotonic.
* ``hidden %``  — fraction of Σ|original line flow| that lived on lines
  the reducer marked intra-cluster (and therefore dropped).  Most
  reliable physical-fidelity signal; rises monotonically as K decreases.
* ``hidden max MW`` — largest single intra-cluster (hidden) line flow.
* ``inter MAE / MAPE`` — error between sum-of-original-flows on each
  surviving corridor and the equivalent line's flow.  MAPE > 100% means
  the equivalent flow can have the wrong sign on some corridors → the
  reduction has structurally diverged.
"""

from __future__ import annotations

import argparse
import sys
import warnings
from dataclasses import dataclass
from pathlib import Path

import numpy as np

warnings.filterwarnings("ignore")

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

try:
    import pandapower as pp
except ImportError as exc:  # pragma: no cover
    sys.exit(
        f"pandapower required; install with `pip install pandapower>=2.13`: {exc}"
    )

from gtopt2pp.convert import convert, load_gtopt_case  # noqa: E402
from gtopt_reduce_network import load_case, save_case  # noqa: E402
from gtopt_reduce_network._reduce import ReduceConfig, reduce_case  # noqa: E402


# ─── pandapower wrappers ────────────────────────────────────────────────────


def solve_dc_opf(case_dict: dict) -> pp.pandapowerNet | None:
    net = convert(case_dict, scenario=None, block=None)
    net.line["max_loading_percent"] = 100.0
    if not net.load.empty:
        net.load["controllable"] = False
    try:
        pp.rundcopp(net)
    except Exception:
        return None
    return net if net.OPF_converged else None


def line_flow_by_uid(net: pp.pandapowerNet, case_dict: dict) -> dict[int, float]:
    return {
        int(ln["uid"]): float(net.res_line.iloc[i]["p_from_mw"])
        for i, ln in enumerate(case_dict["system"]["line_array"])
        if i < len(net.res_line)
    }


def gen_dispatch_by_uid(net: pp.pandapowerNet, case_dict: dict) -> dict[int, float]:
    return {
        int(g["uid"]): float(net.res_gen.iloc[i]["p_mw"])
        for i, g in enumerate(case_dict["system"]["generator_array"])
        if i < len(net.res_gen)
    }


def res_cost(net: pp.pandapowerNet) -> float:
    try:
        return float(net.res_cost)
    except (AttributeError, TypeError):
        return float("nan")


# ─── divergence indices ─────────────────────────────────────────────────────


@dataclass(slots=True)
class Indices:
    n_buses: int
    n_lines: int
    cost_dpct: float
    load_dpct: float
    gen_mae_mw: float
    hidden_share_pct: float
    hidden_max_mw: float
    inter_mae_mw: float
    inter_mape_pct: float


def compute_indices(
    red,
    red_net,
    red_dict,
    orig_disp: dict[int, float],
    orig_flow: dict[int, float],
    orig_cost: float,
    orig_load: float,
    total_flow_mag: float,
) -> Indices:
    rd = gen_dispatch_by_uid(red_net, red_dict)
    rf = line_flow_by_uid(red_net, red_dict)
    rl = float(red_net.res_load["p_mw"].sum())
    rc = res_cost(red_net)
    # Cost / load
    cost_dpct = (
        100 * abs(rc - orig_cost) / abs(orig_cost) if orig_cost else float("nan")
    )
    load_dpct = 100 * (rl - orig_load) / orig_load if orig_load else float("nan")
    # Gen MAE
    keys = sorted(set(orig_disp) | set(rd))
    diffs = np.array([rd.get(u, 0.0) - orig_disp.get(u, 0.0) for u in keys])
    gen_mae = float(np.mean(np.abs(diffs))) if len(diffs) else 0.0
    # Hidden flow
    intra = [r for r in red.linemap if r.rule == "intra-cluster"]
    hidden = [abs(orig_flow.get(r.original_line_uid, 0.0)) for r in intra]
    hidden_share = (sum(hidden) / total_flow_mag) * 100 if total_flow_mag > 0 else 0.0
    hidden_max = max(hidden) if hidden else 0.0
    # Inter-corridor MAE / MAPE
    corridor: dict[int, list[int]] = {}
    for r in red.linemap:
        if r.rule == "inter-cluster" and r.equivalent_line_uid is not None:
            corridor.setdefault(r.equivalent_line_uid, []).append(r.original_line_uid)
    inter_diffs: list[float] = []
    inter_mags: list[float] = []
    for eq_uid, orig_uids in corridor.items():
        agg_orig = sum(orig_flow.get(u, 0.0) for u in orig_uids)
        f_red = rf.get(eq_uid, 0.0)
        inter_diffs.append(f_red - agg_orig)
        inter_mags.append(abs(agg_orig))
    inter_mae = float(np.mean(np.abs(inter_diffs))) if inter_diffs else 0.0
    denom = float(np.sum(inter_mags))
    inter_mape = (
        100 * float(np.sum(np.abs(inter_diffs))) / denom if denom > 0 else 0.0
    )
    return Indices(
        n_buses=len(red.case.array("bus_array")),
        n_lines=len(red.case.array("line_array")),
        cost_dpct=cost_dpct,
        load_dpct=load_dpct,
        gen_mae_mw=gen_mae,
        hidden_share_pct=hidden_share,
        hidden_max_mw=hidden_max,
        inter_mae_mw=inter_mae,
        inter_mape_pct=inter_mape,
    )


# ─── reducer config helpers ─────────────────────────────────────────────────


def make_config(args: argparse.Namespace, K: int) -> ReduceConfig:
    return ReduceConfig(
        target_buses=K,
        distance=args.distance,
        transport_only=args.transport_only,
        loss_mode=args.loss_mode,
        loss_uplift_pct=args.uplift_pct,
    )


def run_sweep(
    case_obj,
    case_path: Path,
    workdir: Path,
    args: argparse.Namespace,
    *,
    extra_label: str = "",
) -> None:
    orig_dict = load_gtopt_case(case_path)
    orig_net = solve_dc_opf(orig_dict)
    if orig_net is None:
        print("ORIGINAL OPF FAILED")
        return
    orig_disp = gen_dispatch_by_uid(orig_net, orig_dict)
    orig_flow = line_flow_by_uid(orig_net, orig_dict)
    orig_cost = res_cost(orig_net)
    orig_load = float(orig_net.res_load["p_mw"].sum())
    total_flow_mag = sum(abs(v) for v in orig_flow.values())
    n_b = len(orig_dict["system"]["bus_array"])
    n_l = len(orig_dict["system"]["line_array"])
    print(
        f"\nCASE: {case_path.name}{(' — ' + extra_label) if extra_label else ''}"
    )
    print("=" * 78)
    print(
        f"  original: {n_b} buses, {n_l} lines  "
        f"cost={orig_cost:.2f}  load={orig_load:.1f} MW  "
        f"Σ|flow|={total_flow_mag:.1f} MW"
    )
    print()
    print(
        f"  {'K':>4} {'buses':>5} {'lines':>5}  "
        f"{'cost Δ%':>8} {'load Δ%':>8} {'gen MAE':>8} "
        f"{'hidden':>7} {'hidden':>7} {'inter':>7} {'inter':>7}"
    )
    print(
        f"  {'':4} {'':5} {'':5}  {'':8} {'':8} {'(MW)':>8} "
        f"{'% flow':>7} {'max MW':>7} {'MAE MW':>7} {'MAPE %':>7}"
    )
    print(
        f"  {'-' * 4} {'-' * 5} {'-' * 5}  {'-' * 8} {'-' * 8} {'-' * 8} "
        f"{'-' * 7} {'-' * 7} {'-' * 7} {'-' * 7}"
    )
    for K in args.k:
        cfg = make_config(args, K)
        red = reduce_case(case_obj, cfg)
        red_path = workdir / f"reduced_K{K}.json"
        save_case(red.case, red_path)
        red_dict = load_gtopt_case(red_path)
        red_net = solve_dc_opf(red_dict)
        if red_net is None:
            print(f"  {K:>4d}  OPF FAILED at K={K}")
            continue
        idx = compute_indices(
            red,
            red_net,
            red_dict,
            orig_disp,
            orig_flow,
            orig_cost,
            orig_load,
            total_flow_mag,
        )
        print(
            f"  {K:>4d} {idx.n_buses:>5d} {idx.n_lines:>5d}  "
            f"{idx.cost_dpct:>7.2f}% {idx.load_dpct:>+7.2f}% "
            f"{idx.gen_mae_mw:>8.2f} "
            f"{idx.hidden_share_pct:>6.1f}% {idx.hidden_max_mw:>7.1f} "
            f"{idx.inter_mae_mw:>7.2f} {idx.inter_mape_pct:>6.1f}%"
        )


def run_compare(
    case_obj,
    case_path: Path,
    workdir: Path,
    args: argparse.Namespace,
) -> None:
    """Two parallel sweeps: baseline vs transport+uplift."""
    orig_dict = load_gtopt_case(case_path)
    orig_net = solve_dc_opf(orig_dict)
    if orig_net is None:
        print("ORIGINAL OPF FAILED")
        return
    orig_disp = gen_dispatch_by_uid(orig_net, orig_dict)
    orig_flow = line_flow_by_uid(orig_net, orig_dict)
    orig_cost = res_cost(orig_net)
    orig_load = float(orig_net.res_load["p_mw"].sum())
    total_flow_mag = sum(abs(v) for v in orig_flow.values())
    n_b = len(orig_dict["system"]["bus_array"])
    n_l = len(orig_dict["system"]["line_array"])
    print(f"\nCASE: {case_path.name}  (side-by-side comparison)")
    print("=" * 86)
    print(
        f"  original: {n_b} buses, {n_l} lines  "
        f"cost={orig_cost:.2f}  load={orig_load:.1f} MW"
    )
    print()
    print(
        f"  {'K':>4}   {'buses':>5} {'lines':>5}  | "
        f"{'baseline':>30s}  |  "
        f"{'transport + uplift ' + str(args.uplift_pct) + '%':>30s}"
    )
    print(
        f"  {'':4}   {'':5} {'':5}  | "
        f"{'cost Δ%':>7} {'load Δ%':>7} {'hidden %':>8} {'MAPE %':>7}  | "
        f"{'cost Δ%':>7} {'load Δ%':>7} {'hidden %':>8} {'MAPE %':>7}"
    )
    print(
        f"  {'-' * 4}   {'-' * 5} {'-' * 5}  | "
        f"{'-' * 7} {'-' * 7} {'-' * 8} {'-' * 7}  | "
        f"{'-' * 7} {'-' * 7} {'-' * 8} {'-' * 7}"
    )

    for K in args.k:
        baseline_cfg = ReduceConfig(target_buses=K, distance=args.distance)
        tu_cfg = ReduceConfig(
            target_buses=K,
            distance=args.distance,
            transport_only=True,
            loss_mode="uplift",
            loss_uplift_pct=args.uplift_pct,
        )
        results = []
        for label, cfg in (("baseline", baseline_cfg), ("tu", tu_cfg)):
            red = reduce_case(case_obj, cfg)
            red_path = workdir / f"{label}_K{K}.json"
            save_case(red.case, red_path)
            red_dict = load_gtopt_case(red_path)
            red_net = solve_dc_opf(red_dict)
            if red_net is None:
                results.append(None)
                continue
            results.append(
                compute_indices(
                    red,
                    red_net,
                    red_dict,
                    orig_disp,
                    orig_flow,
                    orig_cost,
                    orig_load,
                    total_flow_mag,
                )
            )
        b, t = results
        if b is None or t is None:
            print(f"  K={K}: OPF failed in one branch")
            continue
        print(
            f"  {K:>4d}   {b.n_buses:>5d} {b.n_lines:>5d}  | "
            f"{b.cost_dpct:>6.2f}% {b.load_dpct:>+6.2f}% "
            f"{b.hidden_share_pct:>7.1f}% {b.inter_mape_pct:>6.1f}%  | "
            f"{t.cost_dpct:>6.2f}% {t.load_dpct:>+6.2f}% "
            f"{t.hidden_share_pct:>7.1f}% {t.inter_mape_pct:>6.1f}%"
        )


# ─── CLI ────────────────────────────────────────────────────────────────────


def main() -> None:
    parser = argparse.ArgumentParser(
        prog="validate_network_reduction.py",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "case",
        type=Path,
        help="path to a gtopt case JSON (e.g. cases/ieee_57b/ieee_57b.json)",
    )
    parser.add_argument(
        "--k",
        nargs="+",
        type=int,
        default=[50, 40, 30, 20, 15, 10, 5],
        help="target bus counts to sweep (default: 50 40 30 20 15 10 5)",
    )
    parser.add_argument(
        "--distance",
        choices=["reactance-shortest-path", "zbus", "ptdf"],
        default="reactance-shortest-path",
    )
    parser.add_argument(
        "--transport-only",
        action="store_true",
        help="enable --transport-only on every reduction",
    )
    parser.add_argument(
        "--loss-mode",
        choices=["keep", "linear", "off", "uplift"],
        default="keep",
    )
    parser.add_argument(
        "--uplift-pct",
        type=float,
        default=3.0,
        help="uplift percent when --loss-mode=uplift (default: 3.0)",
    )
    parser.add_argument(
        "--compare-transport-uplift",
        action="store_true",
        help=(
            "side-by-side comparison: baseline reduction vs transport-only + "
            "loss-mode=uplift (overrides --transport-only/--loss-mode for "
            "individual columns)"
        ),
    )
    parser.add_argument(
        "--workdir",
        type=Path,
        default=Path("/tmp/validate_network_reduction"),
        help="scratch directory for reduced JSONs",
    )
    args = parser.parse_args()
    args.workdir.mkdir(parents=True, exist_ok=True)
    case_obj = load_case(args.case)
    if args.compare_transport_uplift:
        run_compare(case_obj, args.case, args.workdir, args)
    else:
        extra = ""
        if args.transport_only or args.loss_mode != "keep":
            extra = f"--transport-only={args.transport_only} loss-mode={args.loss_mode}"
            if args.loss_mode == "uplift":
                extra += f" uplift-pct={args.uplift_pct}"
        run_sweep(case_obj, args.case, args.workdir, args, extra_label=extra)


if __name__ == "__main__":
    main()
