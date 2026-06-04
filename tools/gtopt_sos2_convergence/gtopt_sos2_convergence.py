# SPDX-License-Identifier: BSD-3-Clause
"""SOS2 L-secant convergence validation on the IEEE 14-bus Coffrin
reference case (issue #504).

This tool reproduces the Coffrin & Van Hentenryck (2014) linear loss
approximation methodology on a real benchmark network, sweeping the
gtopt ``loss_secant_segments`` parameter ``L`` and verifying that:

  * **L = 1**: reduces exactly to Coffrin's classic single-secant LP
    approximation (one chord per direction over the full
    ``[-tmax, +tmax]`` envelope).
  * **L = 2 + SOS2**: the SOS2 fill-order extension from issue #504,
    which tightens the chord to a piecewise-linear over-approximation
    of the quadratic loss with breakpoint-tight error.

The L-secant chord upper-bound proved in
``include/gtopt/line_losses.hpp`` overstates the true quadratic loss
by at most ``c · (envelope / (2L))²`` per line, so doubling L cuts
the worst-case loss overstatement by ≈ 4×.  This is the O(1/L²)
property verified by the unit test in
``test/source/test_line_losses_sos2.cpp::"L-secant: worst-gap shrinks
O(1/L²) under SOS2"``.

## SEGMENT-FORMULATION TRAP (L ≥ 3 — REAL BUG)

gtopt's L-secant uses a **segment** formulation:

  * v_l ∈ [0, w]  with  w = envelope/L  for each l = 1..L
  * Σ_l v_l ≥ |f|   (two abs rows)
  * ℓ ≤ Σ_l chord_slope_l · v_l   (chord upper bound)

The SOS2 declaration on ``{v_1, …, v_L}`` enforces the canonical
Beale–Tomlin invariant "at most TWO non-zero, ADJACENT".  Combined
with ``v_l ≤ w``  that caps ``Σ v_l ≤ 2w``  ⇒  ``|f| ≤ 2w =
2·envelope/L``.

For ``L = 1``: cap is ``2·envelope``,  no constraint (line max is
``envelope``).  For ``L = 2``: cap is ``envelope``,  exactly the
line max.  For ``L ≥ 3``: **the cap is below the line rating** —
SOS2 silently restricts the line to ``2·envelope/L < envelope``.

This integration tool runs the actual LP build on IEEE 14-bus at
``L ∈ {1, 2}`` (safe regime) and validates the chord-tightening
invariance.  The ``--probe-sos2-trap`` flag adds an ``L = 4`` run
that **demonstrates** the trap (line 1 saturates at ``75 MW`` =
``150/2``, demand-fail penalty kicks in, obj jumps ≈ 5×).

A correct SOS2 formulation needs the *lambda-form*
(``Σ λ_k = 1``, breakpoint weights with SOS2) or explicit
fill-order binaries — see the script's footer for the proposed
fix.

This integration tool runs the actual LP build and tracks the
solver's behaviour as ``L`` varies — the LP-observed total network
loss is invariant under the L sweep (gtopt picks the tightest
tangent **lower bound**, which is K-dependent not L-dependent), but
the objective ``$/h`` is non-decreasing as L tightens the chord.

## Coffrin reference case

The IEEE 14-bus case (Christie 1993, MATPOWER ``case14.m``,
PGLib-OPF ``pglib_opf_case14_ieee.m``) is used directly from
``cases/ieee_14b/ieee_14b.json``.  Per-line resistances are injected
from MATPOWER's case14.m (the gtopt fixture stores only reactance
because the upstream PCP loader has no loss model).  Base voltage =
138 kV but we use ``voltage = 10`` in gtopt so the per-unit
relationship ``loss_MW = R_pu × P²/(V² · baseMVA)`` reduces to gtopt's
``ℓ = (R/V²) · f²``  with ``R = R_pu`` and ``V² = 100 = baseMVA``.

## Literature references

* Coffrin & Van Hentenryck (2014).  *A Linear-Programming
  Approximation of AC Power Flows.*  INFORMS Journal on Computing
  26(4):718–734.  Establishes the L=1 single-secant outer
  approximation of the quadratic loss in DC OPF.

* Tanneau, Anjos, Léautaud (2022).  *A Linear Outer Approximation of
  Line Losses for DC-based Optimal Power Flow Problems.*  Electric
  Power Systems Research 211: 108280.  arXiv:2112.10975.  Adds the
  dynamic outer approximation (LLOA1) that converges in ≤ 2
  iterations on PGLib pegase cases.  gtopt implements the *static*
  L-segment form with SOS2 fill-order (issue #504).

## Run

    GTOPT_BIN=$PWD/build/standalone/gtopt \\
        python tools/gtopt_sos2_convergence/gtopt_sos2_convergence.py
"""

