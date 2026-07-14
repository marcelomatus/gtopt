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
    c2g = _commitment_to_generator(case_dir / "case.json")
    assert c2g[11] == 1 and c2g[16] == 6
