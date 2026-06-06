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
import pathlib

import pytest

from gtopt_shared.testing import assert_golden_file

from igtopt.igtopt import _run as _igtopt_run


_SCRIPTS_DIR = pathlib.Path(__file__).parent.parent.parent
_C0_XLSX = _SCRIPTS_DIR / "cases" / "igtopt_c0" / "system_c0.xlsx"
_GOLDEN_DIR = pathlib.Path(__file__).parent / "fixtures"
_REFRESH_TARGET = "igtopt/tests/test_golden_round_trip.py"


def _scrub_input_directory(data: dict) -> dict:
    """Replace ``options.input_directory`` (the per-run tmp_path absolute path)
    with a fixed sentinel so the golden is independent of pytest's tmp_path.
    """
    options = data.get("options")
    if isinstance(options, dict) and "input_directory" in options:
        options["input_directory"] = "<scrubbed>"
    return data


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
    assert_golden_file(
        "system_c0_golden",
        json_out,
        _GOLDEN_DIR,
        refresh_target=_REFRESH_TARGET,
        scrub=_scrub_input_directory,
    )
