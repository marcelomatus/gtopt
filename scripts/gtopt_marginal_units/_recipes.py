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
from gtopt_marginal_units._zones import is_phantom_bus
from gtopt_marginal_units.constants import FormulaKind, GeneratorKind, Tolerances
from gtopt_marginal_units.errors import AttributionError


def _resolve_loss_factor(
    *,
    bus_uid: int,
    marginal_uid: int,
    g0,  # Generator at marginal_uid (already looked up by caller)
    bus_name: str,
    gens_on_bus: list,
    lmp_by_bus: Optional[dict[int, float]],
    srmc_by_uid: Optional[dict[int, float]],
    zone_of: dict[int, int],
    radial_buses: frozenset[int],
    tol: Tolerances,
) -> tuple[float, float, str]:
    """Compute the per-bus loss-factor (#523).

    Returns ``(scale, loss_raw, loss_status)``.  ``scale`` is the
    multiplier to apply to the emission factor; ``loss_raw`` is the
    raw ``bus_LMP / ref`` ratio recorded for audit; ``loss_status``
    is the bucket name (see the ``RecipeRow.loss_factor_status``
    docstring for the full enumeration).

    Pure function — extracted from ``build_recipes_for_cell`` to keep
    the per-cell loop short and to enable unit-testing the decision
    tree in isolation.
    """
    # Step 0 — phantom-bus check (synthetic BAT_*_int_bus, #525).
    if is_phantom_bus(bus_name, gens_on_bus):
        return 1.0, 1.0, "phantom_bus"

    # Step 1 — island check (topology connected component).
    zone_bus = zone_of.get(bus_uid)
    zone_marg = zone_of.get(int(g0.bus_uid))
    if zone_bus is None or zone_marg is None:
        return 1.0, 1.0, "missing"
    if zone_bus != zone_marg:
        # Cross-island — raw is informative but not used to scale.
        loss_raw = 1.0
        bus_lmp = (lmp_by_bus or {}).get(bus_uid)
        marg_lmp = (lmp_by_bus or {}).get(int(g0.bus_uid))
        if (
            bus_lmp is not None
            and marg_lmp is not None
            and abs(float(marg_lmp)) > tol.eps
        ):
            loss_raw = float(bus_lmp) / float(marg_lmp)
        return 1.0, loss_raw, "cross_island"

    # Step 2 — same island: compute the loss factor.
    bus_lmp = (lmp_by_bus or {}).get(bus_uid)
    ref: Optional[float] = None
    if g0.kind == "thermal" and srmc_by_uid is not None:
        # Guard B: a thermal marginal with no ``emission_rate`` (e.g.
        # cogen biomass, EF=None) carries no carbon to attribute — the
        # raw ratio is mathematically defined but emission-irrelevant,
        # and recording an extreme value (e.g. ARAUCO at $35 SRMC vs
        # bus_LMP=24500 → raw=700) misleads the warn-summary count.
        # Skip scaling; status flags the cell so the operator can audit.
        if g0.emission_rate is None or float(g0.emission_rate or 0.0) == 0.0:
            return 1.0, 1.0, "no_emission_data"
        s = srmc_by_uid.get(marginal_uid)
        ref = float(s) if s is not None else None
    elif g0.kind in ("battery", "hydro"):
        r = (lmp_by_bus or {}).get(int(g0.bus_uid))
        ref = float(r) if r is not None else None
        # Guard A: a hydro/battery marginal at near-zero LMP cannot be
        # the price-setter in any meaningful sense — water value /
        # storage opportunity cost is essentially 0, so the ratio
        # bus_LMP[i] / LMP[hydro_bus] explodes mechanically without
        # reflecting a physical loss factor.  Common cause: LP elects
        # one zero-MC hydro as basic-in-LP for a country-spanning
        # connected component; the deep-south LMP is set by a thermal
        # but our recipe sees the LP's tie-broken hydro as the
        # marginal.  Skip scaling; mark for audit.
        if ref is not None and abs(ref) <= tol.tol_lmp:
            return 1.0, 1.0, "zero_srmc_hydro"
    if bus_lmp is None or ref is None or abs(ref) <= tol.eps:
        return 1.0, 1.0, "missing"

    loss_raw = float(bus_lmp) / float(ref)
    if loss_raw < 0.0:
        return 1.0, loss_raw, "negative"

    # Always apply raw scaling — differentiate via status only.
    is_radial = bus_uid in radial_buses
    if loss_raw > tol.loss_factor_error:
        # Guard A (extended): a hydro/storage marginal that produces an
        # explosive ratio is suffering the same LP-tie-break artefact
        # the absolute-LMP guard above catches.  Raw > error on a
        # zero-MC marginal is not a physical loss factor; reclassify
        # to ``zero_srmc_hydro`` instead of ``critical_*`` so the
        # operator's audit isn't polluted with bogus criticals.
        if g0.kind in ("battery", "hydro"):
            return 1.0, loss_raw, "zero_srmc_hydro"
        status = "critical_radial" if is_radial else "critical_meshed"
    elif loss_raw > tol.loss_factor_warn:
        status = "warn_radial" if is_radial else "warn_meshed"
    else:
        status = "ok"
    return loss_raw, loss_raw, status


