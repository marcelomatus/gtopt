# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for the commitment-feasibility seed repair.

Each rule is exercised on a synthetic fixture distilled from the real CEN
2026-04-12 rejection causes: LAUTARO order pairs, SANISIDRO config-flag
implications (with PAMPL's detached ``- 1 *`` signs), PANGUE inactive rows,
SAN_ANDRES maintenance windows, and min-up/down chattering.
"""

from __future__ import annotations

import json
from pathlib import Path

import pyarrow as pa
import pyarrow.parquet as pq
import pytest

from gtopt_warmstart.build_full_seed import (
    _commitment_meta,
    _commitment_to_generator,
    _order_pairs,
    _status_rows,
    build_full_seed,
    repair_seed,
    verify_seed,
)

BLOCKS = list(range(1, 9))  # 8 blocks


def _system() -> dict:
    return {
        "generator_array": [
            {"uid": 1, "name": "BL1", "pmax": 60.0},
            {"uid": 2, "name": "BL2", "pmax": 60.0},
            {"uid": 3, "name": "FLAG", "pmax": 100.0},
            {"uid": 4, "name": "CFG_A", "pmax": 100.0},
            {
                "uid": 5,
                "name": "MAINT",
                "pmax": [[10.0, 10.0, 0.0, 0.0, 10.0, 10.0, 10.0, 10.0]],
            },
            {"uid": 6, "name": "CHATTER", "pmax": 50.0},
        ],
        "commitment_array": [
            {"uid": 11, "name": "uc_BL1", "generator": "BL1", "pmin": 5.0},
            {"uid": 12, "name": "uc_BL2", "generator": "BL2", "pmin": 0.5},
            {"uid": 13, "name": "uc_FLAG", "generator": "FLAG", "pmin": 1.0},
            {"uid": 14, "name": "uc_CFG_A", "generator": "CFG_A", "pmin": 1.0},
            {"uid": 15, "name": "uc_MAINT", "generator": "MAINT", "pmin": 2.0},
            {
                "uid": 16,
                "name": "uc_CHATTER",
                "generator": "CHATTER",
                "pmin": 1.0,
                "min_up_time": 3.0,
                "min_down_time": 3.0,
                "initial_status": 0.0,
                "ini_hours_down": 5.0,
            },
        ],
    }


PAMPL = """\
# PLEXOS Constraint 'BL_Order': dispatch order
constraint BL_Order:
  -1 * generator("BL1").generation + 1 * generator("BL2").generation <= 0;

# flag <= config (detached '- 1 *' sign, as PAMPL prints mid-expression)
constraint Flag_Impl:
  1 * commitment("uc_CFG_A").status - 1 * commitment("uc_FLAG").status >= 0;

# excluded from the LP — must be ignored by the parser
inactive constraint Ghost_Row:
  1 * commitment("uc_BL1").status <= 0;
"""


@pytest.fixture()
def case_dir(tmp_path: Path) -> Path:
    (tmp_path / "case.json").write_text(json.dumps({"system": _system()}))
    (tmp_path / "uc_test.pampl").write_text(PAMPL)
    return tmp_path


def _parts(case_dir: Path):
    system = _system()
    meta = _commitment_meta(system)
    pairs = _order_pairs(case_dir / "case.json", system, meta)
    srows = _status_rows(case_dir / "case.json", system)
    return meta, pairs, srows


def test_order_pair_and_signed_status_rows_parse(case_dir: Path) -> None:
    _, pairs, srows = _parts(case_dir)
    assert pairs == [(1, 2)]  # BL1 leads, BL2 follows (pmin_BL2 > 0)
    names = [r[0] for r in srows]
    assert "Flag_Impl" in names
    assert "Ghost_Row" not in names  # inactive rows never reach the LP
    flag_impl = next(r for r in srows if r[0] == "Flag_Impl")
    coefs = dict((g, c) for c, g in flag_impl[1])
    assert coefs[4] == 1.0 and coefs[3] == -1.0  # detached '- 1 *' kept


def test_repair_order_pair(case_dir: Path) -> None:
    meta, pairs, srows = _parts(case_dir)
    u = {(g, b): 0 for g in (1, 2, 3, 4, 5, 6) for b in BLOCKS}
    u[(2, 4)] = 1  # follower ON, leader OFF → infeasible under solve_fixed
    assert verify_seed(u, BLOCKS, meta, pairs, srows) > 0
    repair_seed(u, BLOCKS, meta, pairs, srows, raw={})
    assert verify_seed(u, BLOCKS, meta, pairs, srows) == 0
    assert u[(1, 4)] == 1  # leader promoted


def test_repair_flag_implication_demotes_flag(case_dir: Path) -> None:
    meta, pairs, srows = _parts(case_dir)
    u = {(g, b): 0 for g in (1, 2, 3, 4, 5, 6) for b in BLOCKS}
    u[(3, 2)] = 1  # FLAG on, no config on → row violated
    repair_seed(u, BLOCKS, meta, pairs, srows, raw={(3, 2): 0.6, (4, 2): 0.1})
    assert verify_seed(u, BLOCKS, meta, pairs, srows) == 0
    assert u[(3, 2)] == 0  # demote-first policy switches the flag off


def test_maintenance_window_forces_off(case_dir: Path) -> None:
    meta, pairs, srows = _parts(case_dir)
    # pmax profile blocks 3-4 are 0.0 < pmin 2.0 → forced OFF
    u = {(g, b): 0 for g in (1, 2, 3, 4, 5, 6) for b in BLOCKS}
    u[(5, 3)] = 1
    u[(5, 4)] = 1
    stats = repair_seed(u, BLOCKS, meta, pairs, srows, raw={})
    assert stats["pin"] == 2
    assert u[(5, 3)] == 0 and u[(5, 4)] == 0
    assert verify_seed(u, BLOCKS, meta, pairs, srows) == 0


def test_min_up_down_and_initial_window(case_dir: Path) -> None:
    meta, pairs, srows = _parts(case_dir)
    # CHATTER: initial OFF with 5h down < min_down 3 already served; the
    # short interior ON run (1 block) violates min_up 3 → extended.
    u = {(g, b): 0 for g in (1, 2, 3, 4, 5, 6) for b in BLOCKS}
    u[(6, 4)] = 1  # 1-block ON island
    repair_seed(u, BLOCKS, meta, pairs, srows, raw={})
    assert verify_seed(u, BLOCKS, meta, pairs, srows) == 0
    assert u[(6, 5)] == 1 and u[(6, 6)] == 1  # run extended to min_up


def test_build_full_seed_end_to_end(case_dir: Path) -> None:
    out = case_dir / "out" / "Commitment"
    out.mkdir(parents=True)
    # Sparse status_sol: FLAG on at block 2 (violates Flag_Impl), BL2 on at
    # block 4 (violates the order pair).
    tbl = pa.table(
        {
            "uid": pa.array([13, 12], pa.int64()),
            "block": pa.array([2, 4], pa.int64()),
            "value": pa.array([1.0, 1.0]),
        }
    )
    pq.write_table(tbl, out / "status_sol.parquet")
    seed_csv = case_dir / "seed.csv"
    summary = build_full_seed(case_dir / "out", case_dir / "case.json", seed_csv)
    assert summary["residual"] == 0
    assert summary["rows"] == 6 * 2  # 6 generators × 2 blocks seen in output
    text = seed_csv.read_text()
    assert text.startswith("generator_uid,block_uid,u")


def test_commitment_to_generator_maps_by_name(case_dir: Path) -> None:
    c2g = _commitment_to_generator(_system())
    assert c2g[11] == 1 and c2g[16] == 6


def test_soft_and_annotated_rows_excluded(tmp_path: Path) -> None:
    # Soft rows (penalty/rhs/desc annotations between NAME and the colon)
    # carry LP-side slacks — they must never be hard-repaired.
    (tmp_path / "case.json").write_text(json.dumps({"system": _system()}))
    (tmp_path / "uc_soft.pampl").write_text(
        "constraint SOFT_UC penalty 1000:\n"
        '  1 * commitment("uc_BL1").status <= 0;\n\n'
        "constraint HARD_UC:\n"
        '  1 * commitment("uc_BL1").status <= 1;\n'
    )
    srows = _status_rows(tmp_path / "case.json", _system())
    names = [r[0] for r in srows]
    assert "HARD_UC" in names
    assert "SOFT_UC" not in names


def test_scientific_notation_coefficient(tmp_path: Path) -> None:
    (tmp_path / "case.json").write_text(json.dumps({"system": _system()}))
    (tmp_path / "uc_sci.pampl").write_text(
        'constraint SCI:\n  0.5e1 * commitment("uc_BL1").status <= 4;\n'
    )
    srows = _status_rows(tmp_path / "case.json", _system())
    sci = next(r for r in srows if r[0] == "SCI")
    assert sci[1][0][0] == pytest.approx(5.0)


def test_non_unit_coefficient_row(case_dir: Path) -> None:
    (case_dir / "uc_two.pampl").write_text(
        'constraint TWO:\n  2 * commitment("uc_BL1").status <= 1;\n'
    )
    meta, pairs, srows = _parts(case_dir)
    u = {(g, b): 0 for g in (1, 2, 3, 4, 5, 6) for b in BLOCKS}
    u[(1, 1)] = 1  # 2*1 = 2 > 1 → violated; only fix is demote BL1
    repair_seed(u, BLOCKS, meta, pairs, srows, raw={})
    assert u[(1, 1)] == 0
    assert verify_seed(u, BLOCKS, meta, pairs, srows) == 0


def test_pinned_conflict_leaves_residual(tmp_path: Path) -> None:
    # must_run pins BL1 ON while a hard row demands it OFF: unrepairable →
    # nonzero residual (the CSV gate exits 1 on it).
    system = _system()
    system["commitment_array"][0]["must_run"] = True
    (tmp_path / "case.json").write_text(json.dumps({"system": system}))
    (tmp_path / "uc_conflict.pampl").write_text(
        'constraint OFF_ALWAYS:\n  1 * commitment("uc_BL1").status <= 0;\n'
    )
    meta = _commitment_meta(system)
    srows = _status_rows(tmp_path / "case.json", system)
    u = {(g, b): 0 for g in (1, 2, 3, 4, 5, 6) for b in BLOCKS}
    repair_seed(u, BLOCKS, meta, [], srows, raw={})
    assert verify_seed(u, BLOCKS, meta, [], srows) == len(BLOCKS)


def test_initial_window_is_a_pin() -> None:
    # ini ON with remaining min-up: the first blocks are PINNED ON, so no
    # later rule (order demote, run fill) can oscillate against the window.
    from gtopt_warmstart.build_full_seed import _pin

    m = {
        "ini_status": 1,
        "min_up": 3,
        "ini_up": 1.0,
        "min_down": 0,
        "ini_down": 0.0,
    }
    assert _pin(m, 0) == 1 and _pin(m, 1) == 1
    assert _pin(m, 2) is None


def test_scalar_and_profile_edges() -> None:
    from gtopt_warmstart.build_full_seed import _profile, _scalar

    assert _scalar([[1.0, 2.0, 3.0]]) == pytest.approx(3.0)
    assert _scalar(None) == 0.0
    assert _scalar(2.5) == pytest.approx(2.5)
    assert _profile([[1.0, 2.0]]) == [1.0, 2.0]
    assert _profile(4.2) is None


def test_dense_seed_covers_all_model_blocks(case_dir: Path) -> None:
    # status_sol only mentions blocks 2 and 4, but the case JSON declares an
    # 8-block layout: the dense seed must cover ALL 8 (all-OFF blocks would
    # otherwise hide min-up/down windows from the repair).
    case = json.loads((case_dir / "case.json").read_text())
    case["simulation"] = {"block_array": [{"uid": b} for b in BLOCKS]}
    (case_dir / "case.json").write_text(json.dumps(case))
    out = case_dir / "out" / "Commitment"
    out.mkdir(parents=True)
    tbl = pa.table(
        {
            "uid": pa.array([12], pa.int64()),
            "block": pa.array([4], pa.int64()),
            "value": pa.array([1.0]),
        }
    )
    pq.write_table(tbl, out / "status_sol.parquet")
    summary = build_full_seed(
        case_dir / "out", case_dir / "case.json", case_dir / "seed.csv"
    )
    assert summary["blocks"] == len(BLOCKS)
    assert summary["rows"] == 6 * len(BLOCKS)


def test_integer_generator_reference() -> None:
    system = _system()
    system["commitment_array"][0]["generator"] = 1  # uid ref, not name
    meta = _commitment_meta(system)
    assert 1 in meta and meta[1]["pmin"] == pytest.approx(5.0)


def test_no_repair_flag_skips_repair(case_dir: Path) -> None:
    out = case_dir / "out" / "Commitment"
    out.mkdir(parents=True)
    tbl = pa.table(
        {
            "uid": pa.array([12], pa.int64()),
            "block": pa.array([4], pa.int64()),
            "value": pa.array([1.0]),
        }
    )
    pq.write_table(tbl, out / "status_sol.parquet")
    summary = build_full_seed(
        case_dir / "out", case_dir / "case.json", case_dir / "s.csv", repair=False
    )
    # BL2 ON without its order leader is a violation the repair would fix.
    assert not summary["repairs"]
    assert summary["residual"] > 0


def test_fuel_offtake_cap_repair() -> None:
    from gtopt_warmstart.build_full_seed import _fuel_offtakes

    system = _system()
    for g in system["generator_array"]:
        if g["uid"] in (1, 2):
            g["fuel"] = "GNL_X"
            g["heat_rate"] = 1.0
    system["fuel_array"] = [{"name": "GNL_X", "max_offtake": 12.0}]
    fuels = _fuel_offtakes(system)
    assert len(fuels) == 1
    meta = _commitment_meta(system)
    durations = {b: 1.0 for b in BLOCKS}
    # BL1 (pmin 5) ON 3h + BL2 (pmin 0.5) ON 2h → forced 16 > cap 12.
    u = {(g, b): 0 for g in (1, 2, 3, 4, 5, 6) for b in BLOCKS}
    for b in (1, 2, 3):
        u[(1, b)] = 1
    for b in (1, 2):
        u[(2, b)] = 1
    raw = {(1, 1): 0.9, (1, 2): 0.8, (1, 3): 0.2, (2, 1): 0.7, (2, 2): 0.6}
    stats = repair_seed(u, BLOCKS, meta, [], [], raw, fuels, durations)
    assert stats["fuel"] >= 1
    assert verify_seed(u, BLOCKS, meta, [], [], fuels, durations) == 0
    # the least-committed ON hour (BL1 block 3, raw 0.2) went first
    assert u[(1, 3)] == 0
