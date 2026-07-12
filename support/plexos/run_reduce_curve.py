#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""LP-relax performance curve over network reduction ratios.

For one support/plexos PCP case:

  1. convert DATOS*.zip.xz once with ``--lp-relax`` + Capricornio lift
     (same recipe as ``run_lp_relax_loop.py``);
  2. solve the FULL case (Kirchhoff + tangent_signed_flow losses, all
     buses/lines) as the reference point;
  3. for each ratio r in --ratios: ``gtopt-net reduce --bus-ratio r
     --partition louvain-mincut --transport-only --loss-mode uplift
     --loss-uplift-pct 2`` and solve the reduced transport case;
  4. report objective error vs the reference and solve-time speedup.

All solves use the LP-relax recipe: ``--no-scale`` + barrier without
crossover (CPLEX SolutionType=NONBASIC).  Outputs land under
``~/tmp/reduce_curve`` by default.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SUPPORT = Path(__file__).resolve().parent
SCRIPTS = REPO / "scripts"

CAPRICORNIO = "Capricornio110->LaNegra110"


def _log(msg: str) -> None:
    print(f"[curve] {msg}", flush=True)


def run(cmd: list[str], cwd: Path | None, log: Path, env: dict | None = None) -> int:
    _log("  $ " + " ".join(str(c) for c in cmd))
    with log.open("w") as fh:
        return subprocess.run(
            cmd, cwd=cwd, stdout=fh, stderr=subprocess.STDOUT, check=False, env=env
        ).returncode


def _grep_objective(log: Path) -> float | None:
    if not log.exists():
        return None
    pat = re.compile(
        r"(?:objective|obj(?:ective)?\s*value|optimal)\D*([-\d.eE+]+)", re.I
    )
    # CPLEX barrier iteration rows: "  55   1.2586757e+09   1.2586757e+09 ..."
    barrier = re.compile(
        r"^\s*\d+\s+([-\d.]+e[+-]\d+)\s+([-\d.]+e[+-]\d+)\s", re.I
    )
    val = None
    barrier_val = None
    for line in log.read_text(errors="ignore").splitlines():
        m = pat.search(line)
        if m:
            try:
                val = float(m.group(1))
                continue
            except ValueError:
                pass
        b = barrier.match(line)
        if b:
            try:
                barrier_val = float(b.group(1))
            except ValueError:
                pass
    return val if val is not None else barrier_val


def _solve(
    gtopt_bin: str, json_path: Path, tag: str, time_limit: float
) -> dict:
    outdir = json_path.parent
    log = outdir / f"solve_{tag}.log"
    cmd = [
        gtopt_bin,
        json_path.name,
        "--no-scale",
        "--set",
        "solver_options.crossover=none",
        "--set",
        f"output_directory=output_{tag}",
    ]
    if time_limit and time_limit > 0:
        cmd += ["--set", f"solver_options.time_limit={time_limit:g}"]
    cmd += ["-l", json_path.stem]
    t0 = time.time()
    rc = run(cmd, cwd=outdir, log=log)
    return {
        "solve_rc": rc,
        "solve_secs": round(time.time() - t0, 1),
        "objective": _csv_objective(outdir / f"output_{tag}" / "solution.csv")
        or _grep_objective(log),
        "output_dir": str(outdir / f"output_{tag}"),
        "log": str(log),
    }


def _csv_objective(solution_csv: Path) -> float | None:
    """Authoritative objective: sum of obj_value over (scene, phase) rows."""
    if not solution_csv.exists():
        return None
    try:
        lines = solution_csv.read_text().strip().splitlines()
        hdr = lines[0].split(",")
        idx = hdr.index("obj_value")
        return sum(float(ln.split(",")[idx]) for ln in lines[1:])
    except (ValueError, IndexError):
        return None


def _counts(json_path: Path) -> tuple[int, int]:
    sysd = json.loads(json_path.read_text())["system"]
    return len(sysd.get("bus_array", [])), len(sysd.get("line_array", []))


