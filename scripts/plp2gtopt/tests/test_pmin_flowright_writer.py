# -*- coding: utf-8 -*-

"""Unit tests for PminFlowRightWriter and resolve_whitelist().

Covers the writer added by ``--pmin-as-flowright`` in plp2gtopt:

* whitelist resolution (default CSV / explicit path / comma list)
* happy-path emission of FlowRight JSON entry + parquet column
* fail_cost resolution priority
  (CLI override > CCauFal × 3.6 > 2 × max rebalse × 3.6 > 2 × CVert × 3.6 >
  fallback)
* whitelist filtering — non-listed centrals are not converted
* CSV ``enabled=false`` is honoured
* unknown centrals (not in central_parser) are skipped with a warning
"""

from __future__ import annotations

import logging
from pathlib import Path
from typing import Any, Dict, List, Optional

import numpy as np
import pyarrow.parquet as pq

from ..pmin_flowright_writer import (
    PminFlowRightWriter,
    _DEFAULT_FAIL_COST,
    _FLOW_RIGHT_SUFFIX,
    _FLOW_RIGHT_UID_START,
    resolve_whitelist,
)


# ---------------------------------------------------------------------------
# Lightweight fakes — match the duck-typed API the writer expects.
# ---------------------------------------------------------------------------


class FakeCentralParser:
    """Minimal central_parser shim — only ``get_central_by_name`` is used."""

    def __init__(self, centrals: List[Dict[str, Any]]) -> None:
        self._by_name = {str(c["name"]): c for c in centrals}

    def get_central_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        return self._by_name.get(name)


class FakeManceParser:
    """Minimal mance_parser shim — only ``get_mance_by_name`` is used."""

    def __init__(self, mances: List[Dict[str, Any]]) -> None:
        self._by_name = {str(m["name"]): m for m in mances}

    def get_mance_by_name(self, name: str) -> Optional[Dict[str, Any]]:
        return self._by_name.get(name)


class FakeBlockParser:
    """Minimal block_parser shim — exposes ``items`` + ``get_stage_number``."""

    def __init__(self, blocks: List[Dict[str, Any]]) -> None:
        self.items = list(blocks)
        self._stage_map = {int(b["number"]): int(b["stage"]) for b in blocks}

    def get_stage_number(self, block_num: int) -> int:
        return self._stage_map.get(int(block_num), -1)


class FakeStageParser:
    """Minimal stage_parser shim — only its presence/absence matters here."""

    def __init__(self, stages: Optional[List[Dict[str, Any]]] = None) -> None:
        self.items = list(stages or [])


class FakeVrebembParser:
    """Minimal vrebemb_parser shim — only ``get_all`` is used."""

    def __init__(self, entries: List[Dict[str, Any]]) -> None:
        self._entries = list(entries)

    def get_all(self) -> List[Dict[str, Any]]:
        return self._entries


class FakePlpmatParser:
    """Minimal plpmat_parser shim — exposes the two attrs the writer reads."""

    def __init__(self, *, flow_fail_cost: float = 0.0, vert_cost: float = 0.0) -> None:
        self.flow_fail_cost = flow_fail_cost
        self.vert_cost = vert_cost


# ---------------------------------------------------------------------------
# Shared test data
# ---------------------------------------------------------------------------


_BLOCKS: List[Dict[str, Any]] = [
    {"number": 1, "stage": 1, "duration": 1.0},
    {"number": 2, "stage": 1, "duration": 1.0},
]


def _machicura_central(
    *, pmin: float = 12.0, efficiency: float = 0.5
) -> Dict[str, Any]:
    return {
        "name": "MACHICURA",
        "number": 1,
        "bus": 101,
        "pmin": pmin,
        "pmax": 100.0,
        "efficiency": efficiency,
        "ser_hid": 0,
        "ser_ver": 0,
        "afluent": 0.0,
        "type": "serie",
    }


def _planning_with_machicura() -> Dict[str, Any]:
    """Return a planning-system dict with MACHICURA's gen waterway present.

    Mirrors what JunctionWriter emits earlier in the pipeline: a turbine
    pointing at a gen waterway whose downstream junction is the
    consumptive-flow target.
    """
    return {
        "generator_array": [
            {"name": "MACHICURA", "uid": 1, "bus": 101, "pmin": 12.0},
        ],
        "waterway_array": [
            {
                "name": "MACHICURA_1_2",
                "uid": 100,
                "junction_a": "j_up",
                "junction_b": "j_down",
            },
        ],
        "turbine_array": [
            {"name": "MACHICURA", "waterway": "MACHICURA_1_2", "generator": 1},
        ],
    }


