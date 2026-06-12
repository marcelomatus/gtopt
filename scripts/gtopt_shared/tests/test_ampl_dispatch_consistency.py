# SPDX-License-Identifier: BSD-3-Clause
"""Pytest gate for AMPL/PAMPL dispatch consistency.

Wraps ``tools/check_ampl_dispatch.py`` — which scrapes
``source/ampl_dispatch_registry.cpp`` (the C++ source of truth for
``register_ampl_param`` and ``register_ampl_iter`` calls) and
cross-references with ``scripts/gtopt_check_pampl/_checks.py``'s
hard-coded ``ELEMENT_TYPES`` / ``LP_VARIABLES`` sets.

ERROR-class findings (``python-only-class`` — Python validator claims a
class the C++ registry does NOT register) fail the gate.  These are
silent-pass-then-runtime-reject hazards: ``*.pampl`` files referencing
the class look valid to ``gtopt_check_pampl`` but break when gtopt
resolves them.

WARNING-class findings (``cpp-only-class``) are advisory: the C++ side
registers more classes than Python knows about, so the validator
skips coverage for them.  Run the CLI directly to surface them::

    tools/check_ampl_dispatch.py            # human-readable
    tools/check_ampl_dispatch.py --strict   # warnings → exit 2
    tools/check_ampl_dispatch.py --json     # machine-readable
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECKER = REPO_ROOT / "tools" / "check_ampl_dispatch.py"
REGISTRY_FILE = REPO_ROOT / "source" / "ampl_dispatch_registry.cpp"
CHECKS_FILE = REPO_ROOT / "scripts" / "gtopt_check_pampl" / "_checks.py"


def _have_inputs() -> bool:
    return CHECKER.is_file() and REGISTRY_FILE.is_file() and CHECKS_FILE.is_file()


@pytest.fixture(scope="module")
def dispatch_report() -> dict:
    if not _have_inputs():
        pytest.skip(
            "check_ampl_dispatch.py, ampl_dispatch_registry.cpp, or "
            "gtopt_check_pampl/_checks.py not found under repo root"
        )
    proc = subprocess.run(
        [sys.executable, str(CHECKER), "--json"],
        capture_output=True,
        text=True,
        check=False,
        cwd=REPO_ROOT,
    )
    if proc.returncode not in (0, 1):
        pytest.fail(
            f"check_ampl_dispatch.py exited {proc.returncode} "
            "(expected 0 or 1)\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        pytest.fail(
            f"check_ampl_dispatch.py emitted invalid JSON: {exc}\n"
            f"stdout (first 400 chars):\n{proc.stdout[:400]}"
        )
        raise  # pragma: no cover — pytest.fail raises


def test_dispatch_report_is_well_formed(dispatch_report: dict) -> None:
    """The CLI must scan a non-empty registry and a non-empty Python set
    — protects against silent regex regressions that would otherwise
    let the test trivially pass."""
    totals = dispatch_report["totals"]
    assert totals["cpp_iter_classes"] > 0, (
        "no register_ampl_iter calls scraped — has the C++ registry "
        "renamed its registration helpers?"
    )
    assert totals["cpp_param_total"] > 0, (
        "no register_ampl_param calls scraped — has the C++ registry "
        "renamed its registration helpers?"
    )
    assert totals["py_element_types"] > 0, (
        "ELEMENT_TYPES in gtopt_check_pampl/_checks.py is empty — "
        "validator would silently pass everything."
    )


def test_no_python_only_classes(dispatch_report: dict) -> None:
    """ERROR-class findings — Python validator claims classes the C++
    registry doesn't register.  These let bogus PAMPL refs through.

    Run ``tools/check_ampl_dispatch.py`` for the full report; this test
    is the CI gate.
    """
    errors = [f for f in dispatch_report["findings"] if f["severity"] == "ERROR"]
    assert not errors, (
        f"AMPL dispatch has {len(errors)} ERROR finding(s) — Python "
        "ELEMENT_TYPES lists classes the C++ AMPL registry does NOT "
        "register:\n"
        + "\n".join(f"  - [{f['code']}] {f['klass']}: {f['message']}" for f in errors)
    )


def test_findings_have_expected_shape(dispatch_report: dict) -> None:
    """Each finding carries severity / code / klass / field / message —
    the shape consumed by :func:`test_no_python_only_classes`."""
    for finding in dispatch_report["findings"]:
        assert finding["severity"] in {"INFO", "WARNING", "ERROR"}, finding
        for key in ("code", "klass", "field", "message"):
            assert key in finding, f"missing '{key}' in finding: {finding}"
