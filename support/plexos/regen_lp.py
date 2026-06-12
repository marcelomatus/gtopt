#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Regenerate the --no-scale LP files for every support/plexos PCP case.

NO LP-relax (MIP model, as originally analysed).  Per case:
  1. re-convert DATOS*.zip.xz with the Capricornio cap-lift (env + flag;
     the demoter in parsers.extract_lines reads ``GTOPT_LIFT_LINE_CAPS``).
     Picks up the P0 degenerate-pmax floor + P1 decision-variable big-M
     tightening now baked into the converter.
  2. assemble + write the ``--no-scale`` LP (``gtopt --no-scale --lp-only
     -l <stem>``) — build only, no solve.

Then optionally run support/plexos/analyze_lp_numerics.py over the result.
"""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SUPPORT = REPO / "support" / "plexos"
SCRIPTS = REPO / "scripts"
CAPRICORNIO = "Capricornio110->LaNegra110"


def _log(msg: str) -> None:
    print(f"[regen] {msg}", flush=True)


def run(cmd: list[str], cwd: Path | None, log: Path, env: dict | None = None) -> int:
    _log("  $ " + " ".join(str(c) for c in cmd))
    with log.open("w") as fh:
        return subprocess.run(
            cmd, cwd=cwd, stdout=fh, stderr=subprocess.STDOUT, check=False, env=env
        ).returncode


def process_case(case_dir: Path, work: Path, gtopt_bin: str) -> dict:
    name = case_dir.name
    d8 = name.replace("pcp_", "").replace("-", "")
    datos = case_dir / f"DATOS{d8}.zip.xz"
    res: dict = {"case": name, "d8": d8}
    if not datos.exists():
        res["error"] = f"missing {datos}"
        return res
    outdir = work / f"gtopt_PLEXOS{d8}"
    outdir.mkdir(parents=True, exist_ok=True)
    _log(f"=== {name} ===")

    env = dict(os.environ)
    env["GTOPT_LIFT_LINE_CAPS"] = CAPRICORNIO
    env.setdefault("TMPDIR", str(Path.home() / "tmp"))

    res["convert_rc"] = run(
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
        ],
        cwd=SCRIPTS,
        log=outdir / "convert.log",
        env=env,
    )
    json_path = next(
        (
            p
            for p in sorted(outdir.glob("*.json"))
            if not p.name.endswith(".provenance.json")
        ),
        None,
    )
    if json_path is None:
        res["error"] = "no planning JSON produced"
        return res

    # spot-check the two kappa fixes landed
    try:
        sysd = json.loads(json_path.read_text())["system"]
        sr = next(
            (g for g in sysd["generator_array"] if g.get("name") == "SANTA_ROSA"), None
        )
        res["santa_rosa_pmax"] = sr.get("pmax") if sr else "absent"
    except (json.JSONDecodeError, KeyError):
        pass

    res["lp_rc"] = run(
        [gtopt_bin, json_path.name, "--no-scale", "--lp-only", "-l", json_path.stem],
        cwd=outdir,
        log=outdir / "lp.log",
    )
    lp = outdir / f"{json_path.stem}.lp"
    res["lp_file"] = str(lp) if lp.exists() else None
    res["lp_mb"] = round(lp.stat().st_size / 1e6, 1) if lp.exists() else None
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=str(Path.home() / "tmp" / "lp_regen"))
    ap.add_argument("--cases", nargs="*")
    ap.add_argument(
        "--gtopt-bin",
        default=os.environ.get(
            "GTOPT_BIN",
            str(Path.home() / "tmp" / "gtopt-build-ucaudit" / "standalone" / "gtopt"),
        ),
    )
    args = ap.parse_args()
    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    cases = sorted(d for d in SUPPORT.glob("pcp_*") if d.is_dir())
    if args.cases:
        wanted = set(args.cases)
        cases = [c for c in cases if c.name in wanted]

    report: list[dict] = []
    for case_dir in cases:
        try:
            report.append(process_case(case_dir, work, args.gtopt_bin))
        except Exception as exc:  # noqa: BLE001
            report.append({"case": case_dir.name, "error": repr(exc)})
        (work / "regen_report.json").write_text(json.dumps(report, indent=2))

    print("\n==== REGEN SUMMARY ====")
    for r in report:
        print(
            f"{r['case']:18s} conv={r.get('convert_rc')} lp={r.get('lp_rc')} "
            f"SR_pmax={r.get('santa_rosa_pmax')} {r.get('lp_mb')}MB "
            f"{r.get('error', '')}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