def _make_writer(
    *,
    whitelist: List[str],
    centrals: Optional[List[Dict[str, Any]]] = None,
    mances: Optional[List[Dict[str, Any]]] = None,
    blocks: Optional[List[Dict[str, Any]]] = None,
    vrebemb: Optional[FakeVrebembParser] = None,
    plpmat: Optional[FakePlpmatParser] = None,
    options: Optional[Dict[str, Any]] = None,
) -> PminFlowRightWriter:
    """Construct a writer with sensible defaults."""
    return PminFlowRightWriter(
        whitelist=whitelist,
        central_parser=FakeCentralParser(centrals or [_machicura_central()]),
        mance_parser=FakeManceParser(mances or []),
        block_parser=FakeBlockParser(blocks or _BLOCKS),
        stage_parser=FakeStageParser(),
        vrebemb_parser=vrebemb,
        plpmat_parser=plpmat,
        options=options or {},
    )


# ---------------------------------------------------------------------------
# 1. resolve_whitelist
# ---------------------------------------------------------------------------


def test_resolve_whitelist_default_csv_returns_known_names(tmp_path: Path) -> None:
    """Empty spec (`""`) reads names from the supplied default CSV."""
    csv_file = tmp_path / "default.csv"
    csv_file.write_text(
        "name,enabled,description\n"
        "MACHICURA,true,doc1\n"
        "PANGUE,1,doc2\n"
        "DISABLED,false,doc3\n"
    )

    names = resolve_whitelist("", default_csv=csv_file)
    assert names == ["MACHICURA", "PANGUE"]


def test_resolve_whitelist_explicit_csv_path(tmp_path: Path) -> None:
    """An on-disk CSV path is parsed — `default_csv` is ignored."""
    csv_file = tmp_path / "custom.csv"
    csv_file.write_text(
        "name,enabled,description\nABANICO,yes,doc\nANTUCO,on,doc\nOFF,no,doc\n"
    )
    # default_csv should NOT be touched when spec is a real path.
    names = resolve_whitelist(str(csv_file), default_csv=tmp_path / "missing.csv")
    assert names == ["ABANICO", "ANTUCO"]


def test_resolve_whitelist_comma_separated_names(tmp_path: Path) -> None:
    """A non-path spec is treated as a comma-separated name list."""
    names = resolve_whitelist("FOO , BAR,, BAZ ", default_csv=tmp_path / "ignored.csv")
    assert names == ["FOO", "BAR", "BAZ"]


def test_resolve_whitelist_missing_default_csv_raises(tmp_path: Path) -> None:
    """When spec is empty and the default CSV doesn't exist, raise."""
    import pytest

    with pytest.raises(FileNotFoundError):
        resolve_whitelist("", default_csv=tmp_path / "missing.csv")


def test_resolve_whitelist_default_csv_skips_disabled(tmp_path: Path) -> None:
    """``enabled=false`` rows in the default CSV are filtered out."""
    csv_file = tmp_path / "default.csv"
    csv_file.write_text(
        "name,enabled,description\nON_ROW,true,a\nOFF_ROW,false,a\nBLANK_ENABLED,,a\n"
    )
    assert resolve_whitelist("", default_csv=csv_file) == ["ON_ROW"]


# ---------------------------------------------------------------------------
# 2. Happy path: writer emits FlowRight JSON + parquet column
# ---------------------------------------------------------------------------


