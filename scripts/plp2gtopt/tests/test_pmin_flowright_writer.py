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

from pathlib import Path
from typing import Any, Dict, List, Optional

import numpy as np
import pyarrow.parquet as pq

from ..pmin_flowright_writer import (
    PminFlowRightWriter,
    _DEFAULT_FAIL_COST,
    _WATERWAY_FLOW_RIGHT_SUFFIX,
    _WATERWAY_FLOW_RIGHT_UID_START,
    resolve_flow_right_fail_cost,
    resolve_whitelist,
)

#: Suffix the legacy gen-pmin → FlowRight path used (now replaced by
#: soft pmin_fcost).  Kept here purely so the tests below can assert that
#: no such FlowRight is emitted any more.
_FLOW_RIGHT_SUFFIX = "_pmin_as_flow_right"


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
            # ``generator`` carries the generator NAME (as JunctionWriter
            # emits it), so the soft-pmin step can match this turbine to
            # the MACHICURA generator entry.
            {
                "name": "MACHICURA",
                "waterway": "MACHICURA_1_2",
                "generator": "MACHICURA",
            },
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
# 2. Soft hydro pmin: pmin_fcost set, pmin kept, no FlowRight emitted
# ---------------------------------------------------------------------------


def test_soft_pmin_sets_fcost_and_keeps_pmin(tmp_path: Path) -> None:
    """A hydro generator with ``pmin > 0`` keeps ``pmin`` and gains
    ``pmin_fcost`` equal to the water-value anchor; no FlowRight is
    emitted for the generator pmin.

    The anchor is pinned by passing ``water_fail_cost=100.0`` so the
    resolver returns exactly ``100.0`` (the explicit override bypasses
    the auto formula), making the assertion deterministic.
    """
    central = _machicura_central(pmin=12.0, efficiency=0.5)
    writer = _make_writer(
        whitelist=["MACHICURA"],
        centrals=[central],
        # water_fail_cost activates the resolver AND fixes anchor=100.0.
        options={
            "output_dir": tmp_path,
            "output_format": "parquet",
            "water_fail_cost": 100.0,
        },
    )

    planning = _planning_with_machicura()
    result = writer.process(planning)

    gen = planning["generator_array"][0]
    # pmin_fcost == anchor (= explicit water_fail_cost), pmin kept.
    assert gen["pmin_fcost"] == 100.0
    assert gen["pmin"] == 12.0

    # No FlowRight emitted (the soft-pmin step emits none; there is no
    # waterway-fmin floor in this fixture either).
    assert not result
    assert not any(
        str(fr.get("name", "")).endswith(_FLOW_RIGHT_SUFFIX)
        for fr in planning.get("flow_right_array", []) or []
    )
    # No per-FlowRight parquet was written for the generator pmin.
    assert not (
        tmp_path / "FlowRight" / f"MACHICURA{_FLOW_RIGHT_SUFFIX}.parquet"
    ).exists()


def test_soft_pmin_noop_without_water_value_anchor(tmp_path: Path) -> None:
    """With no water-value anchor (no ``water_fail_cost`` /
    ``auto_water_fail_cost``), the resolver is inactive and the generator
    is left completely untouched — no ``pmin_fcost``, ``pmin`` kept."""
    central = _machicura_central(pmin=12.0, efficiency=0.5)
    writer = _make_writer(
        whitelist=["MACHICURA"],
        centrals=[central],
        options={"output_dir": tmp_path, "output_format": "parquet"},
    )

    planning = _planning_with_machicura()
    result = writer.process(planning)

    gen = planning["generator_array"][0]
    assert "pmin_fcost" not in gen
    assert gen["pmin"] == 12.0
    assert not result


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


def test_resolve_flow_right_fail_cost_free_function_branches() -> None:
    """The shared free function (used by both PminFlowRightWriter and the
    JunctionWriter irrigation-diversion path) returns the same value across
    every branch of the resolution cascade — exercised directly, including
    the ``None``-parser fallbacks the method-based tests cannot reach.
    """
    plpmat = FakePlpmatParser(flow_fail_cost=7000.0, vert_cost=999.0)
    rebalse = FakeVrebembParser([{"name": "X", "volume": 0.0, "cost": 250.0}])

    # 0. CLI override wins, converted $/Hm³ → $/(m³/s·h) via × 3.6.
    assert (
        resolve_flow_right_fail_cost({"flow_right_fail_cost": 42.0}, plpmat, rebalse)
        == 42.0 * 3.6
    )
    # 1. CCauFal × 3.6.
    assert resolve_flow_right_fail_cost(None, plpmat, rebalse) == 7000.0 * 3.6
    # 2. CCauFal == 0 → 2 × max(rebalse) × 3.6.
    assert (
        resolve_flow_right_fail_cost(
            None, FakePlpmatParser(flow_fail_cost=0.0, vert_cost=50.0), rebalse
        )
        == 2.0 * 250.0 * 3.6
    )
    # 3. CCauFal and rebalse absent → 2 × CVert × 3.6.
    assert (
        resolve_flow_right_fail_cost(
            None,
            FakePlpmatParser(flow_fail_cost=0.0, vert_cost=50.0),
            FakeVrebembParser([]),
        )
        == 2.0 * 50.0 * 3.6
    )
    # 4. Nothing available (both parsers None) → default constant.
    assert resolve_flow_right_fail_cost(None, None, None) == _DEFAULT_FAIL_COST


def test_resolve_flow_right_fail_cost_matches_method() -> None:
    """``PminFlowRightWriter._resolve_fail_cost`` is a thin delegate — its
    result must equal the shared free function on identical inputs, so the
    irrigation-diversion FlowRights and the soft-flow-right transform pick
    the IDENTICAL number.
    """
    plpmat = FakePlpmatParser(flow_fail_cost=7000.0, vert_cost=999.0)
    rebalse = FakeVrebembParser([{"name": "X", "volume": 0.0, "cost": 100.0}])
    writer = _make_writer(whitelist=["MACHICURA"], plpmat=plpmat, vrebemb=rebalse)
    assert writer._resolve_fail_cost() == resolve_flow_right_fail_cost(
        writer.options, writer.plpmat_parser, writer.vrebemb_parser
    )


# ---------------------------------------------------------------------------
# 4. Whitelist CSV filtering (resolve_whitelist enabled flag)
# ---------------------------------------------------------------------------


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
    assert fr["junction_a"] == "LOS_CONDORES"
    assert fr["direction"] == -1
    assert fr["target"] == fr["name"]
    assert fr["fcost"] == _DEFAULT_FAIL_COST

    # Waterway hard floor zeroed
    assert planning["waterway_array"][0]["fmin"] == 0.0

    # Parquet content
    parquet_file = tmp_path / "FlowRight" / f"{fr['name']}.parquet"
    assert parquet_file.is_file()
    table = pq.read_table(parquet_file)
    # Long layout: columns [block, stage, uid, value]; the FlowRight uid
    # lands in the ``uid`` column and its trajectory in ``value``.
    assert "uid" in table.column_names and "value" in table.column_names
    uids = table.column("uid").to_pylist()
    assert set(uids) == {_WATERWAY_FLOW_RIGHT_UID_START}
    flows = table.column("value").to_pylist()
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