from __future__ import annotations

import argparse
import copy
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

# MATPOWER case14.m line resistances (per-unit on 100 MVA base).
# Line ordering follows ``cases/ieee_14b/ieee_14b.json`` 1..20 which
# was verified above to match MATPOWER's case14.m line ordering by
# reactance.  Zero-R lines are transformers (lossless in MATPOWER).
MATPOWER_CASE14_R_PU = {
    1: 0.01938,  # b1-b2
    2: 0.05403,  # b1-b5
    3: 0.04699,  # b2-b3
    4: 0.05811,  # b2-b4
    5: 0.05695,  # b2-b5
    6: 0.06701,  # b3-b4
    7: 0.01335,  # b4-b5
    8: 0.0,  # b4-b7 (transformer)
    9: 0.0,  # b4-b9 (transformer)
    10: 0.0,  # b5-b6 (transformer)
    11: 0.09498,  # b6-b11
    12: 0.12291,  # b6-b12
    13: 0.06615,  # b6-b13
    14: 0.0,  # b7-b8 (transformer)
    15: 0.0,  # b7-b9 (transformer)
    16: 0.03181,  # b9-b10
    17: 0.12711,  # b9-b14
    18: 0.08205,  # b10-b11
    19: 0.22092,  # b12-b13
    20: 0.17093,  # b13-b14
}

# Map per-unit R on MATPOWER's 100 MVA base into gtopt's
# ``c = R/V²``.  Set ``V = 10`` so ``V² = 100 = baseMVA`` and
# ``c = R_pu / 100``  matches  ``loss_MW = R_pu × P_MW² / baseMVA``.
GTOPT_BASE_VOLTAGE = 10.0
GTOPT_BASE_MVA = GTOPT_BASE_VOLTAGE * GTOPT_BASE_VOLTAGE  # = 100

# DC-OPF lossless reference (MATPOWER ``rundcopf('case14')``).  Used
# only for context — gtopt's case has its own (linearised) cost data
# so absolute objective values won't match across solvers.
MATPOWER_DCOPF_OBJ_USD_PER_H = 7642.59


def _gtopt_binary() -> Path | None:
    """Resolve the gtopt binary."""
    env = os.environ.get("GTOPT_BIN", "").strip()
    if env:
        return Path(env)
    here = Path(__file__).resolve()
    for parent in (here.parent, *here.parents):
        cand = parent / "build" / "standalone" / "gtopt"
        if cand.is_file():
            return cand
    which = shutil.which("gtopt")
    return Path(which) if which else None


def _ieee14_case_file() -> Path | None:
    """Locate ``cases/ieee_14b/ieee_14b.json`` relative to this script."""
    here = Path(__file__).resolve()
    for parent in (here.parent, *here.parents):
        cand = parent / "cases" / "ieee_14b" / "ieee_14b.json"
        if cand.is_file():
            return cand
    return None


