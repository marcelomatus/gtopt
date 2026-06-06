# SPDX-License-Identifier: BSD-3-Clause
"""Golden-fixture regression test for plexos2gtopt JSON output (issue #507 Phase 0).

Runs ``build_planning`` + ``write_planning`` on an empty
:class:`PlexosCase` (smallest stable fixture; the integration test
covers the rich CEN PCP path separately) and compares the canonical
JSON output against a frozen fixture.  Refresh the fixture with
``PYTEST_UPDATE_GOLDEN=1 python -m pytest …`` when an intentional
change to the converter output is made.

Mirrors ``plp2gtopt/tests/test_golden_round_trip.py``.  The empty
case is a structural skeleton — every field that ``build_planning``
emits even with no entities present (options, simulation, system
shell) is pinned by this golden.  Drift in default option values,
simulation skeleton, or top-level keys will fail the test.
"""

import json
import os
from pathlib import Path

import pytest

from plexos2gtopt.entities import BundleSpec, PlexosCase
from plexos2gtopt.gtopt_writer import build_planning, write_planning


_TESTS_DIR = Path(__file__).parent
_GOLDEN_DIR = _TESTS_DIR / "fixtures"
_GOLDEN = _GOLDEN_DIR / "empty_case_golden.json"


def _canonicalise(path: Path) -> str:
    """Return the JSON content sorted by keys with stable indentation."""
    data = json.loads(path.read_text())
    return json.dumps(data, sort_keys=True, indent=2, ensure_ascii=False) + "\n"


@pytest.mark.integration
def test_empty_case_golden_json_round_trip(tmp_path):
    """Empty PlexosCase → JSON output is byte-stable against the golden fixture."""

    case = PlexosCase(bundle=BundleSpec(bundle_name="golden_empty"))
    planning = build_planning(case, name="golden_empty")
    out_path = tmp_path / "golden_empty.json"
    write_planning(planning, out_path)

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
            "plexos2gtopt/tests/test_golden_round_trip.py -q"
        )

    expected = _GOLDEN.read_text(encoding="utf-8")
    assert canonical == expected, (
        "empty PlexosCase JSON output changed; if intentional, refresh with "
        "PYTEST_UPDATE_GOLDEN=1 python -m pytest "
        "plexos2gtopt/tests/test_golden_round_trip.py -q"
    )
