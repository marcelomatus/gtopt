# SPDX-License-Identifier: BSD-3-Clause
"""Regression tests for ``tools/gtopt_compare_csv.py`` column alignment.

The e2e ``compare_solution`` fixtures broke when ``solution.csv`` grew
diagnostic columns (``solve_ticks``, ``solve_time_s``, ``solve_calls``,
``infeasible_count``): the old comparator required byte-identical headers and
equal field counts, so any new column tripped a *structural* failure before a
single value was compared.  ``solve_time_s`` is wall-clock — it can never match
a checked-in golden.  The fix aligns columns by NAME (ignoring actual-only
columns and skipping the non-deterministic ``solve_*`` metadata) while still
catching a golden column that vanished from the actual output.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

_MOD_PATH = Path(__file__).with_name("gtopt_compare_csv.py")
_spec = importlib.util.spec_from_file_location("gtopt_compare_csv", _MOD_PATH)
assert _spec and _spec.loader
gtopt_compare_csv = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(gtopt_compare_csv)
compare_csv = gtopt_compare_csv.compare_csv


def _write(tmp_path: Path, name: str, text: str) -> str:
    path = tmp_path / name
    path.write_text(text)
    return str(path)


def test_actual_superset_columns_are_tolerated(tmp_path) -> None:
    """Actual carrying extra diagnostic columns must not fail structurally."""
    actual = _write(
        tmp_path,
        "actual.csv",
        "scene,phase,obj_value,solve_time_s,infeasible_count\n0,0,23163.4,0.0441,0\n",
    )
    expected = _write(tmp_path, "expected.csv", "scene,phase,obj_value\n0,0,23163.4\n")
    errors, warnings = compare_csv(actual, expected, tolerance=1e-6)
    assert errors == []
    assert warnings == []


def test_nondeterministic_solve_columns_are_skipped(tmp_path) -> None:
    """Even when both sides carry ``solve_time_s`` its wall-clock diff is
    ignored (skip list), so a different time does not fail."""
    actual = _write(
        tmp_path, "actual.csv", "scene,obj_value,solve_time_s\n0,10.0,0.9\n"
    )
    expected = _write(
        tmp_path, "expected.csv", "scene,obj_value,solve_time_s\n0,10.0,0.1\n"
    )
    errors, warnings = compare_csv(actual, expected, tolerance=1e-6, strict=True)
    assert errors == []


def test_missing_expected_column_is_a_regression(tmp_path) -> None:
    """A column the golden declares but the actual dropped must fail."""
    actual = _write(tmp_path, "actual.csv", "scene,obj_value\n0,10.0\n")
    expected = _write(tmp_path, "expected.csv", "scene,obj_value,binding\n0,10.0,1\n")
    errors, _ = compare_csv(actual, expected, tolerance=1e-6)
    assert any("binding" in e for e in errors)


def test_strict_still_catches_real_value_drift(tmp_path) -> None:
    """A genuine value difference on a shared column must fail under --strict
    (the solver-specific golden path) even though columns align."""
    actual = _write(
        tmp_path, "actual.csv", "scene,obj_value,solve_time_s\n0,23163.4,0.04\n"
    )
    expected = _write(tmp_path, "expected.csv", "scene,obj_value\n0,23.1634\n")
    errors, warnings = compare_csv(actual, expected, tolerance=1e-6, strict=True)
    assert any("obj_value" in e or "col 2" in e for e in errors)


def test_shared_value_within_tolerance_passes(tmp_path) -> None:
    """A ~1e-12 difference (O0 vs O2 numerics) stays within tolerance."""
    actual = _write(tmp_path, "actual.csv", "scene,obj_value\n0,23163.424133184064\n")
    expected = _write(
        tmp_path, "expected.csv", "scene,obj_value\n0,23163.424133184090\n"
    )
    errors, warnings = compare_csv(actual, expected, tolerance=1e-6, strict=True)
    assert errors == []
    assert warnings == []
