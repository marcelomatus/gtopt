# -*- coding: utf-8 -*-
"""Unit tests for the shared reservoir water-value library."""

from __future__ import annotations

from types import SimpleNamespace

import pytest

from gtopt_shared.water_values import (
    WaterValueResolver,
    cut_lower_bound,
    default_water_fail_value,
)


# ── cut_lower_bound (replicates C++ cut_soft_cost min) ──────────────────────
def test_cut_lower_bound_negates_max_coeff() -> None:
    # coeffs ship -wv; lower-bound cost = -max(coeff).
    # max(coeff) = -50 → lower bound = 50.
    assert cut_lower_bound([-50.0, -120.0, -90.0]) == pytest.approx(50.0)


def test_cut_lower_bound_empty_is_none() -> None:
    assert cut_lower_bound([]) is None


def test_cut_lower_bound_liability_floors_to_smallest_positive_wv() -> None:
    # A non-negative coeff present → cost = -max = negative → fall back to
    # smallest positive wv = -max(negatives) = 30.
    assert cut_lower_bound([+10.0, -30.0, -200.0]) == pytest.approx(30.0)


def test_cut_lower_bound_all_nonneg_floors_to_one() -> None:
    assert cut_lower_bound([0.0, 5.0]) == pytest.approx(1.0)


# ── default_water_fail_value = max ──────────────────────────────────────────
def test_default_water_fail_is_max() -> None:
    assert default_water_fail_value([10.0, 250.0, 90.0]) == pytest.approx(250.0)


def test_default_water_fail_empty_is_zero() -> None:
    assert default_water_fail_value([]) == 0.0


# ── resolver: anchor, cascade, overwrite ────────────────────────────────────
def _central(num, name, ctype, *, gcost=0.0, eff=0.0, bus=0, ser_hid=0, emax=0.0):
    return {
        "number": num,
        "name": name,
        "type": ctype,
        "gcost": gcost,
        "efficiency": eff,
        "bus": bus,
        "ser_hid": ser_hid,
        "emax": emax,
    }


def _make_resolver(centrals, *, cut_water_values=None, options=None):
    return WaterValueResolver(
        central_parser=SimpleNamespace(centrals=centrals),
        cenre_parser=None,
        options=options or {"auto_water_fail_cost": True},
        cut_water_values=cut_water_values,
    )


def test_anchor_blend() -> None:
    centrals = [
        _central(1, "T1", "termica", gcost=40.0),
        _central(2, "T2", "termica", gcost=60.0),
        _central(3, "F1", "falla", gcost=500.0),
        _central(4, "F2", "falla", gcost=800.0),
    ]
    r = _make_resolver(centrals)
    # 0.75*avg(40,60) + 0.25*min(500,800) = 0.75*50 + 0.25*500 = 37.5 + 125
    assert r.anchor == pytest.approx(162.5)


def test_cascade_lost_pf_stops_at_next_reservoir_and_skips_pasada() -> None:
    centrals = [
        _central(1, "EMB", "embalse", eff=1.0, bus=1, ser_hid=2),
        _central(2, "SER", "serie", eff=2.0, bus=1, ser_hid=3),
        _central(3, "ROR", "pasada", eff=5.0, bus=1, ser_hid=4),  # excluded
        _central(4, "EMB2", "embalse", eff=9.0, bus=1, ser_hid=0),  # stops here
    ]
    r = _make_resolver(centrals)
    # EMB(1.0) + SER(2.0); pasada excluded; stop before EMB2.
    assert r.cascade_lost_pf(1) == pytest.approx(3.0)


def test_efin_cost_for_uses_auto_estimate_without_cut() -> None:
    centrals = [
        _central(1, "EMB", "embalse", eff=1.0, bus=1, ser_hid=0),
        _central(2, "T1", "termica", gcost=40.0),
        _central(3, "F1", "falla", gcost=400.0),
    ]
    r = _make_resolver(centrals)
    lost_pf = r.cascade_lost_pf(1)  # = max_rendi(EMB) = 1.0
    # anchor = 0.75*40 + 0.25*400 = 30 + 100 = 130
    # efin_cost = 130 * 1.0 * 1e6/3600
    expected = round(130.0 * 1.0 * 1.0e6 / 3600.0, 2)
    assert r.efin_cost_for("EMB", lost_pf) == pytest.approx(expected)


