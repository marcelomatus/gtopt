# SPDX-License-Identifier: BSD-3-Clause
"""Round-trip: build the gold fixture, write it, read it back, assert
frames are identical.

This is the schema-lock-in test from master plan §11 P0.3. It runs
on both Phase-1 and Phase-2 sides and pins the parquet contract.
"""

from __future__ import annotations

import pandas as pd

from gtopt_canonical_feed import read_feed


def test_roundtrip_topology(gold_feed_dir, gold_topology):
    topology, _cells, _manifest = read_feed(gold_feed_dir)
    assert topology.bus_uids() == gold_topology.bus_uids()
    assert topology.gen_uids() == gold_topology.gen_uids()
    assert topology.line_uids() == gold_topology.line_uids()
    # Spot-check one generator's full payload.
    g = topology.gen_by_uid(10)
    assert g.name == "cheap_gas"
    assert g.declared_MC == 10.0
    assert g.emission_factor == 400.0


def test_roundtrip_cells_dispatch(gold_feed_dir, gold_cells):
    _topology, cells, _manifest = read_feed(gold_feed_dir)
    pd.testing.assert_frame_equal(
        cells.dispatch.reset_index(drop=True),
        gold_cells.dispatch.reset_index(drop=True),
        check_dtype=False,
    )


def test_roundtrip_cells_lmp(gold_feed_dir, gold_cells):
    _topology, cells, _manifest = read_feed(gold_feed_dir)
    assert cells.has_lmp()
    pd.testing.assert_frame_equal(
        cells.lmp.reset_index(drop=True),
        gold_cells.lmp.reset_index(drop=True),
        check_dtype=False,
    )


def test_drop_lmp_flag_skips_lmp(gold_feed_dir):
    _topology, cells, _manifest = read_feed(gold_feed_dir, drop_lmp=True)
    assert not cells.has_lmp()


def test_manifest_records_row_counts(gold_feed_dir):
    _topology, _cells, manifest = read_feed(gold_feed_dir)
    # 3 hourly phases × 24 hours × 3 generators = 72 dispatch rows.
    assert manifest.row_counts["cells/dispatch.parquet"] == 24 * 3
    assert manifest.row_counts["cells/lmp.parquet"] == 24 * 3
    assert manifest.row_counts["topology/bus.parquet"] == 3
