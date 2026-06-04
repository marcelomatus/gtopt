#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""LP-Relax solve loop over the support/plexos daily PCP cases.

Per case:
  1. re-convert DATOS*.zip.xz with ``--lp-relax`` (commitments relaxed)
     AND the Capricornio cap-lift.  The line demoter in
     ``parsers.extract_lines`` reads ``GTOPT_LIFT_LINE_CAPS`` from the
     environment, so we set BOTH the env var and the CLI flag to be
     safe (the radial 110 kV stepdown ``Capricornio110->LaNegra110``
     that PLEXOS dispatches ~270% over its published rating → demote
     EL=1 hard cap to EL=0).
  2. assemble + write the ``--no-scale`` LP (``-l <stem>``) and SOLVE
     it (no ``--lp-only``) with the default backend (CPLEX).

Captures convert / solve exit codes, objective, and wallclock into a
report.  Reuses the audit round's plexos_cache_plain if present.
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
SUPPORT = REPO / "support" / "plexos"
SCRIPTS = REPO / "scripts"

CAPRICORNIO = "Capricornio110->LaNegra110"


def _log(msg: str) -> None:
    print(f"[lprelax] {msg}", flush=True)


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
    val = None
    for line in log.read_text(errors="ignore").splitlines():
        m = pat.search(line)
        if m:
            try:
                val = float(m.group(1))
            except ValueError:
                continue
    return val


def process_case(case_dir: Path, work: Path, gtopt_bin: str) -> dict:
    name = case_dir.name
    d8 = name.replace("pcp_", "").replace("-", "")
    datos = case_dir / f"DATOS{d8}.zip.xz"
    res: dict = {"case": name, "d8": d8}
    if not datos.exists():
        res["error"] = f"missing {datos}"
        return res

    outdir = work / f"gtopt_relax_PLEXOS{d8}"
    outdir.mkdir(parents=True, exist_ok=True)
    _log(f"=== {name} ===")

    env = dict(os.environ)
    env["GTOPT_LIFT_LINE_CAPS"] = CAPRICORNIO
    env.setdefault("TMPDIR", str(Path.home() / "tmp"))

    # 1. re-convert with lp-relax + Capricornio lift
    res["convert_rc"] = run(
        [
            sys.executable,
            "-m",
            "plexos2gtopt.main",
            str(datos),
            "-o",
            str(outdir),
            "--no-check",
            "--lp-relax",
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
    res["json"] = str(json_path) if json_path else None
    if json_path is None:
        res["error"] = "no planning JSON produced"
        return res

    # verify the Capricornio lift landed: the soft-cap band keeps EL=1 but
    # widens tmax_normal (free, 4x rating) < tmax_ab (10x hard ceiling).
    try:
        sysd = json.loads(json_path.read_text())["system"]
        ln = next((x for x in sysd["line_array"] if x["name"] == CAPRICORNIO), None)
        if ln:
            tn, tm = ln.get("tmax_normal_ab"), ln.get("tmax_ab")
            res["capricornio_lift"] = (
                f"band {tn}->{tm}" if tn and tm and tn < tm else f"NO-LIFT tmax={tm}"
            )
        else:
            res["capricornio_lift"] = "absent"
    except (json.JSONDecodeError, KeyError):
        res["capricornio_lift"] = "unparsed"

    # 2. write --no-scale LP and SOLVE (default CPLEX).  Run from outdir:
    #    JSON references the uc_*.pampl files by basename.
    t0 = time.time()
    solve_log = outdir / "solve.log"
    res["solve_rc"] = run(
        [
            gtopt_bin,
            json_path.name,
            "--no-scale",
            "-l",
            json_path.stem,
        ],
        cwd=outdir,
        log=solve_log,
    )
    res["solve_secs"] = round(time.time() - t0, 1)
    res["objective"] = _grep_objective(solve_log)
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=str(Path.home() / "tmp" / "lp_relax_loop"))
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
        (work / "lp_relax_report.json").write_text(json.dumps(report, indent=2))

    print("\n==== LP-RELAX LOOP SUMMARY ====")
    for r in report:
        print(
            f"{r['case']:18s} conv={r.get('convert_rc')} solve={r.get('solve_rc')} "
            f"lift=[{r.get('capricornio_lift')}] "
            f"obj={r.get('objective')} {r.get('solve_secs')}s {r.get('error', '')}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
