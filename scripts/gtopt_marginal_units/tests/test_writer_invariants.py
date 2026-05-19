# SPDX-License-Identifier: BSD-3-Clause
"""Writer-side invariant tests — master §4.6.4 invariant 1.

The writer asserts |recomputed_lmp − zone_lmp| ≤ tol_price BEFORE
persisting; a violation aborts with AttributionError → CLI exit 3.
"""

from __future__ import annotations

import pytest

from gtopt_canonical_feed import Generator, Topology
from gtopt_marginal_units._reconstruct import ZoneR3Result
from gtopt_marginal_units._recipes import build_recipes_for_cell
from gtopt_marginal_units.constants import Confidence, FormulaKind, Tolerances
from gtopt_marginal_units.errors import AttributionError


_CELL = (1, 1, 1, None, None, "simulated")


def _gen(uid: int, mc: float) -> Generator:
    return Generator(
        uid=uid,
        name=f"g{uid}",
        bus_uid=1,
        pmin=0,
        pmax=100,
        declared_MC=mc,
        kind="thermal",
        emission_rate=400.0,
    )


def test_invariant_passes_when_zone_lmp_matches_recipe():
    topo = Topology(buses=[], generators=[_gen(10, 25.0)], lines=[])
    zr = {
        0: ZoneR3Result(
            zone_id=0,
            lambda_z=25.0,
            formula_kind=FormulaKind.SINGLE_UNIT.value,
            marginal_gen_uids=[10],
            confidence=Confidence.MERIT_ORDER,
            degenerate=False,
            reason="test",
            clamped=False,
        )
    }
    # Should not raise.
    build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
    )


def test_invariant_violation_raises_attribution_error():
    """Hand-built mismatch: the recipe table claims gen 10 sets the
    price (MC=25) but the zone result says λ_z=99. The writer
    must catch this before persistence."""
    topo = Topology(buses=[], generators=[_gen(10, 25.0)], lines=[])
    zr = {
        0: ZoneR3Result(
            zone_id=0,
            lambda_z=99.0,  # ← mismatch: should be 25.0
            formula_kind=FormulaKind.SINGLE_UNIT.value,
            marginal_gen_uids=[10],
            confidence=Confidence.MERIT_ORDER,
            degenerate=False,
            reason="test",
            clamped=False,
        )
    }
    with pytest.raises(AttributionError, match="recipe round-trip"):
        build_recipes_for_cell(
            cell_key=_CELL,
            topology=topo,
            zone_of={1: 0},
            zone_results=zr,
            tol=Tolerances.default(),
        )


def test_invariant_skipped_for_clamped_cells():
    """A demand-fail row's recomputed_lmp comes from the cap, not from
    the captured MCs — the invariant is intentionally skipped for
    those cells. (Master §4.6.4 invariant 1 first paragraph.)"""
    topo = Topology(buses=[], generators=[_gen(10, 25.0)], lines=[])
    zr = {
        0: ZoneR3Result(
            zone_id=0,
            lambda_z=1000.0,
            formula_kind=FormulaKind.DEMAND_FAIL.value,
            marginal_gen_uids=[],
            confidence=Confidence.FALLBACK,
            degenerate=True,
            reason="all_units_at_pmax",
            clamped=False,
        )
    }
    # Should not raise even though "1000 != captured MCs".
    price, _em = build_recipes_for_cell(
        cell_key=_CELL,
        topology=topo,
        zone_of={1: 0},
        zone_results=zr,
        demand_fail_cost=1000.0,
    )
    assert price[0].recomputed_value == 1000.0
