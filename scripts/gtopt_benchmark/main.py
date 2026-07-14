#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Mini machine benchmark to CALIBRATE cross-host solve comparisons.

Motivation: CPLEX deterministic ticks are machine-independent work units,
so ``ticks / second`` on a FIXED reference solve is a direct speed score
for LP/MIP workloads — two hosts' wall times only compare meaningfully
after dividing by their scores.  When no solver/binary is available the
numpy kernels still give a rough FP/memory picture.

The reference solve is a self-contained synthetic unit-commitment MIP
(``synthetic_case``): a mono-bus system with a merit-order generator fleet,
per-unit commitment (min up/down, start-up + no-load cost) and a cyclic
demand profile.  It is fully deterministic (no RNG, integer/rational
coefficients) so every host solves byte-identical work and reports the
SAME deterministic tick count — only the wall/solver time differs.

Usage:
    python -m gtopt_benchmark.main [--gtopt BIN] [--case JSON] [--json]
    python -m gtopt_benchmark.main --emit-case bench.json [--bench-scale S]
Score fields:
    cplex_kticks_per_s   work rate on the reference solve (the primary score)
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
from typing import Any

import numpy as np

_EFFORT_RE = re.compile(r"GTOPT_SOLVE_EFFORT solver_time=([0-9.]+) s ticks=([0-9.]+)")

# Base size of the synthetic reference MIP.  Calibrated so a single
# deterministic-thread CPLEX solve does ~23k deterministic ticks in ~20-30 s on
# a contemporary desktop core — enough tick resolution for a stable ticks/s
# ratio, while staying well under the solve timeout on much slower hosts (the
# deterministic B&B tree is identical everywhere, so wall time scales linearly
# with core speed).  Grow/shrink with --bench-scale.
_BASE_GENS = 140
_BASE_BLOCKS = 168

# CPLEX defaults to OPPORTUNISTIC parallel, whose tick count varies run-to-run
# and with core count — useless as a machine-independent numerator.  Pinning a
# single DETERMINISTIC thread makes the tick count byte-identical on every host
# (only the wall/solver time then differs, which is exactly the speed signal).
# Written next to the case and loaded via ``solver_options.param_file``.
_DET_CPLEX_PRM = (
    "CPLEX Parameter File Version 22.1.1.0\n"
    "CPXPARAM_Threads                                 1\n"
    "CPXPARAM_Parallel                                1\n"
)


def _dgemm_gflops(n: int = 1024, reps: int = 3) -> float:
    a = np.random.default_rng(7).random((n, n))
    b = np.random.default_rng(8).random((n, n))
    _ = a @ b  # warm-up
    t0 = time.perf_counter()
    acc = 0.0
    for _ in range(reps):
        acc += float((a @ b)[0, 0])
    dt = time.perf_counter() - t0
    assert acc != 0.0  # keep the matmul observable
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


def synthetic_case(scale: float = 1.0) -> dict[str, Any]:
    """Build a self-contained, deterministic unit-commitment MIP.

    Mono-bus system, ``n_gens`` thermal units in merit order, each with a
    commitment record (min up/down time, start-up and no-load cost, minimum
    stable output).  A cyclic (daily triangle-wave) demand profile forces
    units to start and stop, so the LP relaxation is fractional and the
    branch-and-bound tree does real work.  ``demand_fail_cost`` acts as a
    high-cost safety valve, guaranteeing the model is always feasible and
    terminates ``optimal``.

    Everything is a deterministic function of the unit index and block, with
    integer/short-decimal coefficients — no RNG, no time/order dependence —
    so the CPLEX deterministic tick count is identical on every host.

    ``scale`` multiplies both the fleet size and the horizon (>=1 grows the
    MIP; use a small scale in unit tests for a fast parse check).
    """
    n_gens = max(3, round(_BASE_GENS * scale))
    n_blocks = max(6, round(_BASE_BLOCKS * scale))

    block_array = [{"uid": b + 1, "duration": 1} for b in range(n_blocks)]
    stage_array = [
        {
            "uid": 1,
            "first_block": 0,
            "count_block": n_blocks,
            "active": 1,
            "chronological": True,
        }
    ]
    scenario_array = [{"uid": 1, "probability_factor": 1}]

    generator_array: list[dict[str, Any]] = []
    commitment_array: list[dict[str, Any]] = []
    total_pmax = 0
    for i in range(n_gens):
        pmax = 40 + (i % 12) * 8  # 40..128, deterministic spread
        total_pmax += pmax
        # Cost bands cluster many units at close (but distinct) marginal
        # costs -> a hard-to-order merit stack that forces branching.
        gcost = round(8.0 + (i % 25) * 1.3, 3)
        generator_array.append(
            {
                "uid": i + 1,
                "name": f"g{i + 1}",
                "bus": "b1",
                "pmin": 0,
                "pmax": pmax,
                "gcost": gcost,
                "capacity": pmax,
            }
        )
        commitment_array.append(
            {
                "uid": i + 1,
                "name": f"g{i + 1}_uc",
                "generator": f"g{i + 1}",
                "pmin": round(0.45 * pmax, 1),  # meaningful minimum stable output
                "noload_cost": 8 + (i % 6) * 4,
                "startup_cost": 150 + (i % 20) * 25,
                "min_up_time": 2 + (i % 5),  # 2..6
                "min_down_time": 2 + (i % 4),  # 2..5
                "initial_status": i % 2,
                "initial_hours": 4,
            }
        )

    # Cyclic daily demand: triangle wave in [trough, peak] over a 24-block
    # period.  Peak below total capacity so the model is always feasible;
    # trough low enough that a large fraction of the fleet must cycle off.
    peak = round(0.72 * total_pmax, 1)
    trough = round(0.35 * total_pmax, 1)
    profile: list[float] = []
    for b in range(n_blocks):
        hour = b % 24
        tent = 1.0 - abs((2.0 * hour / 24.0) - 1.0)  # 0 -> 1 -> 0 over the day
        profile.append(round(trough + (peak - trough) * tent, 1))
    demand_array = [{"uid": 1, "name": "d1", "bus": "b1", "lmax": [profile]}]

    return {
        "options": {
            "annual_discount_rate": 0.0,
            "output_format": "csv",
            "output_compression": "uncompressed",
            "model_options": {
                "use_single_bus": True,
                "scale_objective": 1000,
                "demand_fail_cost": 1000,
            },
        },
        "simulation": {
            "block_array": block_array,
            "stage_array": stage_array,
            "scenario_array": scenario_array,
        },
        "system": {
            "name": "gtopt_bench_synth",
            "bus_array": [{"uid": 1, "name": "b1"}],
            "generator_array": generator_array,
            "demand_array": demand_array,
            "commitment_array": commitment_array,
        },
    }


