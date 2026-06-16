"""Unit tests for the water-shortfall pricing helper.

Covers the public API of
:class:`gtopt_shared.water_values.WaterValueResolver`:

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

from gtopt_shared.water_values import WaterValueResolver


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
# Reference juan/IPLP-style magnitudes for the
# ``ANCHOR = 0.75·avg_thermal_gcost + 0.25·min_falla_gcost`` formula:
#
#   * cheapest curtailment rung   = 100 $/MWh  (FALLA_T1)
#   * thermal plants in fixture   = [PEAKER 200, BASE_THERMAL 50]
#   * average thermal gcost       = (200 + 50) / 2 = 125 $/MWh
#   * anchor                       = 0.75·125 + 0.25·100 = 118.75 $/MWh
#
# The 75/25 split (vs. the previous 50/50) shifts the anchor toward
# typical thermal dispatch cost — the LP operates near typical
# thermal far more often than at the curtailment frontier.  Combined
# with `avg(termica.gcost)` (not max) this keeps water value strictly
# below the SDDP `2 × thermal_cost` LB-overshoot threshold (see DIAG
# ladder in test/source/test_sddp_method.cpp).
#
# ``_FALLA_GCOST_MAX`` is retained for the higher tier-4 rung so the
# fixture still exercises the "min reduction over falla" path
# (T1=100 must win over T4=568.4).
_FALLA_GCOST_MIN = 100.0
_FALLA_GCOST_MAX = 568.4
_PEAKER_GCOST = 200.0  # most expensive non-falla unit
_BASE_THERMAL_GCOST = 50.0  # cheaper thermal — both enter the mean
_AVG_THERMAL_GCOST = (_PEAKER_GCOST + _BASE_THERMAL_GCOST) / 2.0  # 125.0
_ANCHOR_AUTO = 0.75 * _AVG_THERMAL_GCOST + 0.25 * _FALLA_GCOST_MIN  # 118.75


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
        # PEAKER — most expensive thermal generator (drives
        # ``max_unit_gcost`` in the new anchor formula).
        {
            "number": 300,
            "name": "PEAKER",
            "type": "termica",
            "bus": 60,
            "ser_hid": 0,
            "efficiency": 0.0,
            "emax": 0.0,
            "gcost": _PEAKER_GCOST,
        },
        # Lower-cost thermal — must be ignored by the max() reduction.
        {
            "number": 301,
            "name": "BASE_THERMAL",
            "type": "termica",
            "bus": 61,
            "ser_hid": 0,
            "efficiency": 0.0,
            "emax": 0.0,
            "gcost": 50.0,
        },
        # tier-4 falla — kept for fixture richness; the new formula
        # reduces over min(), so this should be IGNORED.
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
        # tier-1 falla — cheapest curtailment rung; drives
        # ``min_falla_gcost`` in the new anchor formula.
        {
            "number": 998,
            "name": "FALLA_T1",
            "type": "falla",
            "bus": 0,
            "ser_hid": 0,
            "efficiency": 0.0,
            "emax": 0.0,
            "gcost": _FALLA_GCOST_MIN,
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
    """Anchor = (avg_thermal_gcost + min_falla_gcost) / 2.

    Fixture thermals: PEAKER (200) + BASE_THERMAL (50) → avg = 125.
    Fixture falla:    FALLA_T1 (100) and FALLA_T4 (568.4) → min = 100.
    Anchor = (125 + 100) / 2 = 112.5 $/MWh.

    Pins the "midpoint between average-thermal-base-power-price and
    cheapest curtailment rung" formula and its directional reductions:
    ``avg(termica.gcost)`` averages both PEAKER and BASE_THERMAL (no
    longer just the max); ``min(falla.gcost)`` selects FALLA_T1 (100)
    over FALLA_T4 (568.4).
    """
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
    """anchor × lost_pf × 1e6 / 3600 [$/hm³].

    LMAULE cascade lost_pf = 10.05 → 112.5 × 10.05 × 1e6 / 3600 ≈ 314,063
    under the ``ANCHOR = (avg_thermal_gcost + min_falla_gcost) / 2``
    formula (= 112.5 from the avg([PEAKER 200, BASE_THERMAL 50]) +
    FALLA_T1 100 fixture).
    """
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    cost = resolver.efin_cost(10.05)
    expected = _ANCHOR_AUTO * 10.05 * 1e6 / 3600.0
    assert cost == pytest.approx(expected, rel=1e-6)


def test_fail_cost_formula() -> None:
    """anchor × lost_pf [$/(m³/s·h)].

    ABANICO own rendi 1.2 → 112.5 × 1.2 = 135 under the avg-thermal anchor.
    ANTUCO own rendi 1.6 → 112.5 × 1.6 = 180.
    """
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
    )
    assert resolver.fail_cost(1.2) == pytest.approx(_ANCHOR_AUTO * 1.2, rel=1e-6)
    assert resolver.fail_cost(1.6) == pytest.approx(_ANCHOR_AUTO * 1.6, rel=1e-6)


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


# ---------------------------------------------------------------------------
# cut_water_values OVERWRITE (boundary-cut lower bound) — efin_cost_for
# ---------------------------------------------------------------------------


def test_efin_cost_for_overwritten_by_cut_value() -> None:
    """``efin_cost_for`` OVERWRITES the auto value with ``cut_water_values[name]``.

    Semantics changed (2026-06-16): a present cut value replaces the
    auto estimate entirely — no longer ``min(auto, cap)``.  The
    overwrite wins whether it is below OR above the auto value.
    """
    cp = _FakeCentralParser(_make_centrals())

    # Pick a small lost_pf so the $/hm³ conversion stays comparable to
    # the cut numbers; the exact value does not matter — the overwrite
    # ignores the auto value entirely when a cut value is present.
    resolver_high = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"water_fail_cost": 500.0 * 3600.0 / 1e6},
        cut_water_values={"LMAULE": 200.0, "COLBUN": 700.0},
    )
    # auto = 500.0 * 1.0 = 500.0
    assert resolver_high.efin_cost(1.0) == pytest.approx(500.0, rel=1e-3)
    # LMAULE: cut 200 OVERWRITES auto 500 → 200 (below auto).
    assert resolver_high.efin_cost_for("LMAULE", 1.0) == pytest.approx(200.0)
    # COLBUN: cut 700 OVERWRITES auto 500 → 700 (ABOVE auto — the old
    # min-cap would have returned 500; overwrite returns 700).
    assert resolver_high.efin_cost_for("COLBUN", 1.0) == pytest.approx(700.0)


def test_efin_cost_for_no_boundary_data_falls_back() -> None:
    """Empty cut dict ⇒ ``efin_cost_for`` returns the auto value."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
        cut_water_values={},
    )
    auto = resolver.efin_cost(10.05)
    assert resolver.efin_cost_for("LMAULE", 10.05) == pytest.approx(auto)


