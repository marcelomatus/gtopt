# SPDX-License-Identifier: BSD-3-Clause
"""Writer round-trip — write a CEN-flavoured feed, then re-read it
through the canonical-feed reader (the same one Phase-1 uses)."""

from __future__ import annotations

from pathlib import Path

import pandas as pd

from cen2gtopt._writer import write_canonical_feed
from gtopt_canonical_feed import Bus, Generator, Line, Topology, read_feed


def test_round_trip_via_canonical_reader(tmp_path: Path):
    topo = Topology(
        buses=[Bus(uid=1, name="Crucero"), Bus(uid=2, name="Charrúa")],
        generators=[
            Generator(
                uid=10,
                name="BOCAMINA II",
                bus_uid=1,
                pmin=0,
                pmax=350,
                declared_MC=35.0,
                kind="thermal",
                emission_rate=950.0,
            ),
        ],
        lines=[
            Line(
                uid=100,
                bus_a_uid=1,
                bus_b_uid=2,
                tmax_ab=1500,
                tmax_ba=1500,
                reactance=0.02,
            ),
        ],
    )
    dispatch_long = pd.DataFrame(
        {
            "date_utc": ["2026-04-01"] * 2,
            "hour": [1, 2],
            "gen_uid": [10, 10],
            "dispatch": [310.0, 305.0],
        }
    )
    out = tmp_path / "feed.parquet"
    manifest = write_canonical_feed(out, topology=topo, dispatch_long=dispatch_long)

    # Re-read via Phase-1's reader.
    topo2, cells, _mf = read_feed(out)
    assert topo2.gen_uids() == [10]
    assert len(cells.dispatch) == 2
    assert manifest.producer == "cen2gtopt"
