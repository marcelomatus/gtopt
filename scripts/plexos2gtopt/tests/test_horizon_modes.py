"""Unit tests for the dual ``--horizon-mode`` design (PLEXOS-native
vs hourly).

Covers:
  * ``plexos_csv.read_wide`` / ``read_long`` ``n_days`` widening
  * ``gtopt_writer._aggregate_to_blocks`` per-block reducers
  * ``gtopt_writer.build_simulation`` emits a block_array matching the
    bundle's layout (hourly vs PLEXOS-native)
  * ``gtopt_writer.build_demand_array`` / ``build_generator_array`` /
    ``build_flow_array`` aggregate their profiles to the same block
    count as the simulation
  * ``plexos_block_layout`` helpers parse a synthetic .accdb dump and
    refuse gracefully when the accdb is missing
"""

from __future__ import annotations

from pathlib import Path
from unittest.mock import patch

from plexos2gtopt.entities import (
    BundleSpec,
    DemandSpec,
    FlowSpec,
    GeneratorSpec,
)
from plexos2gtopt.gtopt_writer import (
    _aggregate_to_blocks,
    build_demand_array,
    build_flow_array,
    build_generator_array,
    build_simulation,
)
from plexos2gtopt.plexos_block_layout import (
    auto_discover_res_zip,
    load_block_layout_from_accdb,
    parse_user_block_layout,
)
from plexos2gtopt.plexos_csv import read_long, read_wide


# ---------------------------------------------------------------------------
# CSV readers — n_days widening
# ---------------------------------------------------------------------------


def test_read_wide_concatenates_two_days(tmp_path: Path) -> None:
    """``n_days = 2`` returns 48-element profiles, day-1 then day-2."""
    csv_path = tmp_path / "Nod_Load.csv"
    csv_path.write_text(
        "YEAR,MONTH,DAY,PERIOD,bus_a,bus_b\n"
        # Day 1: bus_a ramps 100→124, bus_b constant 50
        + "\n".join(f"2026,1,1,{p},{100 + p - 1},50" for p in range(1, 25))
        + "\n"
        # Day 2: bus_a constant 200, bus_b ramps 60→83
        + "\n".join(f"2026,1,2,{p},200,{60 + p - 1}" for p in range(1, 25))
        + "\n"
    )
    out = read_wide(csv_path, n_days=2)
    assert set(out) == {"bus_a", "bus_b"}
    assert len(out["bus_a"]) == 48
    assert len(out["bus_b"]) == 48
    # Day-1 prefix
    assert out["bus_a"][0] == 100
    assert out["bus_a"][23] == 123
    assert out["bus_b"][0] == 50
    # Day-2 suffix
    assert out["bus_a"][24] == 200
    assert out["bus_a"][47] == 200
    assert out["bus_b"][24] == 60
    assert out["bus_b"][47] == 83


def test_read_long_concatenates_two_days(tmp_path: Path) -> None:
    """``n_days = 2`` widens long-format series similarly."""
    csv_path = tmp_path / "Gen_Rating.csv"
    csv_path.write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        # Gen 'A' has day-1 = 100, day-2 = 200
        + "\n".join(f"A,2026,1,1,{p},1,100" for p in range(1, 25))
        + "\n"
        + "\n".join(f"A,2026,1,2,{p},1,200" for p in range(1, 25))
        + "\n"
    )
    out = read_long(csv_path, n_days=2)
    assert len(out["A"]) == 48
    assert out["A"][:24] == [100.0] * 24
    assert out["A"][24:] == [200.0] * 24


def test_read_wide_default_n_days_is_backward_compatible(tmp_path: Path) -> None:
    """``n_days = 1`` keeps the legacy 24-element shape exactly."""
    csv_path = tmp_path / "Nod_Load.csv"
    csv_path.write_text(
        "YEAR,MONTH,DAY,PERIOD,bus_a\n"
        + "\n".join(f"2026,1,1,{p},{p}" for p in range(1, 25))
        + "\n"
    )
    out = read_wide(csv_path)
    assert len(out["bus_a"]) == 24
    assert out["bus_a"][0] == 1
    assert out["bus_a"][23] == 24