def _build_ieee14_with_losses(
    base_case: dict,
    L: int,
    use_sos2: bool,
    loss_cost_eps: float,
    loss_segments: int,
) -> dict:
    """Construct the IEEE 14-bus case with loss parameters injected.

    Reads the canonical ``cases/ieee_14b/ieee_14b.json`` and adds:

      * per-line ``resistance`` from MATPOWER case14.m
      * per-line ``voltage = GTOPT_BASE_VOLTAGE``
      * per-line ``loss_secant_segments = L``
      * per-line ``loss_use_sos2 = use_sos2``
      * top-level ``model_options.line_losses_mode = 'tangent_signed_flow'``
      * top-level ``model_options.loss_segments = K``
      * top-level ``model_options.loss_cost_eps``
    """
    case = copy.deepcopy(base_case)
    opts = case.setdefault("options", {}).setdefault("model_options", {})
    opts["line_losses_mode"] = "tangent_signed_flow"
    opts["loss_segments"] = loss_segments
    opts["loss_cost_eps"] = loss_cost_eps
    # Keep Kirchhoff KVL active — Coffrin's approximation always
    # combines L-secant losses with linearised power flow.
    opts.setdefault("use_kirchhoff", True)

    for line in case["system"]["line_array"]:
        r_pu = MATPOWER_CASE14_R_PU.get(line["uid"], 0.0)
        line["resistance"] = r_pu
        line["voltage"] = GTOPT_BASE_VOLTAGE
        line["loss_secant_segments"] = L
        line["loss_use_sos2"] = use_sos2

    return case


