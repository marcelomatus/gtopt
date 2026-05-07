# SPDX-License-Identifier: BSD-3-Clause
"""Gold-fixture builder for the canonical-feed round-trip tests.

The fixture is a synthetic 3-bus / 3-unit / 24-hour case with
hand-computed expected LMPs across off-peak / shoulder / peak
phases. The same fixture is reused by gtopt_marginal_units and
cen2gtopt round-trip tests.
"""

from __future__ import annotations

from pathlib import Path

import pandas as pd
import pytest

from gtopt_canonical_feed import (
    SCHEMA_VERSION,
    Bus,
    Cells,
    Generator,
    Line,
    Manifest,
    Topology,
    write_feed,
)
from gtopt_canonical_feed.cells import (
    CELL_KEY_COLS,
    COL_BLOCK,
    COL_BUS_UID,
    COL_DATA_SOURCE,
    COL_DATE_UTC,
    COL_DISPATCH,
    COL_FLOW,
    COL_GEN_UID,
    COL_HOUR,
    COL_LINE_UID,
    COL_LMP,
    COL_LOAD,
    COL_SCENARIO,
    COL_STAGE,
)


@pytest.fixture
def gold_topology() -> Topology:
    """3 buses, 3 thermal units (cheap/mid/peaker), 2 lines.

    Bus 1 hosts the cheap unit (gas, gcost=10). Bus 2 hosts the mid
    unit (combined cycle, gcost=30). Bus 3 hosts the peaker
    (diesel, gcost=80) plus the load.
    """
    buses = [
        Bus(uid=1, name="b1", region="north"),
        Bus(uid=2, name="b2", region="north"),
        Bus(uid=3, name="b3", region="south"),
    ]
    generators = [
        Generator(
            uid=10,
            name="cheap_gas",
            bus_uid=1,
            pmin=10,
            pmax=100,
            declared_MC=10.0,
            kind="thermal",
            emission_factor=400.0,
        ),
        Generator(
            uid=20,
            name="midcc_gas",
            bus_uid=2,
            pmin=20,
            pmax=150,
            declared_MC=30.0,
            kind="thermal",
            emission_factor=350.0,
        ),
        Generator(
            uid=30,
            name="peaker_diesel",
            bus_uid=3,
            pmin=0,
            pmax=80,
            declared_MC=80.0,
            kind="thermal",
            emission_factor=700.0,
        ),
    ]
    lines = [
        Line(
            uid=100, bus_a_uid=1, bus_b_uid=2, tmax_ab=120, tmax_ba=120, reactance=0.05
        ),
        Line(uid=101, bus_a_uid=2, bus_b_uid=3, tmax_ab=80, tmax_ba=80, reactance=0.05),
    ]
    return Topology(buses=buses, generators=generators, lines=lines)


@pytest.fixture
def gold_cells() -> Cells:
    """24 hours of dispatch / lmp / flow / load.

    Hours 0-7 (off-peak):  load = 50  → only cheap_gas runs at 50, λ = 10
    Hours 8-15 (shoulder): load = 130 → cheap at 100 + midcc at 30, λ = 30
    Hours 16-23 (peak):    load = 220 → cheap at 100, midcc at 100,
                                        peaker at 20, λ = 80
    No congestion: line 100 carries up to 100 MW < 120, line 101 up
    to 130 < 150 (we tighten line 101 to expose congestion in a
    later test).
    """
    rows_dispatch = []
    rows_lmp = []
    rows_flow = []
    rows_load = []
    for hour in range(24):
        if hour < 8:
            d_cheap, d_mid, d_peak = 50.0, 0.0, 0.0
            lam = 10.0
        elif hour < 16:
            d_cheap, d_mid, d_peak = 100.0, 30.0, 0.0
            lam = 30.0
        else:
            d_cheap, d_mid, d_peak = 100.0, 100.0, 20.0
            lam = 80.0
        cell = _real_cell(hour)
        for gen_uid, dispatch in ((10, d_cheap), (20, d_mid), (30, d_peak)):
            rows_dispatch.append({**cell, COL_GEN_UID: gen_uid, COL_DISPATCH: dispatch})
        for bus_uid in (1, 2, 3):
            rows_lmp.append({**cell, COL_BUS_UID: bus_uid, COL_LMP: lam})
        # flow: net injection at b1 minus local demand 0 → all flows to b3
        flow_100 = d_cheap  # b1 → b2
        flow_101 = d_cheap + d_mid  # b2 → b3
        rows_flow.append({**cell, COL_LINE_UID: 100, COL_FLOW: flow_100})
        rows_flow.append({**cell, COL_LINE_UID: 101, COL_FLOW: flow_101})
        rows_load.append({**cell, COL_BUS_UID: 3, COL_LOAD: d_cheap + d_mid + d_peak})

    dispatch = pd.DataFrame(rows_dispatch)
    lmp = pd.DataFrame(rows_lmp)
    flow = pd.DataFrame(rows_flow)
    load = pd.DataFrame(rows_load)
    return Cells(dispatch=dispatch, lmp=lmp, flow=flow, load=load)


@pytest.fixture
def gold_feed_dir(tmp_path, gold_topology, gold_cells) -> Path:
    root = tmp_path / "gold_feed.parquet"
    manifest = Manifest.make(
        producer="gold_fixture",
        producer_version="0.0.1",
        schema_version=SCHEMA_VERSION,
        extras={"description": "synthetic 3-bus / 3-unit / 24h fixture"},
    )
    write_feed(root, gold_topology, gold_cells, manifest)
    return root


def _real_cell(hour: int) -> dict[str, object]:
    """Build the cell-key dict for a 'real' (date_utc, hour) cell."""
    return {
        COL_SCENARIO: pd.NA,
        COL_STAGE: pd.NA,
        COL_BLOCK: pd.NA,
        COL_DATE_UTC: "2026-04-01",
        COL_HOUR: hour,
        COL_DATA_SOURCE: "real",
    }


# Re-export the column-name constants the tests want without requiring
# them to import from gtopt_canonical_feed.cells directly.
__all__ = [
    "CELL_KEY_COLS",
    "gold_topology",
    "gold_cells",
    "gold_feed_dir",
]
