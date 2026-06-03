# SPDX-License-Identifier: BSD-3-Clause
"""IEEE 118-bus OTS infrastructure smoke test (issue #509).

Reads ``cases/ieee_118b/ieee_118b.json``, generates two OTS-compatible
variants (baseline + LineCommitment on every line, LP-relax), runs gtopt
on each, and reports the savings ratio.

## Golden reference

  > "we find that on the standard 118-bus IEEE test case a savings of
  > 25 percent in system dispatch cost can be achieved"
  > Fisher–O'Neill–Ferris, 2008 — Optimal Transmission Switching, IEEE
  > Transactions on Power Systems

  - Baseline (no switching, DC-OPF only):   $2 054 /h
  - MIP-optimal with 38 lines opened:       $1 543 /h ⇒ 25 % savings
  - Tabulated incremental openings:
        line 153 only                        $1 925 /h ⇒  6.3 %
        lines 132 + 153                      $1 800 /h ⇒ 12.4 %
        lines 132 + 136 + 153                $1 646 /h ⇒ 19.9 %
        38 lines (MIP optimum)               $1 543 /h ⇒ 25.0 %

## Caveat: LP-relax doesn't reproduce the 25 %

This script enables OTS in LP-RELAX mode (``relax: true`` on every
LineCommitment row), which makes ``u_l ∈ [0, 1]`` continuous.  The
LP solver typically picks ``u_l = 1`` on every line (no benefit
to fractional opening), so the LP-relax savings ratio is usually
0 % — much less than Fisher's integer-MIP 25 %.

The integer-OTS benefit comes from fully OPENING lines (``u_l = 0``),
which:
  (a) eliminates their thermal contribution to KVL coupling, and
  (b) frees the LP to reroute flow through cheaper paths.

LP-relax of OTS cannot exploit (a) or (b) effectively because half-
open lines still impose KVL coupling and contribute fractional
capacity.  A meaningful comparison to Fisher 2008 would require:

  1. A full MIP solve with the binary ``u_l`` (drop ``relax: true``).
     gtopt supports this but only on a backend with MIP capability
     (CPLEX or HiGHS).
  2. Realistic per-line thermal limits.  gtopt's stored
     ``ieee_118b`` ships with a uniform 9 900 MW limit, which is
     ~2× the total system demand — no congestion to relieve.  Pass
     ``--line-limit-scale 0.02`` to tighten to ~200 MW (Fisher 2008's
     congestion regime).

## What this script validates

  1. The OTS LP infrastructure builds without crashing on a 118-bus /
     186-line case (every ``LineCommitment`` row is honoured).
  2. ``obj_ots ≤ obj_baseline`` (monotonicity — OTS can only improve).
  3. Both runs produce parseable ``solution.csv`` output.

## Run

    GTOPT_BIN=$PWD/build/standalone/gtopt \\
        python gtopt_ots_ieee118.py [--line-limit-scale 0.02] [--keep]

Exit codes:
   0  Both runs succeeded and obj_ots ≤ obj_baseline.
   2  Monotonicity violation (obj_ots > obj_baseline + 1e-6).
   1  gtopt failed on either run, or file/CLI error.
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


# Fisher–O'Neill–Ferris 2008 golden value (MIP-optimal, 38 lines opened).
FISHER_2008_GOLDEN_SAVINGS = 0.25
# Tabulated partial-opening savings from Fisher 2008 Table III.
FISHER_2008_TABLE = {
    "no switching": {"cost": 2054.0, "saving_pct": 0.0},
    "line 153": {"cost": 1925.0, "saving_pct": 6.3},
    "lines 132, 153": {"cost": 1800.0, "saving_pct": 12.4},
    "lines 132, 136, 153": {"cost": 1646.0, "saving_pct": 19.9},
    "38 lines (MIP optimum)": {"cost": 1543.0, "saving_pct": 25.0},
}


def _project_root() -> Path:
    """Walk upward from this file until a ``cases/`` directory is found."""
    here = Path(__file__).resolve()
    for parent in (here.parent, *here.parents):
        if (parent / "cases" / "ieee_118b" / "ieee_118b.json").is_file():
            return parent
    raise FileNotFoundError(
        "Could not locate the gtopt project root from this script's path. "
        "Expected cases/ieee_118b/ieee_118b.json relative to a parent dir."
    )


def _build_ots_variant(
    base: dict,
    *,
    with_line_commitment: bool,
    line_limit_scale: float = 1.0,
) -> dict:
    """Return a deep copy of ``base`` reconfigured for OTS-compatible solve.

    The IEEE 118-bus case ships with ``method = "cascade"`` and a
    multi-level model_options stack.  OTS rejects cascade (Zou-Ahmed-Sun
    2019 — Benders cuts on a MIP subproblem are unsound), so we flatten
    to ``method = "monolithic"`` with explicit DC-OPF options.

    When ``with_line_commitment = True`` the variant gains a
    ``line_commitment_array`` entry for every line in the system, all
    LP-relaxed (so the resulting problem is a pure LP, not a MIP).

    ``line_limit_scale`` < 1.0 tightens the per-line thermal caps
    uniformly.  gtopt's stored ``ieee_118b`` ships with a uniform
    9 900 MW limit (loose — total demand is only ~4 200 MW), so OTS
    has no congestion to relieve at scale 1.0.  Fisher 2008's reported
    25 % savings assumed realistic per-line limits ~100-500 MW.  Pass
    e.g. 0.02 to bring 9 900 MW down to ~200 MW (a Fisher-style
    congested topology).
    """
    out = copy.deepcopy(base)

    # Flatten cascade → monolithic with DC-OPF + node_angle Kirchhoff.
    out["options"].pop("cascade_options", None)
    out["options"]["method"] = "monolithic"
    mo = out["options"].setdefault("model_options", {})
    mo.update(
        {
            "use_single_bus": False,
            "use_kirchhoff": True,
            "kirchhoff_mode": "node_angle",
            "use_line_losses": False,
        }
    )

    # Tighten line limits when requested.
    if line_limit_scale != 1.0:
        for ln in out["system"]["line_array"]:
            for k in ("tmax_ab", "tmax_ba"):
                if k in ln and isinstance(ln[k], (int, float)):
                    ln[k] = ln[k] * line_limit_scale

    if with_line_commitment:
        lines = out["system"]["line_array"]
        lcs = []
        for i, ln in enumerate(lines, start=1):
            line_ref = ln.get("name") if ln.get("name") else ln["uid"]
            lcs.append(
                {
                    "uid": i,
                    "name": f"lc_{line_ref}",
                    "line": line_ref,
                    "relax": True,
                }
            )
        out["system"]["line_commitment_array"] = lcs

    return out


def _run_gtopt(gtopt_bin: Path, planning_json: Path, output_dir: Path) -> float:
    """Run gtopt on a single planning JSON and return the objective value.

    The objective is read from ``solution.csv``'s ``obj_value`` column.
    Throws on non-zero exit; caller handles that as a hard failure.
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
    print(f"  + {' '.join(cmd)}", flush=True)
    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        print(proc.stdout)
        print(proc.stderr, file=sys.stderr)
        raise SystemExit(f"gtopt failed on {planning_json} (exit={proc.returncode})")

    sol = output_dir / "solution.csv"
    if not sol.is_file():
        raise SystemExit(f"gtopt did not produce a solution.csv under {output_dir}")
    with sol.open() as f:
        header = f.readline().rstrip("\n").split(",")
        row = f.readline().rstrip("\n").split(",")
    try:
        idx = header.index("obj_value")
    except ValueError as exc:
        raise SystemExit(
            f"solution.csv at {sol} has no obj_value column; header was {header}"
        ) from exc
    return float(row[idx])


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument(
        "--gtopt",
        type=Path,
        default=None,
        help="Path to gtopt binary.  Defaults to $GTOPT_BIN or PATH lookup.",
    )
    p.add_argument(
        "--tmp",
        type=Path,
        default=None,
        help="Workspace dir for variants + outputs.  Defaults to a tempdir.",
    )
    p.add_argument(
        "--keep",
        action="store_true",
        help="Keep the workspace dir after the run (for debugging).",
    )
    p.add_argument(
        "--line-limit-scale",
        type=float,
        default=0.02,
        help=(
            "Per-line thermal-limit scale factor.  gtopt's stored "
            "ieee_118b ships with a uniform 9 900 MW limit (no "
            "congestion possible).  Default 0.02 ≈ 200 MW caps, "
            "matching Fisher 2008's per-line range and exposing OTS "
            "savings.  Use 1.0 to leave the original limits unchanged."
        ),
    )
    args = p.parse_args(argv)

    root = _project_root()
    base_json = root / "cases" / "ieee_118b" / "ieee_118b.json"
    print(f"# Project root:       {root}")
    print(f"# Base JSON:          {base_json}")

    # Resolve the gtopt binary.
    gtopt_bin = args.gtopt
    if gtopt_bin is None:
        env = os.environ.get("GTOPT_BIN", "").strip()
        if env:
            gtopt_bin = Path(env)
        else:
            which = shutil.which("gtopt")
            if which is None:
                # Fall back to the canonical build location.
                cand = root / "build" / "standalone" / "gtopt"
                if cand.is_file():
                    gtopt_bin = cand
            else:
                gtopt_bin = Path(which)
    if gtopt_bin is None or not gtopt_bin.is_file():
        raise SystemExit(
            "Could not locate the gtopt binary.  Pass --gtopt or set "
            "GTOPT_BIN, or build the standalone target.",
        )
    print(f"# gtopt binary:       {gtopt_bin}")

    # Workspace.
    workspace = args.tmp
    cleanup = False
    if workspace is None:
        workspace = Path(tempfile.mkdtemp(prefix="gtopt_ots_ieee118_"))
        cleanup = not args.keep
    workspace.mkdir(parents=True, exist_ok=True)
    print(f"# Workspace:          {workspace}")

    with base_json.open() as f:
        base = json.load(f)

    baseline = _build_ots_variant(
        base,
        with_line_commitment=False,
        line_limit_scale=args.line_limit_scale,
    )
    ots = _build_ots_variant(
        base,
        with_line_commitment=True,
        line_limit_scale=args.line_limit_scale,
    )
    print(f"# Line limit scale:   {args.line_limit_scale}")

    baseline_json = workspace / "ieee_118b_baseline.json"
    ots_json = workspace / "ieee_118b_ots.json"
    with baseline_json.open("w") as f:
        json.dump(baseline, f, indent=2)
    with ots_json.open("w") as f:
        json.dump(ots, f, indent=2)

    print("\n# Baseline solve (no LineCommitment) ...")
    out_base = workspace / "out_baseline"
    obj_base = _run_gtopt(gtopt_bin, baseline_json, out_base)
    print(f"  obj_baseline = {obj_base}")

    print("\n# OTS solve (LineCommitment on every line, LP-relax) ...")
    out_ots = workspace / "out_ots"
    obj_ots = _run_gtopt(gtopt_bin, ots_json, out_ots)
    print(f"  obj_ots      = {obj_ots}")

    savings = obj_base - obj_ots
    ratio = savings / obj_base if obj_base != 0.0 else 0.0
    print("\n# Result")
    print(f"  Absolute savings:  {savings:+.4f}")
    print(f"  Savings ratio:     {ratio * 100:+.2f} %")
    print(f"  Fisher 2008 MIP:   {FISHER_2008_GOLDEN_SAVINGS * 100:.2f} %")
    print("  Fisher 2008 table (DC-OPF, with realistic line limits):")
    for label, info in FISHER_2008_TABLE.items():
        print(
            f"    {label:35s} ${info['cost']:>7.0f}/h  "
            f"savings {info['saving_pct']:5.1f} %"
        )
    if abs(ratio) < 1e-6:
        print(
            "\n  Note: LP-relax OTS rarely produces non-zero savings; the\n"
            "  integer-OTS benefit is lost when ``u_l`` can be fractional.\n"
            "  See the script docstring for why and how to get to Fisher's\n"
            "  golden 25 % (full MIP + tightened line limits)."
        )

    if cleanup:
        shutil.rmtree(workspace, ignore_errors=True)

    if obj_ots > obj_base + 1e-6:
        print(
            "ERROR: OTS obj is larger than baseline obj.  OTS is monotone-"
            "improving, so this indicates a bug in either the LP build or "
            "the comparison harness.",
            file=sys.stderr,
        )
        return 2
    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
