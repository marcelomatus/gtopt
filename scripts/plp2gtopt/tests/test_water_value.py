"""Unit tests for the water-shortfall pricing helper.

Covers the public API of :class:`plp2gtopt._water_value.WaterValueResolver`:

* anchor (``water_fail_cost``) — auto-derive vs. explicit override
* ``max_rendi`` — static rendi vs. cenre lift @ vmax
* ``cascade_lost_pf`` — sums max_rendi along ser_hid
* ``junction_lost_pf`` — own rendi for bus>0, 0 for bus=0
* ``fail_cost`` ($/(m³/s·h)) and ``efin_cost`` ($/hm³) formulas
* ``is_active`` — gating semantics

Reference values from the juan/IPLP case (see the helper's docstring
and the auto-water-fail-cost design note) are checked with a ±0.5
absolute tolerance to absorb floating-point rounding.
"""

from __future__ import annotations

from typing import Any, Dict, List

import pytest

from plp2gtopt._water_value import WaterValueResolver


# ---------------------------------------------------------------------------
# Lightweight fakes — we don't need real .dat parsing for these tests, so we
# stand up the minimum surface area :class:`WaterValueResolver` consumes.
# ---------------------------------------------------------------------------


class _FakeCentralParser:
    """Stand-in for :class:`CentralParser` exposing ``centrals``."""

    def __init__(self, centrals: List[Dict[str, Any]]) -> None:
        self.centrals = list(centrals)


class _FakeCenreParser:
    """Stand-in for :class:`CenreParser` exposing ``efficiencies``."""

    def __init__(self, efficiencies: List[Dict[str, Any]]) -> None:
        self.efficiencies = list(efficiencies)


# Reference juan/IPLP magnitudes — the synthetic centrals below mimic the
# tier-4 falla rung (568.4 $/MWh) and a small LMAULE-like cascade so we can
# spot-check the documented numbers without needing the full PLP case on
# disk.
_FALLA_GCOST_MAX = 568.4
_ANCHOR_AUTO = _FALLA_GCOST_MAX + 1.0  # 569.4 — strictly above max(falla.gcost)


def _make_centrals() -> List[Dict[str, Any]]:
    """Synthetic minimal CentralParser fixture.

    LMAULE → LOS_CONDORES → ISLA → CURILLINQUE → LOMA_ALTA → PEHUENCHE → 0
    rendi sum (own LMAULE bus=0 transit + cascade) = 6.0 + 0.81 + 1.01 + 0.45
    + 1.78 = 10.05.

    Plus standalone bus>0 ABANICO (rendi=1.2), bus=0 transit RIEGZACO (rendi=
    0.5 — but bus=0 means it must be excluded), and a tier-4 falla central
    used as the gcost anchor.
    """
    return [
        # LMAULE — pure transit reservoir (bus=0), feeds the cascade below.
        {
            "number": 100,
            "name": "LMAULE",
            "type": "embalse",
            "bus": 0,
            "ser_hid": 101,
            "efficiency": 0.0,
            "emax": 1500.0,
        },
        # LOS_CONDORES — first turbine in the cascade (bus>0, rendi=6.0).
        {
            "number": 101,
            "name": "LOS_CONDORES",
            "type": "serie",
            "bus": 50,
            "ser_hid": 102,
            "efficiency": 6.0,
            "emax": 0.0,
        },
        {
            "number": 102,
            "name": "ISLA",
            "type": "serie",
            "bus": 51,
            "ser_hid": 103,
            "efficiency": 0.81,
            "emax": 0.0,
        },
        {
            "number": 103,
            "name": "CURILLINQUE",
            "type": "serie",
            "bus": 52,
            "ser_hid": 104,
            "efficiency": 1.01,
            "emax": 0.0,
        },
        {
            "number": 104,
            "name": "LOMA_ALTA",
            "type": "serie",
            "bus": 53,
            "ser_hid": 105,
            "efficiency": 0.45,
            "emax": 0.0,
        },
        {
            "number": 105,
            "name": "PEHUENCHE",
            "type": "serie",
            "bus": 54,
            "ser_hid": 0,  # terminal
            "efficiency": 1.78,
            "emax": 0.0,
        },
        # Standalone non-cascading hydro generator — used for junction_lost_pf
        # tests.
        {
            "number": 200,
            "name": "ABANICO",
            "type": "pasada",
            "bus": 105,
            "ser_hid": 0,
            "efficiency": 1.2,
            "emax": 0.0,
        },
        {
            "number": 201,
            "name": "ANTUCO",
            "type": "pasada",
            "bus": 96,
            "ser_hid": 0,
            "efficiency": 1.6,
            "emax": 0.0,
        },
        # Transit-only central — bus=0 means no generator and zero
        # energy-equivalent value.
        {
            "number": 202,
            "name": "RIEGZACO",
            "type": "pasada",
            "bus": 0,
            "ser_hid": 0,
            "efficiency": 0.5,
            "emax": 0.0,
        },
        # tier-4 falla — drives the anchor.
        {
            "number": 999,
            "name": "FALLA_T4",
            "type": "falla",
            "bus": 0,
            "ser_hid": 0,
            "efficiency": 0.0,
            "emax": 0.0,
            "gcost": _FALLA_GCOST_MAX,
        },
        # Lower-tier falla — must be ignored by the max() reduction.
        {
            "number": 998,
            "name": "FALLA_T1",
            "type": "falla",
            "bus": 0,
            "ser_hid": 0,
            "efficiency": 0.0,
            "emax": 0.0,
            "gcost": 100.0,
        },
    ]


