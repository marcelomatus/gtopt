# SPDX-License-Identifier: BSD-3-Clause
"""IEEE 118-bus OTS validation against Fisher 2008's golden 25 % savings.

Reads ``cases/ieee_118b/ieee_118b.json``, generates two OTS-compatible
variants (baseline + LineCommitment on selected candidate lines),
runs gtopt on each as a **MIP** (binary ``u_l``), and reports the
savings ratio vs the published Fisher-O'Neill-Ferris 2008 golden
table.

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

## Settings that influence the result

Three knobs:

  1. **--line-limit-scale**.  gtopt's stored ``ieee_118b`` ships with
     a uniform 9 900 MW thermal limit, which is ~2× total system
     demand — no congestion possible.  Default 0.02 ≈ 200 MW caps
     (Fisher 2008's per-line range, 5 lines saturated at baseline);
     0.01 ≈ 99 MW (much more congestion).

  2. **--candidate-lines** (optional).  Fisher's all-line MIP over
     186 binaries is genuinely hard (Fisher reported hours).
     Restricting to the K most-congested lines makes the MIP
     tractable AND mirrors the practical OTS workflow.  Default:
     all 186 lines.

  3. **--mip-gap / --time-limit**.  Looser gap or shorter time
     limit returns sub-optimal incumbents; if the MIP times out
     before improving on the trivial all-closed solution, you'll
     see ``obj_ots > obj_baseline``.

## Quick-start: reproduce ~3 % savings in ~5 minutes

    GTOPT_BIN=$PWD/build/standalone/gtopt \\
        python gtopt_ots_ieee118.py \\
            --time-limit 600 --mip-gap 0.001 \\
            --line-limit-scale 0.01 \\
            --candidate-lines \\
              'l26_30,l38_65,l89_92,t8_5,t68_69,t38_37,t65_66,t116_68,l8_9,l9_10,t30_17,l82_83,l110_111,l23_25,t65_68,l64_65,l25_27,l77_82,t81_68,t81_80'

(The candidate list is the top 20 most-utilised lines at baseline
with the 0.01 scale.)

## Why don't we reach Fisher's 25 % exactly?

  1. gtopt's ``ieee_118b`` has DIFFERENT generator cost data than
     Fisher 2008's IEEE 118 (gtopt: $20-$40/MWh from pglib-opf;
     Fisher: $0.19-$10/MWh scaled).  Absolute objective values
     aren't comparable; only relative savings.

  2. The savings ratio is highly sensitive to which line limits
     are binding.  Fisher's experiment used a single specific
     load level and line-limit profile; ours uses a uniform scale.

  3. All-line MIP (186 binaries) is the only way to reach 25 %;
     restricting to a subset of K candidates caps the savings at
     whatever those K candidates can deliver.

## Exit codes

   0  Both runs succeeded and obj_ots ≤ obj_baseline.
   2  Monotonicity violation (obj_ots > obj_baseline + 1e-6) —
      usually a sub-optimal MIP incumbent; rerun with looser
      --time-limit or smaller candidate set.
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
    lp_relax: bool = False,
    candidate_lines: list[str] | None = None,
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

    # LineCommitmentLP silently skips on non-chronological stages
    # (the chronological gate is shared with CommitmentLP — see
    # ``include/gtopt/line_commitment_lp.hpp`` v1 scope).  The stored
    # ieee_118b case has ``chronological`` unset on every stage, which
    # defaults to false, so we must override here or the LineCommitment
    # rows below get silently dropped from the LP.
    for stage in out["simulation"]["stage_array"]:
        stage["chronological"] = True

    # Tighten line limits when requested.
    if line_limit_scale != 1.0:
        for ln in out["system"]["line_array"]:
            for k in ("tmax_ab", "tmax_ba"):
                if k in ln and isinstance(ln[k], (int, float)):
                    ln[k] = ln[k] * line_limit_scale

    if with_line_commitment:
        lines = out["system"]["line_array"]
        # Filter to ``candidate_lines`` (by name).  None ⇒ all 186
        # lines are OTS candidates (Fisher 2008's all-line MIP).  A
        # subset (e.g. the top-K most-congested) is more practical
        # because 186-line OTS is a genuinely hard MIP — Fisher
        # reported hours of solve time with mid-2000s solvers.
        cand_set = set(candidate_lines) if candidate_lines else None
        lcs = []
        for i, ln in enumerate(lines, start=1):
            line_ref = ln.get("name") if ln.get("name") else ln["uid"]
            if cand_set is not None and line_ref not in cand_set:
                continue
            entry = {
                "uid": i,
                "name": f"lc_{line_ref}",
                "line": line_ref,
            }
            if lp_relax:
                entry["relax"] = True
            lcs.append(entry)
        out["system"]["line_commitment_array"] = lcs

    return out


def _run_gtopt(
    gtopt_bin: Path,
    planning_json: Path,
    output_dir: Path,
    *,
    mip_gap: float | None = None,
    time_limit: float | None = None,
) -> float:
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
    if mip_gap is not None:
        cmd += ["--mip-gap", str(mip_gap)]
    if time_limit is not None:
        cmd += ["--time-limit", str(time_limit)]
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
    p.add_argument(
        "--lp-relax",
        action="store_true",
        help=(
            "LP-relax mode: stamp ``relax: true`` on every "
            "LineCommitment row so u_l becomes continuous in [0, 1]. "
            "Default is MIP (binary u_l) — the form Fisher 2008's 25 %% "
            "golden value assumes.  Use --lp-relax only as a fast "
            "smoke test; the LP-relax rarely produces non-zero savings."
        ),
    )
    p.add_argument(
        "--mip-gap",
        type=float,
        default=None,
        help=(
            "Optional target relative MIP optimality gap (e.g. 0.01 = "
            "1 %%).  Passed through to gtopt as --mip-gap.  Lower "
            "values find tighter solutions but take longer.  Ignored "
            "in --lp-relax mode."
        ),
    )
    p.add_argument(
        "--time-limit",
        type=float,
        default=None,
        help=(
            "Optional per-solve wall-clock limit in seconds (gtopt "
            "--time-limit).  Useful to cap the MIP solve time on "
            "large cases.  0 = no limit."
        ),
    )
    p.add_argument(
        "--candidate-lines",
        type=str,
        default=None,
        help=(
            "Comma-separated line names to use as OTS switching "
            "candidates (default: all 186 lines).  Useful to scope "
            "the MIP — Fisher 2008's 186-line all-line MIP is hard "
            "(hours of solve time).  Try a 10-20 line subset for a "
            "tractable demo."
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

    candidate_lines: list[str] | None = None
    if args.candidate_lines:
        candidate_lines = [
            n.strip() for n in args.candidate_lines.split(",") if n.strip()
        ]
    baseline = _build_ots_variant(
        base,
        with_line_commitment=False,
        line_limit_scale=args.line_limit_scale,
        lp_relax=args.lp_relax,
        candidate_lines=candidate_lines,
    )
    ots = _build_ots_variant(
        base,
        with_line_commitment=True,
        line_limit_scale=args.line_limit_scale,
        lp_relax=args.lp_relax,
        candidate_lines=candidate_lines,
    )
    print(f"# Line limit scale:   {args.line_limit_scale}")
    print(f"# Mode:               {'LP-relax' if args.lp_relax else 'MIP'}")
    if candidate_lines:
        print(f"# OTS candidates:     {len(candidate_lines)} explicit lines")
    else:
        print("# OTS candidates:     all lines")
    if args.mip_gap is not None:
        print(f"# MIP gap target:     {args.mip_gap}")
    if args.time_limit is not None:
        print(f"# Per-solve time-lim: {args.time_limit} s")

    baseline_json = workspace / "ieee_118b_baseline.json"
    ots_json = workspace / "ieee_118b_ots.json"
    with baseline_json.open("w") as f:
        json.dump(baseline, f, indent=2)
    with ots_json.open("w") as f:
        json.dump(ots, f, indent=2)

    print("\n# Baseline solve (no LineCommitment) ...")
    out_base = workspace / "out_baseline"
    obj_base = _run_gtopt(
        gtopt_bin,
        baseline_json,
        out_base,
        mip_gap=args.mip_gap if not args.lp_relax else None,
        time_limit=args.time_limit,
    )
    print(f"  obj_baseline = {obj_base}")

    mode_label = "LP-relax" if args.lp_relax else "MIP"
    print(f"\n# OTS solve (LineCommitment on every line, {mode_label}) ...")
    out_ots = workspace / "out_ots"
    obj_ots = _run_gtopt(
        gtopt_bin,
        ots_json,
        out_ots,
        mip_gap=args.mip_gap if not args.lp_relax else None,
        time_limit=args.time_limit,
    )
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
