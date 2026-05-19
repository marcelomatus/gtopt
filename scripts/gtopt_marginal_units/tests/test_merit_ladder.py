# SPDX-License-Identifier: BSD-3-Clause
"""Merit-ladder construction tests — master §4.9."""

from __future__ import annotations

import pytest

from gtopt_canonical_feed import Generator
from gtopt_marginal_units._ladder import build_ladder
from gtopt_marginal_units._reconstruct import ZoneR3Result
from gtopt_marginal_units.constants import (
    DEFAULT_MERIT_LADDER_DEPTH,
    Confidence,
    FormulaKind,
)


_CELL = (1, 1, 1, None, None, "simulated")


def _gen(uid: int, mc: float, kind: str = "thermal") -> Generator:
    return Generator(
        uid=uid,
        name=f"g{uid}",
        bus_uid=1,
        pmin=0.0,
        pmax=100.0,
        declared_MC=mc,
        kind=kind,
        emission_rate=400.0,
    )


def _zres(formula_kind: str, marginal_uids: list[int], lambda_z: float = 30.0):
    return ZoneR3Result(
        zone_id=0,
        lambda_z=lambda_z,
        formula_kind=formula_kind,
        marginal_gen_uids=marginal_uids,
        confidence=Confidence.MERIT_ORDER,
        degenerate=False,
        reason="test",
        clamped=False,
    )


def test_ladder_default_depth_three_around_anchor():
    # 5 thermal units at MCs 10, 20, 30, 40, 50; anchor = the 30
    # unit (uid 30). Default depth=3 → ranks -2,-1,0,+1,+2 (the 5 units).
    gens = [_gen(uid=mc, mc=float(mc)) for mc in (10, 20, 30, 40, 50)]
    rungs = build_ladder(
        cell_key=_CELL,
        zone_id=0,
        generators_in_zone=gens,
        dispatch_by_uid={uid: 50.0 for uid in (10, 20, 30, 40, 50)},
        zone_result=_zres(FormulaKind.SINGLE_UNIT.value, [30], lambda_z=30.0),
    )
    ranks = sorted(r.rank for r in rungs)
    assert ranks == [-2, -1, 0, 1, 2]
    # rank 0 must be the anchor.
    rank0 = next(r for r in rungs if r.rank == 0)
    assert rank0.gen_uid == 30
    assert rank0.is_actual_marginal
    # rank +1 should be uid=40 (MC=40).
    rank_plus1 = next(r for r in rungs if r.rank == 1)
    assert rank_plus1.gen_uid == 40
    assert rank_plus1.declared_MC == 40.0


def test_ladder_endpoint_truncates():
    # Anchor is the cheapest unit (uid=10). No downward rungs possible.
    gens = [_gen(uid=mc, mc=float(mc)) for mc in (10, 20, 30, 40)]
    rungs = build_ladder(
        cell_key=_CELL,
        zone_id=0,
        generators_in_zone=gens,
        dispatch_by_uid={uid: 50.0 for uid in (10, 20, 30, 40)},
        zone_result=_zres(FormulaKind.SINGLE_UNIT.value, [10], lambda_z=10.0),
    )
    ranks = sorted(r.rank for r in rungs)
    # depth=3, anchor at idx=0 → ranks 0, +1, +2, +3.
    assert ranks == [0, 1, 2, 3]


def test_ladder_excludes_profile_units():
    # Wind/solar should never appear in the ladder.
    gens = [
        _gen(10, 10.0),
        _gen(20, 20.0),
        Generator(
            uid=99,
            name="solar",
            bus_uid=1,
            pmin=0,
            pmax=100,
            declared_MC=0.0,
            kind="profile",
            emission_rate=0.0,
        ),
    ]
    rungs = build_ladder(
        cell_key=_CELL,
        zone_id=0,
        generators_in_zone=gens,
        dispatch_by_uid={10: 50.0, 20: 50.0, 99: 30.0},
        zone_result=_zres(FormulaKind.SINGLE_UNIT.value, [20], lambda_z=20.0),
    )
    assert all(r.gen_uid != 99 for r in rungs)


def test_synthetic_demand_fail_anchor_only_rank_zero():
    # Pure synthetic anchor: only one rank-0 row, no neighbours.
    gens = [_gen(10, 10.0)]
    rungs = build_ladder(
        cell_key=_CELL,
        zone_id=0,
        generators_in_zone=gens,
        dispatch_by_uid={10: 100.0},
        zone_result=_zres(FormulaKind.DEMAND_FAIL.value, [], lambda_z=1000.0),
    )
    assert len(rungs) == 1
    assert rungs[0].rank == 0
    assert rungs[0].gen_uid is None
    assert rungs[0].hypothetical_lmp == 1000.0


def test_ladder_default_depth_constant():
    """Confirm DEFAULT_MERIT_LADDER_DEPTH=3 per master §4.9."""
    assert DEFAULT_MERIT_LADDER_DEPTH == 3


def test_headroom_correctly_computed():
    gens = [_gen(10, 20.0), _gen(20, 30.0)]
    rungs = build_ladder(
        cell_key=_CELL,
        zone_id=0,
        generators_in_zone=gens,
        dispatch_by_uid={10: 70.0, 20: 50.0},
        zone_result=_zres(FormulaKind.SINGLE_UNIT.value, [10], lambda_z=20.0),
    )
    rank0 = next(r for r in rungs if r.rank == 0)
    assert rank0.gen_uid == 10
    assert rank0.headroom_up_mw == pytest.approx(30.0)  # pmax 100 - 70
    assert rank0.headroom_down_mw == pytest.approx(70.0)  # 70 - pmin 0