def _cplex_kticks_per_s(
    gtopt: str, case: str, param_file: str | None = None
) -> float | None:
    """ticks/s on a fixed reference solve (machine-independent numerator).

    ``param_file`` should point at the deterministic single-thread CPLEX prm
    (see ``_DET_CPLEX_PRM``) so the tick count is identical on every host
    regardless of core count; opportunistic parallel would make ticks vary
    run-to-run and host-to-host.
    """
    with tempfile.TemporaryDirectory() as td:
        cmd = [gtopt, case, "--solver", "cplex", "--set", f"output_directory={td}/out"]
        if param_file:
            cmd += ["--set", f"solver_options.param_file={param_file}"]
        try:
            proc = subprocess.run(
                cmd, capture_output=True, text=True, timeout=900, check=False
            )
        except (OSError, subprocess.TimeoutExpired):
            return None
    m = _EFFORT_RE.search(proc.stderr + proc.stdout)
    if not m:
        return None
    secs, ticks = float(m.group(1)), float(m.group(2))
    if ticks < 2000.0 or secs < 10.0:
        # Small solves measure CPLEX/setup OVERHEAD, not throughput — the
        # score is only meaningful when pure solve work dominates.  Use a
        # mid-size case (thousands of ticks, tens of seconds) shared across
        # hosts, or compute ticks/solver_time from a real run's
        # GTOPT_SOLVE_EFFORT log line instead.
        return None
    return ticks / secs / 1e3 if secs > 0 else None


def machine_bench(
    gtopt: str | None = None, case: str | None = None, scale: float = 1.0
) -> dict:
    """Run the kernels (+ the solver score when a binary is available).

    With no ``--case`` the self-contained ``synthetic_case`` reference MIP is
    generated and solved, so the primary score needs only a gtopt binary.
    """
    out: dict[str, float | str | None] = {
        "host": os.uname().nodename,
        "cpus": float(os.cpu_count() or 0),
        "dgemm_gflops": round(_dgemm_gflops(), 1),
        "triad_gbps": round(_triad_gbps(), 1),
        "pyloop_mops": round(_pyloop_mops(), 1),
        "cplex_kticks_per_s": None,
    }
    gtopt = gtopt or shutil.which("gtopt")
    if gtopt:
        with tempfile.TemporaryDirectory() as td:
            prm_path = str(Path(td) / "cplex_det.prm")
            Path(prm_path).write_text(_DET_CPLEX_PRM, encoding="utf-8")
            if case and Path(case).exists():
                case_path = case
            else:
                case_path = str(Path(td) / "bench_synth.json")
                Path(case_path).write_text(
                    json.dumps(synthetic_case(scale)), encoding="utf-8"
                )
            score = _cplex_kticks_per_s(gtopt, case_path, prm_path)
        out["cplex_kticks_per_s"] = round(score, 3) if score else None
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--gtopt", help="gtopt binary (default: PATH)")
    ap.add_argument(
        "--case",
        help="reference case JSON for the CPLEX ticks/s score; defaults to "
        "the built-in synthetic UC MIP (use the SAME case/scale on every "
        "host you want to compare)",
    )
    ap.add_argument(
        "--bench-scale",
        type=float,
        default=1.0,
        help="size multiplier for the synthetic reference MIP (default 1.0)",
    )
    ap.add_argument(
        "--emit-case",
        metavar="PATH",
        help="write the synthetic reference case JSON to PATH and exit",
    )
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    if args.emit_case:
        Path(args.emit_case).write_text(
            json.dumps(synthetic_case(args.bench_scale), indent=2), encoding="utf-8"
        )
        print(f"wrote synthetic reference case to {args.emit_case}")
        return 0

    res = machine_bench(args.gtopt, args.case, args.bench_scale)
    if args.json:
        print(json.dumps(res))
    else:
        for k, v in res.items():
            print(f"{k:22s} {v}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
