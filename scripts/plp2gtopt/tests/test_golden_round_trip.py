# SPDX-License-Identifier: BSD-3-Clause
"""Golden-fixture regression test for plp2gtopt JSON output.

Runs ``convert_plp_case`` on ``cases/plp_min_1bus`` and compares the
canonical JSON output against a frozen fixture.  Refresh the fixture
with ``PYTEST_UPDATE_GOLDEN=1 python -m pytest …`` when an intentional
change to the converter output is made.
"""

import json
import os
from pathlib import Path

import pytest

from plp2gtopt.plp2gtopt import convert_plp_case


_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLP_MIN_1BUS = _CASES_DIR / "plp_min_1bus"
_GOLDEN_DIR = Path(__file__).parent / "fixtures"
_GOLDEN = _GOLDEN_DIR / "plp_min_1bus_golden.json"


def _canonicalise(path: Path) -> str:
    """Return the JSON content sorted by keys with stable indentation."""
    data = json.loads(path.read_text())
    return json.dumps(data, sort_keys=True, indent=2, ensure_ascii=False) + "\n"


@pytest.mark.integration
def test_plp_min_1bus_golden_json_round_trip(tmp_path):
    """plp_min_1bus → JSON output is byte-stable against the golden fixture."""

    out_dir = tmp_path / "plp_min_1bus"
    out_dir.mkdir(parents=True, exist_ok=True)
    opts = {
        "input_dir": _PLP_MIN_1BUS,
        "output_dir": out_dir,
        "output_file": out_dir / "plp_min_1bus.json",
        "hydrologies": "1",
    }

    convert_plp_case(opts)

    out_path = opts["output_file"]
    assert out_path.exists(), f"converter did not produce {out_path}"
    canonical = _canonicalise(out_path)

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
            "plp2gtopt/tests/test_golden_round_trip.py -q"
        )

    expected = _GOLDEN.read_text(encoding="utf-8")
    assert canonical == expected, (
        "plp_min_1bus.json output changed; if intentional, refresh with "
        "PYTEST_UPDATE_GOLDEN=1 python -m pytest "
        "plp2gtopt/tests/test_golden_round_trip.py -q"
    )