def _make_centrals_with_eltoro() -> List[Dict[str, Any]]:
    """Single-central fixture for the ELTORO cenre-lift test.

    Static rendi 4.8 lifted to 5.028 by cenre @ vmax=5586 with
    ``constant=4.5252, slope=9e-5, volume=0`` (point-slope).
    """
    return [
        {
            "number": 1,
            "name": "ELTORO",
            "type": "embalse",
            "bus": 0,
            "ser_hid": 0,
            "efficiency": 4.8,
            "emax": 5586.0,
        },
    ]


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_water_fail_cost_auto_formula() -> None:
    """Anchor = max(falla.gcost) + 1; juan/IPLP-like → 569.4."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.water_fail_cost == pytest.approx(_ANCHOR_AUTO, abs=0.5)
    # Alias: ``anchor`` returns the same value.
    assert resolver.anchor == pytest.approx(_ANCHOR_AUTO, abs=0.5)


def test_water_fail_cost_explicit_override() -> None:
    """When ``--water-fail-cost`` is set, the auto formula is bypassed."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        # 500 ≠ auto value (569.4); also no auto flag — explicit alone wins.
        options={"water_fail_cost": 500.0},
    )
    assert resolver.water_fail_cost == pytest.approx(500.0)


def test_water_fail_cost_explicit_override_beats_auto() -> None:
    """Explicit override takes priority over the auto gate."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"water_fail_cost": 1234.5, "auto_water_fail_cost": True},
    )
    assert resolver.water_fail_cost == pytest.approx(1234.5)


def test_water_fail_cost_zero_when_no_falla() -> None:
    """Degenerate fixture (no falla) → anchor 0 — caller treats as no-op."""
    cp = _FakeCentralParser([c for c in _make_centrals() if c.get("type") != "falla"])
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.anchor == 0.0


def test_max_rendi_static_when_no_cenre() -> None:
    """Centrals not in cenre return their ``efficiency`` field as-is."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.max_rendi("ABANICO") == pytest.approx(1.2)
    assert resolver.max_rendi("ANTUCO") == pytest.approx(1.6)
    assert resolver.max_rendi("LOS_CONDORES") == pytest.approx(6.0)


def test_max_rendi_lifted_by_cenre_at_vmax() -> None:
    """ELTORO cenre @ vmax (slope=9e-5, const=4.5252, volume=0) → ≈5.028."""
    cp = _FakeCentralParser(_make_centrals_with_eltoro())
    cenre = _FakeCenreParser(
        [
            {
                "name": "ELTORO",
                "reservoir": "ELTORO",
                "mean_production_factor": 4.8,
                "segments": [
                    {"volume": 0.0, "slope": 9.0e-5, "constant": 4.5252},
                ],
            }
        ]
    )
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=cenre,
        options={"auto_water_fail_cost": True},
    )
    # 4.5252 + 9e-5 × 5586 = 5.0276 → rounded to 5.028 in design doc.
    assert resolver.max_rendi("ELTORO") == pytest.approx(5.028, abs=0.005)