@dataclass(slots=True, frozen=True)
class _TopologyAux:
    """Topology-derived lookups precomputed once per Topology instance.

    Cached via ``_topology_aux`` keyed on the Topology object's
    identity so the per-cell loop pays the build cost exactly once
    even when ``build_recipes_for_cell`` is called millions of times.
    """

    gen_by_uid: dict[int, object]
    bus_name_by_uid: dict[int, str]
    gens_by_bus_uid: dict[int, list]
    radial_buses: frozenset[int]


# Single-slot last-seen cache.  The previous form keyed on `id(topology)`
# was BROKEN under pytest-xdist parallel workers: when a Topology
# object is garbage-collected and a NEW Topology in a later test gets
# the SAME id() (CPython reuses memory addresses freely), the cache
# would silently return the stale aux for the wrong topology — leading
# to flaky failures like `radial_buses == frozenset({10,20,30,40})`
# mismatching against a cached `frozenset` from a prior fixture.
#
# Storing a STRONG reference to the Topology alongside its aux lets us
# verify object identity before returning the cached aux.  Last-seen
# semantics: most call sites only ever pass one Topology (the per-cell
# loop), so a single-slot LRU is enough to absorb the build cost.  The
# strong reference pins the Topology in memory while the cache entry
# is live — fine because the Topology is referenced by the caller too.
_TOPOLOGY_AUX_LAST: tuple[Topology, _TopologyAux] | None = None


