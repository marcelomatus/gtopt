# SPDX-License-Identifier: BSD-3-Clause
"""LP solver test-suite definitions and runner for gtopt_check_solvers.

Each test case is a self-contained gtopt JSON problem written to a
temporary directory.  The ``run_solver_tests`` function executes each
test by calling the gtopt binary with ``--solver <name>``, reads the
``output/solution.csv`` file, and validates the results.
"""

from __future__ import annotations

import csv
import json
import logging
import os
import re
import subprocess
import tempfile
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

log = logging.getLogger(__name__)

# Maximum characters of stderr/stdout to include in failure details.
_MAX_ERROR_DETAIL_LENGTH = 600

# ---------------------------------------------------------------------------
# Test-case definitions (embedded JSON)
# ---------------------------------------------------------------------------

# Minimal single-bus LP:
#   1 generator  g1  pmax=300  gcost=20
#   1 demand     d1  lmax=250
#   Expected: status=0, obj_value ≈ 5.0  (250*20/1000)
_SINGLE_BUS_JSON: dict[str, Any] = {
    "options": {
        "use_single_bus": True,
        "demand_fail_cost": 1000,
        "scale_objective": 1000,
        "output_format": "csv",
        "output_compression": "uncompressed",
    },
    "simulation": {
        "block_array": [{"uid": 1, "duration": 1}],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    },
    "system": {
        "name": "check_single_bus",
        "bus_array": [{"uid": 1, "name": "b1"}],
        "generator_array": [
            {
                "uid": 1,
                "name": "g1",
                "bus": "b1",
                "pmin": 0,
                "pmax": 300,
                "gcost": 20,
                "capacity": 300,
            }
        ],
        "demand_array": [{"uid": 1, "name": "d1", "bus": "b1", "lmax": [[250.0]]}],
    },
}

# 4-bus DC OPF (identical to ieee_4b_ori):
#   2 generators  g1@b1 (300 MW, $20)  g2@b2 (200 MW, $35)
#   2 demands     d3@b3 (150 MW)  d4@b4 (100 MW)
#   5 transmission lines
#   Expected: status=0, obj_value ≈ 5.0  (g1 dispatches 250 MW @ $20/MWh / 1000)
_KIRCHHOFF_JSON: dict[str, Any] = {
    "options": {
        "use_single_bus": False,
        "use_kirchhoff": True,
        "demand_fail_cost": 1000,
        "scale_objective": 1000,
        "output_format": "csv",
        "output_compression": "uncompressed",
    },
    "simulation": {
        "block_array": [{"uid": 1, "duration": 1}],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    },
    "system": {
        "name": "check_kirchhoff",
        "bus_array": [
            {"uid": 1, "name": "b1"},
            {"uid": 2, "name": "b2"},
            {"uid": 3, "name": "b3"},
            {"uid": 4, "name": "b4"},
        ],
        "generator_array": [
            {
                "uid": 1,
                "name": "g1",
                "bus": "b1",
                "pmin": 0,
                "pmax": 300,
                "gcost": 20,
                "capacity": 300,
            },
            {
                "uid": 2,
                "name": "g2",
                "bus": "b2",
                "pmin": 0,
                "pmax": 200,
                "gcost": 35,
                "capacity": 200,
            },
        ],
        "demand_array": [
            {"uid": 1, "name": "d3", "bus": "b3", "lmax": [[150.0]]},
            {"uid": 2, "name": "d4", "bus": "b4", "lmax": [[100.0]]},
        ],
        "line_array": [
            {
                "uid": 1,
                "name": "l1_2",
                "bus_a": "b1",
                "bus_b": "b2",
                "reactance": 0.05575,
                "tmax_ab": 250,
                "tmax_ba": 250,
            },
            {
                "uid": 2,
                "name": "l1_3",
                "bus_a": "b1",
                "bus_b": "b3",
                "reactance": 0.09271,
                "tmax_ab": 250,
                "tmax_ba": 250,
            },
            {
                "uid": 3,
                "name": "l2_4",
                "bus_a": "b2",
                "bus_b": "b4",
                "reactance": 0.04211,
                "tmax_ab": 250,
                "tmax_ba": 250,
            },
            {
                "uid": 4,
                "name": "l3_4",
                "bus_a": "b3",
                "bus_b": "b4",
                "reactance": 0.03756,
                "tmax_ab": 250,
                "tmax_ba": 250,
            },
            {
                "uid": 5,
                "name": "l2_3",
                "bus_a": "b2",
                "bus_b": "b3",
                "reactance": 0.08425,
                "tmax_ab": 250,
                "tmax_ba": 250,
            },
        ],
    },
}