def test_max_rendi_keeps_static_when_cenre_lower() -> None:
    """If cenre @ vmax is *below* static rendi, max_rendi keeps the static."""
    cp = _FakeCentralParser(
        [
            {
                "number": 1,
                "name": "C1",
                "type": "embalse",
                "bus": 0,
                "ser_hid": 0,
                "efficiency": 9.0,  # higher than the cenre evaluation
                "emax": 100.0,
            }
        ]
    )
    cenre = _FakeCenreParser(
        [
            {
                "name": "C1",
                "reservoir": "C1",
                "mean_production_factor": 1.0,
                "segments": [
                    {"volume": 0.0, "slope": 0.0, "constant": 1.0},
                ],
            }
        ]
    )
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=cenre,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.max_rendi("C1") == pytest.approx(9.0)


def test_cascade_lost_pf_walks_ser_hid_summing_max_rendi() -> None:
    """LMAULE cascade: 0 (own bus=0) + 6.0 + 0.81 + 1.01 + 0.45 + 1.78 = 10.05."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    # Start at LMAULE (number=100) — bus=0 so its own rendi contributes 0.
    assert resolver.cascade_lost_pf(100) == pytest.approx(10.05, abs=0.01)


def test_cascade_lost_pf_unknown_starting_number() -> None:
    """Unknown start → 0; preserves caller-side fallback semantics."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.cascade_lost_pf(424242) == 0.0


def test_cascade_lost_pf_cycle_guard() -> None:
    """A cyclic ser_hid does not infinite-loop."""
    cp = _FakeCentralParser(
        [
            {
                "number": 1,
                "name": "A",
                "type": "serie",
                "bus": 1,
                "ser_hid": 2,
                "efficiency": 1.0,
                "emax": 0.0,
            },
            {
                "number": 2,
                "name": "B",
                "type": "serie",
                "bus": 2,
                "ser_hid": 1,  # cycle back to A
                "efficiency": 2.0,
                "emax": 0.0,
            },
        ]
    )
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.cascade_lost_pf(1) == pytest.approx(3.0)


def test_junction_lost_pf_returns_central_rendi_when_bus_positive() -> None:
    """ABANICO bus>0 → 1.2, ANTUCO bus>0 → 1.6 (single-junction rule)."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.junction_lost_pf(200) == pytest.approx(1.2)
    assert resolver.junction_lost_pf(201) == pytest.approx(1.6)


def test_junction_lost_pf_returns_zero_for_bus_zero() -> None:
    """RIEGZACO bus=0 → 0.0 (transit-only central, no FR cost)."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.junction_lost_pf(202) == 0.0


def test_junction_lost_pf_unknown_number() -> None:
    """Unknown central → 0.0."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.junction_lost_pf(424242) == 0.0


def test_efin_cost_formula() -> None:
    """anchor × lost_pf × 1e6 / 3600 [$/hm³].  LMAULE: 569.4 × 10.05 ≈ 1,589,575."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    cost = resolver.efin_cost(10.05)
    assert cost == pytest.approx(1_589_575.0, abs=2.0)


def test_fail_cost_formula() -> None:
    """anchor × lost_pf [$/(m³/s·h)].  ABANICO: 569.4 × 1.2 = 683.28."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.fail_cost(1.2) == pytest.approx(683.28, abs=0.5)
    # ANTUCO own rendi 1.6 → 569.4 × 1.6 = 911.04
    assert resolver.fail_cost(1.6) == pytest.approx(911.04, abs=0.5)


def test_resolver_is_inactive_by_default() -> None:
    """No flag set → ``is_active`` is False; legacy paths must remain."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={},
    )
    assert resolver.is_active is False


def test_resolver_is_active_when_auto_flag_on() -> None:
    """``--auto-water-fail-cost`` alone activates the resolver."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.is_active is True


def test_resolver_is_active_when_explicit_override_set() -> None:
    """``--water-fail-cost`` alone (no auto flag) activates the resolver."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"water_fail_cost": 700.0},
    )
    assert resolver.is_active is True
