# SPDX-License-Identifier: BSD-3-Clause
"""Per-entity snapshot tests for pp2gtopt builders (issue #507 Phase 0).

Pins the JSON shape emitted by each module-private ``_build_*``
function in ``pp2gtopt.convert`` against pandapower's built-in
``case_ieee30`` network.  These complement the full-JSON golden in
``test_golden_round_trip.py`` by giving drift attribution at the
per-builder level — when a Phase 4 entity-builder refactor migrates
one inline builder into the shared layer, running just this test
attributes any drift to the right entity.

Refresh with ``PYTEST_UPDATE_GOLDEN=1 python -m pytest …``.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import pytest

# Force pandapower import to fail-soft if missing
pn = pytest.importorskip("pandapower.networks")

# pylint: disable=wrong-import-position,wrong-import-order
from pp2gtopt.convert import (  # noqa: E402  (pandapower must import first)
    _BASE_MVA,
    _build_buses,
    _build_demands,
    _build_ext_grid_gen,
    _build_generators,
    _build_lines,
    _build_physical_lines,
    _build_transformers,
)


_GOLDEN_DIR = Path(__file__).parent / "fixtures" / "entities"


def _canonicalise(obj) -> str:
    """Return JSON sorted-key dump with stable indentation."""
    return json.dumps(obj, sort_keys=True, indent=2, ensure_ascii=False) + "\n"


def _assert_snapshot(name: str, payload) -> None:
    """Compare payload against the named golden under ``fixtures/entities/``."""
    canonical = _canonicalise(payload)
    path = _GOLDEN_DIR / f"{name}.json"

    if os.environ.get("PYTEST_UPDATE_GOLDEN"):
        _GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
        path.write_text(canonical, encoding="utf-8")
        pytest.skip(f"golden written to {path}")

    if not path.exists():
        pytest.skip(
            f"golden missing: {path}; create with "
            "PYTEST_UPDATE_GOLDEN=1 python -m pytest "
            "pp2gtopt/tests/test_entity_snapshots.py -q"
        )

    expected = path.read_text(encoding="utf-8")
    assert canonical == expected, (
        f"{name} entity output changed; if intentional, refresh with "
        "PYTEST_UPDATE_GOLDEN=1 python -m pytest "
        "pp2gtopt/tests/test_entity_snapshots.py -q"
    )


@pytest.fixture(scope="module")
def ieee30_net():
    """Shared pandapower IEEE 30 fixture (loaded once per module)."""
    return pn.case_ieee30()


def test_build_buses_snapshot(ieee30_net) -> None:
    """Bus builder: 1-indexed uid, ext_grid bus marked ``reference_theta=0``."""
    _assert_snapshot("build_buses", _build_buses(ieee30_net))


def test_build_ext_grid_gen_snapshot(ieee30_net) -> None:
    """Ext-grid is gen uid=1 with poly-cost-or-default gcost."""
    _assert_snapshot("build_ext_grid_gen", _build_ext_grid_gen(ieee30_net))


def test_build_generators_snapshot(ieee30_net) -> None:
    """Generators: ext_grid first, then PV gens with 1-indexed bus refs."""
    _assert_snapshot("build_generators", _build_generators(ieee30_net))


def test_build_demands_snapshot(ieee30_net) -> None:
    """Demands: filter zero-load, wrap ``p_mw`` as ``lmax = [[p_mw]]``."""
    _assert_snapshot("build_demands", _build_demands(ieee30_net))


def test_build_physical_lines_snapshot(ieee30_net) -> None:
    """Physical lines: x_pu conversion via ohm→pu, skip degenerate (x<1e-6)."""
    _assert_snapshot(
        "build_physical_lines", _build_physical_lines(ieee30_net, _BASE_MVA)
    )


def test_build_transformers_snapshot(ieee30_net) -> None:
    """Transformers: vk_percent → x_pu, off-nominal tap ratio + phase shift."""
    _assert_snapshot("build_transformers", _build_transformers(ieee30_net, _BASE_MVA))


def test_build_lines_composite_snapshot(ieee30_net) -> None:
    """Lines composite: physical + transformer entries with sequential uids."""
    _assert_snapshot("build_lines", _build_lines(ieee30_net, _BASE_MVA))