def _default_gtopt_bin() -> str:
    """Prefer this repo's build; fall back to the main checkout when this
    script runs from a worktree under .claude/worktrees/<name>/."""
    candidates = [REPO / "build/standalone/gtopt"]
    if REPO.parent.parent.name == ".claude":
        candidates.append(REPO.parents[2] / "build/standalone/gtopt")
    for cand in candidates:
        if cand.exists():
            return str(cand)
    return str(candidates[0])


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", default="pcp_2025-10-19")
    ap.add_argument("--work", default=str(Path.home() / "tmp" / "reduce_curve"))
    ap.add_argument(
        "--ratios",
        nargs="*",
        type=float,
        default=[0.0, 0.1, 0.2, 0.25, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0],
        help="bus-count fractions; 0 = uninodal (K=1 copper plate)",
    )
    ap.add_argument("--uplift-pct", type=float, default=2.0)
    ap.add_argument(
        "--loss-mode",
        choices=["off", "uplift", "gen-lossfactor"],
        default="gen-lossfactor",
        help="loss surrogate for the reduced transport cases "
        "(default: gen-lossfactor — injection penalty factor)",
    )
    ap.add_argument("--time-limit", type=float, default=7200.0)
    ap.add_argument("--skip-reference", action="store_true")
    ap.add_argument(
        "--gtopt-bin",
        default=os.environ.get("GTOPT_BIN", _default_gtopt_bin()),
    )
    args = ap.parse_args()

    case_dir = SUPPORT / args.case
    d8 = args.case.replace("pcp_", "").replace("-", "")
    datos = case_dir / f"DATOS{d8}.zip.xz"
    if not datos.exists():
        _log(f"missing {datos}")
        return 1

    work = Path(args.work)
    outdir = work / f"PLEXOS{d8}"
    outdir.mkdir(parents=True, exist_ok=True)
    report_path = work / f"reduce_curve_{d8}.json"

    env = dict(os.environ)
    env["GTOPT_LIFT_LINE_CAPS"] = CAPRICORNIO
    env.setdefault("TMPDIR", str(Path.home() / "tmp"))

    # 1. Convert once (LP-relax commitments + Capricornio lift). ----------
    json_path = next(
        (
            p
            for p in sorted(outdir.glob("*.json"))
            if not p.name.endswith(".provenance.json")
            and not p.name.startswith("reduced_")
            and p.name != report_path.name
        ),
        None,
    )
    if json_path is None:
        rc = run(
            [
                sys.executable,
                "-m",
                "plexos2gtopt.main",
                str(datos),
                "-o",
                str(outdir),
                "--lp-relax",
                "--no-check",
                "--lift-line-caps",
                CAPRICORNIO,
            ],
            cwd=SCRIPTS,
            log=outdir / "convert.log",
            env=env,
        )
        if rc != 0:
            _log(f"convert failed rc={rc}; see {outdir / 'convert.log'}")
            return 1
        json_path = next(
            (
                p
                for p in sorted(outdir.glob("*.json"))
                if not p.name.endswith(".provenance.json")
                and not p.name.startswith("reduced_")
            ),
            None,
        )
    if json_path is None:
        _log("no planning JSON produced")
        return 1
    _log(f"base case: {json_path}")

    # Resume support: reuse points already solved in a previous run.
    report: dict = {"case": args.case, "base_json": str(json_path), "points": []}
    if report_path.exists():
        try:
            report = json.loads(report_path.read_text())
            # Drop failed points so they re-run cleanly (no duplicates).
            report["points"] = [
                p for p in report["points"] if p.get("solve_rc") == 0
            ]
        except json.JSONDecodeError:
            pass
    done = {p.get("tag") for p in report["points"] if p.get("solve_rc") == 0}

    # 2. Reference: full Kirchhoff + losses + all buses/lines. ------------
    n_bus, n_line = _counts(json_path)
    if not args.skip_reference and "reference" not in done:
        _log(f"reference solve (kirchhoff+losses, {n_bus} buses, {n_line} lines)")
        ref = {"ratio": None, "tag": "reference", "buses": n_bus, "lines": n_line}
        ref.update(_solve(args.gtopt_bin, json_path, "reference", args.time_limit))
        report["points"].append(ref)
        report_path.write_text(json.dumps(report, indent=2))

    # 3. Reduced transport points (ratio 0 = uninodal copper plate). -------
    for ratio in args.ratios:
        tag = f"r{int(round(ratio * 100)):03d}"
        if tag in done:
            _log(f"skip {tag}: already solved")
            continue
        red_json = outdir / f"reduced_{tag}.json"
        size_args = ["-K", "1"] if ratio <= 0.0 else ["--bus-ratio", f"{ratio:g}"]
        reduce_cmd = [
            sys.executable,
            "-m",
            "gtopt_reduce_network.main",
            "reduce",
            str(json_path),
            *size_args,
            "--partition",
            "louvain-mincut",
            "--transport-only",
            "--loss-mode",
            args.loss_mode,
            "--loss-uplift-pct",
            f"{args.uplift_pct:g}",
            "-o",
            str(red_json),
        ]
        t0 = time.time()
        rc = run(reduce_cmd, cwd=SCRIPTS, log=outdir / f"reduce_{tag}.log", env=env)
        point: dict = {
            "ratio": ratio,
            "tag": tag,
            "reduce_rc": rc,
            "reduce_secs": round(time.time() - t0, 1),
        }
        if rc == 0 and red_json.exists():
            point["buses"], point["lines"] = _counts(red_json)
            point.update(_solve(args.gtopt_bin, red_json, tag, args.time_limit))
        report["points"].append(point)
        report_path.write_text(json.dumps(report, indent=2))

    # 4. Summary table. -----------------------------------------------------
    ref_obj = next(
        (p.get("objective") for p in report["points"] if p.get("tag") == "reference"),
        None,
    )
    ref_secs = next(
        (p.get("solve_secs") for p in report["points"] if p.get("tag") == "reference"),
        None,
    )
    print("\n==== REDUCE CURVE SUMMARY ====")
    print(
        f"{'point':10s} {'buses':>6s} {'lines':>6s} {'objective':>16s} "
        f"{'err%':>8s} {'secs':>8s} {'speedup':>8s}"
    )
    points = sorted(
        report["points"],
        key=lambda p: -1.0 if p.get("tag") == "reference" else (p.get("ratio") or 0.0),
    )
    for p in points:
        obj = p.get("objective")
        err = (
            f"{100.0 * (obj - ref_obj) / abs(ref_obj):+.2f}"
            if obj is not None and ref_obj
            else "-"
        )
        spd = (
            f"{ref_secs / p['solve_secs']:.1f}x"
            if ref_secs and p.get("solve_secs")
            else "-"
        )
        print(
            f"{p.get('tag', ''):10s} {p.get('buses', 0):6d} {p.get('lines', 0):6d} "
            f"{obj if obj is not None else float('nan'):16.6g} {err:>8s} "
            f"{p.get('solve_secs', float('nan')):8.1f} {spd:>8s}"
        )
    print(f"\nreport: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
