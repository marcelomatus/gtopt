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


def process_case(
    case_dir: Path, work: Path, gtopt_bin: str, *, lp_relax: bool = True
) -> dict:
    name = case_dir.name
    d8 = name.replace("pcp_", "").replace("-", "")
    datos = case_dir / f"DATOS{d8}.zip.xz"
    res: dict = {"case": name, "d8": d8, "mode": "lp-relax" if lp_relax else "mip"}
    if not datos.exists():
        res["error"] = f"missing {datos}"
        return res

    outdir = work / f"gtopt_{'relax' if lp_relax else 'mip'}_PLEXOS{d8}"
    outdir.mkdir(parents=True, exist_ok=True)
    _log(f"=== {name} ({res['mode']}) ===")

    env = dict(os.environ)
    env["GTOPT_LIFT_LINE_CAPS"] = CAPRICORNIO
    env.setdefault("TMPDIR", str(Path.home() / "tmp"))

    # 1. re-convert with Capricornio lift; --lp-relax only in LP-relax mode
    #    (MIP keeps commitment integrality).
    convert_cmd = [
        sys.executable,
        "-m",
        "plexos2gtopt.main",
        str(datos),
        "-o",
        str(outdir),
        "--no-check",
        "--lift-line-caps",
        CAPRICORNIO,
    ]
    if lp_relax:
        convert_cmd.insert(convert_cmd.index("--no-check") + 1, "--lp-relax")
    res["convert_rc"] = run(
        convert_cmd,
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

    # 2. SOLVE (default CPLEX).  Run from outdir: JSON references the
    #    uc_*.pampl files by basename.  LP-relax mode adds ``--no-scale`` for
    #    raw-coefficient / kappa diagnostics; MIP mode solves the production
    #    LP WITH scaling (the FCF alpha + loss-link conditioning needs it).
    t0 = time.time()
    solve_log = outdir / "solve.log"
    solve_cmd = [gtopt_bin, json_path.name]
    if lp_relax:
        solve_cmd.append("--no-scale")
        # Barrier WITHOUT crossover for the LP-relax main solve — the whole
        # point of the relax loop.  Maps to CPLEX SolutionType=NONBASIC (keep
        # the interior point), so it genuinely skips crossover.  NOT set for
        # MIP: branch & cut needs a basis at the root, so we let CPLEX cross
        # over there (the dual-recovery pass also crosses over on its own).
        solve_cmd += ["--set", "solver_options.crossover=none"]
    solve_cmd += ["-l", json_path.stem]
    res["solve_rc"] = run(solve_cmd, cwd=outdir, log=solve_log)
    res["solve_secs"] = round(time.time() - t0, 1)
    res["objective"] = _grep_objective(solve_log)
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=None)
    ap.add_argument("--cases", nargs="*")
    ap.add_argument(
        "--mip",
        action="store_true",
        help="MIP mode: convert WITHOUT --lp-relax (keep commitment "
        "integrality) and solve the full MIP.  Default is LP-relax.",
    )
    ap.add_argument(
        "--gtopt-bin",
        default=os.environ.get(
            "GTOPT_BIN",
            str(Path.home() / "tmp" / "gtopt-build-ucaudit" / "standalone" / "gtopt"),
        ),
    )
    args = ap.parse_args()

    lp_relax = not args.mip
    default_work = "lp_relax_loop" if lp_relax else "mip_loop"
    work = Path(args.work) if args.work else (Path.home() / "tmp" / default_work)
    work.mkdir(parents=True, exist_ok=True)
    report_name = "lp_relax_report.json" if lp_relax else "mip_report.json"
    by_name = {d.name: d for d in SUPPORT.glob("pcp_*") if d.is_dir()}
    if args.cases:
        # Preserve the caller's order (e.g. fastest-MIP-first) — do NOT
        # re-sort, so the loop solves quick cases first and slow ones last.
        cases = [by_name[n] for n in args.cases if n in by_name]
    else:
        cases = [by_name[n] for n in sorted(by_name)]

    report: list[dict] = []
    for case_dir in cases:
        try:
            report.append(
                process_case(case_dir, work, args.gtopt_bin, lp_relax=lp_relax)
            )
        except Exception as exc:  # noqa: BLE001
            report.append({"case": case_dir.name, "error": repr(exc)})
        (work / report_name).write_text(json.dumps(report, indent=2))

    print(f"\n==== {'LP-RELAX' if lp_relax else 'MIP'} LOOP SUMMARY ====")
    for r in report:
        print(
            f"{r['case']:18s} conv={r.get('convert_rc')} solve={r.get('solve_rc')} "
            f"lift=[{r.get('capricornio_lift')}] "
            f"obj={r.get('objective')} {r.get('solve_secs')}s {r.get('error', '')}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
