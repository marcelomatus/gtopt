# SPDX-License-Identifier: BSD-3-Clause
"""Classifier rule-table tests — master §4.4 8-status table.

Covers every row including hydro_marginal (row 4) and
extramarginal_interior (row 7), which the test-coverage P0 audit
flagged as missing in earlier drafts.
"""

from __future__ import annotations

import pytest

from gtopt_marginal_units._classify import classify
from gtopt_marginal_units.constants import GeneratorKind, Status, Tolerances


_TOL = Tolerances.default()


def _classify(**kw) -> Status:
    """Wrapper that fills in defaults so each test highlights only the
    relevant input."""
    defaults = {
        "dispatch": 50.0,
        "pmin": 0.0,
        "pmax": 100.0,
        "marginal_cost": 20.0,
        "lmp": 20.0,
        "kind": GeneratorKind.THERMAL.value,
        "tol": _TOL,
    }
    defaults.update(kw)
    return classify(**defaults)


# ---------------------------------------------------------------------------
# Row 1 — off
# ---------------------------------------------------------------------------


def test_off_when_dispatch_zero_and_pmin_zero():
    assert _classify(dispatch=0.0, pmin=0.0) == Status.OFF


def test_off_when_dispatch_just_below_eps():
    assert _classify(dispatch=1e-5, pmin=0.0) == Status.OFF


# ---------------------------------------------------------------------------
# Row 2 — forced_pmin
# ---------------------------------------------------------------------------


def test_forced_pmin_when_dispatch_at_pmin():
    assert _classify(dispatch=10.0, pmin=10.0) == Status.FORCED_PMIN


def test_forced_pmin_below_eps_with_positive_pmin():
    # dispatch ≈ 0 but pmin > 0 — treated as forced_pmin so the
    # zone-picker can pick the unit up via the §4.5 fallback.
    assert _classify(dispatch=0.0, pmin=10.0) == Status.FORCED_PMIN


# ---------------------------------------------------------------------------
# Row 3 — capped_pmax
# ---------------------------------------------------------------------------


def test_capped_pmax_when_dispatch_at_pmax():
    assert _classify(dispatch=100.0, pmax=100.0) == Status.CAPPED_PMAX


def test_capped_pmax_just_below_pmax():
    assert _classify(dispatch=99.99999, pmax=100.0) == Status.CAPPED_PMAX


# ---------------------------------------------------------------------------
# Row 4 — hydro_marginal (gap caught by test-coverage P0 audit)
# ---------------------------------------------------------------------------


def test_hydro_interior_classifies_as_hydro_marginal():
    assert (
        _classify(dispatch=40.0, kind=GeneratorKind.HYDRO.value)
        == Status.HYDRO_MARGINAL
    )


def test_battery_interior_classifies_as_hydro_marginal():
    assert (
        _classify(dispatch=40.0, kind=GeneratorKind.BATTERY.value)
        == Status.HYDRO_MARGINAL
    )


def test_hydro_with_no_declared_MC_still_hydro_marginal():
    # Row 4 wins before the MC-vs-λ comparison, so a hydro unit
    # without declared_MC still classifies as hydro_marginal.
    assert (
        _classify(dispatch=40.0, marginal_cost=None, kind=GeneratorKind.HYDRO.value)
        == Status.HYDRO_MARGINAL
    )


# ---------------------------------------------------------------------------
# Row 5 — marginal (MC ≈ λ_b within tol_price)
# ---------------------------------------------------------------------------


def test_marginal_when_mc_matches_lmp_exactly():
    assert _classify(dispatch=50.0, marginal_cost=20.0, lmp=20.0) == Status.MARGINAL


def test_marginal_within_tol_price():
    # tol_price scales with |lmp|; for lmp=100, tol_price = 0.1 by default.
    assert (
        _classify(dispatch=50.0, marginal_cost=99.95, lmp=100.0, pmax=200.0)
        == Status.MARGINAL
    )


# ---------------------------------------------------------------------------
# Row 6 — inframarginal (MC < λ_b)
# ---------------------------------------------------------------------------


def test_inframarginal_when_mc_below_lmp():
    assert (
        _classify(dispatch=50.0, marginal_cost=10.0, lmp=20.0) == Status.INFRAMARGINAL
    )


# ---------------------------------------------------------------------------
# Row 7 — extramarginal_interior (gap caught by test-coverage P0 audit)
# ---------------------------------------------------------------------------


def test_extramarginal_interior_when_mc_above_lmp():
    # MC > λ_b but unit is interior — should not happen in a healthy
    # LP solve. Distinct status so the report can flag the cell.
    assert (
        _classify(dispatch=50.0, marginal_cost=30.0, lmp=10.0)
        == Status.EXTRAMARGINAL_INTERIOR
    )


def test_extramarginal_interior_does_not_collide_with_marginal():
    # Just outside tol_price → must be extramarginal, not marginal.
    assert (
        _classify(dispatch=50.0, marginal_cost=100.5, lmp=100.0, pmax=200.0)
        == Status.EXTRAMARGINAL_INTERIOR
    )


# ---------------------------------------------------------------------------
# Row 8 — profile_dispatched (renewables — never marginal)
# ---------------------------------------------------------------------------