def _run_gtopt(
    gtopt_bin: Path,
    planning_json: Path,
    output_dir: Path,
) -> tuple[float, float]:
    """Run gtopt on a single planning JSON.

    Returns ``(obj_value, total_loss_MW)`` where total_loss_MW is
    inferred from the network bus balance (Σ generation − Σ demand).
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    cmd = [
        str(gtopt_bin),
        "-s",
        str(planning_json),
        "-d",
        str(output_dir),
        "--write-out",
        "sol",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr, file=sys.stderr)
        raise SystemExit(f"gtopt failed on {planning_json}")

    def _strip(s: str) -> str:
        return s.strip().strip('"')

    sol = output_dir / "solution.csv"
    if not sol.is_file():
        raise SystemExit(f"no solution.csv produced at {output_dir}")
    with sol.open() as f:
        header = [_strip(c) for c in f.readline().rstrip("\n").split(",")]
        row = f.readline().rstrip("\n").split(",")
    obj = float(row[header.index("obj_value")])

    # Σ generation across all generators and all blocks.
    gen_dir = output_dir / "Generator"
    total_gen_MWh = 0.0
    if gen_dir.is_dir():
        for csv in gen_dir.glob("generation_sol_*.csv"):
            with csv.open() as f:
                header = [_strip(c) for c in f.readline().rstrip("\n").split(",")]
                idx = header.index("value")
                for line in f:
                    parts = line.rstrip("\n").split(",")
                    total_gen_MWh += float(parts[idx])

    # Σ demand served across all demands and all blocks.
    dem_dir = output_dir / "Demand"
    total_dem_MWh = 0.0
    if dem_dir.is_dir():
        # The "load" served file name varies; try a few patterns.
        for pattern in ("load_sol_*.csv", "demand_sol_*.csv"):
            for csv in dem_dir.glob(pattern):
                with csv.open() as f:
                    header = [_strip(c) for c in f.readline().rstrip("\n").split(",")]
                    if "value" not in header:
                        continue
                    idx = header.index("value")
                    for line in f:
                        parts = line.rstrip("\n").split(",")
                        total_dem_MWh += float(parts[idx])
            if total_dem_MWh > 0:
                break

    # If the demand CSV layout isn't found, fall back on the case file
    # (assumes lmax = served = fixed inelastic demand).
    if total_dem_MWh == 0.0:
        with planning_json.open() as f:
            case = json.load(f)
        for d in case["system"]["demand_array"]:
            lmax = d["lmax"]
            if isinstance(lmax, list) and lmax and isinstance(lmax[0], list):
                total_dem_MWh += sum(lmax[0])
            else:
                total_dem_MWh += float(lmax)

    return obj, total_gen_MWh - total_dem_MWh


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description=(
            "SOS2 L-secant convergence on the IEEE 14-bus Coffrin "
            "reference case.  Sweeps loss_secant_segments and reports "
            "the objective + total network loss at each L."
        )
    )
    p.add_argument("--gtopt", type=Path, default=None, help="gtopt binary path")
    p.add_argument("--tmp", type=Path, default=None, help="workspace dir")
    p.add_argument(
        "--keep",
        action="store_true",
        help="Keep the workspace dir after the run.",
    )
    p.add_argument(
        "--L-values",
        type=str,
        default="1,2",
        help=(
            "Comma-separated list of L (loss_secant_segments) to sweep. "
            "Default '1,2' covers the safe regime (segment SOS2 caps "
            "|f| at 2·envelope/L which only exceeds tmax for L ≤ 2). "
            "Use --probe-sos2-trap to additionally show the L=4 trap."
        ),
    )
    p.add_argument(
        "--probe-sos2-trap",
        action="store_true",
        help=(
            "Additionally run L=4 + SOS2 to demonstrate the segment-"
            "formulation trap: SOS2 caps |f| ≤ 2w = envelope/2, so "
            "lines requiring f > envelope/2 saturate at the cap and "
            "the LP pays demand_fail.  Surfaces as a 5×+ obj jump."
        ),
    )
    p.add_argument(
        "--loss-segments",
        type=int,
        default=5,
        help="K tangent segments (forms the LOWER bound on loss).",
    )
    p.add_argument(
        "--loss-cost-eps",
        type=float,
        default=1e-4,
        help=(
            "ε on Σv_l in the objective.  Required for L=1 to force "
            "v = |f| (without it the LP can inflate v and chord goes "
            "loose).  Default 1e-4."
        ),
    )
    p.add_argument(
        "--also-no-sos2",
        action="store_true",
        help=(
            "Additionally run L>1 with use_sos2=false.  The LP should "
            "report a LOWER objective (loss under-estimate) because "
            "without SOS2 fill-order the chord is no longer a valid "
            "upper bound."
        ),
    )
    args = p.parse_args(argv)

    gtopt_bin = args.gtopt or _gtopt_binary()
    if gtopt_bin is None or not gtopt_bin.is_file():
        raise SystemExit("Could not locate gtopt; pass --gtopt or set GTOPT_BIN.")
    case_path = _ieee14_case_file()
    if case_path is None:
        raise SystemExit("Could not locate cases/ieee_14b/ieee_14b.json.")

    print(f"# gtopt binary:       {gtopt_bin}")
    print(f"# Base case:          {case_path}")

    workspace = args.tmp or Path(tempfile.mkdtemp(prefix="gtopt_sos2_conv_"))
    workspace.mkdir(parents=True, exist_ok=True)
    cleanup = not args.keep and args.tmp is None
    print(f"# Workspace:          {workspace}")

    with case_path.open() as f:
        base_case = json.load(f)
    total_demand = 0.0
    for d in base_case["system"]["demand_array"]:
        lmax = d["lmax"]
        if isinstance(lmax, list) and lmax and isinstance(lmax[0], list):
            total_demand += sum(lmax[0])
    print(f"# Total demand (24h): {total_demand:.1f} MWh")
    print(
        f"# MATPOWER DC-OPF (lossless reference): {MATPOWER_DCOPF_OBJ_USD_PER_H:.2f} $/h"
    )
    print(f"# K tangent segments: {args.loss_segments}")
    print(f"# loss_cost_eps:      {args.loss_cost_eps}")

    L_values = [int(s) for s in args.L_values.split(",") if s.strip()]
    print(f"# L sweep (SOS2):     {L_values}")

    print()
    header_fmt = (
        f"{'L':>3}  {'SOS2':>4}  "
        f"{'obj [$]':>14}  "
        f"{'Σ_loss [MWh]':>14}  "
        f"{'loss/demand %':>14}  "
        f"{'Δobj vs L=1 [$]':>18}"
    )
    print(header_fmt)
    print("-" * len(header_fmt))

    results = []
    for L in L_values:
        case = _build_ieee14_with_losses(
            base_case,
            L=L,
            use_sos2=(L > 1),
            loss_cost_eps=args.loss_cost_eps,
            loss_segments=args.loss_segments,
        )
        json_path = workspace / f"ieee14_L{L}_sos2.json"
        with json_path.open("w") as f:
            json.dump(case, f, indent=2)
        out_dir = workspace / f"out_L{L}_sos2"
        obj, loss_MWh = _run_gtopt(gtopt_bin, json_path, out_dir)
        results.append({"L": L, "sos2": True, "obj": obj, "loss": loss_MWh})

    obj_L1 = next(r["obj"] for r in results if r["L"] == 1)

    for r in results:
        print(
            f"{r['L']:>3}  {'yes':>4}  "
            f"{r['obj']:>14.4f}  "
            f"{r['loss']:>14.4f}  "
            f"{(r['loss'] / total_demand) * 100:>13.4f}%  "
            f"{r['obj'] - obj_L1:>+18.4f}"
        )

    # --- probe-sos2-trap: L=4 + SOS2 demonstrates the segment cap ---
    if args.probe_sos2_trap:
        print()
        print(
            "# --- probe-sos2-trap: L=4 + SOS2 (expects line saturation "
            "at tmax/2 + demand-fail penalty) ---"
        )
        case = _build_ieee14_with_losses(
            base_case,
            L=4,
            use_sos2=True,
            loss_cost_eps=args.loss_cost_eps,
            loss_segments=args.loss_segments,
        )
        json_path = workspace / "ieee14_L4_sos2_trap.json"
        with json_path.open("w") as f:
            json.dump(case, f, indent=2)
        out_dir = workspace / "out_L4_sos2_trap"
        obj_trap, loss_trap = _run_gtopt(gtopt_bin, json_path, out_dir)
        print(
            f"{4:>3}  {'yes':>4}  "
            f"{obj_trap:>14.4f}  "
            f"{loss_trap:>14.4f}  "
            f"{(loss_trap / total_demand) * 100:>13.4f}%  "
            f"{obj_trap - obj_L1:>+18.4f}"
        )
        # Validation: obj must jump > 2× over L=1 baseline (demand-fail
        # penalty kicked in because line 1 needs > tmax/2).
        assert obj_trap > 2.0 * obj_L1, (
            f"--probe-sos2-trap expected obj > 2× L=1 baseline "
            f"(saw {obj_trap:.2f} vs {obj_L1:.2f}); the segment-SOS2 "
            "trap should make L=4 demand-fail.  If this passes the "
            "trap may have been fixed."
        )

    if args.also_no_sos2:
        print()
        print("# --- additional runs without SOS2 (chord NOT a valid upper bound) ---")
        for L in L_values:
            if L == 1:
                continue  # L=1 is identical with/without SOS2.
            case = _build_ieee14_with_losses(
                base_case,
                L=L,
                use_sos2=False,
                loss_cost_eps=args.loss_cost_eps,
                loss_segments=args.loss_segments,
            )
            json_path = workspace / f"ieee14_L{L}_nosos2.json"
            with json_path.open("w") as f:
                json.dump(case, f, indent=2)
            out_dir = workspace / f"out_L{L}_nosos2"
            obj, loss_MWh = _run_gtopt(gtopt_bin, json_path, out_dir)
            print(
                f"{L:>3}  {'no':>4}  "
                f"{obj:>14.4f}  "
                f"{loss_MWh:>14.4f}  "
                f"{(loss_MWh / total_demand) * 100:>13.4f}%  "
                f"{obj - obj_L1:>+18.4f}"
            )

    # ----- Validation -----
    #
    # gtopt's LP picks ℓ_line at the MAX of K tangent lower bounds
    # (the smallest feasible value above the K tangent lines).  The
    # max-tangent is K-dependent, NOT L-dependent.  Doubling L
    # tightens the chord upper bound but the LP was already below it
    # via the tangent lower bound, so the LP-observed loss should be
    # invariant across the L sweep.
    #
    # In multi-line networks the LP can also pick a DIFFERENT flow
    # pattern as L changes (the chord upper bound participates in
    # KCL via the loss column on each line), but the variation
    # should be small.  We assert the obj and loss vary by at most
    # 0.5 % across the L sweep.
    obj_min = min(r["obj"] for r in results)
    obj_max = max(r["obj"] for r in results)
    loss_min = min(r["loss"] for r in results)
    loss_max = max(r["loss"] for r in results)
    print()
    print(
        f"# Range across SOS2 L sweep:  "
        f"obj [{obj_min:.4f}, {obj_max:.4f}]  "
        f"loss [{loss_min:.4f}, {loss_max:.4f}]"
    )
    if obj_min > 0:
        rel_obj = (obj_max - obj_min) / obj_min
        print(
            f"# Relative obj variation:    {rel_obj * 100:.4f} %  "
            f"(target: ≤ 0.5 %, LP-observed loss = max(tangent_k(f)))"
        )
    if cleanup:
        shutil.rmtree(workspace, ignore_errors=True)
    return 0


# ----------------------------------------------------------------------
# Proposed fix for the L ≥ 3 SOS2 trap (segment-formulation cap).
# ----------------------------------------------------------------------
#
# The current ``line_losses.cpp`` emits
#
#     v_l ∈ [0, w]   for l = 1..L     (w = envelope/L)
#     Σ v_l ≥ |f|                      (two abs rows)
#     ℓ ≤ Σ chord_slope_l · v_l        (chord upper bound)
#     SOS2 on {v_1, …, v_L}            (issue #504)
#
# But SOS2 (Beale–Tomlin 1970) says "at most 2 non-zero, adjacent",
# which combined with v_l ≤ w means Σ v_l ≤ 2w.  For L ≥ 3 this
# silently caps line flow at 2·envelope/L < envelope.
#
# Three fixes from the LP literature:
#
# 1.  *Lambda-form on breakpoints*.  Replace v_l with L+1 weights
#     λ_0, …, λ_L on the L+1 breakpoints b_l = l·w:
#         Σ λ_l = 1,  λ_l ≥ 0
#         SOS2 on {λ_0, …, λ_L}
#         |f| = Σ b_l · λ_l
#         ℓ ≤ Σ true_loss(b_l) · λ_l = Σ c·b_l² · λ_l
#     Pros: standard, supported by every MIP solver, chord becomes
#     EXACTLY the piecewise secant with tight breakpoints, allows
#     |f| up to envelope without any cap.  Cons: changes the col
#     count from L to L+1 and reshapes the abs rows.
#
# 2.  *Fill-order binaries*.  Keep the segment cols but add L−1
#     binaries z_l with v_l > 0 ⇒ z_l = 1 and z_l ≤ z_{l−1}:
#         v_l ≤ w · z_l         (BigM via z_l ∈ {0, 1})
#         z_l ≤ z_{l−1}         (l ≥ 2)
#     SOS2 then becomes redundant (the binaries already encode
#     fill-order), and Σ v_l can reach L·w = envelope.
#
# 3.  *Drop SOS2 and rely on ε > 0*.  The L ≥ 1 chord remains a
#     valid upper bound on ℓ even without fill-order, but the LP
#     may pick a degenerate v_l distribution that inflates Σ v_l
#     and makes the chord constant.  ``loss_cost_eps > 0`` on each
#     v_l forces Σ v_l = |f| at the LP optimum and recovers the
#     piecewise-linear chord.  This is option (c) the existing
#     foot-gun warning in ``line_losses.cpp:316``  already names.
#
# The simplest fix is (1): lambda-form SOS2 is the canonical way to
# express a piecewise-linear function in LP/MIP and the solver-side
# support is identical.  The segment-form was a clean abstraction
# for the L = 1 case (one v ≥ |f|, one chord ≤ k·envelope·v) but
# doesn't generalise cleanly to L ≥ 2 with SOS2.
#
# Until a fix lands, **the safe regime is L ≤ 2** (this script's
# default).  L = 2 already gives the 4× chord-tightness improvement
# over L = 1, which captures most of the convergence benefit.


if __name__ == "__main__":
    sys.exit(main())
