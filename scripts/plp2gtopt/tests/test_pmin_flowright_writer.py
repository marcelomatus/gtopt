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
    _WATERWAY_FLOW_RIGHT_SUFFIX,
    _WATERWAY_FLOW_RIGHT_UID_START,
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
    assert fr["fcost"] == _DEFAULT_FAIL_COST

    # Generator pmin zeroed
    assert planning["generator_array"][0]["pmin"] == 0.0

    # Parquet file written under <output_dir>/FlowRight/
    parquet_file = tmp_path / "FlowRight" / f"{fr['name']}.parquet"
    assert parquet_file.is_file()

    table = pq.read_table(parquet_file)
    cols = table.column_names
    uid_col = f"uid:{_FLOW_RIGHT_UID_START}"
    # FlowRight.{fmin,fmax,target,discharge} are now OptTBRealFieldSched
    # (Stage × Block, scenario-independent), so the parquet carries only
    # `block` / `stage` UID columns.  Emitting a `scenario` column would
    # produce duplicate (stage, block) keys on the 2D reader — gtopt
    # treats that as a hard error.
    assert cols == ["block", uid_col, "stage"]
    # flow_min = pmin / Rendi = [10/0.5, 20/0.5] = [20, 40]
    flows = table.column(uid_col).to_pylist()
    assert flows == [20.0, 40.0]

    blocks_out = table.column("block").to_pylist()
    stages_out = table.column("stage").to_pylist()
    assert blocks_out == [1, 2]
    assert stages_out == [1, 1]

    # planning_system was mutated in place
    assert planning.get("flow_right_array") == [fr]


def test_writer_converts_per_block_pmin_trajectory_with_static_fallback(
    tmp_path: Path,
) -> None:
    """The plpmance per-block pmin trajectory is preserved bit-exact in the
    emitted FlowRight parquet (= pmin / Rendi per block); blocks NOT in
    the mance map fall back to the central's static pmin.

    Mirrors the conversion verified end-to-end on juan/IPLP after the
    Generator/pmin column drop: every block of the case is covered, and
    each block's flow_min equals the source pmin / Rendi (no rounding,
    no clipping, no per-stage averaging).  Blocks present in mance with
    pmin = 0 STAY at zero (mance overrides static for those blocks);
    blocks ABSENT from mance use static_pmin / Rendi as the default.
    """
    central = _machicura_central(pmin=12.0, efficiency=0.5)
    # 6-block case spanning 3 stages — wider than the default 2-block
    # fixture so we can see the per-block resolution clearly.
    blocks = [
        {"number": 1, "stage": 1, "duration": 1.0},
        {"number": 2, "stage": 1, "duration": 1.0},
        {"number": 3, "stage": 2, "duration": 1.0},
        {"number": 4, "stage": 2, "duration": 1.0},
        {"number": 5, "stage": 3, "duration": 1.0},
        {"number": 6, "stage": 3, "duration": 1.0},
    ]
    # mance defines a NON-uniform trajectory for blocks 1, 3, 5; blocks 2,
    # 4, 6 are absent from the mance map and should use static_pmin=12.
    # Note block 3's pmin is 0 — that overrides the static (mance wins).
    mance = {
        "name": "MACHICURA",
        "block": np.array([1, 3, 5], dtype=np.int64),
        "pmin": np.array([10.0, 0.0, 25.0], dtype=np.float64),
        "pmax": np.array([100.0, 100.0, 100.0], dtype=np.float64),
    }
    writer = _make_writer(
        whitelist=["MACHICURA"],
        centrals=[central],
        mances=[mance],
        blocks=blocks,
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )
    planning = _planning_with_machicura()
    writer.process(planning)

    parquet_file = tmp_path / "FlowRight" / f"MACHICURA{_FLOW_RIGHT_SUFFIX}.parquet"
    assert parquet_file.is_file()

    table = pq.read_table(parquet_file)
    uid_col = f"uid:{_FLOW_RIGHT_UID_START}"
    blocks_out = table.column("block").to_pylist()
    flows_out = table.column(uid_col).to_pylist()

    # Expected per-block flow_min:
    #   block 1: mance pmin = 10.0  → 10/0.5 = 20.0 m³/s
    #   block 2: not in mance       → static 12/0.5 = 24.0 m³/s
    #   block 3: mance pmin = 0.0   → 0/0.5  =  0.0 m³/s  (mance wins)
    #   block 4: not in mance       → static 12/0.5 = 24.0 m³/s
    #   block 5: mance pmin = 25.0  → 25/0.5 = 50.0 m³/s
    #   block 6: not in mance       → static 12/0.5 = 24.0 m³/s
    expected = [(1, 20.0), (2, 24.0), (3, 0.0), (4, 24.0), (5, 50.0), (6, 24.0)]
    assert list(zip(blocks_out, flows_out)) == expected


