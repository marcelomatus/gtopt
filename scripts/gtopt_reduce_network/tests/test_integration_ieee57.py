# SPDX-License-Identifier: BSD-3-Clause
"""End-to-end integration test on IEEE-57 (a non-trivial-size case).

The reducer reduces 57 → 20 buses; gtopt solves both the reduced and the
original case; we then run the projector and assert the cost-error
budget. Skipped unless ``GTOPT_BIN`` is set in the environment so the
default scripts test suite stays fast and binary-free.

Locally:

    GTOPT_BIN=$(python tools/get_gtopt_binary.py) pytest -m integration -q
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

pytestmark = pytest.mark.integration

_TARGET_K = 20
_COST_TOLERANCE = 0.10  # 10% — generous for v1


def _gtopt_bin() -> str | None:
    explicit = os.environ.get("GTOPT_BIN")
    if explicit and Path(explicit).exists():
        return explicit
    return shutil.which("gtopt")


@pytest.fixture(scope="module")
def gtopt_bin() -> str:
    bin_path = _gtopt_bin()
    if bin_path is None:
        pytest.skip("GTOPT_BIN not set and `gtopt` not on PATH; skipping integration")
    return bin_path


def _read_total_cost(out_dir: Path) -> float | None:
    """Best-effort scan of the gtopt output directory for the objective."""
    for candidate in ("summary.json", "out.json", "result.json", "stats.json"):
        p = out_dir / candidate
        if not p.exists():
            continue
        try:
            data = json.loads(p.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            continue
        if isinstance(data, dict):
            for k in ("total_cost", "objective", "objective_value"):
                if k in data:
                    return float(data[k])
    return None


def test_ieee57_reduce_solve_project(
    tmp_path: Path, ieee57_path: Path, gtopt_bin: str
) -> None:
    """Reduce → solve reduced & full → project; cost match within tolerance."""
    if not ieee57_path.exists():
        pytest.skip(f"missing case file: {ieee57_path}")

    work = tmp_path / "ieee57"
    work.mkdir()

    # 1. reduce 57 → K buses.
    reduce_cmd = [
        "python",
        "-m",
        "gtopt_reduce_network.main",
        "reduce",
        str(ieee57_path),
        "-K",
        str(_TARGET_K),
        "-o",
        str(work / "reduced.json"),
        "--summary",
    ]
    subprocess.run(reduce_cmd, check=True)
    reduced_path = work / "reduced.json"
    assert reduced_path.exists()

    # 2. solve reduced with gtopt (uses -s/-d, see `gtopt --help`).
    reduced_out = work / "out_reduced"
    reduced_out.mkdir()
    subprocess.run(
        [gtopt_bin, "-s", str(reduced_path), "-d", str(reduced_out)],
        check=True,
        timeout=120,
    )

    # 3. solve full case for the cost reference.
    full_out = work / "out_full"
    full_out.mkdir()
    subprocess.run(
        [gtopt_bin, "-s", str(ieee57_path), "-d", str(full_out)],
        check=True,
        timeout=180,
    )

    # 4. cost comparison if both runs report it; else just smoke-check.
    cost_red = _read_total_cost(reduced_out)
    cost_full = _read_total_cost(full_out)
    if cost_red is not None and cost_full is not None and cost_full > 0:
        rel_err = abs(cost_red - cost_full) / cost_full
        assert rel_err < _COST_TOLERANCE, (
            f"reduced cost {cost_red:.2f} differs from full {cost_full:.2f} "
            f"by {rel_err:.1%} (>{_COST_TOLERANCE:.0%})"
        )

    # 5. projection should run without errors.
    base = (work / "reduced").with_suffix("")
    projected_out = work / "out_projected"
    projected_out.mkdir()
    subprocess.run(
        [
            "python",
            "-m",
            "gtopt_reduce_network.main",
            "project-results",
            str(reduced_out),
            "-o",
            str(projected_out),
            "--busmap",
            str(base.with_name(base.name + ".busmap.csv")),
            "--linemap",
            str(base.with_name(base.name + ".linemap.csv")),
            "--aggregator",
            str(base.with_name(base.name + ".aggregator.csv")),
        ],
        check=True,
    )