# ---------------------------------------------------------------------------
# Aggregator: hourly profile → block profile
# ---------------------------------------------------------------------------


def test_aggregate_to_blocks_mean() -> None:
    """``mean`` reducer averages the constituent hourly values."""
    hourly = [1.0, 2.0, 3.0, 4.0, 5.0, 6.0]
    layout = ((1, 2, 3), (4, 5, 6))
    out = _aggregate_to_blocks(hourly, layout, reducer="mean")
    assert out == [2.0, 5.0]


def test_aggregate_to_blocks_min_for_capacity() -> None:
    """``min`` reducer picks the most restrictive value in each block."""
    hourly = [10.0, 5.0, 8.0, 12.0, 3.0, 7.0]
    layout = ((1, 2, 3), (4, 5, 6))
    out = _aggregate_to_blocks(hourly, layout, reducer="min")
    assert out == [5.0, 3.0]


def test_aggregate_to_blocks_sum_for_energy() -> None:
    hourly = [10.0, 20.0, 5.0, 5.0]
    layout = ((1, 2), (3, 4))
    out = _aggregate_to_blocks(hourly, layout, reducer="sum")
    assert out == [30.0, 10.0]


def test_aggregate_to_blocks_empty_layout_returns_input() -> None:
    """No layout ⇒ pass-through (hourly mode behaviour)."""
    hourly = [1.0, 2.0, 3.0]
    assert _aggregate_to_blocks(hourly, (), reducer="mean") == hourly


def test_aggregate_to_blocks_handles_out_of_range_intervals() -> None:
    """Intervals beyond the profile length contribute 0; missing
    blocks fall back to 0 to keep the output length stable."""
    hourly = [1.0, 2.0]
    layout = ((1, 2, 99), (100,))  # last block has only out-of-range intervals
    out = _aggregate_to_blocks(hourly, layout, reducer="mean")
    # Block 1: mean(1, 2) = 1.5 (99 is dropped); block 2: 0 (empty)
    assert out == [1.5, 0.0]


# ---------------------------------------------------------------------------
# build_simulation: emits the right block_array per horizon mode
# ---------------------------------------------------------------------------


def test_build_simulation_hourly_default_one_day() -> None:
    """``n_days = 1`` → 24-block simulation (legacy behaviour)."""
    sim = build_simulation(BundleSpec())
    assert len(sim["block_array"]) == 24
    assert all(b["duration"] == 1.0 for b in sim["block_array"])
    assert sim["stage_array"][0]["count_block"] == 24


def test_build_simulation_hourly_seven_days() -> None:
    """``n_days = 7`` → 168 uniform hourly blocks."""
    sim = build_simulation(BundleSpec(n_days=7))
    assert len(sim["block_array"]) == 168
    assert all(b["duration"] == 1.0 for b in sim["block_array"])


def test_build_simulation_plexos_layout() -> None:
    """A non-empty ``block_layout`` is emitted block-by-block with
    duration = ``len(intervals)``."""
    # 3 blocks: 1-hour, 2-hour aggregated, 3-hour aggregated
    layout = (
        (1,),
        (2, 3),
        (4, 5, 6),
    )
    sim = build_simulation(BundleSpec(block_layout=layout))
    assert len(sim["block_array"]) == 3
    assert sim["block_array"][0]["duration"] == 1.0
    assert sim["block_array"][1]["duration"] == 2.0
    assert sim["block_array"][2]["duration"] == 3.0
    assert sim["stage_array"][0]["count_block"] == 3


# ---------------------------------------------------------------------------
# Per-array aggregation in the writer
# ---------------------------------------------------------------------------


def test_build_demand_array_aggregates_under_plexos_layout() -> None:
    """``DemandSpec.lmax_profile`` is aggregated to one value per block."""
    layout = ((1, 2, 3), (4,))
    out = build_demand_array(
        (
            DemandSpec(
                name="load_x",
                bus_name="bus_x",
                lmax_profile=(100.0, 110.0, 120.0, 200.0),
            ),
        ),
        block_layout=layout,
    )
    # mean(100, 110, 120) = 110; block 2 = mean(200) = 200
    assert out[0]["lmax"] == [[110.0, 200.0]]