def test_writer_no_mance_uses_static_pmin_uniformly(tmp_path: Path) -> None:
    """When ``mance_parser`` has no entry for the central, every block's
    flow_min is the constant ``static_pmin / Rendi``.

    Covers the fallback branch in ``_build_flow_values`` for centrals
    that have a non-zero static pmin but no plpmance trajectory.
    """
    central = _machicura_central(pmin=8.0, efficiency=0.5)
    writer = _make_writer(
        whitelist=["MACHICURA"],
        centrals=[central],
        mances=[],  # empty — get_mance_by_name returns None
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )
    planning = _planning_with_machicura()
    writer.process(planning)

    parquet_file = tmp_path / "FlowRight" / f"MACHICURA{_FLOW_RIGHT_SUFFIX}.parquet"
    table = pq.read_table(parquet_file)
    uid_col = f"uid:{_FLOW_RIGHT_UID_START}"
    flows = table.column(uid_col).to_pylist()
    # Every block uses static 8 / 0.5 = 16.0
    assert flows == [16.0, 16.0]


def test_writer_drops_generator_pmin_parquet_column(tmp_path: Path) -> None:
    """After conversion, the converted central's column is removed from
    ``Generator/pmin.parquet`` so the per-stage trajectory cannot
    silently re-enforce a hard pmin alongside the new soft FlowRight.

    Without this, gtopt's parser COULD (depending on its precedence
    rules) read the parquet column and apply the trajectory as a hard
    schedule even though ``Generator.pmin = 0.0`` in the JSON, double-
    counting the obligation.
    """
    import pandas as pd  # noqa: PLC0415

    central = _machicura_central(pmin=10.0, efficiency=0.5)
    mance = {
        "name": "MACHICURA",
        "block": np.array([1, 2], dtype=np.int64),
        "pmin": np.array([10.0, 20.0], dtype=np.float64),
        "pmax": np.array([100.0, 100.0], dtype=np.float64),
    }
    # Pre-create Generator/pmin.parquet with a column for MACHICURA's
    # generator UID and one for an unrelated thermal generator.  After
    # conversion, only MACHICURA's column should be dropped.
    gen_dir = tmp_path / "Generator"
    gen_dir.mkdir(parents=True, exist_ok=True)
    initial = pd.DataFrame(
        {
            "block": [1, 2],
            "uid:1": [10.0, 20.0],  # MACHICURA (generator uid=1)
            "uid:99": [50.0, 60.0],  # unrelated thermal — must be preserved
        }
    )
    initial.to_parquet(gen_dir / "pmin.parquet", index=False)

    writer = _make_writer(
        whitelist=["MACHICURA"],
        centrals=[central],
        mances=[mance],
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )
    planning = _planning_with_machicura()
    writer.process(planning)

    after = pd.read_parquet(gen_dir / "pmin.parquet")
    assert "uid:1" not in after.columns, (
        f"MACHICURA's pmin column should have been dropped: {list(after.columns)}"
    )
    assert "uid:99" in after.columns, (
        f"unrelated generator's pmin column should be preserved: {list(after.columns)}"
    )
    # The remaining column's data is untouched.
    assert after["uid:99"].tolist() == [50.0, 60.0]


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


