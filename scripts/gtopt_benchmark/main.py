#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Mini machine benchmark to CALIBRATE cross-host solve comparisons.

Motivation: CPLEX deterministic ticks are machine-independent work units,
so ``ticks / second`` on a FIXED reference solve is a direct speed score
for LP/MIP workloads — two hosts' wall times only compare meaningfully
after dividing by their scores.  When no solver/binary is available the
numpy kernels still give a rough FP/memory picture.

Usage:
    python -m gtopt_benchmark.main [--gtopt BIN] [--case JSON] [--json]
Score fields:
    cplex_kticks_per_s   work rate on the reference LP (the primary score)
    dgemm_gflops         dense FP throughput (numpy, 1024x1024 matmul)
    triad_gbps           memory bandwidth (numpy triad, 64M doubles)
    pyloop_mops          single-thread interpreter rate (proxy scalar work)
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import tempfile
import time
from pathlib import Path

import numpy as np

_EFFORT_RE = re.compile(r"GTOPT_SOLVE_EFFORT solver_time=([0-9.]+) s ticks=([0-9.]+)")


def _dgemm_gflops(n: int = 1024, reps: int = 3) -> float:
    a = np.random.default_rng(7).random((n, n))
    b = np.random.default_rng(8).random((n, n))
    _ = a @ b  # warm-up
    t0 = time.perf_counter()
    for _ in range(reps):
        a @ b
    dt = time.perf_counter() - t0
    return 2.0 * n**3 * reps / dt / 1e9


def _triad_gbps(n: int = 64_000_000, reps: int = 3) -> float:
    a = np.ones(n)
    b = np.ones(n)
    t0 = time.perf_counter()
    for _ in range(reps):
        a = b + 0.5 * a
    dt = time.perf_counter() - t0
    return 3 * 8 * n * reps / dt / 1e9


def _pyloop_mops(n: int = 3_000_000) -> float:
    t0 = time.perf_counter()
    s = 0
    for i in range(n):
        s += i & 7
    dt = time.perf_counter() - t0
    return n / dt / 1e6 if s >= 0 else 0.0


def _cplex_kticks_per_s(gtopt: str, case: str) -> float | None:
    """ticks/s on a fixed reference solve (machine-independent numerator)."""
    with tempfile.TemporaryDirectory() as td:
        try:
            proc = subprocess.run(
                [
                    gtopt,
                    case,
                    "--solver",
                    "cplex",
                    "--set",
                    f"output_directory={td}/out",
                ],
                capture_output=True,
                text=True,
                timeout=300,
                check=False,
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
    m = _EFFORT_RE.search(proc.stderr + proc.stdout)
    if not m:
        return None
    secs, ticks = float(m.group(1)), float(m.group(2))
    if ticks < 100.0:
        # A trivial case yields ~1 tick — no resolution for calibration.
        # Use a mid-size case (thousands of ticks) shared across hosts.
        return None
    return ticks / secs / 1e3 if secs > 0 else None


def machine_bench(gtopt: str | None = None, case: str | None = None) -> dict:
    """Run the kernels (+ the solver score when a binary/case is given)."""
    out: dict[str, float | str | None] = {
        "host": os.uname().nodename,
        "cpus": float(os.cpu_count() or 0),
        "dgemm_gflops": round(_dgemm_gflops(), 1),
        "triad_gbps": round(_triad_gbps(), 1),
        "pyloop_mops": round(_pyloop_mops(), 1),
        "cplex_kticks_per_s": None,
    }
    gtopt = gtopt or shutil.which("gtopt")
    if gtopt and case and Path(case).exists():
        score = _cplex_kticks_per_s(gtopt, case)
        out["cplex_kticks_per_s"] = round(score, 2) if score else None
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gtopt", help="gtopt binary (default: PATH)")
    ap.add_argument(
        "--case",
        help="small reference case JSON for the CPLEX ticks/s score "
        "(e.g. cases/ieee_9b/ieee_9b.json — use the SAME case on every "
        "host you want to compare)",
    )
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    res = machine_bench(args.gtopt, args.case)
    if args.json:
        print(json.dumps(res))
    else:
        for k, v in res.items():
            print(f"{k:22s} {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
