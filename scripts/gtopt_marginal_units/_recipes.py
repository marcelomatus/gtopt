# SPDX-License-Identifier: BSD-3-Clause
"""Bus price recipe + bus emission-intensity recipe — the auditable
formulas that let downstream consumers recompute λ_b and ε_b under
alternative cost / emission catalogues.

Master plan §4.6.4 (price) and §4.12.2 (emission). Both share
``marginal_gen_uids`` and ``marginal_weights`` by construction (Lin
& Tang 2024) — only the per-unit datum differs.

Writer-side invariant (master §4.6.4 invariant 1):
    |recomputed_lmp − zone_lmp| ≤ tol_price
A violation aborts the run with exit 3 — never silently writes a
broken recipe.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Optional

from gtopt_canonical_feed import Topology
from gtopt_marginal_units._reconstruct import ZoneR3Result
from gtopt_marginal_units.constants import FormulaKind, Tolerances
from gtopt_marginal_units.errors import AttributionError


def _isnan(value: float) -> bool:
    return math.isnan(value)


@dataclass(slots=True)
class RecipeRow:
    """One row of bus_price_recipe.parquet (or its emission-intensity
    sibling — same shape, swap the per-unit datum)."""

    cell_key: tuple[object, ...]
    bus_uid: int
    zone_id: int
    formula_kind: str
    marginal_gen_uids: list[int]
    marginal_weights: list[float] = field(default_factory=list)
    marginal_data: list[float] = field(default_factory=list)  # MC or emission_rate
    formula_constant: float = 0.0
    formula_explanation: str = ""
    recomputed_value: float = 0.0  # λ_b for price recipe; ε_b for emission
    # Emission recipe only: the "consequential MOER" — the emission rate
    # of the gen that would absorb +1 MWh of demand if the marginal were
    # forced to its bound (i.e. the next-up dispatchable thermal in the
    # merit order).  For hydro / renewable marginals (``recomputed_value
    # ≈ 0``), this carries the **carbon opportunity cost of water /
    # storage** — what extra demand actually costs in CO2eq, vs the
    # zero "direct attribution" of the marginal hydro itself.  Always
    # 0 on price recipe rows.
    consequential_co2eq: float = 0.0
    # The gen uid that drives ``consequential_co2eq`` (the next-up
    # thermal).  ``None`` when the marginal already has non-zero
    # emission (direct == consequential), or when no headroom-bearing
    # thermal is reachable in the cell (demand-fail / island).
    consequential_gen_uid: Optional[int] = None

    def to_dict(self, value_col: str) -> dict[str, object]:
        base = {
            **_unpack_cell_key(self.cell_key),
            "bus_uid": self.bus_uid,
            "zone_id": self.zone_id,
            "formula_kind": self.formula_kind,
            "marginal_gen_uids": list(self.marginal_gen_uids),
            "marginal_weights": list(self.marginal_weights),
            (
                "marginal_costs" if value_col == "lmp" else "marginal_emission_factors"
            ): list(self.marginal_data),
            "formula_constant": float(self.formula_constant),
            "formula_explanation": self.formula_explanation,
            (
                "recomputed_lmp"
                if value_col == "lmp"
                else "recomputed_emission_intensity"
            ): float(self.recomputed_value),
        }
        if value_col != "lmp":
            # Emission recipe carries the consequential MOER + its source.
            base["consequential_co2eq_kg_per_mwh"] = float(self.consequential_co2eq)
            base["consequential_gen_uid"] = (
                int(self.consequential_gen_uid)
                if self.consequential_gen_uid is not None
                else -1
            )
        return base


def _unpack_cell_key(cell_key: tuple) -> dict[str, object]:
    """The cell_key is (scenario, stage, block, date_utc, hour, data_source)."""
    scenario, stage, block, date_utc, hour, data_source = cell_key
    return {
        "scenario": scenario,
        "stage": stage,
        "block": block,
        "date_utc": date_utc,
        "hour": hour,
        "data_source": data_source,
    }


def _compute_consequential_moer(
    gens_in_zone: list,
    dispatch_by_uid: dict[int, float],
    marginal_uids: set[int],
    tol: Tolerances,
) -> tuple[float, Optional[int]]:
    """Find the per-zone "consequential MOER" — the emission rate of the
    next thermal in merit order, skipping every battery / reservoir /
    renewable on the way up.

    Used by the emission recipe to assign a physically-meaningful
    "marginal CO2eq" to bus-cells whose direct marginal is hydro /
    renewable / battery (emission_rate ≈ 0).  By LP duality and the
    multi-criteria SDDP framing (carbon opportunity cost of water /
    storage), the true marginal emission of demand-shift in those
    cells is the emission rate of the **next thermal** that would
    backfill — not zero.

    Algorithm (matches the operator-side "walk up the island merit
    order until you hit a thermal with headroom" recipe):

      1. Build the merit order for the zone — every gen with
         ``declared_MC ≥ 0`` and ``pmax > 0``, sorted ascending by
         ``declared_MC`` (tie-break by uid).  This is the
         hypothetical-dispatch order if demand grew incrementally.
      2. Walk up that list.  Skip:
           * every gen that has zero ``emission_rate`` (batteries,
             reservoirs, RoR hydros, solar, wind, geothermal-flagged-
             renewable, etc. — anything not a combustion thermal)
           * every gen in the actual marginal set
           * every thermal already at ``pmax`` (``headroom_up ≤ eps``;
             it's saturated, can't absorb +1 MWh of demand, KEEP walking)
      3. Return the FIRST thermal hit with positive headroom — that's
         the unit that would actually backfill an extra MWh of demand.
         Notably this CAN be a thermal currently dispatching at 0 MW
         (full pmax available) — the operator's "cold-start the next
         peaker" answer is captured naturally.

    Returns ``(0.0, None)`` when no headroom-bearing thermal exists in
    the zone at all (truly thermal-saturated or all-renewable island).
    Such cells genuinely have zero local backfill capacity — demand
    extra would be served by imports (cross-zone) or demand_fail; the
    per-zone heuristic correctly returns 0 there.
    """
    eligible = sorted(
        (
            g
            for g in gens_in_zone
            if (
                g.declared_MC is not None
                and g.emission_rate is not None
                and float(g.emission_rate) > 0.0
                and float(g.pmax) > tol.eps
                and g.uid not in marginal_uids
            )
        ),
        key=lambda g: (float(g.declared_MC), g.uid),
    )
    eps = max(tol.eps, 1e-6)
    for g in eligible:
        disp = float(dispatch_by_uid.get(g.uid, 0.0))
        headroom_up = float(g.pmax) - disp
        if headroom_up > eps:
            return float(g.emission_rate), int(g.uid)
    return 0.0, None


def build_recipes_for_cell(
    *,
    cell_key: tuple[object, ...],
    topology: Topology,
    zone_of: dict[int, int],
    zone_results: dict[int, ZoneR3Result],
    dispatch_by_uid: Optional[dict[int, float]] = None,
    demand_fail_cost: float = 1000.0,
    tol: Tolerances = Tolerances.default(),
) -> tuple[list[RecipeRow], list[RecipeRow]]:
    """Build (price_recipe_rows, emission_recipe_rows) for one cell.

    The two lists have identical ``marginal_gen_uids`` and
    ``marginal_weights`` per the Lin & Tang theorem — only
    ``marginal_data`` differs.

    Raises ``AttributionError`` when the writer-side invariant
    fails: |recomputed_lmp − zone_lmp| > tol_price.
    """
    # Index generators by uid for quick lookup.
    gen_by_uid = {g.uid: g for g in topology.generators}

    # Precompute "next-up thermal" per zone (consequential MOER) so we
    # can stamp it on every bus-cell row of the emission recipe.  The
    # ladder walk inspects every gen in the zone once per cell, not
    # once per (bus, cell), so this is O(zones × gens) not
    # O(buses × gens).
    dispatch_by_uid = dispatch_by_uid or {}
    consequential_by_zone: dict[int, tuple[float, Optional[int]]] = {}
    if zone_results:
        gens_by_zone: dict[int, list] = {}
        for g in topology.generators:
            z = zone_of.get(g.bus_uid)
            if z is None:
                continue
            gens_by_zone.setdefault(z, []).append(g)
        for zid, zres in zone_results.items():
            marginal_set = set(int(u) for u in zres.marginal_gen_uids)
            consequential_by_zone[zid] = _compute_consequential_moer(
                gens_by_zone.get(zid, []),
                dispatch_by_uid,
                marginal_set,
                tol,
            )

    price_rows: list[RecipeRow] = []
    emission_rows: list[RecipeRow] = []

    for bus_uid, zid in zone_of.items():
        zres = zone_results.get(zid)
        if zres is None:
            continue

        # Resolve formula data per FormulaKind.
        kind_str = zres.formula_kind
        marginal_uids = list(zres.marginal_gen_uids)
        weights, mcs, ems = _formula_data(kind_str, marginal_uids, gen_by_uid)

        # Compute recomputed_lmp and recomputed_ε from the *captured* data.
        if kind_str == FormulaKind.DEMAND_FAIL.value:
            r_lmp = demand_fail_cost
            r_em = 0.0
            constant_lmp = demand_fail_cost
            constant_em = 0.0
        elif kind_str == FormulaKind.RENEWABLE_CURTAILMENT.value:
            r_lmp = 0.0
            r_em = 0.0
            constant_lmp = 0.0
            constant_em = 0.0
        elif kind_str == FormulaKind.UNATTRIBUTED.value:
            r_lmp = float("nan")
            r_em = float("nan")
            constant_lmp = 0.0
            constant_em = 0.0
        elif kind_str == FormulaKind.HYDRO_MARGINAL.value:
            # Hydro/battery interior: ε is zero by master §4.12.2 convention.
            r_lmp = sum(w * m for w, m in zip(weights, mcs)) if mcs else zres.lambda_z
            r_em = 0.0
            constant_lmp = 0.0
            constant_em = 0.0
        else:  # single_unit / tied_units / forced_pmin_marginal
            # rc-based picks (``interior_rc_zero_lp_dual``) select the
            # basic-in-LP column whose JSON ``declared_MC`` may be 0
            # (hydro, renewables) or otherwise diverge from the LP's
            # marginal price.  In that case the LP-derived
            # ``zres.lambda_z`` IS the true LMP — use it directly.
            # The declared_MC weighted sum is meaningful only for the
            # legacy ``interior_match_lp_dual`` fallback path.
            if zres.reason == "interior_rc_zero_lp_dual":
                r_lmp = zres.lambda_z
            else:
                r_lmp = (
                    sum(w * m for w, m in zip(weights, mcs)) if mcs else zres.lambda_z
                )
            r_em = sum(w * e for w, e in zip(weights, ems)) if ems else 0.0
            constant_lmp = 0.0
            constant_em = 0.0

        # Writer-side invariant — but only check for the unit-driven
        # formulas where mcs is non-empty (degenerate / clamped /
        # unattributed cells are exempt: their lambda_z came from a
        # cap, not from the captured MCs).
        # Tolerance scales with |lambda_z| to match the classifier's
        # `max(tol_price, tol_price * abs(lmp))` rule — without this
        # scaling, a $0.04 absolute drift on a $48 LMP would
        # spuriously fire the invariant (real-mode LP-derived LMP vs
        # synthesised merit-order MC always carries some FP noise).
        #
        # Skip the round-trip when the picker used LP reduced costs
        # (``interior_rc_zero_lp_dual``): rc-based attribution selects
        # the basic-in-LP column, whose JSON ``declared_MC`` is a static
        # snapshot that may differ from the LP's actual marginal price
        # (hydro water-value, piecewise-segment slope, loss-adjusted
        # SRMC, etc.).  ``lambda_z`` IS the true marginal price by
        # construction; the recipe explanation still names the gens and
        # the consumer can refine ``r_lmp`` later by joining the
        # ``Reservoir/water_value_dual`` / ``Generator/srmc_sol``
        # parquet streams gtopt emits.
        scaled_tol = max(tol.tol_price, tol.tol_price * abs(zres.lambda_z))
        if (
            kind_str
            in {
                FormulaKind.SINGLE_UNIT.value,
                FormulaKind.TIED_UNITS.value,
            }
            and zres.reason != "interior_rc_zero_lp_dual"
            and mcs
            and abs(r_lmp - zres.lambda_z) > scaled_tol + 1e-9
        ):
            raise AttributionError(
                f"recipe round-trip mismatch on bus {bus_uid} zone {zid}: "
                f"recomputed={r_lmp:.6f} but zone_lmp={zres.lambda_z:.6f} "
                f"(scaled_tol={scaled_tol:.6f}, tol_price={tol.tol_price})"
            )

        explanation_lmp = _explain_lmp(kind_str, marginal_uids)
        explanation_em = _explain_em(kind_str, marginal_uids)

        price_rows.append(
            RecipeRow(
                cell_key=cell_key,
                bus_uid=bus_uid,
                zone_id=zid,
                formula_kind=kind_str,
                marginal_gen_uids=marginal_uids,
                marginal_weights=weights,
                marginal_data=mcs,
                formula_constant=constant_lmp,
                formula_explanation=explanation_lmp,
                recomputed_value=r_lmp
                if not _isnan(r_lmp)
                else 0.0,  # NaN→0 for parquet
            )
        )
        # Consequential MOER for this zone (the next-up thermal that
        # would absorb +1 MWh of demand if the current marginal moved
        # to its bound).  When the direct marginal already has non-zero
        # emission, the consequential rate IS the direct rate — no
        # displacement-chain needed.  Otherwise stamp the next-up
        # thermal's rate so consumers see the "carbon opportunity cost
        # of water / storage" rather than a misleading zero.
        cons_rate, cons_uid = consequential_by_zone.get(zid, (0.0, None))
        direct_em = r_em if not _isnan(r_em) else 0.0
        if direct_em > tol.eps:
            # Direct marginal already emits — consequential = direct.
            cons_rate = direct_em
            cons_uid = marginal_uids[0] if marginal_uids else None
        emission_rows.append(
            RecipeRow(
                cell_key=cell_key,
                bus_uid=bus_uid,
                zone_id=zid,
                formula_kind=kind_str,
                marginal_gen_uids=marginal_uids,
                marginal_weights=weights,
                marginal_data=ems,
                formula_constant=constant_em,
                formula_explanation=explanation_em,
                recomputed_value=direct_em,
                consequential_co2eq=cons_rate,
                consequential_gen_uid=cons_uid,
            )
        )

    return price_rows, emission_rows


def _formula_data(
    kind: str,
    marginal_uids: list[int],
    gen_by_uid: dict,
) -> tuple[list[float], list[float], list[float]]:
    """Resolve (weights, mcs, ems) for the given FormulaKind."""
    if not marginal_uids:
        return [], [], []
    weights = [1.0 / len(marginal_uids)] * len(marginal_uids)
    mcs: list[float] = []
    ems: list[float] = []
    for uid in marginal_uids:
        g = gen_by_uid.get(uid)
        if g is None:
            mcs.append(float("nan"))
            ems.append(float("nan"))
            continue
        mcs.append(float(g.declared_MC) if g.declared_MC is not None else float("nan"))
        ems.append(
            float(g.emission_rate) if g.emission_rate is not None else float("nan")
        )
    return weights, mcs, ems


def _explain_lmp(kind: str, marginal_uids: list[int]) -> str:
    if kind == FormulaKind.SINGLE_UNIT.value:
        return f"λ_b = MC of g{marginal_uids[0]}"
    if kind == FormulaKind.TIED_UNITS.value:
        return f"λ_b = MC of any of g{marginal_uids} (tied)"
    if kind == FormulaKind.FORCED_PMIN_MARGINAL.value:
        return f"λ_b = MC of forced-pmin g{marginal_uids}"
    if kind == FormulaKind.HYDRO_MARGINAL.value:
        return f"λ_b = water_value of g{marginal_uids[0]} (hydro)"
    if kind == FormulaKind.DEMAND_FAIL.value:
        return "λ_b = demand_fail_cost (rationing)"
    if kind == FormulaKind.RENEWABLE_CURTAILMENT.value:
        return "λ_b = 0 (renewable curtailment)"
    return "λ_b = NA (unattributed)"


def _explain_em(kind: str, marginal_uids: list[int]) -> str:
    if kind == FormulaKind.SINGLE_UNIT.value:
        return f"ε_b = emission_rate of g{marginal_uids[0]}"
    if kind == FormulaKind.TIED_UNITS.value:
        return f"ε_b = mean of emission_factors of g{marginal_uids}"
    if kind == FormulaKind.FORCED_PMIN_MARGINAL.value:
        return f"ε_b = emission_rate of forced-pmin g{marginal_uids[0]}"
    if kind == FormulaKind.HYDRO_MARGINAL.value:
        return "ε_b = 0 (hydro at the bus bar)"
    if kind == FormulaKind.DEMAND_FAIL.value:
        return "ε_b = 0 (rationing — no MWh generated to serve load)"
    if kind == FormulaKind.RENEWABLE_CURTAILMENT.value:
        return "ε_b = 0 (renewable curtailment)"
    return "ε_b = NA (unattributed)"