def test_writer_emits_flow_right_and_parquet(tmp_path: Path) -> None:
    """End-to-end: whitelist matched → FlowRight + parquet both produced."""
    central = _machicura_central(pmin=10.0, efficiency=0.5)
    mance = {
        "name": "MACHICURA",
        "block": np.array([1, 2], dtype=np.int64),
        "pmin": np.array([10.0, 20.0], dtype=np.float64),
        "pmax": np.array([100.0, 100.0], dtype=np.float64),
    }
    writer = _make_writer(
        whitelist=["MACHICURA"],
        centrals=[central],
        mances=[mance],
        # No plpmat_parser → fall back to _DEFAULT_FAIL_COST.
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )

    planning = _planning_with_machicura()
    result = writer.process(planning)

    # FlowRight JSON
    assert len(result) == 1
    fr = result[0]
    assert fr["uid"] == _FLOW_RIGHT_UID_START
    assert fr["name"] == f"MACHICURA{_FLOW_RIGHT_SUFFIX}"
    assert fr["junction"] == "j_down"
    assert fr["direction"] == -1
    assert fr["discharge"] == fr["name"]
    assert fr["fail_cost"] == _DEFAULT_FAIL_COST

    # Generator pmin zeroed
    assert planning["generator_array"][0]["pmin"] == 0.0

    # Parquet file written under <output_dir>/FlowRight/
    parquet_file = tmp_path / "FlowRight" / f"{fr['name']}.parquet"
    assert parquet_file.is_file()

    table = pq.read_table(parquet_file)
    cols = table.column_names
    uid_col = f"uid:{_FLOW_RIGHT_UID_START}"
    # FlowRight.discharge is STBRealFieldSched (Scenario × sTage × Block);
    # the writer mirrors Flow/discharge.parquet by emitting a ``scenario``
    # column in addition to ``block`` / ``stage``.  When no scenarios are
    # configured on the writer, it falls back to ``[1]``.
    assert cols == ["scenario", "block", uid_col, "stage"]
    # flow_min = pmin / Rendi = [10/0.5, 20/0.5] = [20, 40]
    flows = table.column(uid_col).to_pylist()
    assert flows == [20.0, 40.0]

    scenarios_out = table.column("scenario").to_pylist()
    blocks_out = table.column("block").to_pylist()
    stages_out = table.column("stage").to_pylist()
    assert scenarios_out == [1, 1]
    assert blocks_out == [1, 2]
    assert stages_out == [1, 1]

    # planning_system was mutated in place
    assert planning.get("flow_right_array") == [fr]


# ---------------------------------------------------------------------------
# 3. fail_cost resolution priority
# ---------------------------------------------------------------------------


def test_fail_cost_cli_override_wins_over_data() -> None:
    """``options['flow_right_fail_cost']`` beats every data-derived value.

    The CLI value is interpreted as PLP-native ``$/Hm³`` and converted
    to gtopt's internal ``$/(m³/s·h)`` via ``× 3.6``.  Both the override
    and the CCauFal/rebalse paths share the same conversion, so the
    user can pass values in the same units they'd read off plpmat.dat.
    """
    writer = _make_writer(
        whitelist=["MACHICURA"],
        plpmat=FakePlpmatParser(flow_fail_cost=7000.0, vert_cost=999.0),
        vrebemb=FakeVrebembParser([{"name": "X", "volume": 0.0, "cost": 100.0}]),
        options={"flow_right_fail_cost": 42.0},
    )
    # 42 × 3.6 = 151.2 — the override wins (would be 7000 × 3.6 = 25200
    # without the override) so the assertion pins both the priority and
    # the unit conversion in one shot.
    assert writer._resolve_fail_cost() == 42.0 * 3.6


def test_fail_cost_uses_ccaufal_times_36() -> None:
    """No CLI override → ``CCauFal × 3.6`` is used."""
    writer = _make_writer(
        whitelist=["MACHICURA"],
        plpmat=FakePlpmatParser(flow_fail_cost=7000.0, vert_cost=999.0),
        # Even with rebalse data present, CCauFal takes precedence.
        vrebemb=FakeVrebembParser([{"name": "X", "volume": 0.0, "cost": 100.0}]),
    )
    assert writer._resolve_fail_cost() == 7000.0 * 3.6


def test_fail_cost_uses_max_rebalse_when_ccaufal_zero() -> None:
    """``CCauFal == 0`` falls through to ``2 × max(rebalse_cost) × 3.6``."""
    writer = _make_writer(
        whitelist=["MACHICURA"],
        plpmat=FakePlpmatParser(flow_fail_cost=0.0, vert_cost=50.0),
        vrebemb=FakeVrebembParser(
            [
                {"name": "A", "volume": 0.0, "cost": 100.0},
                {"name": "B", "volume": 0.0, "cost": 250.0},  # max
                {"name": "C", "volume": 0.0, "cost": 75.0},
            ]
        ),
    )
    assert writer._resolve_fail_cost() == 2.0 * 250.0 * 3.6


