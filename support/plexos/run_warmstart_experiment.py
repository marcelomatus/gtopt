#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""MIP warm-start experiment: reduced transport solution → full-MIP seed.

For one support/plexos PCP case:

  1. convert DATOS*.zip.xz once in MIP mode (WITHOUT --lp-relax, so
     commitments keep integrality) + Capricornio lift;
  2. BASELINE — solve the full MIP with NO seed
     (monolithic_options.mip_start.enabled=false), record time + gap;
  3. for each network-reduction level L (default 30% → 25% → 20%, the
     structurally-faithful band; 0% uninodal dropped — under-commits):
       a. gtopt-net reduce at L (transport + gen-lossfactor 2%);
       b. solve the reduced case LP-relaxed (--no-mip) — cheap — to get a
          commitment schedule;
       c. dump (generator_uid, block_uid, round(u)) → seed CSV;
       d. solve the full MIP WITH the seed
          (mip_start.enabled=true, mip_start.seed_solution_file=seed.csv),
          record time + gap;
  4. report baseline vs each warm-start: time-to-target-gap and final gap.

Outputs under ~/tmp/warmstart_exp by default.  Resume-aware.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import pyarrow.dataset as pads

REPO = Path(__file__).resolve().parents[2]
SUPPORT = Path(__file__).resolve().parent
SCRIPTS = REPO / "scripts"
CAPRICORNIO = "Capricornio110->LaNegra110"


def _log(msg: str) -> None:
    print(f"[warmstart] {msg}", flush=True)


def run(cmd: list[str], cwd: Path | None, log: Path, env: dict | None = None) -> int:
    _log("  $ " + " ".join(str(c) for c in cmd))
    with log.open("w") as fh:
        return subprocess.run(
            cmd, cwd=cwd, stdout=fh, stderr=subprocess.STDOUT, check=False, env=env
        ).returncode


def _default_gtopt_bin() -> str:
    cands = [REPO / "build/standalone/gtopt"]
    if REPO.parent.parent.name == ".claude":
        cands.append(REPO.parents[2] / "build/standalone/gtopt")
    for c in cands:
        if c.exists():
            return str(c)
    return str(cands[0])


def _grep_ticks(log: Path) -> tuple[float | None, float | None]:
    """Deterministic CPLEX work from the ``GTOPT_SOLVE_EFFORT`` line(s).

    Format: ``GTOPT_SOLVE_EFFORT solver_time=354.9 s ticks=98871.8 solves=1``.
    Ticks are deterministic (independent of CPU contention), so they are
    the primary time-cost metric here — several jobs may share the host.
    Returns ``(total_ticks, total_solver_time)`` summed across solves.
    """
    if not log.exists():
        return None, None
    ticks = 0.0
    stime = 0.0
    seen = False
    for line in log.read_text(errors="ignore").splitlines():
        if "GTOPT_SOLVE_EFFORT" not in line:
            continue
        mt = re.search(r"ticks=([\d.eE+]+)", line)
        ms = re.search(r"solver_time=([\d.eE+]+)", line)
        if mt:
            ticks += float(mt.group(1))
            seen = True
        if ms:
            stime += float(ms.group(1))
    return (ticks if seen else None), (stime if seen else None)


def _grep_gap_status(log: Path) -> dict:
    """Extract final MIP gap / status / incumbent from a CPLEX solve log."""
    out: dict = {"gap": None, "incumbent": None, "status": None}
    if not log.exists():
        return out
    txt = log.read_text(errors="ignore")
    # CPLEX "Solution status" / "MIP - Integer optimal" / gap lines
    for m in re.finditer(r"gap\s*=?\s*([\d.]+)\s*%", txt, re.I):
        try:
            out["gap"] = float(m.group(1)) / 100.0
        except ValueError:
            pass
    m = re.search(r"Best (?:integer|objective)[^\d-]*([-\d.eE+]+)", txt)
    if m:
        try:
            out["incumbent"] = float(m.group(1))
        except ValueError:
            pass
    for key in ("optimal", "time limit", "integer infeasible", "aborted"):
        if key in txt.lower():
            out["status"] = key
    return out


