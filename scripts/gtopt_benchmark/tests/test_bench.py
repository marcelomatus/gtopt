# SPDX-License-Identifier: BSD-3-Clause
"""Tests: kernels report sane magnitudes; the synthetic reference case is
deterministic, schema-valid and (when a gtopt binary is present) builds an
LP that gtopt accepts."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

from gtopt_benchmark.main import (
    _dgemm_gflops,
    _pyloop_mops,
    machine_bench,
    synthetic_case,
)

_REPO_ROOT = Path(__file__).resolve().parents[3]


def _find_gtopt() -> str | None:
    """Locate a gtopt binary: $GTOPT_BIN, common build dirs, then PATH."""
    env = os.environ.get("GTOPT_BIN")
    if env and Path(env).exists():
        return env
    for rel in ("build_release/standalone/gtopt", "build/standalone/gtopt"):
        cand = _REPO_ROOT / rel
        if cand.exists():
            return str(cand)
    return shutil.which("gtopt")


def test_kernels_report_positive() -> None:
    assert _dgemm_gflops(n=128, reps=1) > 0.1
    assert _pyloop_mops(n=100_000) > 0.1


def test_machine_bench_shape() -> None:
    res = machine_bench(gtopt="/nonexistent/gtopt")
    assert res["dgemm_gflops"] > 0
    assert res["triad_gbps"] > 0
    assert "cplex_kticks_per_s" in res


def test_synthetic_case_parses() -> None:
    """The generated case round-trips through JSON with the expected keys."""
    case = json.loads(json.dumps(synthetic_case(0.1)))
    assert set(case) == {"options", "simulation", "system"}
    assert case["options"]["model_options"]["use_single_bus"] is True
    for key in ("block_array", "stage_array", "scenario_array"):
        assert case["simulation"][key], key
    for key in ("bus_array", "generator_array", "demand_array", "commitment_array"):
        assert case["system"][key], key


def test_synthetic_case_structure() -> None:
    """One commitment per generator; demand profile length == block count."""
    case = synthetic_case(0.2)
    n_gens = len(case["system"]["generator_array"])
    n_blocks = len(case["simulation"]["block_array"])
    assert len(case["system"]["commitment_array"]) == n_gens
    assert case["simulation"]["stage_array"][0]["count_block"] == n_blocks
    profile = case["system"]["demand_array"][0]["lmax"]
    assert len(profile) == 1  # one scenario
    assert len(profile[0]) == n_blocks
    # A commitment record carries the MIP-making fields.
    uc0 = case["system"]["commitment_array"][0]
    for field in ("pmin", "startup_cost", "min_up_time", "min_down_time"):
        assert field in uc0


def test_synthetic_case_deterministic() -> None:
    """No RNG / time / order dependence: identical bytes every call."""
    assert json.dumps(synthetic_case(0.3)) == json.dumps(synthetic_case(0.3))


def test_synthetic_case_scale_monotone() -> None:
    small = synthetic_case(0.1)
    big = synthetic_case(0.5)
    assert len(big["system"]["generator_array"]) > len(
        small["system"]["generator_array"]
    )
    assert len(big["simulation"]["block_array"]) > len(
        small["simulation"]["block_array"]
    )


def test_gtopt_builds_lp(tmp_path: Path) -> None:
    """Smoke: gtopt accepts the synthetic case and builds its LP (--lp-only)."""
    gtopt = _find_gtopt()
    if not gtopt:
        pytest.skip("no gtopt binary available")
    case = tmp_path / "bench.json"
    case.write_text(json.dumps(synthetic_case(0.08)), encoding="utf-8")
    proc = subprocess.run(
        [
            gtopt,
            str(case),
            "--lp-only",
            "--set",
            f"output_directory={tmp_path}/out",
        ],
        capture_output=True,
        text=True,
        timeout=180,
        check=False,
    )
    assert proc.returncode == 0, proc.stderr[-2000:]