def test_fail_cost_uses_cvert_when_ccaufal_and_rebalse_zero() -> None:
    """Both above absent → ``2 × CVert × 3.6``."""
    writer = _make_writer(
        whitelist=["MACHICURA"],
        plpmat=FakePlpmatParser(flow_fail_cost=0.0, vert_cost=50.0),
        vrebemb=FakeVrebembParser([]),
    )
    assert writer._resolve_fail_cost() == 2.0 * 50.0 * 3.6


def test_fail_cost_default_fallback_when_nothing_available() -> None:
    """No parsers, no override → final ``_DEFAULT_FAIL_COST`` constant."""
    writer = _make_writer(whitelist=["MACHICURA"])
    assert writer._resolve_fail_cost() == _DEFAULT_FAIL_COST


# ---------------------------------------------------------------------------
# 4. Whitelist filtering: non-whitelisted centrals are not touched
# ---------------------------------------------------------------------------


def test_central_not_in_whitelist_is_skipped(tmp_path: Path) -> None:
    """A central present in central_parser but absent from the whitelist
    is left untouched — no FlowRight, no parquet, no pmin zero-out."""
    machicura = _machicura_central(pmin=10.0, efficiency=0.5)
    pangue = {
        "name": "PANGUE",
        "number": 2,
        "bus": 102,
        "pmin": 20.0,
        "pmax": 50.0,
        "efficiency": 0.8,
        "ser_hid": 0,
        "ser_ver": 0,
        "afluent": 0.0,
        "type": "serie",
    }
    writer = _make_writer(
        whitelist=["MACHICURA"],  # only MACHICURA — PANGUE excluded
        centrals=[machicura, pangue],
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )

    planning = _planning_with_machicura()
    # Append PANGUE generator to verify it is not zeroed.
    planning["generator_array"].append(
        {"name": "PANGUE", "uid": 2, "bus": 102, "pmin": 20.0}
    )

    result = writer.process(planning)

    # Only one FlowRight (MACHICURA).
    assert [r["name"] for r in result] == [f"MACHICURA{_FLOW_RIGHT_SUFFIX}"]

    # PANGUE generator unchanged.
    pangue_gen = next(g for g in planning["generator_array"] if g["name"] == "PANGUE")
    assert pangue_gen["pmin"] == 20.0

    # No parquet for PANGUE.
    assert not (tmp_path / "FlowRight" / f"PANGUE{_FLOW_RIGHT_SUFFIX}.parquet").exists()


def test_csv_enabled_false_excludes_central(tmp_path: Path) -> None:
    """A central with ``enabled=false`` in the whitelist CSV is skipped."""
    csv_file = tmp_path / "wl.csv"
    csv_file.write_text(
        "name,enabled,description\nMACHICURA,true,doc\nPANGUE,false,doc\n"
    )
    names = resolve_whitelist(str(csv_file), default_csv=tmp_path / "ignored.csv")
    assert names == ["MACHICURA"]
    assert "PANGUE" not in names


# ---------------------------------------------------------------------------
# 5. Unknown central path: warns + skips, doesn't crash
# ---------------------------------------------------------------------------


def test_unknown_central_warns_and_skips(tmp_path: Path, caplog: Any) -> None:
    """Whitelist entry not found in central_parser → warn + skip + no crash."""
    writer = _make_writer(
        whitelist=["GHOST_CENTRAL"],  # not in central_parser
        centrals=[_machicura_central()],  # only MACHICURA known
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )
    planning = _planning_with_machicura()

    with caplog.at_level(logging.WARNING):
        result = writer.process(planning)

    assert not result
    # The central was missing from central_parser, AND missing from the
    # gen-junction map (because there is no GHOST_CENTRAL turbine) —
    # either warning is acceptable; what matters is at least one fired.
    assert any("GHOST_CENTRAL" in r.message for r in caplog.records)
    # No parquet emitted.
    assert not (tmp_path / "FlowRight").exists() or not list(
        (tmp_path / "FlowRight").iterdir()
    )
    # Generator pmin untouched.
    assert planning["generator_array"][0]["pmin"] == 12.0