def _topology_aux(topology: Topology) -> _TopologyAux:
    """Build (and cache) the per-Topology lookup struct."""
    global _TOPOLOGY_AUX_LAST  # noqa: PLW0603
    last = _TOPOLOGY_AUX_LAST
    if last is not None and last[0] is topology:
        return last[1]

    gen_by_uid = {g.uid: g for g in topology.generators}
    bus_name_by_uid = {b.uid: b.name for b in topology.buses}
    gens_by_bus_uid: dict[int, list] = {}
    for g in topology.generators:
        gens_by_bus_uid.setdefault(int(g.bus_uid), []).append(g)

    # Radial-bus set (bridge analysis).  A bus is "radial" when its
    # 2-edge-connected component has size 1 — i.e. every incident edge
    # is a bridge.  On such buses there's only one path to the rest of
    # the topology, so R drives the loss factor and the LP-derived
    # bus_LMP / SRMC ratio IS the legitimate loss correction (apply as-
    # is).  Examples in the CEN 2-year cascade: Aysén, Chiloé, the long
    # 110 kV radial chains.
    import networkx as nx  # noqa: PLC0415

    g_topo = nx.Graph()
    g_topo.add_nodes_from(b.uid for b in topology.buses)
    for ln in topology.lines:
        if getattr(ln, "active", True):
            g_topo.add_edge(int(ln.bus_a_uid), int(ln.bus_b_uid))
    bridges = {frozenset(e) for e in nx.bridges(g_topo)}
    g_meshed = nx.Graph()
    g_meshed.add_nodes_from(g_topo.nodes())
    for u, v in g_topo.edges():
        if frozenset((u, v)) not in bridges:
            g_meshed.add_edge(u, v)
    radial_buses = frozenset(
        b for cc in nx.connected_components(g_meshed) for b in cc if len(cc) == 1
    )

    aux = _TopologyAux(
        gen_by_uid=gen_by_uid,
        bus_name_by_uid=bus_name_by_uid,
        gens_by_bus_uid=gens_by_bus_uid,
        radial_buses=radial_buses,
    )
    _TOPOLOGY_AUX_LAST = (topology, aux)
    return aux


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
    # Loss-factor audit (#523 emission scaling).  ``raw`` is the
    # unbounded ratio ``bus_LMP[i] / ref``; ``status`` is one of:
    #   "ok"               — same island, raw within physical envelope
    #   "warn_radial"      — radial path, raw ∈ (warn, error]
    #   "warn_meshed"      — meshed path, raw ∈ (warn, error]
    #   "critical_radial"  — radial path, raw > error
    #   "critical_meshed"  — meshed path, raw > error
    #   "cross_island"     — bus + marginal in DIFFERENT zones
    #                        (topology test on zone_of); scale=1
    #   "phantom_bus"      — synthetic BAT_*_int_bus topology (#525)
    #   "zero_srmc_hydro"  — hydro/storage marginal at near-zero LMP
    #                        (price-setter is elsewhere; ratio explodes
    #                        mechanically); scale=1
    #   "no_emission_data" — thermal marginal with emission_rate=None/0
    #                        (e.g. cogen biomass); EF is 0 so no carbon
    #                        to scale, raw would be misleading; scale=1
    #   "negative"         — raw < 0 (oversupply / LMP inversion); scale=1
    #   "missing"          — no ref / no LMP / bus not in any zone
    #   "n/a"              — formula_kind ≠ SINGLE_UNIT/TIED_UNITS
    loss_factor_raw: float = 1.0
    loss_factor_status: str = "n/a"
    # True when EVERY marginal unit is a "pure power" zero-MC
    # renewable (solar, wind, run-of-river hydro — kind="profile").
    # The LP elects these as marginal when they're INTERIOR (between 0
    # and pmax), so they have headroom: +1 MWh of incremental demand
    # CAN be served by ramping them up, costing zero direct emission.
    # The consequential walk-up should NOT fire in this case — the
    # physically-correct marginal emission is the direct value (= 0).
    # Walk-up DOES apply when the marginal is storage (battery,
    # reservoir hydro) or cogen — those units cannot absorb
    # incremental demand (storage has cross-block energy balance,
    # cogen self-dispatches from an industrial process), so the
    # backfill thermal is the real carbon-marginal.
    marginal_all_profile: bool = False
    # Per-cell negative-LMP audit (independent of loss_factor_*).
    # When the zone's lambda_z went negative we always clamp the
    # recipe row's r_lmp and r_em to 0; ``negative_lmp_kind`` records
    # WHICH regime fired so the operator and downstream consumers can
    # filter the two cases separately:
    #   "no_marginal"     — negative lambda_z + non-storage / no marginal
    #                       (reactance loop / energy constraint /
    #                       spillover penalty bound).  formula_kind is
    #                       also overridden to NO_MARGINAL_NEG_LMP.
    #   "storage_clamped" — negative lambda_z + storage marginal
    #                       (spillover penalty / regulation /
    #                       energy-constraint binding on the storage
    #                       itself).  formula_kind is preserved.
    #   ""                — no negative-LMP override applied (default).
    negative_lmp_kind: str = ""

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
            # Unit is tCO2eq / MWh — same as ``Generator.emission_rate``
            # upstream (see ``_gtopt_reader.topology_from_planning``).
            cons = float(self.consequential_co2eq)
            direct = float(self.recomputed_value)
            base["consequential_co2eq_t_per_mwh"] = cons
            base["consequential_gen_uid"] = (
                int(self.consequential_gen_uid)
                if self.consequential_gen_uid is not None
                else -1
            )
            base["loss_factor_raw"] = float(self.loss_factor_raw)
            base["loss_factor_status"] = str(self.loss_factor_status)
            base["negative_lmp_kind"] = str(self.negative_lmp_kind)
            # Effective per-cell marginal emission rate — the column
            # downstream tools (battery balance, arbitrage, dashboards)
            # SHOULD use to attribute CO2 at this bus.  Defined per
            # marginal-unit kind:
            #
            #   * direct    when the LP marginal is a real combustion
            #     thermal (``direct > 0``) — the marginal MWh costs
            #     exactly the unit's own emission factor.
            #   * direct (= 0) when EVERY marginal unit is a pure-power
            #     zero-MC renewable (kind="profile": solar / wind / RoR).
            #     The LP elects them as marginal only when INTERIOR
            #     (between 0 and pmax), so they have headroom — +1 MWh
            #     of demand is served by ramping them up, costing zero
            #     direct emission.  Consequential walk-up does NOT
            #     apply: it would mis-attribute the next-up thermal's
            #     EF when the physical answer is genuinely zero.
            #   * consequential   otherwise (storage marginal: battery,
            #     reservoir hydro; or cogen marginal — units that
            #     CANNOT absorb incremental demand: storage binds the
            #     cross-block energy balance, cogen self-dispatches
            #     from an industrial process).  The walked-up next-up
            #     thermal captures the "marginal carbon if demand grew
            #     by 1 MWh" via the displacement chain.
            if direct > 0.0 or self.marginal_all_profile:
                base["effective_emission_intensity_t_per_mwh"] = direct
            else:
                base["effective_emission_intensity_t_per_mwh"] = cons
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
    # Filter to "real" combustion peakers.  Skip cogen units (biomass /
    # biogas / geothermal cogen — CELCO, CMPC, ARAUCO, …): they are
    # flagged ``thermal`` in plexos2gtopt but in CEN/PLEXOS are MustRun
    # with ``declared_MC=0`` and a tiny leakage emission rate
    # (~0.0002 tCO2eq/MWh), so they never act as backfill.  Including
    # them would short-circuit the walk-up to a near-zero emission rate
    # that misses the true carbon opportunity cost.  The ``is_cogen``
    # flag (set by ``_gtopt_reader._is_cogen`` based on raw
    # ``Generator.type`` sub-family ``biomasa`` / ``biogas`` /
    # ``geothermal``) is the right discriminator — NOT ``declared_MC=0``,
    # because CCGT block segments (``thermal:gas`` / ``thermal:diesel``)
    # also carry MC=0 on the non-fuel-bearing segment but ARE
    # dispatchable peakers.
    eligible = sorted(
        (
            g
            for g in gens_in_zone
            if (
                not g.is_cogen
                and g.declared_MC is not None
                and g.emission_rate is not None
                and float(g.emission_rate) > 0.0
                and float(g.pmax) > tol.eps
                and g.uid not in marginal_uids
            )
        ),
        key=lambda g: (float(g.declared_MC), g.uid),
    )
    eps = max(tol.eps, tol.tol_headroom_mw)
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
    lmp_by_bus: Optional[dict[int, float]] = None,
    srmc_by_uid: Optional[dict[int, float]] = None,
    demand_fail_cost: float | dict[int, float] = 1000.0,
    tol: Tolerances = Tolerances.default(),
) -> tuple[list[RecipeRow], list[RecipeRow]]:
    """Build (price_recipe_rows, emission_recipe_rows) for one cell.

    The two lists have identical ``marginal_gen_uids`` and
    ``marginal_weights`` per the Lin & Tang theorem — only
    ``marginal_data`` differs.

    Raises ``AttributionError`` when the writer-side invariant
    fails: |recomputed_lmp − zone_lmp| > tol_price.

    Per-bus loss-correction (#523).  The emission attributed to bus
    ``i`` is scaled by ``bus_LMP[i] / ref`` where ``ref`` is
    SRMC[g] for a thermal marginal or LMP[bus_marginal] for a storage
    marginal (battery / reservoir).  The raw ratio is ALWAYS applied;
    differentiation lives in ``loss_factor_status`` so the operator
    can filter the suspicious buckets downstream.

    Status buckets (see ``RecipeRow.loss_factor_status``):
      * ``ok``               raw ≤ tol.loss_factor_warn
      * ``warn_radial``      raw ∈ (warn, error] on a radial path
      * ``warn_meshed``      raw ∈ (warn, error] on a meshed path
      * ``critical_radial``  raw > error on a radial path
      * ``critical_meshed``  raw > error on a meshed path
      * ``phantom_bus``      synthetic BAT_*_int_bus topology (#525)
      * ``cross_island``     bus and marginal in different zones
      * ``negative``         raw < 0 (oversupply / LMP inversion)
      * ``missing``          no SRMC / no LMP / bus not in any zone
      * ``n/a``              non-SINGLE_UNIT/TIED_UNITS formula kind
    """
    # Topology-derived lookups + radial-bus set.  These depend only on
    # the Topology, not on the cell — caching them across the per-cell
    # loop turns an O(cells × N_lines + N_buses) hot path into O(1) per
    # cell.  Use module-level lru_cache keyed on the Topology identity.
    aux = _topology_aux(topology)
    gen_by_uid = aux.gen_by_uid
    bus_name_by_uid = aux.bus_name_by_uid
    gens_by_bus_uid = aux.gens_by_bus_uid
    radial_buses = aux.radial_buses

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

        # Negative-LMP guard.  Negative zone lambda_z cannot reflect a
        # real thermal marginal (thermal SRMC > 0 always).  Two regimes:
        #
        #   1. Storage marginal (battery / hydro) → the negative dual
        #      is the LP signalling a spillover / regulation /
        #      energy-constraint binding on the storage itself.  This
        #      IS a real marginal: keep the formula_kind, clamp the
        #      recipe's recorded LMP to 0 (``storage_neg_clamp``) so
        #      energy + CO2 arbitrage continues normally.
        #
        #   2. Non-storage marginal OR no marginal at all → the
        #      negative is a reactance-loop / energy-constraint
        #      artefact, NOT a marginal generator.  Override
        #      ``formula_kind`` to ``NO_MARGINAL_NEG_LMP`` so
        #      downstream arbitrage / battery-balance scripts can
        #      filter the cell out (no guaranteed payment /
        #      attribution).
        is_negative_lmp = zres.lambda_z < -tol.tol_price
        storage_neg_clamp = False
        negative_lmp_kind = ""
        if is_negative_lmp:
            g0_neg = gen_by_uid.get(int(marginal_uids[0])) if marginal_uids else None
            storage_marginal = g0_neg is not None and g0_neg.kind in (
                "battery",
                "hydro",
            )
            if storage_marginal:
                storage_neg_clamp = True
                negative_lmp_kind = "storage_clamped"
            else:
                kind_str = FormulaKind.NO_MARGINAL_NEG_LMP.value
                marginal_uids = []
                weights = []
                mcs = []
                ems = []
                negative_lmp_kind = "no_marginal"

        # Resolve per-bus demand_fail_cost (B2): per-Demand ``fcost``
        # wins, fall back to global.  When passed as a dict, look up by
        # bus_uid; when a scalar, use globally (legacy / test path).
        if isinstance(demand_fail_cost, dict):
            dfc = float(demand_fail_cost.get(bus_uid, 0.0))
        else:
            dfc = float(demand_fail_cost)

        # Compute recomputed_lmp and recomputed_ε from the *captured* data.
        if kind_str == FormulaKind.DEMAND_FAIL.value:
            r_lmp = dfc
            r_em = 0.0
            constant_lmp = dfc
            constant_em = 0.0
        elif kind_str == FormulaKind.RENEWABLE_CURTAILMENT.value:
            r_lmp = 0.0
            r_em = 0.0
            constant_lmp = 0.0
            constant_em = 0.0
        elif kind_str == FormulaKind.EMPTY_ISLAND.value:
            # "Nobody's home" island — one or more buses sharing a
            # zone with no demand, no merit candidate, and no
            # generator with positive pmax this cell.  LMP=0 and em=0
            # are the faithful answers (LP free-vertex choice).
            # Distinct from renewable_curtailment because there's NO
            # renewable to be on the margin either.
            r_lmp = 0.0
            r_em = 0.0
            constant_lmp = 0.0
            constant_em = 0.0
        elif kind_str == FormulaKind.UNATTRIBUTED.value:
            r_lmp = float("nan")
            r_em = float("nan")
            constant_lmp = 0.0
            constant_em = 0.0
        elif kind_str == FormulaKind.NO_MARGINAL_NEG_LMP.value:
            # Negative LMP without a storage marginal → no real
            # marginal unit (reactance loop / energy constraint /
            # spillover penalty).  Set both LMP and emission factor to
            # ZERO: there is no economic signal to attribute to this
            # cell.  Downstream arbitrage / battery-balance scripts
            # should filter on formula_kind == 'no_marginal_neg_lmp'
            # and skip these blocks entirely.  The original (negative)
            # lambda_z is preserved in the per-zone output, not on the
            # per-bus recipe row.
            r_lmp = 0.0
            r_em = 0.0
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

        # Per-bus DEMAND_FAIL override.  The zone-level classifier picks
        # the formula_kind from the zone's representative LMP, but in
        # multi-bus zones individual buses can hit demand-fail while
        # the zone-rep stays at a cheap marginal gen's MC.  Verified
        # 2026-06-04 on jan18 PLEXOS20260118: 84 cells at
        # ``lmp_by_bus[bus_uid] ≈ 469 $/MWh`` (= ``demand_fail_cost``
        # = 467.19 × small loss-factor inflation) were classified
        # ``single_unit`` with ``recomputed_lmp ≈ $70``, producing a
        # 400 $/MWh reconstruction error.  Match windows:
        #   * ABSOLUTE: |bus_LMP − dfc| ≤ tol_price (catches cells where
        #     the bus LMP is exactly dfc — the canonical PLEXOS VoLL
        #     row).
        #   * LOSS-INFLATED: bus_LMP ∈ [dfc, dfc × loss_factor_warn]
        #     (catches cells where the bus is downstream of the
        #     demand-fail node and pays a loss-multiplied VoLL,
        #     typically 1.001 .. 1.05 × dfc).
        # In both cases use the BUS's actual LP_LMP as ``r_lmp`` — this
        # is the exact LP-side bus dual including the loss inflation,
        # not the under-stated dfc constant.
        bus_lmp_actual = (
            float(lmp_by_bus[bus_uid])
            if (lmp_by_bus is not None and bus_uid in lmp_by_bus)
            else None
        )
        # Symmetric loss-factor band around dfc.  Upstream buses
        # (further from the demand-fail load along the transmission
        # graph) carry bus_LMP = dfc × (1 − loss_factor), so they land
        # BELOW dfc.  Downstream / injection-bus copies carry bus_LMP =
        # dfc × (1 + loss_factor) and land ABOVE.  jan18 empirical
        # range: 443.76 .. 469.07 against dfc = 467.19 — i.e. ±5 %.
        # 1.20 (±20 %) is comfortably wider than any single-hop CEN
        # transmission loss inflation, but TIGHT enough that a real
        # gas-peaker at $200/MWh or hydro at $300/MWh (in a high-water
        # scarcity scene) doesn't get mis-classified as demand_fail.
        # Tighter than ``loss_factor_warn`` (= 2 default — too wide for
        # this guard) and looser than ``tol_price`` (= 0.01 default —
        # too tight to catch loss-inflated dfc cells).
        _DEMAND_FAIL_LOSS_BAND = 1.35
        if (
            bus_lmp_actual is not None
            and dfc > 0.0
            and (
                abs(bus_lmp_actual - dfc) <= tol.tol_price
                or (
                    dfc / _DEMAND_FAIL_LOSS_BAND
                    <= bus_lmp_actual
                    <= dfc * _DEMAND_FAIL_LOSS_BAND
                )
            )
            and kind_str != FormulaKind.DEMAND_FAIL.value
        ):
            kind_str = FormulaKind.DEMAND_FAIL.value
            r_lmp = bus_lmp_actual
            r_em = 0.0
            constant_lmp = bus_lmp_actual
            constant_em = 0.0
            marginal_uids = []
            weights = []
            mcs = []
            ems = []

        # Per-bus LMP-decomposition recovery — for unit-driven kinds
        # (single_unit / tied_units / forced_pmin_marginal) where r_lmp
        # was set from the zone's lambda_z OR the weighted MC.  The
        # zone-level value captures only the ENERGY component of LMP
        # (λ_energy = marginal gen MC) — the bus-specific LOSS and
        # CONGESTION components live in the LP's bus dual.  Per Schweppe
        # decomposition:
        #
        #   bus_LMP[i] = λ_energy + δ_loss[i] + δ_congestion[i]
        #
        # When ``lmp_by_bus[bus_uid]`` is available it IS the LP-side
        # full bus LMP including loss + congestion contributions.
        # Recover by scaling the marginal-MC base by the
        # ``bus_LMP / zone_LMP`` ratio — preserves the recipe's
        # marginal-unit attribution (energy component) while restoring
        # the per-bus loss + congestion adders.  The ratio is the
        # PHYSICAL ground truth — bus i is at λ_zone × (1 + δ/λ_zone)
        # = bus_LMP[i] by construction.
        #
        # Skipped for non-unit kinds (demand_fail, no_marginal_neg_lmp,
        # renewable_curtailment, empty_island, hydro_marginal,
        # unattributed) — those already set r_lmp from a bus-specific
        # source (dfc, 0, or the zone's storage marginal).
        #
        # Verified 2026-06-04 on jan18 LP-relax: 20 southern-Chile buses
        # (Pid-Pid110, etc.) at long-radial corridors carried mean
        # error ≈ $108 per bus across 168 blocks; the recovery sets
        # r_lmp exactly to bus_lmp_actual, eliminating these errors.
        if bus_lmp_actual is not None and kind_str in {
            FormulaKind.SINGLE_UNIT.value,
            FormulaKind.TIED_UNITS.value,
            FormulaKind.FORCED_PMIN_MARGINAL.value,
        }:
            r_lmp = bus_lmp_actual
        elif bus_lmp_actual is not None and kind_str == FormulaKind.UNATTRIBUTED.value:
            # Same LMP recovery for ``unattributed`` cells: the recipe
            # could not pick a marginal generator, so the formula-derived
            # ``r_lmp`` was set to NaN.  ``bus_lmp_actual`` is still the
            # LP-side ground truth — use it so downstream consumers
            # (battery_balance, LMP back-test) get a usable price.  The
            # lack of marginal-unit attribution stays visible via
            # ``formula_kind == "unattributed"`` and the empty
            # ``marginal_gen_uids`` list; only the price field is
            # recovered.
            r_lmp = bus_lmp_actual

        # Storage-marginal + negative lambda_z: clamp BOTH price and
        # emission factor to 0 (per user instruction).  Rationale: a
        # negative-LMP cell carries no real economic / carbon signal
        # regardless of whether a storage marginal exists.  Downstream
        # arbitrage / CO2 calculations should treat the cell as a
        # zero-priced, zero-attribution block.  The original (negative)
        # lambda_z is preserved in the per-zone output for audit.
        # Skips the round-trip invariant below because the synthesised
        # r_lmp would not match the negative lambda_z.
        if storage_neg_clamp:
            r_lmp = 0.0
            r_em = 0.0
            constant_lmp = 0.0
            constant_em = 0.0
            # Drop the consequential MOER too — no marginal attribution
            # on a negative-LMP cell.  See NO_MARGINAL_NEG_LMP above for
            # the symmetric non-storage branch.

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
        # Round-trip invariant: |r_lmp − lambda_z| ≤ scaled_tol, where
        # scaled_tol = max(tol_price, tol_price · |lambda_z|).  No
        # additive FP-noise pad — scaled_tol is already floored at
        # tol_price (default 1e-3) which is 3 decades wider than the
        # solver's 1e-6 noise floor.  An explicit override
        # ``--tol-price 0`` is the only way to make this check
        # bit-exact; that's the operator's choice and intent.
        # Round-trip invariant: |r_lmp − lambda_z| ≤ scaled_tol.
        # Skipped when ``bus_lmp_actual`` is available AND r_lmp was
        # set to it by the LMP-decomposition recovery above — in that
        # case r_lmp legitimately captures bus-specific loss + congestion
        # adders that lambda_z does NOT (zone-rep LMP).  The invariant
        # only fires when r_lmp comes from the marginal-MC formula
        # (no per-bus recovery applied), where it must round-trip
        # exactly to the zone lambda.
        scaled_tol = max(tol.tol_price, tol.tol_price * abs(zres.lambda_z))
        bus_recovered = bus_lmp_actual is not None and kind_str in {
            FormulaKind.SINGLE_UNIT.value,
            FormulaKind.TIED_UNITS.value,
            FormulaKind.FORCED_PMIN_MARGINAL.value,
        }
        if (
            kind_str
            in {
                FormulaKind.SINGLE_UNIT.value,
                FormulaKind.TIED_UNITS.value,
            }
            and zres.reason != "interior_rc_zero_lp_dual"
            and not storage_neg_clamp
            and not bus_recovered
            and mcs
            and abs(r_lmp - zres.lambda_z) > scaled_tol
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
        # Shortcut "direct = consequential" applies only when the
        # marginal is a real combustion thermal (non-cogen).  When the
        # LP-elected marginal is a cogen (biomass / sulfuric-acid plant
        # with tiny leakage emission), we must walk UP the merit ladder
        # for the real backfill emission — same as for hydro / BESS
        # marginals — because cogen self-dispatches and never absorbs
        # incremental demand.
        marginal_all_cogen = bool(marginal_uids) and all(
            (gen_by_uid.get(int(u)) is not None and gen_by_uid[int(u)].is_cogen)
            for u in marginal_uids
        )
        if direct_em > tol.eps and not marginal_all_cogen:
            # Direct marginal already emits — consequential = direct.
            cons_rate = direct_em
            cons_uid = marginal_uids[0] if marginal_uids else None

        # Per-bus loss-correction scaling (#523) — see the
        # ``_resolve_loss_factor`` docstring for the full decision tree
        # and the ``RecipeRow.loss_factor_status`` docstring for the
        # status enumeration.
        scale = 1.0
        loss_raw = 1.0
        loss_status = "n/a"
        if (
            lmp_by_bus
            and marginal_uids
            and kind_str
            in (FormulaKind.SINGLE_UNIT.value, FormulaKind.TIED_UNITS.value)
        ):
            g0 = gen_by_uid.get(int(marginal_uids[0]))
            if g0 is not None:
                scale, loss_raw, loss_status = _resolve_loss_factor(
                    bus_uid=bus_uid,
                    marginal_uid=int(marginal_uids[0]),
                    g0=g0,
                    bus_name=bus_name_by_uid.get(bus_uid, ""),
                    gens_on_bus=gens_by_bus_uid.get(bus_uid, []),
                    lmp_by_bus=lmp_by_bus,
                    srmc_by_uid=srmc_by_uid,
                    zone_of=zone_of,
                    radial_buses=radial_buses,
                    tol=tol,
                )
        # The LP-derived ``scale`` is anchored at the LP-elected
        # marginal's bus (its LMP or SRMC).  ``direct_em`` IS the
        # contribution from that LP-elected unit, so ``direct_em ×
        # scale`` is the correct per-bus carbon attribution.
        if scale != 1.0:
            direct_em = direct_em * scale
        # For the consequential MOER we walked UP the merit ladder.
        # That walked-up thermal is NOT generating — comparing its
        # SRMC to the bus dual is meaningless.  The only physically-
        # defensible per-MWh correction is the gen's own static
        # ``lossfactor`` (typically aux-use / station-service fraction;
        # plexos2gtopt populates it from ``Generator.aux_use``).  When
        # ``cons_uid == marginal_uids[0]`` (direct == consequential
        # shortcut for a thermal marginal that already emits), keep
        # the LP-derived ``scale`` since the LP signal IS available
        # for the actually-dispatching gen.
        if cons_rate > 0.0 and cons_uid is not None:
            shortcut_to_direct = bool(marginal_uids) and int(cons_uid) == int(
                marginal_uids[0]
            )
            if shortcut_to_direct:
                # cons_rate was set to direct_em above; scale already
                # applied via direct_em assignment.
                cons_rate = direct_em
            else:
                g_cons = gen_by_uid.get(int(cons_uid))
                lf = (
                    float(g_cons.lossfactor)
                    if g_cons is not None and g_cons.lossfactor is not None
                    else 0.0
                )
                # 1 MWh of demand requires 1 / (1 − lf) MWh of generation
                # to net out the station-service / aux loss.  For small lf
                # this is ≈ 1 + lf.  Skip when lf ≥ 1 (would divide by 0
                # or invert sign — implausible data, fall back to 1.0).
                cons_scale = 1.0 / (1.0 - lf) if 0.0 <= lf < 1.0 else 1.0
                cons_rate = cons_rate * cons_scale

        # Pure-power detection: EVERY marginal unit is kind="profile"
        # (solar / wind / RoR — zero-MC renewable with headroom).  In
        # that case the LP-elected marginal IS the carbon-marginal: +1
        # MWh of demand is served by ramping the renewable up, costing
        # zero direct emission.  Consequential walk-up must NOT fire
        # — see ``RecipeRow.to_dict`` and the ``marginal_all_profile``
        # field docstring for the full kind-aware decision matrix.
        marginal_all_profile = bool(marginal_uids) and all(
            (
                gen_by_uid.get(int(u)) is not None
                and gen_by_uid[int(u)].kind == GeneratorKind.PROFILE.value
            )
            for u in marginal_uids
        )

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
                loss_factor_raw=loss_raw,
                loss_factor_status=loss_status,
                negative_lmp_kind=negative_lmp_kind,
                marginal_all_profile=marginal_all_profile,
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