# ---------------------------------------------------------------------------
# 5. Waterway fmin → FlowRight transform (transit centrals, bus = 0)
# ---------------------------------------------------------------------------


def _planning_with_waterway_fmin_scalar() -> Dict[str, Any]:
    """Planning system with a waterway carrying a positive scalar ``fmin``."""
    return {
        "generator_array": [],
        "waterway_array": [
            {
                "uid": 1,
                "name": "LMAULE_gen_1_2",
                "junction_a": "LMAULE",
                "junction_b": "LOS_CONDORES",
                "fmin": 17.5,
            }
        ],
        "turbine_array": [],
    }


def test_waterway_fmin_scalar_emits_flow_right(tmp_path: Path) -> None:
    """Positive scalar ``fmin`` on a waterway → soft FlowRight, fmin zeroed."""
    writer = _make_writer(
        whitelist=[],  # no gen-pmin whitelist; only waterway path runs
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )
    planning = _planning_with_waterway_fmin_scalar()

    result = writer.process(planning)

    assert len(result) == 1
    fr = result[0]
    assert fr["uid"] == _WATERWAY_FLOW_RIGHT_UID_START
    assert fr["name"] == f"LMAULE_gen_1_2{_WATERWAY_FLOW_RIGHT_SUFFIX}"
    assert fr["junction"] == "LOS_CONDORES"
    assert fr["direction"] == -1
    assert fr["discharge"] == fr["name"]
    assert fr["fcost"] == _DEFAULT_FAIL_COST

    # Waterway hard floor zeroed
    assert planning["waterway_array"][0]["fmin"] == 0.0

    # Parquet content
    parquet_file = tmp_path / "FlowRight" / f"{fr['name']}.parquet"
    assert parquet_file.is_file()
    table = pq.read_table(parquet_file)
    uid_col = f"uid:{_WATERWAY_FLOW_RIGHT_UID_START}"
    assert uid_col in table.column_names
    flows = table.column(uid_col).to_pylist()
    # Scalar broadcast across all blocks
    assert flows == [17.5, 17.5]


def test_waterway_fmin_parquet_ref_emits_flow_right_and_drops_column(
    tmp_path: Path,
) -> None:
    """Per-block parquet trajectory survives, source column is removed."""
    import pandas as pd

    # Pre-populate Waterway/fmin.parquet with two columns; only uid:1 has
    # non-zero values.  The transform should convert uid:1 and leave the
    # parquet file with only uid:23 (or remove it entirely if uid:23 is
    # also empty).
    ww_dir = tmp_path / "Waterway"
    ww_dir.mkdir(parents=True, exist_ok=True)
    fmin_df = pd.DataFrame(
        {
            "block": np.array([1, 2], dtype=np.int32),
            "uid:1": np.array([17.5, 17.5], dtype=np.float64),
            "uid:23": np.array([4.7, 4.7], dtype=np.float64),
            "stage": np.array([1, 1], dtype=np.int32),
        }
    )
    fmin_df.to_parquet(ww_dir / "fmin.parquet", index=False)

    planning = {
        "generator_array": [],
        "waterway_array": [
            {
                "uid": 1,
                "name": "LMAULE_gen_1_2",
                "junction_a": "LMAULE",
                "junction_b": "LOS_CONDORES",
                "fmin": "fmin",  # parquet ref
            },
            {
                "uid": 23,
                "name": "B_LaMina_gen_3_5",
                "junction_a": "B_LaMina",
                "junction_b": "B_M_Isla",
                "fmin": "fmin",
            },
        ],
        "turbine_array": [],
    }

    writer = _make_writer(
        whitelist=[],
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )
    result = writer.process(planning)

    assert len(result) == 2
    names = sorted(fr["name"] for fr in result)
    assert names == [
        f"B_LaMina_gen_3_5{_WATERWAY_FLOW_RIGHT_SUFFIX}",
        f"LMAULE_gen_1_2{_WATERWAY_FLOW_RIGHT_SUFFIX}",
    ]
    # Both waterways had their hard fmin zeroed.
    for ww in planning["waterway_array"]:
        assert ww["fmin"] == 0.0

    # Both columns were converted → file was deleted (no surviving uid:N).
    assert not (ww_dir / "fmin.parquet").exists()