def test_build_demand_array_empty_layout_keeps_full_profile() -> None:
    out = build_demand_array(
        (
            DemandSpec(
                name="load_x",
                bus_name="bus_x",
                lmax_profile=(1.0, 2.0, 3.0),
            ),
        )
    )
    assert out[0]["lmax"] == [[1.0, 2.0, 3.0]]


def test_build_generator_array_aggregates_pmax_profile() -> None:
    """A varying renewable pmax_profile is aggregated to the layout."""
    layout = ((1, 2), (3, 4))
    out = build_generator_array(
        (
            GeneratorSpec(
                object_id=1,
                name="solar",
                bus_name="bus_a",
                pmax=100.0,
                pmax_profile=(50.0, 70.0, 30.0, 10.0),
            ),
        ),
        block_layout=layout,
    )
    # mean(50, 70) = 60; mean(30, 10) = 20
    assert out[0]["pmax"] == [[60.0, 20.0]]


def test_build_flow_array_aggregates_discharge() -> None:
    layout = ((1, 2), (3,))
    out = build_flow_array(
        (
            FlowSpec(
                name="flow_x",
                junction_name="j_x",
                discharge_profile=(100.0, 200.0, 300.0),
            ),
        ),
        block_layout=layout,
    )
    assert out[0]["discharge"] == [[[150.0, 300.0]]]


# ---------------------------------------------------------------------------
# Block layout loaders: graceful fallback
# ---------------------------------------------------------------------------


def test_load_block_layout_from_accdb_missing_file(tmp_path: Path) -> None:
    """Missing accdb ⇒ empty tuple, no exception."""
    out = load_block_layout_from_accdb(tmp_path / "does_not_exist.accdb")
    assert not out


def test_load_block_layout_from_accdb_without_mdbtools(tmp_path: Path) -> None:
    """mdb-export missing ⇒ empty tuple, no exception."""
    fake_accdb = tmp_path / "fake.accdb"
    fake_accdb.write_bytes(b"fake")
    with patch("plexos2gtopt.plexos_block_layout._have_mdb_tools", return_value=False):
        out = load_block_layout_from_accdb(fake_accdb)
    assert not out


def test_load_block_layout_from_accdb_parses_t_phase_3(tmp_path: Path) -> None:
    """``mdb-export`` stdout with interval_id,period_id → layout tuples."""
    fake_accdb = tmp_path / "fake.accdb"
    fake_accdb.write_bytes(b"")
    # 5 hourly intervals; PLEXOS groups them as: [1], [2, 3], [4, 5].
    fake_stdout = "interval_id,period_id\n1,1\n2,2\n3,2\n4,3\n5,3\n"
    with patch("plexos2gtopt.plexos_block_layout._have_mdb_tools", return_value=True):
        with patch("plexos2gtopt.plexos_block_layout.subprocess.run") as run:
            run.return_value.stdout = fake_stdout
            run.return_value.returncode = 0
            out = load_block_layout_from_accdb(fake_accdb)
    assert out == ((1,), (2, 3), (4, 5))


# ---------------------------------------------------------------------------
# auto_discover_res_zip: DATOS<date>.zip[.xz] → RES<date>.zip[.xz]
# ---------------------------------------------------------------------------


def test_auto_discover_res_zip_finds_sibling(tmp_path: Path) -> None:
    datos = tmp_path / "DATOS20260422.zip.xz"
    datos.write_bytes(b"")
    res = tmp_path / "RES20260422.zip.xz"
    res.write_bytes(b"")
    assert auto_discover_res_zip(datos) == res


def test_auto_discover_res_zip_returns_none_when_missing(
    tmp_path: Path,
) -> None:
    datos = tmp_path / "DATOS20260422.zip.xz"
    datos.write_bytes(b"")
    assert auto_discover_res_zip(datos) is None


# ---------------------------------------------------------------------------
# T5: read_long fill_forward carries the last defined value across gaps
# ---------------------------------------------------------------------------