def test_efin_cost_for_missing_reservoir_falls_back() -> None:
    """Reservoirs not in the cut dict keep the auto estimate."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
        cut_water_values={"OTHER": 1.0},
    )
    auto = resolver.efin_cost(10.05)
    assert resolver.efin_cost_for("LMAULE", 10.05) == pytest.approx(auto)


def test_efin_cost_back_compat_ignores_cut_values() -> None:
    """Back-compat: ``efin_cost(lost_pf)`` ignores the cut dict (auto only)."""
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
        cut_water_values={"LMAULE": 1.0},  # would overwrite via efin_cost_for
    )
    # The legacy non-reservoir-aware method must not apply the override.
    expected = _ANCHOR_AUTO * 10.05 * 1e6 / 3600.0
    assert resolver.efin_cost(10.05) == pytest.approx(expected, rel=1e-6)


def test_soft_emin_cost_uses_auto_not_overwrite() -> None:
    """The cut dict is plumbed only through :meth:`efin_cost_for`.

    The legacy :meth:`efin_cost` method (used by the soft-emin path via
    the same cost variable in junction_writer) is unaffected — the
    boundary-cut overwrite belongs on ``efin_cost``, not ``soft_emin``.
    """
    cp = _FakeCentralParser(_make_centrals())
    resolver = WaterValueResolver(
        central_parser=cp,
        cenre_parser=None,
        options={"auto_water_fail_cost": True},
        cut_water_values={"LMAULE": 1.0},
    )
    # Identical input → identical output: the overwrite is not applied.
    expected = _ANCHOR_AUTO * 10.05 * 1e6 / 3600.0
    assert resolver.efin_cost(10.05) == pytest.approx(expected, rel=1e-6)


# ---------------------------------------------------------------------------
# default_water_fail_value — global fallback = max over per-reservoir values
# ---------------------------------------------------------------------------


def test_default_water_fail_value_is_max() -> None:
    """The global default water-fail value is the max of all inputs."""
    from gtopt_shared.water_values import default_water_fail_value  # noqa: PLC0415

    assert default_water_fail_value([10.0, 250.0, 99.0]) == pytest.approx(250.0)
    # Empty / all-None → 0.0 (caller treats as no-op).
    assert default_water_fail_value([]) == pytest.approx(0.0)


# ─────────────────────────────────────────────────────────────────────────────
# HydroMixin._build_cut_water_values — PV-discounting of cut lower bounds
# ─────────────────────────────────────────────────────────────────────────────


class _FakePlanosParser:
    """Stub mirroring ``PlanosParser.lower_bound_water_value_by_reservoir``."""

    def __init__(self, raw: Dict[str, float]) -> None:
        self._raw = dict(raw)

    def lower_bound_water_value_by_reservoir(
        self, *, num_scenarios: Any = None, apply_fescala: bool = True
    ) -> Dict[str, float]:
        # Both kwargs are accepted but ignored — this fixture pre-computes
        # the lower bound to keep the tests focused on the discount step.
        _ = (num_scenarios, apply_fescala)
        return dict(self._raw)


class _FakeStageParser:
    """Stub mirroring ``StageParser.stages``."""

    def __init__(self, stages: List[Dict[str, Any]]) -> None:
        self.stages = list(stages)


class _FakeParser:
    """Stub mirroring ``Plp2GtoptParser.parsed_data`` for HydroMixin."""

    def __init__(self, parsed_data: Dict[str, Any]) -> None:
        self.parsed_data = dict(parsed_data)


def _build_cut_values(parsed_data: Dict[str, Any]) -> Dict[str, float]:
    """Invoke ``HydroMixin._build_cut_water_values`` against the stub parser."""
    from plp2gtopt._writer_hydro import HydroMixin  # noqa: PLC0415

    class _Probe(HydroMixin):
        pass

    probe = _Probe()
    probe.parser = _FakeParser(parsed_data)  # type: ignore[assignment]
    probe.planning = {}  # type: ignore[assignment]
    return probe._build_cut_water_values()  # pylint: disable=protected-access


def test_build_cut_water_values_undiscounts_to_face_value() -> None:
    """Cut lower bound is **divided** by the last stage discount factor.

    PLP's cut gradients on disk are already-discounted (``∂E[Z*]/∂v_i``
    where ``Z*`` is the LP's NPV objective).  Dividing the raw value by
    the last stage's ``discount_factor = 1 / FactTasa`` un-discounts the
    cuts back to the auto's face-value frame — raising the value by
    ``FactTasa`` (== ``1 / discount_factor``).
    """
    raw = {"LMAULE": 1000.0, "COLBUN": 250.0}
    stages = [
        {"number": 1, "month": 1, "duration": 720.0, "discount_factor": 1.0},
        {"number": 2, "month": 2, "duration": 720.0, "discount_factor": 0.5},
    ]
    out = _build_cut_values(
        {
            "planos_parser": _FakePlanosParser(raw),
            "stage_parser": _FakeStageParser(stages),
        }
    )
    assert out["LMAULE"] == pytest.approx(2000.0)  # 1000 / 0.5
    assert out["COLBUN"] == pytest.approx(500.0)  # 250 / 0.5


def test_build_cut_water_values_unity_discount_is_identity() -> None:
    """A trivial 1.0 discount factor must not perturb the raw value."""
    raw = {"LMAULE": 1000.0}
    stages = [{"number": 1, "month": 1, "duration": 720.0, "discount_factor": 1.0}]
    out = _build_cut_values(
        {
            "planos_parser": _FakePlanosParser(raw),
            "stage_parser": _FakeStageParser(stages),
        }
    )
    assert out["LMAULE"] == pytest.approx(1000.0)


def test_build_cut_water_values_missing_stage_parser_keeps_raw() -> None:
    """Without a stage parser the raw boundary-stage value is returned."""
    raw = {"LMAULE": 999.0}
    out = _build_cut_values({"planos_parser": _FakePlanosParser(raw)})
    assert out == pytest.approx(raw)


def test_build_cut_water_values_no_planos_parser_returns_empty() -> None:
    """No boundary cuts → empty dict → resolver keeps auto estimates."""
    out = _build_cut_values({})
    assert out == {}


def test_build_cut_water_values_negative_discount_falls_back_to_raw() -> None:
    """A non-positive discount factor is treated as ``no information``.

    Guards against pathological plpeta.dat entries (e.g. a malformed
    FactTasa).  The method must not return a negative value, which would
    silently flip the per-reservoir overwrite in ``efin_cost_for``.
    """
    raw = {"LMAULE": 500.0}
    stages = [
        {"number": 1, "month": 1, "duration": 720.0, "discount_factor": -0.5},
    ]
    out = _build_cut_values(
        {
            "planos_parser": _FakePlanosParser(raw),
            "stage_parser": _FakeStageParser(stages),
        }
    )
    assert out == pytest.approx(raw)


# ─────────────────────────────────────────────────────────────────────────────
# HydroMixin.apply_default_water_fail — default WF = max over priced reservoirs
# ─────────────────────────────────────────────────────────────────────────────


def _apply_default_wf(reservoir_array: List[Dict[str, Any]]) -> List[Dict[str, Any]]:
    """Run ``HydroMixin.apply_default_water_fail`` over a reservoir array."""
    from plp2gtopt._writer_hydro import HydroMixin  # noqa: PLC0415

    class _Probe(HydroMixin):
        pass

    probe = _Probe()
    probe.planning = {"system": {"reservoir_array": reservoir_array}}
    probe.apply_default_water_fail()  # pylint: disable=protected-access
    return reservoir_array


def test_apply_default_water_fail_stamps_max_on_unpriced() -> None:
    """The default WF = max(priced efin_cost) lands only on efin-but-unpriced."""
    reservoirs = [
        {"name": "A", "efin": 10.0, "efin_cost": 100.0},
        {"name": "B", "efin": 20.0, "efin_cost": 250.0},  # max priced
        {"name": "C", "efin": 30.0},  # efin but no efin_cost → patched
    ]
    out = _apply_default_wf(reservoirs)
    # Priced reservoirs are left untouched.
    assert out[0]["efin_cost"] == pytest.approx(100.0)
    assert out[1]["efin_cost"] == pytest.approx(250.0)
    # Unpriced reservoir inherits the global default = max(100, 250) = 250.
    assert out[2]["efin_cost"] == pytest.approx(250.0)


def test_apply_default_water_fail_skips_reservoirs_without_efin() -> None:
    """A reservoir with no terminal volume floor is never stamped."""
    reservoirs: List[Dict[str, Any]] = [
        {"name": "A", "efin": 10.0, "efin_cost": 100.0},
        {"name": "B"},  # no efin → not patched
        {"name": "C", "efin": 0.0},  # zero efin → not patched
    ]
    out = _apply_default_wf(reservoirs)
    assert "efin_cost" not in out[1]
    assert "efin_cost" not in out[2]


def test_apply_default_water_fail_noop_when_no_priced_reservoir() -> None:
    """With no positive efin_cost anywhere, nothing is stamped."""
    reservoirs = [
        {"name": "A", "efin": 10.0},
        {"name": "B", "efin": 20.0, "efin_cost": 0.0},
    ]
    out = _apply_default_wf(reservoirs)
    assert "efin_cost" not in out[0]
    assert out[1]["efin_cost"] == 0.0  # untouched


def test_apply_default_water_fail_empty_reservoir_array_is_noop() -> None:
    """No reservoirs → no error, no mutation."""
    assert not _apply_default_wf([])
