# SPDX-License-Identifier: BSD-3-Clause
"""Solver benchmark: solve an LP file with every available solver/algo/thread combination."""

from __future__ import annotations

import logging
import re
import shutil
import subprocess
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

from . import _colors as col
from ._compress import as_plain_lp
from ._solvers import find_cplex_binary

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Result data
# ---------------------------------------------------------------------------


@dataclass
class BenchmarkResult:
    """Timing result for a single solver/algo/thread combination."""

    solver: str
    algorithm: str
    threads: int
    time_s: float
    status: str
    objective: str
    rows: int = 0
    cols: int = 0


@dataclass
class BenchmarkReport:
    """Collection of benchmark results with LP metadata."""

    lp_path: Path
    results: list[BenchmarkResult] = field(default_factory=list)


# ---------------------------------------------------------------------------
# Solver-specific runners
# ---------------------------------------------------------------------------


def _parse_time(output: str) -> float:
    """Extract solve time (seconds) from solver output, or return -1."""
    # CPLEX: "Solution time =    0.12 sec."
    m = re.search(r"Solution time\s*=\s*([\d.]+)\s*sec", output)
    if m:
        return float(m.group(1))
    # HiGHS: "HiGHS run time      :          0.05"
    m = re.search(r"HiGHS run time\s*:\s*([\d.eE+-]+)", output)
    if m:
        return float(m.group(1))
    # CLP: "Total time (CPU seconds):       0.02"
    m = re.search(r"Total time \(CPU seconds\):\s*([\d.]+)", output)
    if m:
        return float(m.group(1))
    return -1.0


def _parse_objective(output: str) -> str:
    """Extract objective value from solver output."""
    # HiGHS: "Objective value     :  1.0000000000e+01"
    m = re.search(r"Objective value\s*:\s*([-+\d.eE]+)", output)
    if m:
        return m.group(1)
    # CPLEX: "Objective =  1.2345e+06" or "objectiveValue = 1.23"
    m = re.search(r"[Oo]bjective\s*(?:value)?\s*=\s*([-+\d.eE]+)", output)
    if m:
        return m.group(1)
    # CLP: "Objective value:  10" or "Optimal objective 10"
    m = re.search(r"[Oo]bjective\s+([-+\d.eE]+)", output)
    if m:
        return m.group(1)
    return "N/A"


def _parse_status(output: str) -> str:
    """Extract solution status from solver output."""
    # HiGHS: "Model status        : Optimal"
    m = re.search(r"Model\s+status\s*:\s*(\S+)", output)
    if m:
        return m.group(1).lower()
    # CPLEX/CLP: look for keywords
    if re.search(r"\boptimal\b", output, re.IGNORECASE):
        return "optimal"
    if re.search(r"\binfeasible\b", output, re.IGNORECASE):
        return "infeasible"
    if re.search(r"\bunbounded\b", output, re.IGNORECASE):
        return "unbounded"
    return "unknown"


def _parse_dimensions(output: str) -> tuple[int, int]:
    """Extract (rows, cols) from solver output."""
    rows, cols = 0, 0
    # HiGHS: "LP problem has 3 rows; 2 cols; 5 nonzeros"
    m = re.search(r"has\s+(\d+)\s+rows;\s+(\d+)\s+cols", output)
    if m:
        return int(m.group(1)), int(m.group(2))
    # CPLEX/CLP generic
    m = re.search(r"(\d+)\s+rows", output, re.IGNORECASE)
    if m:
        rows = int(m.group(1))
    m = re.search(r"(\d+)\s+col(?:umn)?s", output, re.IGNORECASE)
    if m:
        cols = int(m.group(1))
    return rows, cols