def test_efin_cost_for_overwrites_with_cut_lower_bound() -> None:
    centrals = [
        _central(1, "EMB", "embalse", eff=1.0, bus=1, ser_hid=0),
        _central(2, "T1", "termica", gcost=40.0),
        _central(3, "F1", "falla", gcost=400.0),
    ]
    # Cut lower bound for EMB present → overwrites the (much larger) auto value.
    r = _make_resolver(centrals, cut_water_values={"EMB": 77.0})
    lost_pf = r.cascade_lost_pf(1)
    assert r.efin_cost_for("EMB", lost_pf) == pytest.approx(77.0)


# ── max_rendi (static rendi vs cenre lift @ vmax) ───────────────────────────
def test_max_rendi_static_when_no_cenre() -> None:
    r = _make_resolver([_central(1, "EMB", "embalse", eff=1.5, bus=1)])
    assert r.max_rendi("EMB") == pytest.approx(1.5)
    assert r.max_rendi("UNKNOWN") == 0.0  # unknown central → 0


def test_max_rendi_lifts_to_cenre_at_vmax() -> None:
    # cenre segment: constant 3.0 + slope 0.1*(V-0) at vmax=20 → 5.0 > static 1.5.
    centrals = [_central(1, "EMB", "embalse", eff=1.5, bus=1, emax=20.0)]
    cenre = SimpleNamespace(
        efficiencies=[
            {
                "name": "EMB",
                "segments": [{"constant": 3.0, "slope": 0.1, "volume": 0.0}],
            }
        ]
    )
    r = WaterValueResolver(
        central_parser=SimpleNamespace(centrals=centrals),
        cenre_parser=cenre,
        options={"auto_water_fail_cost": True},
    )
    assert r.max_rendi("EMB") == pytest.approx(5.0)


# ── junction_lost_pf (single-junction FlowRight rule) ───────────────────────
def test_junction_lost_pf_uses_own_rendi_when_bus_positive() -> None:
    r = _make_resolver([_central(7, "GEN", "serie", eff=2.5, bus=1)])
    assert r.junction_lost_pf(7) == pytest.approx(2.5)


def test_junction_lost_pf_zero_when_no_generator() -> None:
    r = _make_resolver([_central(7, "TRANSIT", "serie", eff=2.5, bus=0)])
    assert r.junction_lost_pf(7) == 0.0
    assert r.junction_lost_pf(999) == 0.0  # unknown central


# ── fail_cost + is_active gate ──────────────────────────────────────────────
def test_fail_cost_is_anchor_times_lost_pf() -> None:
    centrals = [
        _central(2, "T1", "termica", gcost=40.0),
        _central(3, "F1", "falla", gcost=400.0),
    ]
    r = _make_resolver(centrals)  # anchor = 0.75*40 + 0.25*400 = 130
    assert r.fail_cost(2.0) == pytest.approx(260.0)
    assert r.water_fail_cost == pytest.approx(130.0)  # alias for anchor


def test_is_active_gate() -> None:
    centrals = [_central(2, "T1", "termica", gcost=40.0)]
    cp = SimpleNamespace(centrals=centrals)
    assert WaterValueResolver(
        central_parser=cp, options={"auto_water_fail_cost": True}
    ).is_active
    assert WaterValueResolver(
        central_parser=cp, options={"water_fail_cost": 50.0}
    ).is_active
    # No gate set → inactive (callers fall back to legacy pricing).
    assert not WaterValueResolver(central_parser=cp, options={}).is_active


def test_explicit_water_fail_cost_overrides_anchor() -> None:
    centrals = [
        _central(2, "T1", "termica", gcost=40.0),
        _central(3, "F1", "falla", gcost=400.0),
    ]
    r = _make_resolver(centrals, options={"water_fail_cost": 99.0})
    assert r.anchor == pytest.approx(99.0)
