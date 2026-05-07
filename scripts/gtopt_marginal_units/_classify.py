# SPDX-License-Identifier: BSD-3-Clause
"""§4.4 classifier — map per-cell, per-generator state to one of the
8 status labels, in priority order.

The rule table is the canonical reference; the implementation here
tracks it line-for-line. Test coverage is in
``tests/test_classify.py`` and includes every row, including the
``hydro_marginal`` row 4 and ``extramarginal_interior`` row 7 that
the test-coverage P0 audit flagged.
"""

from __future__ import annotations

from typing import Optional

from gtopt_marginal_units.constants import (
    GeneratorKind,
    Status,
    Tolerances,
)


def classify(
    *,
    dispatch: float,
    pmin: float,
    pmax: float,
    marginal_cost: Optional[float],
    lmp: float,
    kind: str,
    tol: Tolerances = Tolerances.default(),
) -> Status:
    """Apply the master §4.4 priority-ordered rule table.

    The rules are evaluated top-to-bottom; the first match wins. This
    is deliberate so that bound proximity dominates economic position
    (a unit at pmin is never confused with a marginal unit even if
    its MC happens to equal the LMP).

    Args:
        dispatch: realised generation g [MW].
        pmin: technical minimum [MW].
        pmax: technical maximum [MW].
        marginal_cost: per-segment slope MC [$/MWh] at the active
            segment, or None for hydro/profile units without a
            declared MC.
        lmp: bus LMP λ_b [$/MWh].
        kind: GeneratorKind value (thermal / hydro / battery / profile).
        tol: tolerance bundle.

    Returns:
        Status — one of the 8 base labels in master §4.4.
    """
    # Profile (wind/solar) units are special-cased before the bound
    # tests — they can be at pmax during peak insolation but should
    # never be classified as `capped_pmax` in the price-setter sense.
    if kind == GeneratorKind.PROFILE.value and dispatch > tol.eps:
        return Status.PROFILE_DISPATCHED

    eps = max(tol.eps, tol.eps * max(pmax, 1.0))

    # Row 1 — off (and pmin allows it).
    if dispatch <= eps and pmin <= eps:
        return Status.OFF

    # Row 2 — forced at pmin (must-run / coincidentally-at-pmin).
    if pmin > eps and (pmin - eps) <= dispatch <= (pmin + eps):
        return Status.FORCED_PMIN

    # Row 3 — capped at pmax (binding-up; reduced cost negative in LP).
    if dispatch >= (pmax - eps):
        return Status.CAPPED_PMAX

    # Rows 4-7 — interior dispatch.
    interior = (pmin + eps) < dispatch < (pmax - eps)
    if interior:
        # Row 4 — hydro/battery interior. These set price via water value;
        # we tag them separately even when declared MC is present so the
        # consumer knows the recipe is hydro-driven (master §4.6.4 row).
        if kind in (GeneratorKind.HYDRO.value, GeneratorKind.BATTERY.value):
            return Status.HYDRO_MARGINAL

        # Row 5 — marginal (MC ≈ λ_b).
        # Row 6 — inframarginal (MC < λ_b − tol).
        # Row 7 — extramarginal interior (MC > λ_b + tol; should not
        #         occur in a non-degenerate LP, flag as such).
        if marginal_cost is None:
            # No declared MC and not hydro/battery — usually a
            # configuration bug. Mark inframarginal so the unit
            # appears in the per_bus table but not as price-setter.
            return Status.INFRAMARGINAL
        gap = marginal_cost - lmp
        price_tol = max(tol.tol_price, tol.tol_price * abs(lmp))
        if abs(gap) <= price_tol:
            return Status.MARGINAL
        if gap < 0:
            return Status.INFRAMARGINAL
        return Status.EXTRAMARGINAL_INTERIOR

    # Catch-all: dispatch ≤ eps but pmin > eps (rare — unit went off
    # but its lower bound is non-zero). Treat as forced_pmin so the
    # zone-picker can still pick it up if §4.5 forced-pmin fallback
    # fires.
    return Status.FORCED_PMIN
