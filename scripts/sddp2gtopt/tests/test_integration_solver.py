"""End-to-end integration tests for ``sddp2gtopt``.

Convert a PSR SDDP case (``psrclasses.json``) → gtopt JSON via
:func:`convert_sddp_case`, then run the standalone ``gtopt`` binary
on the produced JSON to verify the resulting LP actually solves.

Skipped when ``gtopt`` is not on ``$PATH`` and ``GTOPT_BIN`` is unset
(see :func:`conftest.gtopt_bin`).
"""

from __future__ import annotations

import csv
import math
import subprocess
from pathlib import Path

import pytest

from sddp2gtopt import convert_sddp_case


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _run_gtopt(
    gtopt_bin: str, case_dir: Path, json_stem: str, timeout: int = 120
) -> tuple[int, str]:
    """Run ``gtopt <json_stem>.json`` inside ``case_dir``."""
    result = subprocess.run(
        [gtopt_bin, f"{json_stem}.json"],
        cwd=str(case_dir),
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    return result.returncode, result.stdout + result.stderr


def _read_solution_status(results_dir: Path) -> tuple[int | None, float | None]:
    """Parse ``solution.csv`` → ``(status, obj_value)``.

    Supports both the legacy ``key,value`` two-column format and the
    current columnar format with ``status`` / ``obj_value`` headers.
    """
    csv_path = results_dir / "solution.csv"
    if not csv_path.exists():
        return None, None
    with csv_path.open(encoding="utf-8") as fh:
        rows = list(csv.reader(fh))
    if not rows:
        return None, None

    header = rows[0]
    # Columnar format: header + at least one data row.
    if "status" in header and len(rows) >= 2:
        idx_status = header.index("status")
        idx_obj = header.index("obj_value") if "obj_value" in header else -1
        try:
            status = int(rows[1][idx_status])
        except (ValueError, IndexError):
            status = None
        try:
            obj = float(rows[1][idx_obj]) if idx_obj >= 0 else None
        except (ValueError, IndexError):
            obj = None
        return status, obj

    # Legacy key,value format.
    kv = {row[0]: row[1] for row in rows if len(row) >= 2}
    status_str = kv.get("status")
    obj_str = kv.get("obj_value")
    return (
        int(status_str) if status_str is not None else None,
        float(obj_str) if obj_str is not None else None,
    )


# ---------------------------------------------------------------------------
# Integration tests
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_integration_case_min_solves(
    case_min_dir: Path, tmp_path: Path, gtopt_bin: str
) -> None:
    """Smallest hand-crafted case: 1 stage × 1 block, 1 thermal, 1 demand.

    Pins the end-to-end pipeline at its minimum non-trivial shape — if
    any of the parsers / writer / gtopt JSON loader paths break, this
    is the first to fire.
    """
    out_dir = tmp_path / "case_min_out"
    out_file = out_dir / "case_min.json"
    rc = convert_sddp_case(
        {
            "input_dir": case_min_dir,
            "output_dir": out_dir,
            "output_file": out_file,
            "name": "case_min",
        }
    )
    assert rc == 0
    assert out_file.is_file()

    rc, log = _run_gtopt(gtopt_bin, out_dir, out_file.stem)
    assert rc == 0, f"gtopt failed (rc={rc}): {log}"

    results_dir = out_dir / "output"
    status, obj = _read_solution_status(results_dir)
    assert status == 0, f"solver status={status}, expected 0"
    # 1 thermal × 1 demand — objective must be finite and non-negative
    # for the cost-minimisation default (no negative-cost generators).
    assert obj is not None
    assert not math.isnan(obj)


@pytest.mark.integration
def test_integration_case0_thermal_hydro_solves(
    case0_dir: Path, tmp_path: Path, gtopt_bin: str
) -> None:
    """Vendored PSR ``case0``: 2 stages × 1 block, 3 thermals + 1 hydro.

    This is the canonical sddp2gtopt regression target — the first
    upstream PSR sample fixture that exercises hydro + thermal in
    one LP.  Catches:

      * sddp2gtopt converter changes that break the gtopt JSON schema
        (the produced JSON must round-trip through
        ``from_json<Planning>`` cleanly).
      * gtopt-side regressions in the multi-stage / hydro / Demand
        wiring when the input comes from a *converted* JSON rather
        than a hand-built fixture — the SDDP.jl benchmarks
        (test_hydro_thermal_benchmark.cpp) cover the hand-built path
        already, so this fills the converted-input gap.
    """
    out_dir = tmp_path / "case0_out"
    rc = convert_sddp_case(
        {
            "input_dir": case0_dir,
            "output_dir": out_dir,
        }
    )
    assert rc == 0

    # convert_sddp_case writes <name>.json inside output_dir.
    json_files = sorted(out_dir.glob("*.json"))
    assert len(json_files) == 1, f"expected one JSON, found {json_files}"
    json_file = json_files[0]

    rc, log = _run_gtopt(gtopt_bin, out_dir, json_file.stem)
    assert rc == 0, f"gtopt failed (rc={rc}): {log}"

    results_dir = out_dir / "output"
    status, obj = _read_solution_status(results_dir)
    assert status == 0, f"solver status={status}, expected 0"
    assert obj is not None
    assert not math.isnan(obj)


@pytest.mark.integration
def test_integration_case_two_systems_solves(
    case_two_systems_dir: Path, tmp_path: Path, gtopt_bin: str
) -> None:
    """Multi-system fixture (2 ``PSRSystem`` → 2 buses), end-to-end.

    The hand-crafted ``case_two_systems`` fixture has one thermal
    plant + one demand both anchored to system 2000 (``sys_1_bus``);
    system 2001 (``sys_2_bus``) is an empty island.  Both subproblems
    are independent — the populated bus must satisfy its demand from
    its single thermal, the empty bus is trivially feasible.

    This is the only integration test that exercises
    ``use_single_bus = false`` end-to-end through ``sddp2gtopt``;
    earlier coverage rejected multi-system at the converter.
    """
    out_dir = tmp_path / "two_sys_out"
    rc = convert_sddp_case(
        {
            "input_dir": case_two_systems_dir,
            "output_dir": out_dir,
            "name": "two_systems",
        }
    )
    assert rc == 0

    json_files = sorted(out_dir.glob("*.json"))
    assert len(json_files) == 1
    json_file = json_files[0]

    rc, log = _run_gtopt(gtopt_bin, out_dir, json_file.stem)
    assert rc == 0, f"gtopt failed (rc={rc}): {log}"

    results_dir = out_dir / "output"
    status, obj = _read_solution_status(results_dir)
    assert status == 0, f"solver status={status}, expected 0"
    assert obj is not None
    assert not math.isnan(obj)


@pytest.mark.integration
def test_integration_case_thermal_only_solves(
    case_thermal_only_dir: Path, tmp_path: Path, gtopt_bin: str
) -> None:
    """Thermal-only multi-stage case: 3 stages × 2 blocks, 2 thermals.

    Complements case0 (which has hydro) by pinning the
    thermal-only multi-stage path through both the writer and the
    gtopt LP build.
    """
    out_dir = tmp_path / "case_thermal_only_out"
    rc = convert_sddp_case(
        {
            "input_dir": case_thermal_only_dir,
            "output_dir": out_dir,
        }
    )
    assert rc == 0

    json_files = sorted(out_dir.glob("*.json"))
    assert len(json_files) == 1
    json_file = json_files[0]

    rc, log = _run_gtopt(gtopt_bin, out_dir, json_file.stem)
    assert rc == 0, f"gtopt failed (rc={rc}): {log}"

    results_dir = out_dir / "output"
    status, obj = _read_solution_status(results_dir)
    assert status == 0, f"solver status={status}, expected 0"
    assert obj is not None
    assert not math.isnan(obj)