def test_waterway_fmin_zero_column_just_clears_ref(tmp_path: Path) -> None:
    """All-zero column: no FlowRight emitted; ref normalised to 0.0; column dropped."""
    import pandas as pd

    ww_dir = tmp_path / "Waterway"
    ww_dir.mkdir(parents=True, exist_ok=True)
    fmin_df = pd.DataFrame(
        {
            "block": np.array([1, 2], dtype=np.int32),
            "uid:1": np.array([0.0, 0.0], dtype=np.float64),
            "uid:23": np.array([4.7, 4.7], dtype=np.float64),
            "stage": np.array([1, 1], dtype=np.int32),
        }
    )
    fmin_df.to_parquet(ww_dir / "fmin.parquet", index=False)

    planning = {
        "generator_array": [],
        "waterway_array": [
            {
                "uid": 1,
                "name": "LMAULE_gen_1_2",
                "junction_a": "LMAULE",
                "junction_b": "LOS_CONDORES",
                "fmin": "fmin",
            },
            {
                "uid": 23,
                "name": "B_LaMina_gen_3_5",
                "junction_a": "B_LaMina",
                "junction_b": "B_M_Isla",
                "fmin": "fmin",
            },
        ],
        "turbine_array": [],
    }

    writer = _make_writer(
        whitelist=[],
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )
    result = writer.process(planning)

    # Only B_LaMina (uid:23) had non-zero values → 1 FR
    assert len(result) == 1
    assert result[0]["name"].startswith("B_LaMina_gen_3_5")
    # uid:1 ref was normalised to 0.0 (no dangling parquet ref); uid:23
    # was converted and zeroed.
    assert planning["waterway_array"][0]["fmin"] == 0.0
    assert planning["waterway_array"][1]["fmin"] == 0.0


def test_waterway_fmin_skipped_when_disabled() -> None:
    """``fmin = 0`` and ``fmin = None`` produce no FRs (negative cases)."""
    writer = _make_writer(
        whitelist=[],
        options={"output_format": "parquet"},  # no output_dir → no parquet read
    )
    planning = {
        "generator_array": [],
        "waterway_array": [
            {
                "uid": 1,
                "name": "Z1",
                "junction_a": "A",
                "junction_b": "B",
                "fmin": 0.0,
            },
            {
                "uid": 2,
                "name": "Z2",
                "junction_a": "A",
                "junction_b": "B",
            },
        ],
        "turbine_array": [],
    }
    result = writer.process(planning)
    assert not result


def test_parquet_emits_one_row_per_stage_block_pair(tmp_path: Path) -> None:
    """Regression for the 2026-05-16 fix: the writer must NOT replicate
    rows per scenario.

    `FlowRight.{fmin,fmax,target,discharge}` are `OptTBRealFieldSched`
    (2D Stage×Block, scenario-independent); emitting a `scenario`
    column would produce duplicate (stage, block) keys for the 2D
    Arrow reader and the C++ side now throws on duplicate UID keys.
    """
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
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )
    result = writer.process(_planning_with_machicura())
    assert len(result) == 1

    fr = result[0]
    parquet_file = tmp_path / "FlowRight" / f"{fr['name']}.parquet"
    table = pq.read_table(parquet_file)
    assert table.column_names == ["block", f"uid:{_FLOW_RIGHT_UID_START}", "stage"]
    # Exactly one row per (stage, block) pair — never replicated per scenario.
    assert table.num_rows == 2  # 2 blocks × 1 stage = 2 rows
    assert table.column("stage").to_pylist() == [1, 1]
    assert table.column("block").to_pylist() == [1, 2]