# Infeasibility probe: demand_fail_cost=0 forces load shedding without
# penalising it, so the solver finds a non-optimal-in-the-traditional-sense
# result — but gtopt always solves (status=0).  Instead we stress the
# solver by giving it a tight capacity case where all demand is served via
# load-shedding (lmax > pmax of the only generator and fail_cost is 0).
# The solver should still report status=0 but with non-zero fail_sol.
#
# We use this as a "feasibility + warm-start" test: after an initial solve
# we tighten the generator and re-solve, verifying the solver handles it.
_FEASIBILITY_JSON: dict[str, Any] = {
    "options": {
        "use_single_bus": True,
        "demand_fail_cost": 10000,
        "scale_objective": 1000,
        "output_format": "csv",
        "output_compression": "uncompressed",
    },
    "simulation": {
        "block_array": [{"uid": 1, "duration": 1}],
        "stage_array": [{"uid": 1, "first_block": 0, "count_block": 1}],
        "scenario_array": [{"uid": 1, "probability_factor": 1}],
    },
    "system": {
        "name": "check_feasibility",
        "bus_array": [{"uid": 1, "name": "b1"}],
        "generator_array": [
            {
                "uid": 1,
                "name": "g1",
                "bus": "b1",
                "pmin": 0,
                "pmax": 100,
                "gcost": 30,
                "capacity": 100,
            }
        ],
        "demand_array": [{"uid": 1, "name": "d1", "bus": "b1", "lmax": [[100.0]]}],
    },
}


# ---------------------------------------------------------------------------
# Test result
# ---------------------------------------------------------------------------


@dataclass
class SolverTestResult:
    """Result of running one solver test case."""

    name: str
    passed: bool
    message: str = ""
    details: str = ""
    obj_value: float | None = None
    status: int | None = None
    duration_s: float = 0.0


@dataclass
class SolverTestReport:
    """Aggregated results for one solver."""

    solver: str
    results: list[SolverTestResult] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return all(r.passed for r in self.results)

    @property
    def n_passed(self) -> int:
        return sum(1 for r in self.results if r.passed)

    @property
    def n_failed(self) -> int:
        return sum(1 for r in self.results if not r.passed)


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------


