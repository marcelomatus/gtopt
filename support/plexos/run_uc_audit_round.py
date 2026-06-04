#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Drive the uc_audit round across the support/plexos daily PCP cases.

Per case:
  1. locate the sibling RES*.zip[.xz], extract the .accdb, and
     mdb-export the four solution tables uc_audit needs into a plain
     CSV cache (uc_audit reads t_object/t_membership/t_key/t_data_0).
  2. convert DATOS*.zip.xz -> gtopt JSON + uc_*.pampl (default flags).
  3. assemble the LP with `gtopt --no-scale --lp-only -l <stem>`
     (no solve).
  4. run uc_audit --plexos-cache <cache> --gtopt-dir <outdir>.

Captures each step's exit code and the audit summary into a round
report.  Does NOT run the solver.
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

CACHE_TABLES = ("t_object", "t_membership", "t_key", "t_data_0")


def _log(msg: str) -> None:
    print(f"[round] {msg}", flush=True)


def extract_cache(case_dir: Path, d8: str, cache_dir: Path) -> bool:
    """Extract the PLEXOS solution accdb and dump the 4 tables as CSV."""
    sys.path.insert(0, str(SCRIPTS))
    from plexos2gtopt.plexos_block_layout import (  # noqa: E402
        auto_discover_res_zip,
        extract_accdb_from_res_zip,
    )

    res_zip = auto_discover_res_zip(case_dir / f"DATOS{d8}.zip.xz")
    if res_zip is None:
        # fall back to explicit name
        cand = case_dir / f"RES{d8}.zip.xz"
        res_zip = cand if cand.exists() else None
    if res_zip is None:
        _log(f"  ! no RES zip found in {case_dir}")
        return False
    _log(f"  extracting accdb from {res_zip.name}")
    accdb = extract_accdb_from_res_zip(res_zip)
    if accdb is None:
        _log("  ! accdb extraction failed")
        return False
    cache_dir.mkdir(parents=True, exist_ok=True)
    for table in CACHE_TABLES:
        out = cache_dir / f"{table}.csv"
        if out.exists() and out.stat().st_size > 0:
            continue
        with out.open("wb") as fh:
            r = subprocess.run(
                ["mdb-export", str(accdb), table], stdout=fh, check=False
            )
        if r.returncode != 0 or out.stat().st_size == 0:
            _log(f"  ! mdb-export failed for {table}")
            return False
    return True


def run(cmd: list[str], cwd: Path | None = None, log: Path | None = None) -> int:
    _log("  $ " + " ".join(str(c) for c in cmd))
    if log is not None:
        with log.open("w") as fh:
            return subprocess.run(
                cmd, cwd=cwd, stdout=fh, stderr=subprocess.STDOUT, check=False
            ).returncode
    return subprocess.run(cmd, cwd=cwd, check=False).returncode


def process_case(case_dir: Path, work: Path, gtopt_bin: str) -> dict:
    name = case_dir.name  # pcp_YYYY-MM-DD
    d8 = name.replace("pcp_", "").replace("-", "")
    datos = case_dir / f"DATOS{d8}.zip.xz"
    res = {"case": name, "d8": d8}
    if not datos.exists():
        res["error"] = f"missing {datos}"
        return res

    outdir = work / f"gtopt_PLEXOS{d8}"
    outdir.mkdir(parents=True, exist_ok=True)
    cache_dir = outdir / "plexos_cache_plain"

    _log(f"=== {name} (D8={d8}) ===")

    # 1. PLEXOS solution cache
    res["cache_ok"] = extract_cache(case_dir, d8, cache_dir)
    if not res["cache_ok"]:
        return res

    # 2. convert
    conv_log = outdir / "convert.log"
    res["convert_rc"] = run(
        [
            sys.executable,
            "-m",
            "plexos2gtopt.main",
            str(datos),
            "-o",
            str(outdir),
            "--no-check",
        ],
        cwd=SCRIPTS,
        log=conv_log,
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

    # 3. LP assembly (no solve).  PAMPL files are referenced by basename
    #    in the JSON, so gtopt must run from the output dir.
    lp_log = outdir / "lp.log"
    res["lp_rc"] = run(
        [
            gtopt_bin,
            json_path.name,
            "--no-scale",
            "--lp-only",
            "-l",
            json_path.stem,
        ],
        cwd=outdir,
        log=lp_log,
    )

    # 4. uc_audit
    audit_json = outdir / "audit.json"
    audit_log = outdir / "audit.log"
    res["audit_rc"] = run(
        [
            sys.executable,
            "-m",
            "plexos2gtopt.uc_audit",
            "--plexos-cache",
            str(cache_dir),
            "--gtopt-dir",
            str(outdir),
            "--output",
            str(audit_json),
            "--strict",
        ],
        cwd=SCRIPTS,
        log=audit_log,
    )
    if audit_json.exists():
        try:
            data = json.loads(audit_json.read_text())
            res["summary"] = data.get("summary", {}).get("bucket_counts", {})
            res["audit_json"] = str(audit_json)
        except json.JSONDecodeError:
            res["error"] = "audit.json unparseable"
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=str(Path.home() / "tmp" / "uc_audit_round"))
    ap.add_argument("--cases", nargs="*", help="restrict to these pcp_* names")
    ap.add_argument("--gtopt-bin", default=os.environ.get("GTOPT_BIN", "gtopt"))
    args = ap.parse_args()

    work = Path(args.work)
    work.mkdir(parents=True, exist_ok=True)

    cases = sorted(d for d in SUPPORT.glob("pcp_*") if d.is_dir())
    if args.cases:
        wanted = set(args.cases)
        cases = [c for c in cases if c.name in wanted]

    report = []
    for case_dir in cases:
        try:
            report.append(process_case(case_dir, work, args.gtopt_bin))
        except Exception as exc:  # noqa: BLE001
            report.append({"case": case_dir.name, "error": repr(exc)})
        (work / "round_report.json").write_text(json.dumps(report, indent=2))

    print("\n==== ROUND SUMMARY ====")
    for r in report:
        tag = "PASS" if r.get("audit_rc") == 0 else "FAIL"
        b2 = r.get("summary", {}).get("B2_rhs_mismatch", 0)
        b5 = r.get("summary", {}).get("B5_hard_in_plexos_soft_in_gtopt", 0)
        print(
            f"{tag}  {r['case']:18s} audit_rc={r.get('audit_rc')} "
            f"conv={r.get('convert_rc')} lp={r.get('lp_rc')} "
            f"B2={b2} B5={b5} {r.get('error', '')}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
