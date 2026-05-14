# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for _line_schedules.py — per-line parquet schedule
aggregation under corridor reduction.

Each test builds a tiny synthetic case (≤3 stages, ≤2 corridors), writes
parquet files to a temp dir, calls ``aggregate_line_schedules``, and
asserts the per-rule physics.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

import numpy as np
import pandas as pd
import pyarrow.parquet as pq
import pytest

from gtopt_reduce_network._aggregate import AggregatedLine
from gtopt_reduce_network._line_schedules import aggregate_line_schedules


# ─── Helpers ────────────────────────────────────────────────────────────


def _write_stage_parquet(
    path: Path, stages: list[int], cols: dict[int, list[float]], dtype: str = "float64"
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    df = pd.DataFrame({"stage": np.asarray(stages, dtype=np.int32)})
    for uid, vals in cols.items():
        df[f"uid:{uid}"] = np.asarray(vals, dtype=dtype)
    df.to_parquet(path, index=False)


def _write_block_parquet(
    path: Path,
    stages: list[int],
    blocks: list[int],
    cols: dict[int, list[float]],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    df = pd.DataFrame(
        {
            "block": np.asarray(blocks, dtype=np.int32),
        }
    )
    for uid, vals in cols.items():
        df[f"uid:{uid}"] = np.asarray(vals, dtype="float64")
    df["stage"] = np.asarray(stages, dtype=np.int32)
    df.to_parquet(path, index=False)


def _read_parquet(path: Path) -> pd.DataFrame:
    return pq.read_table(path).to_pandas()


def _line(uid: int, **kwargs: Any) -> dict[str, Any]:
    base: dict[str, Any] = {
        "uid": uid,
        "bus_a": 1,
        "bus_b": 2,
        "tmax_ab": 100.0,
        "tmax_ba": 100.0,
        "reactance": 0.1,
        "resistance": 0.01,
        "voltage": 1.0,
        "active": 1,
    }
    base.update(kwargs)
    return base


def _eq(eq_uid: int, absorbed: list[int], **kwargs: Any) -> AggregatedLine:
    return AggregatedLine(
        uid=eq_uid,
        name=f"agg_{eq_uid}",
        bus_a_uid=1,
        bus_b_uid=2,
        reactance=kwargs.get("reactance", 0.05),
        resistance=kwargs.get("resistance", 0.005),
        tmax_ab=kwargs.get("tmax_ab", 200.0),
        tmax_ba=kwargs.get("tmax_ba", 200.0),
        absorbed=absorbed,
    )


# ─── Rule: active OR ────────────────────────────────────────────────────


def test_active_or_across_disjoint_windows(tmp_path: Path) -> None:
    """3 originals; #1 active stages 0-1, #2 active stages 2-3, #3 always.
    Corridor active == 1 every stage (any covers it)."""
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    _write_stage_parquet(
        line_dir / "active.parquet",
        stages=[1, 2, 3, 4],
        cols={
            1: [1, 1, 0, 0],
            2: [0, 0, 1, 1],
            3: [1, 1, 1, 1],
        },
        dtype="int32",
    )
    field_to_stem = aggregate_line_schedules(
        case_dir=case_dir,
        input_directory=".",
        reduced_tag="t",
        aggregated_lines=[_eq(99, absorbed=[1, 2, 3])],
        original_line_array=[_line(1), _line(2), _line(3)],
    )
    assert "active" in field_to_stem
    df = _read_parquet(line_dir / "active_t.parquet")
    assert (df["uid:99"] == 1).all(), "Corridor must be active every stage"


def test_active_or_zero_when_all_inactive(tmp_path: Path) -> None:
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    _write_stage_parquet(
        line_dir / "active.parquet",
        stages=[1, 2, 3],
        cols={1: [1, 0, 1], 2: [0, 0, 1]},
        dtype="int32",
    )
    aggregate_line_schedules(
        case_dir=case_dir,
        input_directory=".",
        reduced_tag="t",
        aggregated_lines=[_eq(99, absorbed=[1, 2])],
        original_line_array=[_line(1), _line(2)],
    )
    df = _read_parquet(line_dir / "active_t.parquet")
    assert df["uid:99"].tolist() == [1, 0, 1]


# ─── Rule: tmax_ab gated-sum ───────────────────────────────────────────


def test_tmax_ab_gated_sum_with_maintenance(tmp_path: Path) -> None:
    """Two-line corridor; #2 out at stages 2-3 → tmax_ab(s≥2) = tmax_ab[1] only."""
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    _write_stage_parquet(
        line_dir / "active.parquet",
        stages=[1, 2, 3, 4],
        cols={1: [1, 1, 1, 1], 2: [1, 0, 0, 1]},
        dtype="int32",
    )
    # 8 blocks (2 per stage) — block index just incrementing, stage column 1,1,2,2,3,3,4,4
    _write_block_parquet(
        line_dir / "tmax_ab.parquet",
        stages=[1, 1, 2, 2, 3, 3, 4, 4],
        blocks=list(range(1, 9)),
        cols={
            1: [100.0] * 8,
            2: [50.0] * 8,
        },
    )
    aggregate_line_schedules(
        case_dir=case_dir,
        input_directory=".",
        reduced_tag="t",
        aggregated_lines=[_eq(99, absorbed=[1, 2])],
        original_line_array=[_line(1), _line(2)],
    )
    df = _read_parquet(line_dir / "tmax_ab_t.parquet")
    # stages 1, 4 → both active → 150; stages 2, 3 → only #1 → 100.
    assert df.sort_values("block")["uid:99"].tolist() == [
        150.0,
        150.0,  # stage 1, both active
        100.0,
        100.0,  # stage 2, #2 out
        100.0,
        100.0,  # stage 3, #2 out
        150.0,
        150.0,  # stage 4, both back
    ]


def test_tmax_ab_with_no_active_parquet_treats_all_active(tmp_path: Path) -> None:
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    _write_block_parquet(
        line_dir / "tmax_ab.parquet",
        stages=[1, 1],
        blocks=[1, 2],
        cols={1: [40.0, 60.0], 2: [30.0, 70.0]},
    )
    aggregate_line_schedules(
        case_dir=case_dir,
        input_directory=".",
        reduced_tag="t",
        aggregated_lines=[_eq(99, absorbed=[1, 2])],
        original_line_array=[_line(1), _line(2)],
    )
    df = _read_parquet(line_dir / "tmax_ab_t.parquet")
    assert df.sort_values("block")["uid:99"].tolist() == [70.0, 130.0]


# ─── Rule: voltage cap-weighted ────────────────────────────────────────


def test_voltage_cap_weighted(tmp_path: Path) -> None:
    """Two-line corridor: cap=100 @ 220 kV; cap=300 @ 500 kV → V_eq ≈ 430."""
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    _write_stage_parquet(
        line_dir / "voltage.parquet",
        stages=[1, 2],
        cols={1: [220.0, 220.0], 2: [500.0, 500.0]},
    )
    aggregate_line_schedules(
        case_dir=case_dir,
        input_directory=".",
        reduced_tag="t",
        aggregated_lines=[_eq(99, absorbed=[1, 2])],
        original_line_array=[
            _line(1, voltage=220.0, tmax_ab=100.0),
            _line(2, voltage=500.0, tmax_ab=300.0),
        ],
    )
    df = _read_parquet(line_dir / "voltage_t.parquet")
    # cap-weighted: (100·220 + 300·500) / (100 + 300) = 430.0
    assert df["uid:99"].tolist() == [pytest.approx(430.0), pytest.approx(430.0)]


# ─── Rule: parallel-X in p.u. ──────────────────────────────────────────


def test_parallel_x_uniform_voltage(tmp_path: Path) -> None:
    """Two parallel lines V=1, X=10, X=10 → X_eq=5 every stage."""
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    _write_stage_parquet(
        line_dir / "reactance.parquet",
        stages=[1, 2],
        cols={1: [10.0, 10.0], 2: [10.0, 10.0]},
    )
    aggregate_line_schedules(
        case_dir=case_dir,
        input_directory=".",
        reduced_tag="t",
        aggregated_lines=[_eq(99, absorbed=[1, 2])],
        original_line_array=[
            _line(1, reactance=10.0),
            _line(2, reactance=10.0),
        ],
    )
    df = _read_parquet(line_dir / "reactance_t.parquet")
    assert df["uid:99"].tolist() == [pytest.approx(5.0), pytest.approx(5.0)]


def test_parallel_x_handles_maintenance(tmp_path: Path) -> None:
    """3 originals, #2 out at stage 2 → X_eq(2) = parallel of {1,3} not {1,2,3}."""
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    _write_stage_parquet(
        line_dir / "active.parquet",
        stages=[1, 2, 3],
        cols={2: [1, 0, 1]},
        dtype="int32",
    )
    _write_stage_parquet(
        line_dir / "reactance.parquet",
        stages=[1, 2, 3],
        cols={1: [6.0] * 3, 2: [6.0] * 3, 3: [6.0] * 3},
    )
    aggregate_line_schedules(
        case_dir=case_dir,
        input_directory=".",
        reduced_tag="t",
        aggregated_lines=[_eq(99, absorbed=[1, 2, 3])],
        original_line_array=[
            _line(1, reactance=6.0),
            _line(2, reactance=6.0),
            _line(3, reactance=6.0),
        ],
    )
    df = _read_parquet(line_dir / "reactance_t.parquet")
    # Stage 1, 3: 3 parallels of 6 → 2.0; stage 2: 2 parallels → 3.0
    vals = df["uid:99"].tolist()
    assert vals[0] == pytest.approx(2.0)
    assert vals[1] == pytest.approx(3.0)
    assert vals[2] == pytest.approx(2.0)


# ─── Rule: resistance follows X (p.u.) ─────────────────────────────────


def test_resistance_follows_x_pu(tmp_path: Path) -> None:
    """R_eq = X_eq² · Σ R·act/X² · V²/MVA_base.  For 2 equal parallels with
    V=1, X=10, R=1, expect R_eq = 0.25 (= X_eq²·Σ R/X² = 25 · 2·1/100 = 0.5… no wait).

    Let me re-derive for V=1, MVA_base=100:
      X_pu = X · 100 / 1² = 1000  (huge)
      R_pu = R · 100 / 1² = 100
      X_pu_eq = 1 / (1/1000 + 1/1000) = 500
      R_pu_eq = 500² · (100/1000² + 100/1000²) = 250000 · 0.0002 = 50
      R_eq = R_pu_eq · 1 / 100 = 0.5

    For two identical parallels of X=10, R=1, expected R_eq = 0.5.
    """
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    _write_stage_parquet(
        line_dir / "resistance.parquet",
        stages=[1, 2],
        cols={1: [1.0, 1.0], 2: [1.0, 1.0]},
    )
    _write_stage_parquet(
        line_dir / "reactance.parquet",
        stages=[1, 2],
        cols={1: [10.0, 10.0], 2: [10.0, 10.0]},
    )
    aggregate_line_schedules(
        case_dir=case_dir,
        input_directory=".",
        reduced_tag="t",
        aggregated_lines=[_eq(99, absorbed=[1, 2])],
        original_line_array=[
            _line(1, reactance=10.0, resistance=1.0),
            _line(2, reactance=10.0, resistance=1.0),
        ],
    )
    df = _read_parquet(line_dir / "resistance_t.parquet")
    assert df["uid:99"].tolist() == [pytest.approx(0.5), pytest.approx(0.5)]


# ─── Unknown / unhandled fields warned and skipped ─────────────────────


def test_unhandled_field_logs_warning_and_skips(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    # lossfactor is in _UNHANDLED_FIELDS — should warn + skip.
    _write_stage_parquet(
        line_dir / "lossfactor.parquet",
        stages=[1, 2],
        cols={1: [0.01, 0.02]},
    )
    import logging

    with caplog.at_level(logging.WARNING):
        out = aggregate_line_schedules(
            case_dir=case_dir,
            input_directory=".",
            reduced_tag="t",
            aggregated_lines=[_eq(99, absorbed=[1])],
            original_line_array=[_line(1)],
        )
    assert "lossfactor" in caplog.text
    assert (line_dir / "lossfactor_t.parquet").exists() is False
    assert "lossfactor" not in out


def test_unknown_field_logs_warning_and_skips(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    _write_stage_parquet(
        line_dir / "completely_made_up_field.parquet",
        stages=[1],
        cols={1: [42.0]},
    )
    import logging

    with caplog.at_level(logging.WARNING):
        out = aggregate_line_schedules(
            case_dir=case_dir,
            input_directory=".",
            reduced_tag="t",
            aggregated_lines=[_eq(99, absorbed=[1])],
            original_line_array=[_line(1)],
        )
    assert "no aggregation rule" in caplog.text
    assert "completely_made_up_field" not in out


# ─── Empty / no-Line directory short-circuit ────────────────────────────


def test_no_line_dir_returns_empty(tmp_path: Path) -> None:
    out = aggregate_line_schedules(
        case_dir=tmp_path,
        input_directory=".",
        reduced_tag="t",
        aggregated_lines=[_eq(99, absorbed=[1])],
        original_line_array=[_line(1)],
    )
    assert not out


def test_empty_tag_short_circuits(tmp_path: Path) -> None:
    """reduced_tag='' is the off-switch for schedule aggregation."""
    case_dir = tmp_path / "case"
    line_dir = case_dir / "Line"
    _write_stage_parquet(
        line_dir / "active.parquet",
        stages=[1],
        cols={1: [1]},
        dtype="int32",
    )
    out = aggregate_line_schedules(
        case_dir=case_dir,
        input_directory=".",
        reduced_tag="",
        aggregated_lines=[_eq(99, absorbed=[1])],
        original_line_array=[_line(1)],
    )
    assert not out
    assert not list(line_dir.glob("*_*.parquet"))
