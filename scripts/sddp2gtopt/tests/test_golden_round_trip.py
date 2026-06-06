# SPDX-License-Identifier: BSD-3-Clause
"""Golden-fixture regression test for sddp2gtopt JSON output (issue #507 Phase 0).

Runs ``convert_sddp_case`` on ``tests/data/case0`` and compares the
canonical JSON output against a frozen fixture.  Refresh the fixture
with ``PYTEST_UPDATE_GOLDEN=1 python -m pytest …`` when an intentional
change to the converter output is made.

Mirrors ``plp2gtopt/tests/test_golden_round_trip.py``.
"""

from pathlib import Path

import pytest

from gtopt_shared.testing import assert_golden_file

from sddp2gtopt.sddp2gtopt import convert_sddp_case


_TESTS_DIR = Path(__file__).parent
_CASE0 = _TESTS_DIR / "data" / "case0"
_GOLDEN_DIR = _TESTS_DIR / "fixtures"
_REFRESH_TARGET = "sddp2gtopt/tests/test_golden_round_trip.py"


@pytest.mark.integration
def test_case0_golden_json_round_trip(tmp_path):
    """case0 → JSON output is byte-stable against the golden fixture."""

    out_dir = tmp_path / "case0"
    out_dir.mkdir(parents=True, exist_ok=True)
    opts = {
        "input_dir": _CASE0,
        "output_dir": out_dir,
        "output_file": out_dir / "case0.json",
        "name": "case0",
    }

    convert_sddp_case(opts)
    assert_golden_file(
        "case0_golden",
        opts["output_file"],
        _GOLDEN_DIR,
        refresh_target=_REFRESH_TARGET,
    )
