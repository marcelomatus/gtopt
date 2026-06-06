# SPDX-License-Identifier: BSD-3-Clause
"""Golden-fixture regression test for pp2gtopt JSON output (issue #507 Phase 0).

Runs ``convert`` on pandapower's built-in ``case_ieee30`` and compares
the canonical JSON output against a frozen fixture.  Refresh the
fixture with ``PYTEST_UPDATE_GOLDEN=1 python -m pytest …`` when an
intentional change to the converter output is made.

Mirrors ``plp2gtopt/tests/test_golden_round_trip.py``.
"""

from pathlib import Path

import pytest

from gtopt_shared.testing import assert_golden_file


_TESTS_DIR = Path(__file__).parent
_GOLDEN_DIR = _TESTS_DIR / "fixtures"
_REFRESH_TARGET = "pp2gtopt/tests/test_golden_round_trip.py"


@pytest.mark.integration
def test_ieee30b_golden_json_round_trip(tmp_path):
    """case_ieee30 → JSON output is byte-stable against the golden fixture."""
    # Import inside the test so test discovery does not pull pandapower.
    pn = pytest.importorskip("pandapower.networks")
    from pp2gtopt.convert import convert  # pylint: disable=import-outside-toplevel

    net = pn.case_ieee30()
    out_path = tmp_path / "ieee30b.json"
    convert(output_path=out_path, net=net, name="ieee30b", solver_type="cascade")
    assert_golden_file(
        "ieee30b_golden", out_path, _GOLDEN_DIR, refresh_target=_REFRESH_TARGET
    )
