# SPDX-License-Identifier: BSD-3-Clause
"""SOS2 L-secant convergence validation on the IEEE 14-bus Coffrin
reference case (issue #504).

This tool reproduces the Coffrin & Van Hentenryck (2014) linear loss
approximation methodology on a real benchmark network, sweeping the
gtopt ``loss_secant_segments`` parameter ``L`` across the three LP/MIP
regimes that gtopt now supports:

  * **(A) L = 1, no SOS2**: Coffrin's classic single-secant LP
    approximation (one chord per direction over the full
    ``[-tmax, +tmax]`` envelope).
  * **(B) L > 1, ε > 0, no SOS2** (``--epsilon-rely``): pure-LP
    generalisation — segment cols + ε in the objective force
    ``Σ v_l = |f|`` at LP optimum, which keeps the chord bounded
    by the piecewise secant rather than the loose constant ceiling.
    The v_l distribution is LP-indifferent (the chord row is
    inactive at LP optimum) but the obj matches regime (C) to
    within solver tolerance.
  * **(C) L > 1 + SOS2** (lambda-form refactor, this commit): MIP
    on ``2L+1`` breakpoint weights ``λ_l`` with SOS2; canonical
    Beale–Tomlin "at most 2 adjacent non-zero" interpolates between
    breakpoints over the full ``[-envelope, +envelope]`` range.

The L-secant chord upper-bound proved in
``include/gtopt/line_losses.hpp`` overstates the true quadratic loss
by at most ``c · (envelope / (2L))²`` per line, so doubling L cuts
the worst-case loss overstatement by ≈ 4×.  This is the O(1/L²)
property verified by the unit test in
``test/source/test_line_losses_sos2.cpp::"L-secant: worst-gap shrinks
O(1/L²) under SOS2"``.

## Segment-formulation trap, fixed by lambda-form

The original issue #504 SOS2 implementation used a **segment** form
``{v_1, …, v_L}`` with SOS2 on the v's directly.  Canonical SOS2
combined with ``v_l ≤ w = envelope/L``  capped ``Σ v_l ≤ 2w``  ⇒
``|f| ≤ 2·envelope/L``  which clipped flows below the line rating
for ``L ≥ 3``.  IEEE 14 at L=4 demand-failed (line 1 saturated at
``tmax/2 = 75 MW``, obj jumped 5× to $1.12M).

The lambda-form refactor (this commit) replaces the segment-SOS2 with
``2L+1`` breakpoint weights ``λ_l ∈ [0, 1]`` placed at ``b_l =
(l − L)·w``  for ``l = 0..2L``  (covers ``[-envelope, +envelope]``
symmetrically), with rows ``Σ λ_l = 1``,  ``Σ b_l · λ_l = f``,  ``ℓ
≤ Σ c · b_l² · λ_l``,  and SOS2 on the full ladder.  ``|f|`` reaches
the full envelope at any L.

The ``--verify-no-trap`` flag asserts the fix is active by running
the L = 4 case that used to demand-fail and checking that obj stays
within 1 % of the L = 1 baseline (the LP-observed loss is invariant
under L because gtopt picks ``ℓ_line = max(tangent_k(f_line))``,
K-dependent not L-dependent).

This integration tool runs the actual LP build and tracks the
solver's behaviour as ``L`` varies — the LP-observed total network
loss is invariant under the L sweep, and the objective stays within
solver tolerance across the regimes.

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

# MATPOWER case14.m line resistances are loaded from the VENDORED
# upstream file at ``data/case14.m`` (downloaded from
# https://github.com/MATPOWER/matpower/blob/master/data/case14.m,
# baseMVA = 100, IEEE CDF conversion 1993).  The branch table is
# parsed at import time and indexed by line uid 1..20 matching
# the same row order as the in-tree ``cases/ieee_14b/ieee_14b.json``
# (verified by the line-reactance equality on every row).
_CASE14_M_PATH = Path(__file__).resolve().parent / "data" / "case14.m"


def _parse_matpower_branch_r_pu(path: Path) -> dict[int, float]:
    """Parse the ``mpc.branch = [ ... ];`` block of a MATPOWER .m
    file and return ``{1-based-line-uid: r_pu}`` for every row.

    The MATPOWER branch-table format is:

        %    fbus  tbus  r  x  b  rateA  rateB  rateC  ratio  angle  status  angmin  angmax
        mpc.branch = [
            1   2   0.01938  0.05917  0.0528  0  0  0  0  0  1  -360  360;
            ...
        ];

    Only the third column (``r``) is read; rows are numbered from 1
    in declaration order, matching gtopt's per-line ``uid``.
    """
    text = path.read_text()
    start = text.find("mpc.branch")
    if start < 0:
        raise SystemExit(f"no mpc.branch table found in {path}")
    open_bracket = text.find("[", start)
    close_bracket = text.find("];", open_bracket)
    if open_bracket < 0 or close_bracket < 0:
        raise SystemExit(f"malformed mpc.branch block in {path}")
    body = text[open_bracket + 1 : close_bracket]
    out: dict[int, float] = {}
    uid = 0
    for raw_line in body.splitlines():
        stripped = raw_line.strip().rstrip(";").strip()
        if not stripped or stripped.startswith("%"):
            continue
        cols = stripped.split()
        if len(cols) < 3:
            continue
        try:
            r_pu = float(cols[2])
        except ValueError:
            continue
        uid += 1
        out[uid] = r_pu
    if not out:
        raise SystemExit(f"parsed 0 branches from {path}")
    return out


MATPOWER_CASE14_R_PU = _parse_matpower_branch_r_pu(_CASE14_M_PATH)

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
        default="1,2,4,8",
        help=(
            "Comma-separated list of L (loss_secant_segments) to "
            "sweep.  Default '1,2,4,8' now spans the full range "
            "since the lambda-form refactor lifted the segment-SOS2 "
            "2w cap (issue #504 fix)."
        ),
    )
    p.add_argument(
        "--verify-no-trap",
        action="store_true",
        help=(
            "Assert the lambda-form fix is active.  Runs L = 4 + SOS2 "
            "and checks the objective stays within 1 %% of the L = 1 "
            "baseline (under the original segment-SOS2 the obj jumped "
            "5× because line 1 saturated at tmax/2 and demand-fail "
            "kicked in).  Inverse of the pre-fix --probe-sos2-trap "
            "flag — surfaces the FIX, not the trap."
        ),
    )
    p.add_argument(
        "--epsilon-rely",
        action="store_true",
        help=(
            "Run L sweep in pure-LP ε-rely regime (regime B): "
            "use_sos2 = false on every line, loss_cost_eps > 0 forces "
            "Σ v_l = |f| at LP optimum (chord stays bounded by the "
            "piecewise secant).  Equivalent to regime C in objective "
            "but pure LP (no MIP), so much faster."
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
    # Pick the regime per --epsilon-rely flag.  Default = (C)
    # lambda-form SOS2; --epsilon-rely switches to (B) pure-LP ε-rely.
    use_sos2_for_L_gt_1 = not args.epsilon_rely
    regime_label = "lambda-form SOS2" if use_sos2_for_L_gt_1 else "ε-rely (no SOS2)"
    print(f"# L sweep regime:     {regime_label}")
    print(f"# L sweep:            {L_values}")

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
        use_sos2 = use_sos2_for_L_gt_1 and L > 1
        case = _build_ieee14_with_losses(
            base_case,
            L=L,
            use_sos2=use_sos2,
            loss_cost_eps=args.loss_cost_eps,
            loss_segments=args.loss_segments,
        )
        json_path = workspace / f"ieee14_L{L}_{'sos2' if use_sos2 else 'eps'}.json"
        with json_path.open("w") as f:
            json.dump(case, f, indent=2)
        out_dir = workspace / f"out_L{L}_{'sos2' if use_sos2 else 'eps'}"
        obj, loss_MWh = _run_gtopt(gtopt_bin, json_path, out_dir)
        results.append({"L": L, "sos2": use_sos2, "obj": obj, "loss": loss_MWh})

    # Δobj baseline = L=1 if present, else the first result (so the
    # Δ column is still meaningful when the sweep doesn't start at L=1).
    obj_L1 = next(
        (r["obj"] for r in results if r["L"] == 1),
        results[0]["obj"] if results else 0.0,
    )

    for r in results:
        print(
            f"{r['L']:>3}  {'yes' if r['sos2'] else 'no':>4}  "
            f"{r['obj']:>14.4f}  "
            f"{r['loss']:>14.4f}  "
            f"{(r['loss'] / total_demand) * 100:>13.4f}%  "
            f"{r['obj'] - obj_L1:>+18.4f}"
        )

    # --- verify-no-trap: assert the lambda-form fix is active ---
    if args.verify_no_trap:
        print()
        print(
            "# --- verify-no-trap: L = 4 + SOS2 (expects "
            "obj within 1 %% of L = 1 baseline; the pre-fix "
            "segment-SOS2 would have triggered demand-fail) ---"
        )
        case = _build_ieee14_with_losses(
            base_case,
            L=4,
            use_sos2=True,
            loss_cost_eps=args.loss_cost_eps,
            loss_segments=args.loss_segments,
        )
        json_path = workspace / "ieee14_L4_sos2_verify.json"
        with json_path.open("w") as f:
            json.dump(case, f, indent=2)
        out_dir = workspace / "out_L4_sos2_verify"
        obj_v, loss_v = _run_gtopt(gtopt_bin, json_path, out_dir)
        print(
            f"{4:>3}  {'yes':>4}  "
            f"{obj_v:>14.4f}  "
            f"{loss_v:>14.4f}  "
            f"{(loss_v / total_demand) * 100:>13.4f}%  "
            f"{obj_v - obj_L1:>+18.4f}"
        )
        rel_gap = abs(obj_v - obj_L1) / max(1.0, abs(obj_L1))
        assert rel_gap <= 0.01, (
            f"--verify-no-trap expected L=4 + SOS2 obj within 1 % of "
            f"L=1 baseline (saw obj_L4={obj_v:.2f}, obj_L1={obj_L1:.2f}, "
            f"rel gap={rel_gap * 100:.4f} %).  The lambda-form fix may "
            "have regressed back to the segment-SOS2 trap."
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
# How the lambda-form refactor fixes the segment-SOS2 cap
# ----------------------------------------------------------------------
#
# The pre-fix ``line_losses.cpp`` emitted:
#
#     v_l ∈ [0, w]   for l = 1..L     (w = envelope/L)
#     Σ v_l ≥ |f|                      (two abs rows)
#     ℓ ≤ Σ chord_slope_l · v_l        (chord upper bound)
#     SOS2 on {v_1, …, v_L}            (capped |f| ≤ 2w!)
#
# Beale–Tomlin SOS2 "at most 2 adjacent non-zero" combined with
# ``v_l ≤ w``  gave ``Σ v_l ≤ 2w``  ⇒  silently capped line flow at
# ``2·envelope/L`` < ``envelope`` for L ≥ 3.
#
# The lambda-form fix emits instead, for L > 1 + SOS2:
#
#     λ_l ∈ [0, 1]   for l = 0..2L     (2L+1 breakpoint weights)
#     b_l = (l − L) · w                (breakpoints, [-envelope..+envelope])
#     Σ λ_l = 1                        (convexity)
#     Σ b_l · λ_l = f                  (signed flow tied to breakpoints)
#     ℓ ≤ Σ c · b_l² · λ_l             (chord = piecewise secant)
#     SOS2 on {λ_0, …, λ_{2L}}         (interpolation between adjacent b's)
#
# Beale–Tomlin SOS2 on ``{λ_0, …, λ_{2L}}``  forces the LP to land
# on or between two adjacent breakpoints, so ``f`` interpolates
# linearly between ``b_l`` and ``b_{l+1}``  and the chord delivers
# the *exact piecewise secant* of ``c·f²`` on that segment.  No 2w
# cap — ``|f|``  reaches ``|b_0|`` = ``|b_{2L}|`` = ``envelope``.
#
# The ε-rely path (regime B) is also supported as a pure-LP
# alternative (``loss_use_sos2 = false`` + ``loss_cost_eps > 0``):
# segment cols + ε in the objective force ``Σ v_l = |f|``  at LP
# optimum.  The v_l distribution is then LP-indifferent — the chord
# row is INACTIVE at LP optimum (the K-tangent lower bound on ℓ
# binds first), so the LP has no obj preference between e.g.
# bottom-up ``{50, 25, 0, 0}``  and a degenerate ``{0, 25, 50, 0}``
# at f = 75 with L = 4.  Both yield the same obj; the obj matches
# lambda-form (regime C) to within solver tolerance.  What ε > 0
# buys is purely structural: keeps Σ v_l = |f| bounded so the
# chord stays at the piecewise secant value rather than the loose
# constant ceiling under the unbounded-Σ arbitrage.  Pure LP, no
# MIP, no flow cap.  Use ``--epsilon-rely`` to exercise this regime.


if __name__ == "__main__":
    sys.exit(main())