def _obj_from_solution(out_dir: Path) -> float | None:
    csvp = out_dir / "solution.csv"
    if not csvp.exists():
        return None
    try:
        rows = csvp.read_text().strip().splitlines()
        idx = rows[0].split(",").index("obj_value")
        gapidx = rows[0].split(",").index("gap") if "gap" in rows[0] else None
        obj = sum(float(r.split(",")[idx]) for r in rows[1:])
        return obj
    except (ValueError, IndexError):
        return None


def _solution_gap(out_dir: Path) -> float | None:
    csvp = out_dir / "solution.csv"
    if not csvp.exists():
        return None
    try:
        rows = csvp.read_text().strip().splitlines()
        hdr = rows[0].split(",")
        if "gap" not in hdr:
            return None
        gi = hdr.index("gap")
        return max(float(r.split(",")[gi]) for r in rows[1:])
    except (ValueError, IndexError):
        return None


def _commitment_to_generator(mip_json: Path) -> dict[int, int]:
    """Map commitment uid → generator uid via commitment_array.generator."""
    sysd = json.loads(mip_json.read_text())["system"]
    gname = {int(g["uid"]): g.get("name") for g in sysd["generator_array"]}
    name2uid = {v: k for k, v in gname.items() if v is not None}
    out: dict[int, int] = {}
    for c in sysd.get("commitment_array", []):
        ref = c.get("generator")
        guid = None
        if isinstance(ref, int) and not isinstance(ref, bool):
            guid = ref if ref in gname else None
        elif isinstance(ref, str):
            guid = name2uid.get(ref)
        if guid is not None:
            out[int(c["uid"])] = guid
    return out