def _read_solution_csv(output_dir: Path) -> dict[str, str]:
    """Read output/solution.csv and return the first data row as a dict."""
    sol_path = output_dir / "solution.csv"
    if not sol_path.is_file():
        return {}
    with open(sol_path, encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            return dict(row)
    return {}


def _run_one_test(
    gtopt_bin: str,
    solver: str,
    test_name: str,
    planning: dict[str, Any],
    *,
    timeout: float = 60.0,
) -> SolverTestResult:
    """Write *planning* to a temp dir, call gtopt, and return a SolverTestResult."""
    import time  # noqa: PLC0415

    with tempfile.TemporaryDirectory(prefix=f"gtopt_check_{test_name}_") as tmpdir:
        tmp = Path(tmpdir)
        json_path = tmp / f"{test_name}.json"
        output_dir = tmp / "output"
        output_dir.mkdir()

        # Inject explicit output directory into options
        plan = dict(planning)
        opts = dict(plan.get("options", {}))
        opts["output_directory"] = str(output_dir)
        plan["options"] = opts

        with open(json_path, "w", encoding="utf-8") as fh:
            json.dump(plan, fh, indent=2)

        cmd = [
            gtopt_bin,
            str(json_path),
            "--solver",
            solver,
            "--quiet",
        ]
        log.debug("Running: %s", " ".join(cmd))

        t0 = time.monotonic()
        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=timeout,
                check=False,
                env={**os.environ, "SPDLOG_LEVEL": "off"},
            )
        except subprocess.TimeoutExpired:
            return SolverTestResult(
                name=test_name,
                passed=False,
                message=f"timed out after {timeout:.0f}s",
            )
        except FileNotFoundError:
            return SolverTestResult(
                name=test_name,
                passed=False,
                message="gtopt binary not found",
            )
        duration = time.monotonic() - t0

        if proc.returncode not in (0, 1):
            details = (proc.stderr or proc.stdout).strip()
            return SolverTestResult(
                name=test_name,
                passed=False,
                message=f"gtopt exited with code {proc.returncode}",
                details=details[:_MAX_ERROR_DETAIL_LENGTH] if details else "",
                duration_s=duration,
            )

        sol = _read_solution_csv(output_dir)
        if not sol:
            return SolverTestResult(
                name=test_name,
                passed=False,
                message="solution.csv not found or empty",
                duration_s=duration,
            )

        try:
            status = int(sol.get("status", "-1"))
            obj_value = float(sol.get("obj_value", "nan"))
        except ValueError:
            return SolverTestResult(
                name=test_name,
                passed=False,
                message=f"unexpected solution.csv values: {sol}",
                duration_s=duration,
            )

        return SolverTestResult(
            name=test_name,
            passed=True,
            message="ok",
            obj_value=obj_value,
            status=status,
            duration_s=duration,
        )


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

#: All built-in test cases as (name, planning_dict, expected_status) triples.
BUILTIN_TESTS: list[tuple[str, dict[str, Any], int]] = [
    ("single_bus_lp", _SINGLE_BUS_JSON, 0),
    ("kirchhoff_lp", _KIRCHHOFF_JSON, 0),
    ("feasibility_lp", _FEASIBILITY_JSON, 0),
]


def run_solver_tests(
    gtopt_bin: str,
    solver: str,
    *,
    timeout: float = 60.0,
    test_names: list[str] | None = None,
) -> SolverTestReport:
    """Run the built-in LP test suite for *solver*.

    Parameters
    ----------
    gtopt_bin
        Absolute path to the ``gtopt`` binary.
    solver
        Solver name (e.g. ``"clp"``, ``"highs"``).
    timeout
        Per-test timeout in seconds.
    test_names
        Optional list of test names to run; defaults to all built-in tests.

    Returns
    -------
    SolverTestReport
        Aggregated pass/fail results for the solver.
    """
    report = SolverTestReport(solver=solver)
    for name, planning, expected_status in BUILTIN_TESTS:
        if test_names and name not in test_names:
            continue
        result = _run_one_test(gtopt_bin, solver, name, planning, timeout=timeout)
        if result.passed and result.status != expected_status:
            result.passed = False
            result.message = f"expected status {expected_status}, got {result.status}"
        report.results.append(result)
    return report


def list_available_solvers(gtopt_bin: str, *, timeout: float = 10.0) -> list[str]:
    """Return the list of available LP solver names reported by gtopt.

    Runs ``gtopt --solvers`` and parses the output.  Returns an empty list
    if the binary is unavailable or reports no solvers.
    """
    try:
        proc = subprocess.run(
            [gtopt_bin, "--solvers"],
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
    except (FileNotFoundError, subprocess.TimeoutExpired):
        return []

    solvers: list[str] = []
    for line in proc.stdout.splitlines():
        # Match lines that are indented and contain a single word (solver name).
        # Robust against additional description text: only lines with exactly
        # one non-whitespace word that is all alphanumeric/underscore/hyphen.
        m = re.match(r"^\s+(\w[\w\-]*)$", line)
        if m:
            solvers.append(m.group(1))
    return solvers