def _run_cplex(
    lp_path: Path,
    algo: str,
    threads: int,
    timeout: int,
) -> BenchmarkResult | None:
    """Solve with CPLEX and return timing."""
    cplex_bin = find_cplex_binary()
    if cplex_bin is None:
        return None

    method_map = {"dual": 2, "barrier": 4, "primal": 1, "default": 0}
    method = method_map.get(algo, 0)

    script = (
        f"read {lp_path}\n"
        f"set lpmethod {method}\n"
        f"set threads {threads}\n"
        "optimize\n"
        "display solution objective\n"
        "quit\n"
    )

    try:
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".cplex_cmds", delete=False, encoding="utf-8"
        ) as tf:
            tf.write(script)
            script_path = tf.name

        t0 = time.monotonic()
        result = subprocess.run(
            [cplex_bin, "-f", script_path],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        wall = time.monotonic() - t0
        output = result.stdout + result.stderr

        solve_time = _parse_time(output)
        if solve_time < 0:
            solve_time = wall

        rows, cols = _parse_dimensions(output)
        return BenchmarkResult(
            solver="cplex",
            algorithm=algo,
            threads=threads,
            time_s=solve_time,
            status=_parse_status(output),
            objective=_parse_objective(output),
            rows=rows,
            cols=cols,
        )
    except (subprocess.TimeoutExpired, OSError) as exc:
        return BenchmarkResult(
            solver="cplex",
            algorithm=algo,
            threads=threads,
            time_s=-1.0,
            status=f"error: {exc}",
            objective="N/A",
        )
    finally:
        Path(script_path).unlink(missing_ok=True)


def _run_highs(
    lp_path: Path,
    algo: str,
    threads: int,
    timeout: int,
) -> BenchmarkResult | None:
    """Solve with HiGHS binary and return timing."""
    highs_bin = shutil.which("highs")
    if highs_bin is None:
        return None

    solver_arg = "ipm" if algo == "barrier" else "simplex"

    cmd = [
        highs_bin,
        "--model_file",
        str(lp_path),
        "--solver",
        solver_arg,
    ]

    # HiGHS CLI only supports --parallel on/off, not a thread count
    if threads > 0:
        cmd += ["--parallel", "on"]

    try:
        t0 = time.monotonic()
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        wall = time.monotonic() - t0
        output = result.stdout + result.stderr

        solve_time = _parse_time(output)
        if solve_time < 0:
            solve_time = wall

        rows, cols = _parse_dimensions(output)
        return BenchmarkResult(
            solver="highs",
            algorithm=algo,
            threads=threads,
            time_s=solve_time,
            status=_parse_status(output),
            objective=_parse_objective(output),
            rows=rows,
            cols=cols,
        )
    except (subprocess.TimeoutExpired, OSError) as exc:
        return BenchmarkResult(
            solver="highs",
            algorithm=algo,
            threads=threads,
            time_s=-1.0,
            status=f"error: {exc}",
            objective="N/A",
        )


def _run_clp(
    lp_path: Path,
    algo: str,
    threads: int,
    timeout: int,
) -> BenchmarkResult | None:
    """Solve with CLP binary and return timing."""
    clp_bin = shutil.which("clp")
    if clp_bin is None:
        return None

    algo_map = {
        "dual": "dualS",
        "primal": "primalS",
        "barrier": "barrier",
        "default": "solve",
    }
    solve_cmd = algo_map.get(algo, "solve")

    cmd = [clp_bin, str(lp_path)]
    if threads > 0:
        cmd += ["-threads", str(threads)]
    cmd += [solve_cmd, "-statistics", "-quit"]

    try:
        t0 = time.monotonic()
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        wall = time.monotonic() - t0
        output = result.stdout + result.stderr

        solve_time = _parse_time(output)
        if solve_time < 0:
            solve_time = wall

        rows, cols = _parse_dimensions(output)
        return BenchmarkResult(
            solver="clp",
            algorithm=algo,
            threads=threads,
            time_s=solve_time,
            status=_parse_status(output),
            objective=_parse_objective(output),
            rows=rows,
            cols=cols,
        )
    except (subprocess.TimeoutExpired, OSError) as exc:
        return BenchmarkResult(
            solver="clp",
            algorithm=algo,
            threads=threads,
            time_s=-1.0,
            status=f"error: {exc}",
            objective="N/A",
        )


def _run_cbc(
    lp_path: Path,
    algo: str,
    threads: int,
    timeout: int,
) -> BenchmarkResult | None:
    """Solve with CBC binary and return timing."""
    cbc_bin = shutil.which("cbc")
    if cbc_bin is None:
        return None

    algo_map = {
        "dual": "dualS",
        "primal": "primalS",
        "barrier": "barrier",
        "default": "solve",
    }
    solve_cmd = algo_map.get(algo, "solve")

    cmd = [cbc_bin, str(lp_path)]
    if threads > 0:
        cmd += ["-threads", str(threads)]
    cmd += [solve_cmd, "-statistics", "-quit"]

    try:
        t0 = time.monotonic()
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        wall = time.monotonic() - t0
        output = result.stdout + result.stderr

        solve_time = _parse_time(output)
        if solve_time < 0:
            solve_time = wall

        rows, cols = _parse_dimensions(output)
        return BenchmarkResult(
            solver="cbc",
            algorithm=algo,
            threads=threads,
            time_s=solve_time,
            status=_parse_status(output),
            objective=_parse_objective(output),
            rows=rows,
            cols=cols,
        )
    except (subprocess.TimeoutExpired, OSError) as exc:
        return BenchmarkResult(
            solver="cbc",
            algorithm=algo,
            threads=threads,
            time_s=-1.0,
            status=f"error: {exc}",
            objective="N/A",
        )


# ---------------------------------------------------------------------------
# Main benchmark driver
# ---------------------------------------------------------------------------

_SOLVER_RUNNERS = [
    ("cplex", _run_cplex),
    ("highs", _run_highs),
    ("clp", _run_clp),
    ("cbc", _run_cbc),
]

_DEFAULT_ALGOS = ["dual", "barrier"]
_DEFAULT_THREADS = [0, 2, 4]


def run_benchmark(
    lp_path: Path,
    *,
    algos: list[str] | None = None,
    thread_counts: list[int] | None = None,
    timeout: int = 300,
) -> BenchmarkReport:
    """Benchmark all available solvers on the given LP file.

    Parameters
    ----------
    lp_path:
        Path to the LP file (plain or compressed).
    algos:
        List of algorithms to test (default: dual, barrier).
    thread_counts:
        List of thread counts to test (default: 0, 2, 4).
    timeout:
        Maximum seconds per individual solve.

    Returns
    -------
    BenchmarkReport with all results.
    """
    if algos is None:
        algos = list(_DEFAULT_ALGOS)
    if thread_counts is None:
        thread_counts = list(_DEFAULT_THREADS)

    report = BenchmarkReport(lp_path=lp_path)

    with as_plain_lp(lp_path) as plain_path:
        for _, runner in _SOLVER_RUNNERS:
            for algo in algos:
                for threads in thread_counts:
                    result = runner(plain_path, algo, threads, timeout)
                    if result is not None:
                        report.results.append(result)

    return report


def format_benchmark_table(report: BenchmarkReport, use_color: bool = True) -> str:
    """Format benchmark results as a readable table."""
    lines: list[str] = []
    bold = col._BOLD if use_color else ""  # noqa: SLF001
    reset = col._RESET if use_color else ""  # noqa: SLF001
    green = col._GREEN if use_color else ""  # noqa: SLF001
    red = col._RED if use_color else ""  # noqa: SLF001

    lines.append(f"\n{bold}Solver Benchmark: {report.lp_path.name}{reset}")

    # Show LP dimensions from first result that has them
    for r in report.results:
        if r.rows > 0 or r.cols > 0:
            lines.append(f"  LP size: {r.rows} rows, {r.cols} columns")
            break

    if not report.results:
        lines.append("  No solvers available for benchmarking.")
        return "\n".join(lines)

    # Table header
    header = (
        f"  {'Solver':<10} {'Algorithm':<10} {'Threads':>7}"
        f" {'Time(s)':>10} {'Objective':>16} {'Status':>10}"
    )
    sep = "  " + "-" * (len(header) - 2)
    lines.append("")
    lines.append(header)
    lines.append(sep)

    for r in report.results:
        time_str = f"{r.time_s:>10.4f}" if r.time_s >= 0 else "    error"
        status_col = green if r.status == "optimal" else red
        status_str = (
            f"{status_col}{r.status:>10}{reset}" if use_color else f"{r.status:>10}"
        )
        lines.append(
            f"  {r.solver:<10} {r.algorithm:<10} {r.threads:>7} "
            f"{time_str} {r.objective:>16} {status_str}"
        )

    lines.append("")
    return "\n".join(lines)