def test_profile_dispatched_at_pmax_not_capped():
    # Profile unit at full output is profile_dispatched, NOT capped_pmax —
    # solar/wind producing at the curtailment cap doesn't "cap" the price.
    assert (
        _classify(dispatch=100.0, pmax=100.0, kind=GeneratorKind.PROFILE.value)
        == Status.PROFILE_DISPATCHED
    )


def test_profile_off_classifies_as_off():
    assert _classify(dispatch=0.0, kind=GeneratorKind.PROFILE.value) == Status.OFF


# ---------------------------------------------------------------------------
# Priority-order check: when two rules could match, the higher-priority one
# wins. Row 2 (forced_pmin) must beat row 5 (marginal) when dispatch is at
# pmin AND MC happens to equal lmp.
# ---------------------------------------------------------------------------


def test_priority_forced_pmin_beats_marginal():
    assert (
        _classify(dispatch=10.0, pmin=10.0, marginal_cost=20.0, lmp=20.0)
        == Status.FORCED_PMIN
    )


def test_priority_capped_pmax_beats_marginal():
    assert (
        _classify(dispatch=100.0, pmax=100.0, marginal_cost=20.0, lmp=20.0)
        == Status.CAPPED_PMAX
    )


# ---------------------------------------------------------------------------
# Tolerance edge case: gap exactly at tol_price (should resolve as marginal).
# ---------------------------------------------------------------------------


@pytest.mark.parametrize("gap_sign", [+1.0, -1.0])
def test_marginal_just_inside_tol_price(gap_sign):
    # Slightly inside tol_price (avoids float boundary precision issues).
    # tol_price scales with |lmp|; for lmp=1.0, default tol_price = 1e-3.
    lmp = 1.0
    gap = gap_sign * 0.999e-3  # well inside the 1e-3 envelope
    assert _classify(dispatch=50.0, marginal_cost=lmp + gap, lmp=lmp) == Status.MARGINAL


@pytest.mark.parametrize("gap_sign", [+1.0, -1.0])
def test_just_outside_tol_price_falls_off_marginal(gap_sign):
    # Slightly outside tol_price — must classify as either inframarginal
    # (gap < 0) or extramarginal_interior (gap > 0), not marginal.
    lmp = 1.0
    gap = gap_sign * 1.5e-3
    expected = Status.EXTRAMARGINAL_INTERIOR if gap_sign > 0 else Status.INFRAMARGINAL
    assert _classify(dispatch=50.0, marginal_cost=lmp + gap, lmp=lmp) == expected


# ---------------------------------------------------------------------------
# Bound-priority over hydro/battery rule (Row 4 must lose to Rows 2 and 3).
#
# The review's "Priority order" subsection only enumerates thermal cases. A
# hydro unit at exactly pmin should still classify as ``forced_pmin`` and at
# exactly pmax as ``capped_pmax`` — the kind-driven Row 4 rule only fires
# for *interior* dispatch. Without these tests the implementation could
# silently route an at-bound hydro unit through ``hydro_marginal`` and
# corrupt the per-bus recipe table.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "kind", [GeneratorKind.HYDRO.value, GeneratorKind.BATTERY.value]
)
def test_hydro_at_pmin_is_forced_pmin_not_hydro_marginal(kind):
    assert (
        _classify(dispatch=10.0, pmin=10.0, pmax=100.0, kind=kind) == Status.FORCED_PMIN
    )


@pytest.mark.parametrize(
    "kind", [GeneratorKind.HYDRO.value, GeneratorKind.BATTERY.value]
)
def test_hydro_at_pmax_is_capped_pmax_not_hydro_marginal(kind):
    assert (
        _classify(dispatch=100.0, pmin=0.0, pmax=100.0, kind=kind) == Status.CAPPED_PMAX
    )


# ---------------------------------------------------------------------------
# Interior thermal with no declared MC — Rows 5–7 require a numeric MC; the
# fallback branch in ``_classify`` must classify the unit as ``inframarginal``
# rather than crashing on the gap arithmetic.
# ---------------------------------------------------------------------------


def test_thermal_interior_with_null_mc_is_inframarginal():
    assert (
        _classify(
            dispatch=50.0,
            pmin=0.0,
            pmax=100.0,
            marginal_cost=None,
            lmp=20.0,
            kind=GeneratorKind.THERMAL.value,
        )
        == Status.INFRAMARGINAL
    )


# ---------------------------------------------------------------------------
# Profile-unit boundary: at exactly ``dispatch == tol.eps`` the profile
# special-case (``dispatch > tol.eps``) does NOT fire and Row 1 (off) wins.
# This locks in the strict-greater-than semantics of the implementation.
# ---------------------------------------------------------------------------


def test_profile_at_exactly_eps_classifies_as_off():
    # Default tol.eps = 1e-4. Pass dispatch == eps (boundary).
    assert (
        _classify(
            dispatch=_TOL.eps,
            pmin=0.0,
            pmax=100.0,
            kind=GeneratorKind.PROFILE.value,
        )
        == Status.OFF
    )


def test_profile_just_above_eps_classifies_as_profile_dispatched():
    assert (
        _classify(
            dispatch=_TOL.eps * 10.0,  # comfortably above eps
            pmin=0.0,
            pmax=100.0,
            kind=GeneratorKind.PROFILE.value,
        )
        == Status.PROFILE_DISPATCHED
    )