def test_read_long_fill_forward_carries_value(tmp_path: Path) -> None:
    """PLEXOS DLR files ship sparse rows: period N's value carries
    forward until the next defined period.  ``fill_forward=True``
    mirrors that semantic; ``fill_forward=False`` (default) leaves
    gaps at 0.0 — the legacy behaviour that produced overnight
    rating drops for Dynamic Line Rating corridors when only
    period-1 had a row.
    """
    csv_path = tmp_path / "Lin_MaxRating.csv"
    csv_path.write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "line_ns,2026,1,1,1,1,500\n"
        "line_ns,2026,1,1,7,1,600\n"
    )
    out = read_long(csv_path, fill_forward=True, periods=24)
    series = out["line_ns"]
    assert len(series) == 24
    # Period 1 value lands at slot 0.
    assert series[0] == 500.0
    # Slots 1..5 inherit period-1's value via carry-forward.
    assert series[1] == 500.0
    assert series[5] == 500.0
    # Period 7 (slot 6) hits 600; the new value carries forward from there.
    assert series[6] == 600.0
    assert series[7] == 600.0
    assert series[23] == 600.0

    # fill_forward=False keeps the legacy gap-as-zero behaviour.
    out2 = read_long(csv_path, fill_forward=False, periods=24)
    series2 = out2["line_ns"]
    assert series2[0] == 500.0
    # Gaps stay 0 without the forward-fill.
    assert series2[1] == 0.0
    assert series2[5] == 0.0
    assert series2[6] == 600.0


# ---------------------------------------------------------------------------
# parse_user_block_layout: user-defined block grouping (CSV / inline string)
# ---------------------------------------------------------------------------


def test_parse_user_block_layout_inline_dict() -> None:
    """'{uid:dur,...}' builds the chronological interval grouping."""
    out = parse_user_block_layout("{1:1,2:4,3:2}")
    assert out == ((1,), (2, 3, 4, 5), (6, 7))
    assert [len(b) for b in out] == [1, 4, 2]
    assert sum(len(b) for b in out) == 7


def test_parse_user_block_layout_bare_list() -> None:
    """'d1,d2,...' (durations in block order) matches the dict form."""
    assert parse_user_block_layout("1,4,2") == ((1,), (2, 3, 4, 5), (6, 7))


def test_parse_user_block_layout_csv(tmp_path: Path) -> None:
    """CSV with a 'duration' column; 'block_uid' sets the order."""
    csv_path = tmp_path / "block_layout.csv"
    csv_path.write_text("block_uid,duration\n3,2\n1,1\n2,4\n")
    # Out-of-order rows are sorted by block_uid before grouping.
    assert parse_user_block_layout(str(csv_path)) == ((1,), (2, 3, 4, 5), (6, 7))


def test_parse_user_block_layout_rejects_empty() -> None:
    """A spec with no positive durations raises ValueError."""
    import pytest

    with pytest.raises(ValueError, match="no positive durations"):
        parse_user_block_layout("0,0")


def test_infer_horizon_days_from_input(tmp_path: Path) -> None:
    """The horizon day-count is parsed from the Horizon object NAME
    (``..._7d``) for the uniform-hourly fallback when no solution exists."""
    from plexos2gtopt.plexos_block_layout import infer_horizon_days_from_input

    xml = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml.write_text(
        "<MasterDataSet>"
        "<t_class><class_id>1</class_id><name>Horizon</name></t_class>"
        "<t_class><class_id>2</class_id><name>Generator</name></t_class>"
        "<t_object><object_id>10</object_id><class_id>1</class_id>"
        "<name>Coordinador_diario_1H_7d</name></t_object>"
        "<t_object><object_id>11</object_id><class_id>2</class_id>"
        "<name>g1</name></t_object>"
        "</MasterDataSet>"
    )
    assert infer_horizon_days_from_input(xml) == 7


def test_infer_horizon_days_missing_returns_none(tmp_path: Path) -> None:
    """No Horizon object / no ``Nd`` token ⇒ None (caller defaults)."""
    from plexos2gtopt.plexos_block_layout import infer_horizon_days_from_input

    xml = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml.write_text(
        "<MasterDataSet>"
        "<t_class><class_id>2</class_id><name>Generator</name></t_class>"
        "</MasterDataSet>"
    )
    assert infer_horizon_days_from_input(xml) is None
    assert infer_horizon_days_from_input(tmp_path / "ghost.xml") is None
