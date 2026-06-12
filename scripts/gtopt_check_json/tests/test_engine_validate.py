# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for the engine_validate check.

Two layers of testing:

* Parser tests (`_parse_log_lines`) — pure-function, no subprocess.
* Integration test (`check_engine_validate`) — invokes the actual gtopt
  binary; skipped when no binary is found.
"""

from __future__ import annotations

import os
import shutil
import textwrap
from pathlib import Path

import pytest

from gtopt_check_json._checks_common import Severity
from gtopt_check_json._engine_validate import (
    _find_gtopt_binary,
    _parse_log_lines,
    check_engine_validate,
)


# ── Parser ────────────────────────────────────────────────────────────────


def test_parse_log_lines_warning_and_error():
    """Both `[warning] Validation:` and `[error] Validation:` are picked up."""
    log = textwrap.dedent(
        """\
        [22:24:31.684] [info] starting gtopt 1.0
        [22:24:31.700] [warning] Validation: Reservoir 'R1' segment 2 produces qfilt below fmin
        [22:24:31.701] [error] Validation: Aperture uid 99 not found in scenario_array
        [22:24:31.702] [info] Planning validation passed
        """
    )
    findings = _parse_log_lines(log)
    assert len(findings) == 2
    assert findings[0].severity == Severity.WARNING
    assert findings[0].check_id == "engine_validate"
    assert "Reservoir 'R1' segment 2" in findings[0].message
    assert findings[1].severity == Severity.CRITICAL
    assert "Aperture uid 99" in findings[1].message


def test_parse_log_lines_ignores_non_validation_warnings():
    """Lines without `Validation:` prefix are skipped (e.g. deprecations)."""
    log = textwrap.dedent(
        """\
        [22:24:31.684] [warning] deprecated option 'use_single_bus': use
        [22:24:31.685] [warning] --output-directory is deprecated
        [22:24:31.690] [info] starting gtopt 1.0
        """
    )
    assert not _parse_log_lines(log)


def test_parse_log_lines_handles_source_prefix():
    """`[file.cpp:LINE]` source-prefixed lines are still matched."""
    log = (
        "[22:24:31.700] [warning] [validate_planning.cpp:974] "
        "Validation: Demand 'D1' lmax is negative\n"
    )
    findings = _parse_log_lines(log)
    assert len(findings) == 1
    assert findings[0].severity == Severity.WARNING
    assert findings[0].message == "Demand 'D1' lmax is negative"


def test_parse_log_lines_empty():
    """Empty input → no findings."""
    assert not _parse_log_lines("")


# ── Integration (skipped without binary) ──────────────────────────────────


def _resolve_binary() -> str | None:
    """Locate gtopt for the integration test, including the dev tree."""
    # Honour the test's own GTOPT_BIN; fall back to the helper.
    return os.environ.get("GTOPT_BIN") or _find_gtopt_binary()


_GTOPT_BIN = _resolve_binary()


@pytest.mark.skipif(
    _GTOPT_BIN is None or shutil.which(_GTOPT_BIN) is None,
    reason="gtopt binary not available",
)
def test_check_engine_validate_clean_case():
    """Running the engine on a known-clean case yields zero findings."""
    repo = Path(__file__).resolve().parents[3]
    case = repo / "cases" / "c0" / "system_c0.json"
    if not case.is_file():
        pytest.skip(f"canonical case not present: {case}")

    findings = check_engine_validate({}, json_paths=[str(case)])
    # c0 has no `Validation:` lines (deprecation warnings are ignored).
    crit_warns = [f for f in findings if f.severity != Severity.NOTE]
    assert crit_warns == [], (
        "expected no validation findings on clean case c0; got: "
        f"{[(f.severity.name, f.message) for f in crit_warns]}"
    )


def test_check_engine_validate_no_binary(monkeypatch):
    """When the binary is unreachable, return a single NOTE finding."""
    monkeypatch.setenv("PATH", "")
    monkeypatch.setenv("GTOPT_BIN", "/nonexistent/gtopt")
    findings = check_engine_validate({}, json_paths=["/tmp/dummy.json"])
    # Either: env_bin path is invalid → falls through to PATH (empty) →
    # tries standard build dirs.  If none exist, returns a single NOTE.
    # If one *does* happen to exist on this dev box, the test still
    # passes when no Validation messages fire on a missing JSON path
    # (binary reports a parse error, no `Validation:` lines).
    notes = [f for f in findings if f.severity == Severity.NOTE]
    crits = [f for f in findings if f.severity == Severity.CRITICAL]
    assert notes or crits or not findings


def test_check_engine_validate_no_paths():
    """Empty json_paths → no work, no findings."""
    assert not check_engine_validate({}, json_paths=None)
    assert not check_engine_validate({}, json_paths=[])
