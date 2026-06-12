# SPDX-License-Identifier: BSD-3-Clause
"""§4.7 reconstruction algorithm — recover the bus LMP from realised
operation when the LP duals are absent.

Five steps:
* R1 — recover the saturation pattern (CDC declaration / LP dual /
  flow-only / PTDF estimate, in priority order).
* R2 — partition into zones via §4.3.
* R3 — for each zone, pick λ_z from the merit-order rule with the
  five corner cases (demand-fail / all-pmax / interior /
  forced-pmin fallback / unattributed).
* R4 — set lmp[bus] := λ_z for every bus in zone z; classifier runs
  unchanged with confidence='merit_order'.
* R5 — when the feed *did* contain a published LMP, compute
  lmp_delta and the audit columns.

Per the lp-numerics P0 review: the all-pmax corner sets λ_z =
demand_fail_cost (NOT cheapest pmax-pinned MC), the merit-order rule
filters by ``merit_eligible`` (§4.11.2), and the final clamp pins λ_z
∈ [0, demand_fail_cost].
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Optional

from gtopt_canonical_feed import Generator, Topology
from gtopt_marginal_units.constants import (
    Confidence,
    GeneratorKind,
    PROFILE_KINDS,
    Tolerances,
)


@dataclass(slots=True)
class ZoneR3Result:
    """Result of R3 for one zone."""

    zone_id: int
    lambda_z: float
    formula_kind: str  # FormulaKind.value
    marginal_gen_uids: list[int]
    confidence: Confidence
    degenerate: bool
    reason: str
    clamped: bool  # True if the final demand-fail-cost clamp fired


def reconstruct_zone_lambda(
    *,
    zone_id: int,
    zone_buses: list[int],
    generators_in_zone: list[Generator],
    dispatch_by_uid: dict[int, float],
    zone_load: float,
    demand_fail_cost: float,
    tol: Tolerances = Tolerances.default(),
    merit_eligible_by_uid: Optional[dict[int, bool]] = None,
) -> ZoneR3Result:
    """Apply the master §4.7 R3 cascade to one zone.

    Cascade priority (must remain in this order):
      1. Zone load > Σ pmax of merit-eligible units → demand_fail.
      2. All units in zone pinned at pmax → demand_fail (lp-numerics P0.4).
      3. At least one merit-eligible interior unit → max declared_MC.
      4. Forced-pmin fallback (no interior; |zone_load − Σpmin| ≤ tol_load_mw).
      5. Unattributed.

    The final clamp pins λ_z ∈ [0, demand_fail_cost]; values above the
    cap are clamped with confidence=fallback.
    """
    if merit_eligible_by_uid is None:
        merit_eligible_by_uid = {
            g.uid: g.kind not in {k.value for k in PROFILE_KINDS}
            for g in generators_in_zone
        }

    eps = tol.eps

    # --- Step (1): demand-fail by capacity exhaustion ------------------------
    eligible_pmax_sum = sum(
        g.pmax for g in generators_in_zone if merit_eligible_by_uid.get(g.uid, True)
    )
    if zone_load > eligible_pmax_sum + tol.tol_load_mw:
        return _maybe_clamp(
            ZoneR3Result(
                zone_id=zone_id,
                lambda_z=demand_fail_cost,
                formula_kind="demand_fail",
                marginal_gen_uids=[],
                confidence=Confidence.FALLBACK,
                degenerate=True,
                reason="zone_load_exceeds_sum_pmax",
                clamped=False,
            ),
            demand_fail_cost,
            tol,
        )

    # --- Step (2): all-pmax corner -------------------------------------------
    # An eligible unit is "at pmax" if dispatch ≥ pmax − eps.
    eligible_units = [
        g for g in generators_in_zone if merit_eligible_by_uid.get(g.uid, True)
    ]
    if eligible_units:
        all_at_pmax = all(
            dispatch_by_uid.get(g.uid, 0.0) >= g.pmax - eps for g in eligible_units
        )
        # Note: an empty zone (no eligible units) is degenerate; skip
        # this branch and fall through to the unattributed terminal.
        if all_at_pmax:
            return _maybe_clamp(
                ZoneR3Result(
                    zone_id=zone_id,
                    lambda_z=demand_fail_cost,
                    formula_kind="demand_fail",
                    marginal_gen_uids=[],
                    confidence=Confidence.FALLBACK,
                    degenerate=True,
                    reason="all_units_at_pmax",  # lp-numerics P0.4 rule
                    clamped=False,
                ),
                demand_fail_cost,
                tol,
            )

    # --- Step (3): interior merit-order pick ---------------------------------
    interior = []
    for g in eligible_units:
        d = dispatch_by_uid.get(g.uid, 0.0)
        if (g.pmin + eps) < d < (g.pmax - eps):
            interior.append(g)
    if interior:
        # Hydro / battery interior units have no declared_MC by
        # convention — they are price-takers in v1, so we filter them
        # out for the max-MC rule and let the consumer interpret the
        # FormulaKind.HYDRO_MARGINAL case explicitly via §4.5.
        with_mc = [g for g in interior if g.declared_MC is not None]
        hydro_or_battery = [
            g
            for g in interior
            if g.kind in (GeneratorKind.HYDRO.value, GeneratorKind.BATTERY.value)
        ]
        if with_mc:
            # ``with_mc`` filters to declared_MC is not None, so the
            # cast to float here is safe — but mypy can't see that.
            max_mc: float = max(
                float(g.declared_MC) for g in with_mc if g.declared_MC is not None
            )
            tied = [
                g
                for g in with_mc
                if g.declared_MC is not None
                and abs(float(g.declared_MC) - max_mc) <= tol.tol_price
            ]
            tied.sort(key=lambda g: g.uid)  # deterministic tie-break
            kind = "single_unit" if len(tied) == 1 else "tied_units"
            return _maybe_clamp(
                ZoneR3Result(
                    zone_id=zone_id,
                    lambda_z=float(max_mc),
                    formula_kind=kind,
                    marginal_gen_uids=[g.uid for g in tied],
                    confidence=Confidence.MERIT_ORDER,
                    degenerate=False,
                    reason="interior_merit_order_max",
                    clamped=False,
                ),
                demand_fail_cost,
                tol,
            )
        if hydro_or_battery:
            # Pick the lowest-uid hydro/battery as the formula anchor;
            # consumer can audit via the recipe table.
            anchor = sorted(hydro_or_battery, key=lambda g: g.uid)[0]
            return _maybe_clamp(
                ZoneR3Result(
                    zone_id=zone_id,
                    lambda_z=anchor.declared_MC
                    if anchor.declared_MC is not None
                    else 0.0,
                    formula_kind="hydro_marginal",
                    marginal_gen_uids=[anchor.uid],
                    confidence=Confidence.MERIT_ORDER,
                    degenerate=anchor.declared_MC is None,
                    reason="interior_hydro_only",
                    clamped=False,
                ),
                demand_fail_cost,
                tol,
            )

    # --- Step (4): forced-pmin fallback --------------------------------------
    # No interior unit found, but the realised dispatch sums to
    # exactly Σ pmin (within tol_load_mw) of must-run units → those
    # units set the price together.
    forced_units = [
        g
        for g in eligible_units
        if g.pmin > eps
        and dispatch_by_uid.get(g.uid, 0.0) <= g.pmin + eps
        and dispatch_by_uid.get(g.uid, 0.0) >= g.pmin - eps
    ]
    if forced_units:
        sum_pmin = sum(g.pmin for g in forced_units)
        if abs(zone_load - sum_pmin) <= tol.tol_load_mw:
            forced_units.sort(key=lambda g: (g.declared_MC or 0.0, g.uid))
            anchor = forced_units[0]
            mc = anchor.declared_MC if anchor.declared_MC is not None else 0.0
            return _maybe_clamp(
                ZoneR3Result(
                    zone_id=zone_id,
                    lambda_z=float(mc),
                    formula_kind="forced_pmin_marginal",
                    marginal_gen_uids=[g.uid for g in forced_units],
                    confidence=Confidence.MERIT_ORDER,
                    degenerate=True,
                    reason="forced_pmin_zone_picker",
                    clamped=False,
                ),
                demand_fail_cost,
                tol,
            )

    # --- Step (5): unattributed ----------------------------------------------
    return _maybe_clamp(
        ZoneR3Result(
            zone_id=zone_id,
            lambda_z=0.0,
            formula_kind="unattributed",
            marginal_gen_uids=[],
            confidence=Confidence.FALLBACK,
            degenerate=True,
            reason="no_eligible_marginal_unit",
            clamped=False,
        ),
        demand_fail_cost,
        tol,
    )


def _maybe_clamp(
    result: ZoneR3Result,
    demand_fail_cost: float,
    tol: Tolerances,
) -> ZoneR3Result:
    """Apply the final-clamp rule from master §4.7 R3 (last paragraph)."""
    if result.lambda_z > demand_fail_cost + tol.tol_price:
        return ZoneR3Result(
            zone_id=result.zone_id,
            lambda_z=demand_fail_cost,
            formula_kind=result.formula_kind,
            marginal_gen_uids=result.marginal_gen_uids,
            confidence=Confidence.FALLBACK,
            degenerate=True,
            reason=result.reason + ";clamped_to_demand_fail_cost",
            clamped=True,
        )
    if result.lambda_z < 0.0:
        return ZoneR3Result(
            zone_id=result.zone_id,
            lambda_z=0.0,
            formula_kind=result.formula_kind,
            marginal_gen_uids=result.marginal_gen_uids,
            confidence=Confidence.FALLBACK,
            degenerate=True,
            reason=result.reason + ";clamped_negative_to_zero",
            clamped=True,
        )
    return result


# ---------------------------------------------------------------------------
# Convenience: apply the cascade across all zones in one call.
# ---------------------------------------------------------------------------


def reconstruct_all_zones(
    *,
    topology: Topology,
    zone_of: dict[int, int],
    dispatch_by_uid: dict[int, float],
    load_by_bus: dict[int, float],
    demand_fail_cost: float,
    tol: Tolerances = Tolerances.default(),
    merit_eligible_by_uid: Optional[dict[int, bool]] = None,
) -> dict[int, ZoneR3Result]:
    """Apply R3 to every zone. Returns {zone_id: ZoneR3Result}."""
    # Group generators by zone.
    gens_by_zone: dict[int, list[Generator]] = {}
    for g in topology.generators:
        z = zone_of.get(g.bus_uid)
        if z is None:
            continue
        gens_by_zone.setdefault(z, []).append(g)

    # Sum loads per zone.
    load_by_zone: dict[int, float] = {}
    for bus_uid, lz in zone_of.items():
        load_by_zone[lz] = load_by_zone.get(lz, 0.0) + float(
            load_by_bus.get(bus_uid, 0.0)
        )

    out: dict[int, ZoneR3Result] = {}
    for zid in sorted(set(zone_of.values())):
        zone_buses = [u for u, z in zone_of.items() if z == zid]
        out[zid] = reconstruct_zone_lambda(
            zone_id=zid,
            zone_buses=zone_buses,
            generators_in_zone=gens_by_zone.get(zid, []),
            dispatch_by_uid=dispatch_by_uid,
            zone_load=load_by_zone.get(zid, 0.0),
            demand_fail_cost=demand_fail_cost,
            tol=tol,
            merit_eligible_by_uid=merit_eligible_by_uid,
        )
    return out
