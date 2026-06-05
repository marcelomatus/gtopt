# SPDX-License-Identifier: BSD-3-Clause
"""Pytest gate for ``share/gtopt/naming_dialects.json`` consistency.

Wraps the standalone CLI checker ``tools/check_naming_dialects.py`` so
ERROR-class findings — stale ``canonical`` entries after a C++ field
rename / removal, or aliases that collide with another class's
canonical name — fail unit tests instead of waiting for a runtime
``canonicalize_json_keys`` ambiguity.

WARNING-class findings (C++ contract fields with no dictionary entry)
are advisory: the dictionary intentionally covers only fields that need
a human-friendly alias.  These do NOT fail the gate; run the CLI
directly for the full coverage report::

    tools/check_naming_dialects.py
    tools/check_naming_dialects.py --strict   # promote warnings → exit 2
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECKER = REPO_ROOT / "tools" / "check_naming_dialects.py"
DICT_FILE = REPO_ROOT / "share" / "gtopt" / "naming_dialects.json"
JSON_HEADERS_DIR = REPO_ROOT / "include" / "gtopt" / "json"


def _have_inputs() -> bool:
    return CHECKER.is_file() and DICT_FILE.is_file() and JSON_HEADERS_DIR.is_dir()


@pytest.fixture(scope="module")
def dialects_report() -> dict:
    if not _have_inputs():
        pytest.skip(
            "check_naming_dialects.py, naming_dialects.json or "
            "include/gtopt/json/ not found under repo root"
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
            f"check_naming_dialects.py exited {proc.returncode} "
            "(expected 0 or 1)\n"
            f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
        )
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as exc:
        pytest.fail(
            f"check_naming_dialects.py emitted invalid JSON: {exc}\n"
            f"stdout (first 400 chars):\n{proc.stdout[:400]}"
        )
        raise  # pragma: no cover — pytest.fail raises


def test_dialects_report_is_well_formed(dialects_report: dict) -> None:
    assert "totals" in dialects_report
    assert "findings" in dialects_report
    totals = dialects_report["totals"]
    assert totals["classes"] > 0, "no element classes scanned — header glob broken?"
    assert totals["fields"] > 0, "no contract fields scanned — daw::json regex broken?"


def test_no_error_class_findings(dialects_report: dict) -> None:
    """ERROR-class findings break the canonicalize_json_keys contract
    and MUST be zero.  Run ``tools/check_naming_dialects.py`` for the
    human-readable report."""
    errors = [f for f in dialects_report["findings"] if f["severity"] == "ERROR"]
    assert not errors, (
        f"naming_dialects.json has {len(errors)} ERROR finding(s):\n"
        + "\n".join(
            f"  - [{f['code']}] {f.get('klass', '?')}.{f.get('field', '?')}: "
            f"{f.get('message', '')}"
            for f in errors
        )
    )


def test_findings_have_expected_shape(dialects_report: dict) -> None:
    """Each finding carries severity / code / klass / field / message —
    the shape consumed by the assertion message in
    :func:`test_no_error_class_findings`."""
    for finding in dialects_report["findings"]:
        assert finding["severity"] in {"INFO", "WARNING", "ERROR"}, finding
        for key in ("code", "klass", "field", "message"):
            assert key in finding, f"missing '{key}' in finding: {finding}"
