# SPDX-License-Identifier: BSD-3-Clause
"""Tests for B2: per-Demand demand_fail_cost with global fallback.

Mirrors the gtopt LP resolution order
(``source/demand_lp.cpp``):
  1. Per-Demand ``fcost`` scalar
  2. ``model_options.demand_fail_cost``
  3. CLI ``--demand-fail-cost`` fallback

When multiple Demands share a bus, the LOWEST scalar wins (cheapest
demand to ration first sets the marginal price).
"""

from __future__ import annotations

import pytest

from gtopt_marginal_units._gtopt_reader import demand_fail_cost_by_bus


def test_per_demand_fcost_wins_over_global():
    """A demand with explicit fcost overrides the CLI global."""
    planning = {
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}],
            "demand_array": [
                {"uid": 10, "bus": "b1", "fcost": 50.0},
            ],
        }
    }
    out = demand_fail_cost_by_bus(planning, global_fallback=1000.0)
    assert out[1] == pytest.approx(50.0)


def test_demand_without_fcost_falls_back_to_model_options():
    """No per-Demand fcost → model_options.demand_fail_cost wins over CLI."""
    planning = {
        "options": {"model_options": {"demand_fail_cost": 200.0}},
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}],
            "demand_array": [
                {"uid": 10, "bus": "b1"},  # no fcost
            ],
        },
    }
    out = demand_fail_cost_by_bus(planning, global_fallback=1000.0)
    assert out[1] == pytest.approx(200.0)


def test_no_fcost_no_model_options_uses_cli_global():
    """Neither per-Demand nor model_options → CLI fallback."""
    planning = {
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}],
            "demand_array": [{"uid": 10, "bus": "b1"}],
        }
    }
    out = demand_fail_cost_by_bus(planning, global_fallback=1000.0)
    assert out[1] == pytest.approx(1000.0)


def test_multi_demand_same_bus_picks_lowest():
    """Two demands at the same bus → cheapest-to-ration sets the price."""
    planning = {
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}],
            "demand_array": [
                {"uid": 10, "bus": "b1", "fcost": 800.0},
                {"uid": 11, "bus": "b1", "fcost": 300.0},
                {"uid": 12, "bus": "b1", "fcost": 500.0},
            ],
        }
    }
    out = demand_fail_cost_by_bus(planning, global_fallback=1000.0)
    assert out[1] == pytest.approx(300.0)


def test_bus_without_demand_inherits_global():
    """A bus with no Demand element still gets a value (the global)."""
    planning = {
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}, {"uid": 2, "name": "b2"}],
            "demand_array": [{"uid": 10, "bus": "b1", "fcost": 50.0}],
        }
    }
    out = demand_fail_cost_by_bus(planning, global_fallback=1000.0)
    assert out[1] == pytest.approx(50.0)
    assert out[2] == pytest.approx(1000.0)


def test_non_scalar_fcost_inherits_global():
    """FileSched / dict fcost (per-block schedule) cannot be reduced to
    a scalar → bus inherits the global fallback.  v1 limitation."""
    planning = {
        "system": {
            "bus_array": [{"uid": 1, "name": "b1"}],
            "demand_array": [
                {
                    "uid": 10,
                    "bus": "b1",
                    "fcost": "parquet:demand_fcost_schedule",
                },
            ],
        }
    }
    out = demand_fail_cost_by_bus(planning, global_fallback=1000.0)
    assert out[1] == pytest.approx(1000.0)


def test_recipe_uses_per_bus_value_in_demand_fail_branch():
    """End-to-end: build_recipes_for_cell with a dict-shaped
    demand_fail_cost should write per-bus values into both
    ``recomputed_value`` and ``formula_constant`` of the DEMAND_FAIL row."""
    from gtopt_canonical_feed import Bus, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    bus_a = Bus(uid=10, name="A")
    bus_b = Bus(uid=20, name="B")
    topo = Topology(buses=[bus_a, bus_b], generators=[], lines=[])
    # Demand-fail zone (rationing).
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=500.0,
        formula_kind="demand_fail",
        marginal_gen_uids=[],
        confidence=Confidence.LP_DUAL,
        degenerate=True,
        reason="lp_dual_at_demand_fail_cost",
        clamped=False,
    )
    rows, _ = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0, 20: 0},
        zone_results={0: zres},
        dispatch_by_uid={},
        lmp_by_bus={10: 500.0, 20: 500.0},
        srmc_by_uid={},
        demand_fail_cost={10: 500.0, 20: 1000.0},  # per-bus override
        tol=Tolerances.default(),
    )
    by_bus = {r.bus_uid: r for r in rows}
    assert by_bus[10].recomputed_value == pytest.approx(500.0)
    assert by_bus[20].recomputed_value == pytest.approx(1000.0)
    # formula_constant for demand_fail = the rationing cap itself
    assert by_bus[10].formula_constant == pytest.approx(500.0)
    assert by_bus[20].formula_constant == pytest.approx(1000.0)


def test_recipe_scalar_demand_fail_cost_still_works():
    """Back-compat: a scalar ``demand_fail_cost`` keeps working everywhere
    (test fixtures, --feed-parquet inputs without a planning JSON)."""
    from gtopt_canonical_feed import Bus, Topology

    from gtopt_marginal_units._recipes import build_recipes_for_cell
    from gtopt_marginal_units._reconstruct import (
        Confidence,
        ZoneR3Result,
    )
    from gtopt_marginal_units.constants import Tolerances

    bus_a = Bus(uid=10, name="A")
    topo = Topology(buses=[bus_a], generators=[], lines=[])
    zres = ZoneR3Result(
        zone_id=0,
        lambda_z=1000.0,
        formula_kind="demand_fail",
        marginal_gen_uids=[],
        confidence=Confidence.LP_DUAL,
        degenerate=True,
        reason="lp_dual_at_demand_fail_cost",
        clamped=False,
    )
    rows, _ = build_recipes_for_cell(
        cell_key=(1, 1, 1, None, None, None),
        topology=topo,
        zone_of={10: 0},
        zone_results={0: zres},
        dispatch_by_uid={},
        lmp_by_bus={10: 1000.0},
        srmc_by_uid={},
        demand_fail_cost=1000.0,  # scalar — legacy / test path
        tol=Tolerances.default(),
    )
    assert rows[0].recomputed_value == pytest.approx(1000.0)
    assert rows[0].formula_constant == pytest.approx(1000.0)