def build_seed_csv(
    out_dir: Path,
    mip_json: Path,
    seed_csv: Path,
    seed_mode: str = "round",
    decisive_eps: float = 0.01,
) -> int:
    """Write (generator_uid, block_uid, u) from a solved reduced case.

    seed_mode:
      round    — threshold-round every status at 0.5 (full seed).
      decisive — seed ONLY the confident cells (u>1-eps → 1, u<eps → 0) and
                 DROP the fractional ones, so CPLEX decides the ambiguous
                 commitments itself with the full relaxation instead of an
                 arbitrary 0.5-threshold flip (no rounding of uncertain units).
    """
    status_dir = out_dir / "Commitment" / "status_sol.parquet"
    if not status_dir.exists():
        return 0
    df = pads.dataset(status_dir).to_table(
        columns=["block", "uid", "value"]
    ).to_pandas()
    c2g = _commitment_to_generator(mip_json)
    df["generator_uid"] = df["uid"].map(c2g)
    df = df[df["generator_uid"].notna()].copy()
    # One row per (generator, block): a generator's commitments OR (any-on) —
    # take the max status so a fractional and a decisive share resolve high.
    grp = df.groupby(["generator_uid", "block"])["value"].max().reset_index()
    if seed_mode == "decisive":
        # keep only confident cells; drop the fractional band (eps, 1-eps)
        decisive = grp[(grp["value"] >= 1.0 - decisive_eps) | (grp["value"] <= decisive_eps)]
        decisive = decisive.copy()
        decisive["u"] = (decisive["value"] >= 0.5).astype(int)
        rows = decisive
    else:  # round
        grp["u"] = (grp["value"] >= 0.5).astype(int)
        rows = grp
    with seed_csv.open("w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(["generator_uid", "block_uid", "u"])
        for _, r in rows.iterrows():
            w.writerow([int(r["generator_uid"]), int(r["block"]), int(r["u"])])
    return len(rows)


def _convert_mip(datos: Path, outdir: Path, env: dict) -> Path | None:
    outdir.mkdir(parents=True, exist_ok=True)
    json_path = next(
        (
            p
            for p in sorted(outdir.glob("*.json"))
            if not p.name.endswith(".provenance.json")
            and not p.name.startswith("reduced_")
        ),
        None,
    )
    if json_path is not None:
        return json_path
    rc = run(
        [
            sys.executable,
            "-m",
            "plexos2gtopt.main",
            str(datos),
            "-o",
            str(outdir),
            "--no-check",
            "--lift-line-caps",
            CAPRICORNIO,
        ],  # NOTE: no --lp-relax → commitments keep integrality (MIP)
        cwd=SCRIPTS,
        log=outdir / "convert.log",
        env=env,
    )
    if rc != 0:
        return None
    return next(
        (
            p
            for p in sorted(outdir.glob("*.json"))
            if not p.name.endswith(".provenance.json")
            and not p.name.startswith("reduced_")
        ),
        None,
    )


def _solve_full_mip(
    gtopt: str,
    json_path: Path,
    tag: str,
    seed_csv: Path | None,
    time_limit: float,
    mipgap: float,
    skip_relaxation: bool = False,
) -> dict:
    outdir = json_path.parent
    log = outdir / f"solve_{tag}.log"
    cmd = [gtopt, json_path.name, "--set", f"output_directory=output_{tag}"]
    if seed_csv is not None:
        cmd += [
            "--set",
            "monolithic_options.mip_start.enabled=true",
            "--set",
            f"monolithic_options.mip_start.seed_solution_file={seed_csv}",
        ]
        if skip_relaxation:
            cmd += ["--set", "monolithic_options.mip_start.skip_relaxation=true"]
    else:
        cmd += ["--set", "monolithic_options.mip_start.enabled=false"]
    cmd += ["--set", f"solver_options.time_limit={time_limit:g}"]
    cmd += ["--set", f"solver_options.mip_gap={mipgap:g}"]
    cmd += ["-l", json_path.stem]
    t0 = time.time()
    rc = run(cmd, cwd=outdir, log=log)
    ticks, stime = _grep_ticks(log)
    res = {
        "tag": tag,
        "solve_rc": rc,
        "solve_secs": round(time.time() - t0, 1),
        "solver_secs": stime,
        "ticks": ticks,
        "objective": _obj_from_solution(outdir / f"output_{tag}"),
        "gap": _solution_gap(outdir / f"output_{tag}"),
        "seed": str(seed_csv) if seed_csv else None,
    }
    res.update({f"log_{k}": v for k, v in _grep_gap_status(log).items()})
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--case", default="pcp_2025-10-19")
    ap.add_argument("--work", default=str(Path.home() / "tmp" / "warmstart_exp"))
    ap.add_argument(
        "--levels",
        nargs="*",
        type=float,
        default=[0.30, 0.25, 0.20],  # reverse order, 0% dropped (under-commits)
        help="network reduction levels (bus fractions), solved in given order",
    )
    ap.add_argument("--uplift-pct", type=float, default=2.0)
    ap.add_argument("--time-limit", type=float, default=1800.0)
    ap.add_argument("--reduced-time-limit", type=float, default=400.0)
    ap.add_argument("--mip-gap", type=float, default=0.01)
    ap.add_argument(
        "--reduced-mode",
        choices=["lp", "mip"],
        default="lp",
        help="solve the reduced case as LP-relax (cheap, rounded seed) or "
        "MIP (integer commitment, better seed, dearer)",
    )
    ap.add_argument("--reduced-mip-gap", type=float, default=0.02)
    ap.add_argument(
        "--skip-relaxation",
        action="store_true",
        help="full MIP uses mip_start.skip_relaxation=true (no throwaway "
        "relaxation solve; seed injected direct, solve_fixed)",
    )
    ap.add_argument("--skip-baseline", action="store_true")
    ap.add_argument("--gtopt-bin", default=os.environ.get("GTOPT_BIN", _default_gtopt_bin()))
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
    report_path = work / f"warmstart_{d8}.json"

    env = dict(os.environ)
    env["GTOPT_LIFT_LINE_CAPS"] = CAPRICORNIO
    env.setdefault("TMPDIR", str(Path.home() / "tmp"))

    report: dict = {"case": args.case, "points": []}
    if report_path.exists():
        try:
            report = json.loads(report_path.read_text())
        except json.JSONDecodeError:
            pass
    done = {p.get("tag") for p in report["points"] if p.get("solve_rc") == 0}

    # 1. MIP-mode conversion (no --lp-relax). --------------------------------
    mip_json = _convert_mip(datos, outdir, env)
    if mip_json is None:
        _log("MIP conversion failed")
        return 1
    _log(f"MIP base: {mip_json}")

    def save() -> None:
        report_path.write_text(json.dumps(report, indent=2))

    # 2. Baseline full MIP (no seed). ---------------------------------------
    if not args.skip_baseline and "baseline" not in done:
        _log("baseline: full MIP, no seed")
        r = _solve_full_mip(
            args.gtopt_bin, mip_json, "baseline", None, args.time_limit, args.mip_gap
        )
        report["points"].append(r)
        save()

    # 3. Warm-start levels (reverse order given by --levels). ----------------
    for ratio in args.levels:
        tag = f"ws{int(round(ratio * 100)):03d}"
        if tag in done:
            _log(f"skip {tag}: already done")
            continue
        red_json = outdir / f"reduced_{tag}.json"
        _log(f"=== level {ratio:.0%} ({tag}) ===")
        # a. reduce
        rc = run(
            [
                sys.executable,
                "-m",
                "gtopt_reduce_network.main",
                "reduce",
                str(mip_json),
                "--bus-ratio",
                f"{ratio:g}",
                "--partition",
                "louvain-mincut",
                "--transport-only",
                "--loss-mode",
                "gen-lossfactor",
                "--loss-uplift-pct",
                f"{args.uplift_pct:g}",
                "-o",
                str(red_json),
            ],
            cwd=SCRIPTS,
            log=outdir / f"reduce_{tag}.log",
            env=env,
        )
        if rc != 0 or not red_json.exists():
            report["points"].append({"tag": tag, "reduce_rc": rc, "solve_rc": 1})
            save()
            continue
        # b. solve reduced case → commitment.  LP-relax (--no-mip, cheap,
        #    rounded seed) or MIP (integer commitment, better seed, dearer).
        red_out = f"output_red_{tag}"
        red_cmd = [args.gtopt_bin, red_json.name]
        if args.reduced_mode == "lp":
            red_cmd += ["--no-mip", "--no-scale", "--set", "solver_options.crossover=none"]
        else:  # mip
            red_cmd += ["--set", f"solver_options.mip_gap={args.reduced_mip_gap:g}"]
        red_cmd += [
            "--set",
            f"solver_options.time_limit={args.reduced_time_limit:g}",
            "--set",
            f"output_directory={red_out}",
            "-l",
            red_json.stem,
        ]
        t0 = time.time()
        rc = run(red_cmd, cwd=outdir, log=outdir / f"solve_red_{tag}.log")
        red_secs = round(time.time() - t0, 1)
        red_ticks, _ = _grep_ticks(outdir / f"solve_red_{tag}.log")
        # c. seed CSV
        seed_csv = outdir / f"seed_{tag}.csv"
        n_seed = build_seed_csv(outdir / red_out, mip_json, seed_csv)
        _log(
            f"{tag}: reduced {args.reduced_mode.upper()} {red_secs}s "
            f"({red_ticks} ticks), seed rows={n_seed}"
        )
        # d. full MIP with seed
        r = _solve_full_mip(
            args.gtopt_bin,
            mip_json,
            tag,
            seed_csv,
            args.time_limit,
            args.mip_gap,
            skip_relaxation=args.skip_relaxation,
        )
        r.update(
            {
                "ratio": ratio,
                "reduced_secs": red_secs,
                "reduced_ticks": red_ticks,
                "seed_rows": n_seed,
            }
        )
        report["points"].append(r)
        save()

    # 4. Summary — ticks are the deterministic cost metric (contention-proof).
    base = next((p for p in report["points"] if p.get("tag") == "baseline"), {})
    bticks = base.get("ticks")
    print("\n==== WARM-START SUMMARY (ticks = deterministic MIP cost) ====")
    print(
        f"{'point':10s} {'red_tk':>9s} {'mip_ticks':>11s} {'tick_spd':>8s} "
        f"{'mip_s':>6s} {'gap':>8s} {'objective':>15s} {'seed':>7s}"
    )
    for p in report["points"]:
        mt = p.get("ticks")
        spd = (
            f"{bticks / mt:.2f}x"
            if (bticks and mt and p.get("tag") != "baseline")
            else ("1.00x" if p.get("tag") == "baseline" else "-")
        )
        g = p.get("gap")
        print(
            f"{p.get('tag',''):10s} "
            f"{(p.get('reduced_ticks') or 0):9.0f} "
            f"{(mt if mt is not None else float('nan')):11.0f} {spd:>8s} "
            f"{(p.get('solve_secs') or float('nan')):6.0f} "
            f"{(g if g is not None else float('nan')):8.4f} "
            f"{(p.get('objective') or float('nan')):15.6g} "
            f"{(p.get('seed_rows') or 0):7d}"
        )
    print(f"\nreport: {report_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
