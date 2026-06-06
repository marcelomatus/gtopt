# SPDX-License-Identifier: BSD-3-Clause
"""Golden-fixture regression test for igtopt JSON output (issue #507 Phase 0).

Runs ``igtopt._run()`` on ``scripts/cases/igtopt_c0/system_c0.xlsx`` and
compares the canonical JSON output against a frozen fixture.  Refresh
the fixture with ``PYTEST_UPDATE_GOLDEN=1 python -m pytest …`` when an
intentional change to the converter output is made.

Mirrors ``plp2gtopt/tests/test_golden_round_trip.py``.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib

import pytest

from igtopt.igtopt import _run as _igtopt_run


_SCRIPTS_DIR = pathlib.Path(__file__).parent.parent.parent
_C0_XLSX = _SCRIPTS_DIR / "cases" / "igtopt_c0" / "system_c0.xlsx"
_GOLDEN_DIR = pathlib.Path(__file__).parent / "fixtures"
_GOLDEN = _GOLDEN_DIR / "system_c0_golden.json"


def _canonicalise(path: pathlib.Path) -> str:
    """Return the JSON content sorted by keys with stable indentation.

    Scrubs the ``options.input_directory`` field (which igtopt sets to
    the absolute path of the time-series output dir) so the golden
    comparison is independent of the test's ``tmp_path``.
    """
    data = json.loads(path.read_text())
    options = data.get("options")
    if isinstance(options, dict) and "input_directory" in options:
        options["input_directory"] = "<scrubbed>"
    return json.dumps(data, sort_keys=True, indent=2, ensure_ascii=False) + "\n"


@pytest.mark.skipif(
    not _C0_XLSX.exists(),
    reason="system_c0.xlsx fixture not present",
)
@pytest.mark.integration
def test_system_c0_golden_json_round_trip(tmp_path):
    """igtopt_c0 → JSON output is byte-stable against the golden fixture."""

    json_out = tmp_path / "system_c0.json"
    input_dir = tmp_path / "system_c0"
    args = argparse.Namespace(
        filenames=[str(_C0_XLSX)],
        json_file=json_out,
        input_directory=input_dir,
        input_format="parquet",
        name="system_c0",
        compression="gzip",
        skip_nulls=True,
        parse_unexpected_sheets=False,
        pretty=False,
        zip=False,
    )

    rc = _igtopt_run(args)
    assert rc == 0, "igtopt._run() returned non-zero"
    assert json_out.exists(), f"JSON not created at {json_out}"
    canonical = _canonicalise(json_out)

    if os.environ.get("PYTEST_UPDATE_GOLDEN"):
        _GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        _GOLDEN.write_text(canonical, encoding="utf-8")
        pytest.skip(
            f"golden fixture written to {_GOLDEN}; re-run without "
            "PYTEST_UPDATE_GOLDEN to verify"
        )

    if not _GOLDEN.exists():
        pytest.skip(
            f"golden fixture missing: {_GOLDEN}; create it with "
            "PYTEST_UPDATE_GOLDEN=1 python -m pytest "
            "igtopt/tests/test_golden_round_trip.py -q"
        )

    expected = _GOLDEN.read_text(encoding="utf-8")
    assert canonical == expected, (
        "system_c0.json output changed; if intentional, refresh with "
        "PYTEST_UPDATE_GOLDEN=1 python -m pytest "
        "igtopt/tests/test_golden_round_trip.py -q"
    )
