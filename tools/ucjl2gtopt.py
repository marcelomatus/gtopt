#!/usr/bin/env python3
"""Convert UnitCommitment.jl benchmark JSON to gtopt system JSON.

UC.jl problems map onto gtopt's native unit-commitment formulation:

* **1 scenario × 1 chronological stage × T consecutive blocks of 1 h** —
  every UC.jl horizon is hourly, deterministic, and single-stage.
* **Demand.lmax** carries the per-block load time series.
* **Commitment** mirrors UC.jl's thermal generator parameters one-for-one:
  startup_cost / shutdown_cost (hot/warm/cold tiers when present),
  min_up_time / min_down_time, ramp_up / ramp_down, startup_ramp /
  shutdown_ramp, initial_status / initial_hours, piecewise heat rates
  (pmax_segments + heat_rate_segments).
* **ReserveZone** + **ReserveProvision** mirror UC.jl's ``Reserves``
  block and per-generator ``Reserve eligibility``.  Reserve types are
  classified into two directions:

    - **Up-only** (``spinning``, ``non-spinning``, ``replacement``,
      ``regulation-up``): emits ``urreq`` / ``urcost`` only;
      providers get ``ur_provision_factor = 1``.
    - **Bidirectional** (``flexiramp``, ``regulation``): emits both
      ``urreq`` / ``urcost`` AND ``drreq`` / ``drcost`` (symmetric,
      same Amount + penalty); providers also get
      ``dr_provision_factor = 1``.

  Unknown types are skipped with a warning so the user can extend
  the type set explicitly.
* **GeneratorProfile** — UC.jl v0.3 ships time-varying renewable
  availability as a list-of-lists inside ``Production cost curve
  (MW)`` (outer length 1 = single capacity segment, inner is the
  per-hour available MW).  When detected by
  ``_detect_time_varying_capacity``, the converter emits a
  ``Generator`` with ``pmax = max(ts)`` plus a ``GeneratorProfile``
  carrying ``ts / pmax`` as the per-block availability factor in
  gtopt's ``STBRealFieldSched`` 3-D shape.  Such gens skip
  Commitment (renewables have no on/off semantics).
* **Price-sensitive loads** (``Price-sensitive loads``):
  ``{Bus, Revenue ($/MW), Demand (MW)}`` → extra ``Demand`` with
  ``lmax = Demand (MW)`` and per-element ``fcost = Revenue``.
  gtopt's ``demand_fail_cost`` resolution honors per-Demand ``fcost``
  (source/demand_lp.cpp:100), so this entry's curtailment cost is the
  marginal revenue earned by serving the load — the LP serves up to
  ``Demand`` only when ``Revenue`` exceeds the marginal cost of
  generation, matching UC.jl's elastic-demand semantics.
* **Contingencies (N-1)** map onto a soft PAMPL block.  For each
  single-line outage in UC.jl's ``Contingencies`` list, the
  converter computes the LODF (Line Outage Distribution Factor)
  from the network reactance graph (``_compute_lodf``) and emits
  one ``UserConstraint`` per (monitored, outaged) line pair of the
  form ``|f_mon + LODF[mon, out] × f_out| ≤ Emergency_mon``, soft
  with ``penalty = Flow limit penalty ($/MW)``.  Coefficients
  below ``contingency_eps`` (default 0.10) are dropped to keep the
  row count tractable on case118-scale networks.  ``--no-
  contingencies`` skips the block entirely.
* **UserConstraint (PAMPL)** mirrors UC.jl's ``Interfaces``:
  ``Branches = {l_i: c_i}`` + ``Net flow upper/lower limit`` +
  ``Flow limit penalty ($/MW)`` → one PAMPL constraint per (interface
  × bound), with the branch coefficients folded into a
  ``Σ c_i * line('l_i').flow {<=,>=} limit`` expression.  Time-varying
  bounds expand to one constraint per block via PAMPL's
  ``for(block in {N})`` scope.  ``Flow limit penalty`` becomes the
  ``penalty`` field so the LP can violate the bound at a known cost.
* **Battery** mirrors UC.jl's ``Storage units`` (BESS):
  ``Maximum/Minimum level (MWh)`` → ``emax`` / ``emin``,
  ``Maximum charge/discharge rate (MW)`` → ``pmax_charge`` /
  ``pmax_discharge``, ``Charge/Discharge efficiency`` →
  ``input_efficiency`` / ``output_efficiency``,
  ``Initial level (MWh)`` → ``eini``,
  ``Last period minimum level (MWh)`` → ``efin``,
  ``Discharge cost ($/MW)`` → ``gcost``,
  ``Charge cost ($/MW)`` → ``charge_cost``,
  ``Loss factor`` → folded into both efficiencies via the
  symmetric multiplier ``√(1 − λ)``.  gtopt's
  ``System::expand_batteries()`` auto-instantiates the discharge
  Generator / charge Demand / linking Converter at LP-build time;
  ``charge_cost`` rides on the synthetic Demand as a negative
  ``fcost`` so the demand-LP substitution stamps it onto the
  charging column with the correct positive sign.
* **relax: true** by default so the LP relaxation solves with any LP
  backend (HiGHS / CLP / CPLEX).  Drop ``--relax`` from the CLI to emit
  MIP commitment (requires a MIP solver loaded at run time).

* **Expansion / TEP** (``Investment cost ($)`` on generators or lines)
  maps onto gtopt's expansion contract via ``_expansion_fields``:
  ``capacity = 0`` (no pre-existing), ``expcap = nameplate``,
  ``expmod = 1`` (one module per candidate), ``capmax = nameplate``,
  ``annual_capcost = invest_cost``.  The LP build then constructs
  ``capainst = capacity + expcap × expmod_var ∈ [0, expcap]`` and
  ``dispatch ≤ capainst`` (see ``source/capacity_object_lp.cpp``).
  Candidate generators also get ``pmin = 0`` so the LP is feasible
  when ``expmod_var = 0`` (no commitment to gate the static floor).

Not yet mapped (scoped out, will silently no-op when present):

* AC-specific dispatch (gtopt is DC-only).  UC.jl's per-line
  ``Emergency flow limit`` is used as the N-1 RHS in
  ``Contingencies`` mapping above but is NOT applied to the
  pre-contingency normal-state flow constraints.  UC.jl's per-line
  ``Flow limit penalty`` is used for contingency soft-constraint
  penalty but the global ``demand_fail_cost`` carries the steady-
  state Power balance penalty separately.
* Per-block min charge / discharge rates, last-period max level
  (``Last period maximum level (MWh)``), and ``Allow simultaneous
  charging and discharging`` — see the inline comment at the Storage
  block for the dropped fields.  Per-block schedules on storage
  rate / efficiency / cost lists are collapsed to first-hour scalar
  because ``Battery`` exposes ``OptTRealFieldSched`` (per-stage), not
  ``OptTBRealFieldSched`` (per-block).
"""

import argparse
import json
import math
import os
from typing import Any


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

# Synthetic fuel name used to anchor piecewise heat-rate segments emitted
# on Generator after the 16fbdde45 Commitment refactor.  Set Fuel.price=1
# so heat_rate_segments[k] × 1.0 acts as the direct $/MWh slope cost.
_UCJL_UNIT_FUEL_NAME = "_ucjl_unit"


def _first_hour(value):
    """Reduce a UC.jl piecewise breakpoint to a scalar.

    UC.jl v0.2/v0.4 emits ``[mw0, mw1, ...]`` (scalar breakpoints) but
    v0.3 wraps each breakpoint as a per-hour time series
    ``[[ts0, ts1, ...]]`` for profiled generators.  This helper
    collapses either shape to the first hour's value.
    """
    if isinstance(value, list):
        return _first_hour(value[0]) if value else 0.0
    return value


def _time_series(value, T):
    """Return a length-T list, broadcasting a scalar if needed."""
    if isinstance(value, list):
        if not value:
            return [0.0] * T
        # First-hour list → use as-is if length matches, else broadcast.
        if isinstance(value[0], list):
            # Already 2D — flatten one level (UC.jl profiled per-segment).
            value = value[0]
        if len(value) == T:
            return [float(v) for v in value]
        if len(value) == 1:
            return [float(value[0])] * T
        # Length mismatch: best effort — truncate or pad with last value.
        if len(value) >= T:
            return [float(v) for v in value[:T]]
        return [float(v) for v in value] + [float(value[-1])] * (T - len(value))
    return [float(value)] * T


def _compute_pieces(curve_mw, curve_cost):
    """Compute ``pmin, pmax, gcost, noload, pmax_segments, heat_rate_segments``.

    UC.jl piecewise breakpoints are scalar lists; v0.3 profiled gens wrap
    each breakpoint in a time series.  We collapse to hour 0 for the
    static fields, then build a heat-rate-style segmentation that gtopt
    consumes via ``Commitment.pmax_segments`` + ``heat_rate_segments``
    when the curve has >=3 breakpoints (else fall back to single slope).

    Returned ``gcost`` is the cost slope between the first and second
    breakpoint — gtopt's per-unit gcost is overridden by the per-segment
    heat-rate cost when both segment arrays are present.

    Returned ``noload`` is the cost-curve intercept ``cost[0]``, the
    cost UC.jl charges for being committed at ``pmin``.  Sent to gtopt
    as ``Commitment.noload_cost`` so dispatching at pmin charges
    ``cost[0]`` exactly, matching UC.jl's piecewise convention
    ``f(p) = c_pmin + Σ_k h_k δ_k``.  Without this, gtopt's LP under-
    prices thermals at high dispatch and case14/congested's g1 becomes
    uneconomic, forcing ~150 MW of curtailment UC.jl never pays.
    """
    mw = [_first_hour(p) for p in curve_mw] if curve_mw else [0.0, 100.0]
    cost = [_first_hour(p) for p in curve_cost] if curve_cost else [0.0, 1000.0]
    pmin = float(mw[0])
    pmax = float(mw[-1])
    if pmax > pmin and len(mw) >= 2:
        # Slope of the first segment.
        slope0 = (cost[1] - cost[0]) / (mw[1] - mw[0])
    elif len(mw) == 1 and mw[0] > 0:
        # Single breakpoint (e.g. ``pmax=pmin=100``, cost=$10 000):
        # treat ``cost[0] / mw[0]`` as the marginal cost at that point.
        slope0 = cost[0] / mw[0]
    else:
        slope0 = 0.0

    # Noload offset for gtopt's ``gcost × p + noload × u`` formulation.
    #
    # UC.jl encodes the dispatch curve as ``f(p) = cost[0] + Σ_k h_k δ_k``
    # where ``δ_k`` is the share of ``p`` in segment ``k`` and ``cost[0]``
    # is the total cost paid at ``p = pmin`` (committed).  In gtopt the
    # LP applies ``gcost × p`` to the full dispatch (including the
    # 0..pmin range that UC.jl doesn't bill — it's covered by
    # ``cost[0]``).  Setting ``noload = cost[0]`` directly therefore
    # double-counts ``slope0 × pmin`` per committed (gen, block):
    #
    #     gtopt @ p = pmin, u = 1  →  slope0 × pmin + cost[0]
    #     UC.jl @ p = pmin         →  cost[0]
    #
    # On case14/base this added ~$2000 per committed thermal per block;
    # the four-block MIP obj drifted ~$12K from UC.jl's -$369 876
    # golden, breaking the cross-check.
    #
    # Subtract the over-counted region so gtopt's LP cost at any
    # ``p ≥ pmin`` matches UC.jl's curve to the cent:
    #
    #     noload = cost[0] - slope0 × pmin
    #
    # For ``pmin = 0`` gens (e.g. UC.jl's ``g2``: mw[0] = 0, cost[0] =
    # 0) this collapses to the legacy ``noload = cost[0]`` so the
    # case14/congested fix that motivated the original ``noload = cost[0]``
    # workaround (under-pricing thermals at high dispatch) is preserved.
    noload = float(cost[0]) - slope0 * pmin if cost else 0.0

    pmax_segments = []
    heat_rate_segments = []
    if len(mw) >= 3 and pmax > pmin:
        # Piecewise: cumulative power breakpoints (excluding pmin) and
        # per-segment marginal cost.  ``heat_rate_segments`` is treated
        # by gtopt as $/MWh when no fuel_cost is set, so we feed
        # marginal costs directly and leave fuel_cost unset.
        for k in range(1, len(mw)):
            dp = mw[k] - mw[k - 1]
            if dp <= 0:
                continue
            pmax_segments.append(round(float(mw[k]), 6))
            heat_rate_segments.append(round((cost[k] - cost[k - 1]) / dp, 6))

        # gtopt requires *strictly* increasing heat rates so the LP cost
        # is convex.  UC.jl case118 ships generators whose piecewise
        # curves have two consecutive segments with the same slope
        # (e.g. g5_uc: heat_rate_segments[2] == heat_rate_segments[3] ==
        # 192.698545).  Adjacent segments with equal slopes are
        # mathematically redundant — they describe a single longer
        # linear interval.  Merge them by keeping the LATER breakpoint
        # (and dropping the earlier duplicate-slope entry).
        if len(heat_rate_segments) >= 2:
            merged_p: list[float] = [pmax_segments[0]]
            merged_h: list[float] = [heat_rate_segments[0]]
            for p, h in zip(pmax_segments[1:], heat_rate_segments[1:]):
                if h == merged_h[-1]:
                    # Same slope — extend the previous segment to this
                    # breakpoint.
                    merged_p[-1] = p
                else:
                    merged_p.append(p)
                    merged_h.append(h)
            pmax_segments, heat_rate_segments = merged_p, merged_h

    return pmin, pmax, slope0, noload, pmax_segments, heat_rate_segments


def _initial_status_to_uc(initial_status_h):
    """UC.jl ``Initial status (h)``: +N = online for N h, -N = offline for N h.

    gtopt's ``Commitment``: ``initial_status`` ∈ {0, 1}, ``initial_hours``
    is the magnitude.
    """
    if initial_status_h is None:
        return None, None
    return (1.0 if initial_status_h > 0 else 0.0), float(abs(initial_status_h))


def _reactance_from_line(ldata):
    """Resolve a line's series reactance from any of UC.jl's three forms.

    UC.jl ships ``Reactance (p.u.)`` (TEP fixtures), ``Reactance (ohms)``
    (case14 / case118), or only ``Susceptance (S)``.  All three are
    equivalent for DC OPF: gtopt's ``Line.reactance`` expects p.u. with
    ``voltage`` left at the default 1.0, so we hand back whichever form
    UC.jl gave us (or invert susceptance).  Returns ``None`` when no
    impedance information is available — the caller drops the line.
    """
    for key in ("Reactance (p.u.)", "Reactance (ohms)", "Reactance"):
        val = ldata.get(key)
        if val is not None and float(val) > 0:
            return float(val)
    susceptance = ldata.get("Susceptance (S)")
    if susceptance is not None and float(susceptance) > 0:
        return 1.0 / float(susceptance)
    return None


def _expansion_fields(invest_cost, nameplate_mw, integer=False):
    """Translate UC.jl's ``Investment cost ($)`` into gtopt expansion fields.

    UC.jl TEP fixtures ship one ``Investment cost ($)`` figure per
    *candidate* generator / line — the LP-side annualized cost of
    activating that unit's nameplate capacity.  gtopt's LP-side
    expansion contract (see ``source/capacity_object_lp.cpp:121–192``)
    constructs::

        capainst_var ∈ [capacity, capmax]
        capainst_var = capacity + expcap × expmod_var
        expmod_var   ∈ [0, expmod]
        dispatch ≤ capainst_var          (per-block, via capacity row)

    so the candidate's *optional-build* semantics requires::

        capacity = 0                     (no pre-existing capacity)
        expcap   = nameplate_mw          (one module = full nameplate)
        expmod   = 1                     (at most one module)
        capmax   = nameplate_mw          (= capacity + expcap)

    With ``expmod_var = 0`` the LP gets ``capainst = 0`` → dispatch is
    forced to zero (line carries no flow / generator produces nothing).
    With ``expmod_var = 1`` the LP pays ``annual_capcost`` and gets
    ``capainst = nameplate_mw`` → full operational range.  Integrality
    of the build decision is gated by ``integer_expmod`` (MIP TEP).

    ``capacity = 0`` must be carried by the *element itself* — i.e. the
    caller is responsible for emitting ``Generator.capacity = 0`` or
    ``Line.capacity = 0`` on candidate elements; this helper only owns
    the expansion-side fields.  Returns an empty dict when there is no
    investment cost so that non-expandable elements skip the machinery
    entirely.
    """
    if invest_cost is None:
        return {}
    cost = float(invest_cost)
    if cost <= 0.0 or nameplate_mw <= 0.0:
        return {}
    fields: dict = {
        "capacity": 0.0,  # no pre-existing capacity for candidates
        "expcap": float(nameplate_mw),
        "expmod": 1,
        "capmax": float(nameplate_mw),
        "annual_capcost": cost,
    }
    if integer:
        fields["integer_expmod"] = True
    return fields


def _detect_time_varying_capacity(curve_mw, T):
    """Extract a renewable / profiled gen's time-varying availability.

    UC.jl v0.3 ships profiled-generator capacity as a *list-of-lists*
    inside ``Production cost curve (MW)``: the outer list is the
    piecewise segments (typically length 1 for renewables — a single
    "all-or-nothing" production segment) and the inner list is the
    per-hour available MW (e.g. an RTPV rooftop-solar profile is
    mostly 0 at night and peaks midday).  We collapse to a single
    capacity-factor schedule by:

      * ``nameplate = max(inner)`` — the peak MW across the horizon.
      * ``profile[t] = inner[t] / nameplate`` — per-block availability
        in ``[0, 1]``.
      * Returning ``(profile, nameplate)`` for downstream emission as
        a ``GeneratorProfile`` entry alongside a Generator with the
        ``nameplate`` as its ``pmax``.

    Returns ``(None, None)`` for flat / scalar-breakpoint cost curves
    where there is no time variation to model.
    """
    if not curve_mw or not isinstance(curve_mw[0], list):
        return None, None
    # Use the OUTERMOST segment's inner time series — for 2-segment
    # profiled gens (RTS-GMLC has some with shape [[ts_low], [ts_high]])
    # the last segment carries the cumulative capacity ceiling.
    inner = curve_mw[-1]
    if not isinstance(inner, list) or not inner:
        return None, None
    ts = [float(v) for v in inner]
    nameplate = max(ts)
    if nameplate <= 0.0:
        return None, None
    if len(ts) < T:
        ts = ts + [ts[-1]] * (T - len(ts))
    elif len(ts) > T:
        ts = ts[:T]
    profile = [v / nameplate for v in ts]
    # Skip degenerate flat-1.0 profiles — they add LP rows without
    # changing the dispatch envelope.
    if all(abs(p - 1.0) <= 1e-9 for p in profile):
        return None, None
    return profile, nameplate


def _compute_lodf(line_specs, n_buses, slack_idx=0, eps=0.10):
    """Compute Line Outage Distribution Factors for an N-1 security model.

    UC.jl ``Contingencies`` are single-element line outages.  The
    standard DC-PF N-1 formulation says: when line ``out`` trips, the
    flow on every remaining line ``mon`` shifts by ``LODF[mon, out] ×
    f_out_pre`` where ``f_out_pre`` is the pre-contingency flow on the
    outaged line.  We encode this in PAMPL as one ``UserConstraint``
    per ``(mon, out)`` pair::

        f_mon + LODF[mon, out] × f_out  ≤  Emergency_mon
        f_mon + LODF[mon, out] × f_out  ≥ -Emergency_mon

    The LODF coefficient comes from the network reactance graph::

        PTDF[ℓ, k]      = ∂f_ℓ / ∂injection_k   (DC shift factor)
        LODF[mon, out]  = (PTDF[mon, k_out] - PTDF[mon, l_out])
                          / (1 - (PTDF[out, k_out] - PTDF[out, l_out]))

    where ``k_out`` and ``l_out`` are the from- / to-bus indices of
    the outaged line.  A vanishing denominator means the outage
    disconnects the network (radial branch) — that contingency is
    silently dropped from the LODF table.

    Parameters
    ----------
    line_specs : list of ``(name, from_bus_idx, to_bus_idx, reactance)``
    n_buses    : total bus count
    slack_idx  : reference bus index (default 0)
    eps        : LODF coefficients below this magnitude are dropped to
                 keep the emitted UserConstraint count manageable.
                 Default 0.10 (10 %) — drops the long tail of weak
                 sensitivities that wouldn't bind in practice and
                 keeps the emitted count tractable even on case118-
                 scale networks.  Tune via the converter's
                 ``--contingency-eps`` CLI flag for studies that need
                 finer N-1 fidelity.

    Returns
    -------
    dict ``{(mon_name, out_name): lodf_coeff}`` for every non-negligible
    pair — typically a few hundred entries on case14, scales as
    ``L²`` in the worst case.
    """
    import numpy as np  # pylint: disable=import-outside-toplevel  # lazy import

    n_lines = len(line_specs)
    if n_lines == 0 or n_buses == 0:
        return {}

    # Build bus admittance ``B`` (n × n) and line incidence ``Bf`` (L × n).
    bus_admittance = np.zeros((n_buses, n_buses))
    line_to_bus = np.zeros((n_lines, n_buses))
    for ell, (_, i_bus, j_bus, x_pu) in enumerate(line_specs):
        if x_pu <= 0.0:
            continue
        b_pu = 1.0 / x_pu
        line_to_bus[ell, i_bus] = b_pu
        line_to_bus[ell, j_bus] = -b_pu
        bus_admittance[i_bus, i_bus] += b_pu
        bus_admittance[j_bus, j_bus] += b_pu
        bus_admittance[i_bus, j_bus] -= b_pu
        bus_admittance[j_bus, i_bus] -= b_pu

    # Reduce ``B`` by removing slack row / column, invert, then pad with
    # zeros (slack angle is fixed at 0 by convention).
    non_slack = [k for k in range(n_buses) if k != slack_idx]
    try:
        x_reduced = np.linalg.inv(bus_admittance[np.ix_(non_slack, non_slack)])
    except np.linalg.LinAlgError:
        return {}  # singular bus admittance — network is disconnected

    x_full = np.zeros((n_buses, n_buses))
    x_full[np.ix_(non_slack, non_slack)] = x_reduced

    # PTDF[ℓ, k] = ∂f_ℓ / ∂injection_k.
    ptdf = line_to_bus @ x_full

    lodf_table: dict[tuple[str, str], float] = {}
    for out_idx, (out_name, k_out, l_out, _) in enumerate(line_specs):
        tap = ptdf[out_idx, k_out] - ptdf[out_idx, l_out]
        denominator = 1.0 - tap
        if abs(denominator) < 1e-9:
            continue  # radial / island-creating outage — skip
        for mon_idx, (mon_name, _, _, _) in enumerate(line_specs):
            if mon_idx == out_idx:
                continue
            shift = ptdf[mon_idx, k_out] - ptdf[mon_idx, l_out]
            coef = shift / denominator
            if abs(coef) >= eps:
                lodf_table[(mon_name, out_name)] = coef
    return lodf_table


def _gen_bounds(gdata):
    """Pmin / Pmax / gcost / piecewise-segments for either Thermal or Profiled.

    Thermal gens carry ``Production cost curve (MW)`` + ``...($)`` —
    full piecewise (handled by ``_compute_pieces``).

    Profiled / TEP gens (``Type = "Profiled"``) carry the simpler
    ``Maximum power (MW)`` / ``Minimum power (MW)`` / ``Cost ($/MW)``
    flat-cost schema with no piecewise segments and no Commitment.
    """
    if "Production cost curve (MW)" in gdata:
        return _compute_pieces(
            gdata.get("Production cost curve (MW)", [0, 100]),
            gdata.get("Production cost curve ($)", [0, 1000]),
        )
    # TEP / Profiled flat-cost schema.  UC.jl's ``Minimum power (MW)``
    # for ``Type = "Profiled"`` is a HARD must-run floor (no on/off
    # commitment to gate it), so we preserve it as the Generator's
    # static column lower bound.  Profiled gens emit NO Commitment in
    # our converter, which means ``CommitmentLP::add_to_lp``'s pmin
    # migration to a u-gated C2 row never fires for them — the static
    # ``lowb`` stays in place and forces dispatch ≥ pmin every block,
    # matching UC.jl semantics.
    #
    # TEP candidate units (``Investment cost ($) > 0``) need pmin = 0
    # because ``capainst = 0`` forces dispatch = 0 when not built;
    # the caller clamps that separately (see ``is_candidate`` branch).
    pmax = float(gdata.get("Maximum power (MW)", 0.0))
    pmin = float(gdata.get("Minimum power (MW)", 0.0))
    gcost = float(gdata.get("Cost ($/MW)", 0.0))
    # Flat-cost gens (TEP / Profiled): no piecewise → no intercept.
    return pmin, pmax, gcost, 0.0, [], []


# ---------------------------------------------------------------------------
# Main conversion
# ---------------------------------------------------------------------------


def convert(  # pylint: disable=too-many-locals,too-many-statements,too-many-branches
    ucjl_path,
    output_path=None,
    relax_commitment=True,
    copperplate=False,
    contingency_eps=0.10,
    skip_contingencies=False,
):
    with open(ucjl_path, encoding="utf-8") as f:
        data = json.load(f)

    params = data.get("Parameters", {})
    # UC.jl v0.2 / v0.4 uses "Time horizon (h)"; v0.3 renamed to "Time (h)".
    t_h = params.get("Time horizon (h)", params.get("Time (h)", 24))
    t_min = params.get("Time horizon (min)", params.get("Time (min)"))
    if t_min is not None:
        t_h = t_min / 60
    # ``Time step (min)`` controls the block resolution.  Default 60 min
    # (hourly) — but sub-hourly fixtures (case14-sub-hourly.json) ship
    # ``"Time step (min)": 30`` to indicate 30-minute blocks.  The total
    # block count is ``T_h × 60 / step_min``; each block carries
    # ``duration = step_min / 60`` hours.
    step_min = float(params.get("Time step (min)", 60))
    block_duration_h = step_min / 60.0
    T = int(round(float(t_h) * 60.0 / step_min))
    power_balance_penalty = float(params.get("Power balance penalty ($/MW)", 1000.0))

    # 1 scenario × 1 chronological stage × T 1-hour blocks.
    scenario_array = [{"uid": 1, "probability_factor": 1.0}]
    stage_array = [
        {
            "uid": 1,
            "first_block": 0,
            "count_block": T,
            "active": 1,
            "chronological": True,
        }
    ]
    block_array = [{"uid": t + 1, "duration": block_duration_h} for t in range(T)]

    # --- Buses + Demands -----------------------------------------------------
    uc_buses = data.get("Buses", {})
    name_to_uid = {}
    bus_array = []
    demand_array = []
    demand_uid = 0
    for i, (bname, bdata) in enumerate(uc_buses.items()):
        uid = i + 1
        name_to_uid[bname] = uid
        bus_array.append({"uid": uid, "name": f"b{uid}"})

        load = bdata.get("Load (MW)", 0.0)
        load_ts = _time_series(load, T)
        if any(v > 0.0 for v in load_ts):
            demand_uid += 1
            demand_array.append(
                {
                    "uid": demand_uid,
                    "name": f"d{demand_uid}",
                    "bus": uid,
                    "lmax": [load_ts],  # outer = 1 stage, inner = T blocks
                }
            )

    # --- Price-sensitive loads -----------------------------------------------
    # UC.jl: ``Price-sensitive loads = {ps1: {Bus, Revenue ($/MW),
    # Demand (MW)}}``.  Models elastic demand: the LP serves up to
    # ``Demand (MW)`` MW at this bus only when it's profitable
    # (i.e. ``Revenue`` exceeds the marginal cost of generation).  Maps
    # onto an extra gtopt ``Demand`` entry with the per-element ``fcost``
    # override set to ``Revenue`` — gtopt's
    # ``SystemContext::demand_fail_cost(stage, fcost)`` resolves to the
    # per-element value when set (source/demand_lp.cpp:100), so this
    # entry's curtailment cost overrides the global
    # ``demand_fail_cost``.  ``Revenue ($/MW)`` is interpreted as
    # $/MWh per hour-block: the LP gains ``Revenue`` per MW served
    # (equivalent to "curtailment costs Revenue per MW").  ``forced``
    # is left unset so the LP can choose any served amount in
    # ``[0, Demand (MW)]``.
    uc_psl = data.get("Price-sensitive loads", {}) or {}
    for psname, psdata in uc_psl.items():
        bus_name = psdata.get("Bus")
        if bus_name not in name_to_uid:
            continue
        demand_mw = psdata.get("Demand (MW)", 0.0)
        revenue = psdata.get("Revenue ($/MW)")
        if revenue is None:
            continue
        # gtopt's ``Demand.fcost`` is now ``OptTBRealFieldSched``
        # (per-stage, per-block).  When UC.jl ships a per-hour
        # ``Revenue ($/MW)`` list, we round-trip it as a 2-D
        # ``[[hour0, hour1, ...]]`` shape so the LP cost coefficient
        # varies per block.  Scalar revenue still rides as a plain
        # number (broadcast everywhere).
        if isinstance(revenue, list):
            revenue_ts = _time_series(revenue, T)
            fcost_value: Any = [list(revenue_ts)]
            revenue_max = max(revenue_ts) if revenue_ts else 0.0
        else:
            fcost_value = float(revenue)
            revenue_max = float(revenue)
        # Skip entries where both demand and revenue are zero — UC.jl
        # base_with_storage ships a degenerate ps3 with {Revenue: 0,
        # Demand: 0} that would add a no-op Demand otherwise.
        demand_max = (
            max(_time_series(demand_mw, T))
            if isinstance(demand_mw, list)
            else float(demand_mw)
        )
        if revenue_max == 0.0 and demand_max == 0.0:
            continue
        demand_uid += 1
        ps_ts = _time_series(demand_mw, T)
        demand_array.append(
            {
                "uid": demand_uid,
                "name": psname,
                "bus": name_to_uid[bus_name],
                "lmax": [ps_ts],
                "fcost": fcost_value,
            }
        )

    # --- Reserve zones -------------------------------------------------------
    # UC.jl: ``Reserves = {r1: {Type, Amount (MW), Shortfall penalty ($/MW)}}``.
    # gtopt's ``ReserveZone`` distinguishes up-reserve (``urreq`` /
    # ``urcost``) from down-reserve (``drreq`` / ``drcost``) but is
    # agnostic to UC.jl's spinning / non-spinning distinction (which
    # is about WHICH gens can provide, not about WHAT direction the
    # reserve is in).  Map by UC.jl reserve type:
    #
    #   * spinning, non-spinning, regulation-up, replacement →
    #     up-reserve only (urreq, urcost).
    #   * flexiramp, regulation → bidirectional, treat as symmetric
    #     (urreq = drreq = Amount, urcost = drcost = penalty).
    #     Tracking the type per zone lets the provision loop below
    #     decide whether to emit a ``dr_provision_factor`` alongside
    #     the ``ur_provision_factor``.
    #
    # Per-zone metadata is stored alongside the name → uid map so the
    # provision loop can look up the direction without re-reading the
    # source ``Type``.
    _UP_RESERVE_TYPES = {
        "spinning",
        "non-spinning",
        "non_spinning",
        "nonspinning",
        "replacement",
        "regulation-up",
        "regulation_up",
    }
    _BIDIRECTIONAL_RESERVE_TYPES = {
        "flexiramp",
        "flexi-ramp",
        "regulation",
        "regulation-down",
        "regulation_down",
    }
    uc_reserves = data.get("Reserves", {}) or {}
    reserve_zone_array = []
    reserve_zone_name_to_uid: dict[str, int] = {}
    # Per-zone direction tag: "up" (urreq only) or "bidirectional" (urreq + drreq).
    reserve_zone_direction: dict[str, str] = {}
    for zname, zdata in uc_reserves.items():
        rtype = str(zdata.get("Type", "Spinning")).strip().lower()
        if rtype in _UP_RESERVE_TYPES:
            direction = "up"
        elif rtype in _BIDIRECTIONAL_RESERVE_TYPES:
            direction = "bidirectional"
        else:
            # Unknown reserve type — skip silently for now; emit a print
            # so the user knows the converter saw something it doesn't
            # model.
            print(
                f"  warning: skipping reserve zone {zname!r} with unmapped Type={rtype!r}"
            )
            continue
        zuid = len(reserve_zone_array) + 1
        reserve_zone_name_to_uid[zname] = zuid
        reserve_zone_direction[zname] = direction
        # ``Amount (MW)`` may be a scalar OR a per-hour list (UC.jl v0.3
        # case118-initcond ships a length-T time series).  Always emit
        # the 2-D ``[stage][block]`` shape that ``OptTBRealFieldSched``
        # broadcasts cleanly across the LP build (the per-block lookup
        # at source/reserve_zone_lp.cpp:40 would otherwise skip blocks
        # with no explicit value).
        amount = zdata.get("Amount (MW)", 0.0)
        req_ts = _time_series(amount, T)
        penalty = float(zdata.get("Shortfall penalty ($/MW)", 1000.0))
        zone_entry: dict = {
            "uid": zuid,
            "name": zname,
            "urreq": [req_ts],
            "urcost": penalty,
        }
        if direction == "bidirectional":
            zone_entry["drreq"] = [req_ts]
            zone_entry["drcost"] = penalty
        reserve_zone_array.append(zone_entry)

    # --- Generators + Commitments + ReserveProvisions ------------------------
    uc_gens = data.get("Generators", {})
    generator_array = []
    commitment_array = []
    reserve_provision_array = []
    generator_profile_array = []
    fuel_array: list[dict] = []
    gen_uid = 0
    commitment_uid = 0
    provision_uid = 0
    profile_uid = 0
    # Shared synthetic Fuel for piecewise-cost gens. UC.jl's cost-curve
    # slopes are already in $/MWh, so we link a Fuel with `price = 1.0`
    # and let `generator_lp.cpp` multiply `heat_rate_segments[k] × 1.0`
    # to recover the direct slope cost.
    _ucjl_fuel_emitted = False

    for gname, gdata in uc_gens.items():
        bus_name = gdata.get("Bus")
        if bus_name not in name_to_uid:
            continue
        bus_uid = name_to_uid[bus_name]
        gtype = str(gdata.get("Type", "Thermal")).lower()

        pmin, pmax, gcost, noload, pmax_segs, hr_segs = _gen_bounds(gdata)
        invest_cost = gdata.get("Investment cost ($)")
        is_candidate = invest_cost is not None and float(invest_cost) > 0.0
        # Candidate gens are FORCED to pmin=0: when expmod = 0 the LP
        # gives capainst = 0 → dispatch ≤ 0; a positive pmin would then
        # make the LP infeasible unless we install the gen.  The
        # original Profiled-branch in `_gen_bounds` already does this
        # unconditionally, but Thermal candidates can have piecewise
        # pmin > 0 — clamp here for the candidate case.
        if is_candidate:
            pmin = 0.0

        # Detect a UC.jl v0.3 time-varying renewable profile inside
        # ``Production cost curve (MW)``.  When present, override pmax
        # with the nameplate (peak across the horizon) and queue a
        # ``GeneratorProfile`` entry carrying the per-block availability
        # factor.  Profiled gens with NO Commitment (renewables) accept
        # this naturally; thermal gens with the same shape would also
        # work (effective profile gates dispatch upper bound).
        profile_ts = None
        curve_mw_raw = gdata.get("Production cost curve (MW)")
        if curve_mw_raw is not None:
            profile_ts, profile_nameplate = _detect_time_varying_capacity(
                curve_mw_raw, T
            )
            if profile_ts is not None:
                pmax = float(profile_nameplate)
                pmin = 0.0  # renewable: dispatch ≤ pmax × profile, no floor
                # The piecewise segments derived from the (cumulative)
                # MW breakpoints are meaningless on a time-varying
                # profile — drop them so gtopt's Commitment doesn't
                # try to enforce a stale heat-rate decomposition.
                pmax_segs, hr_segs = [], []

        gen_uid += 1
        # UC.jl's ``Production cost curve (MW)[0]`` (which we feed
        # into ``pmin``) is the WHEN-COMMITTED floor: dispatch is
        # required to be ≥ pmin only when the unit is committed (u =
        # 1).  Since the ``refactor(commitment): separate dispatch-
        # cost concerns + DecisionVariable`` (commit 16fbdde45) split
        # the field into two:
        #
        #   * ``Generator.pmin``  = always-on floor (column lower bound)
        #   * ``Commitment.pmin`` = when-committed floor (row gated by u)
        #
        # we must emit the UC.jl pmin onto the COMMITMENT entry, not
        # the Generator entry.  Otherwise the generator column gets
        # ``lowb = pmin > 0`` and forces dispatch even when the unit
        # is uncommitted — which is exactly the regression that broke
        # 22 of the UC.jl golden cross-checks (e.g. ``GenX`` pmin =
        # 100 became a hard always-on floor instead of a soft commit-
        # gated floor; the MIP then over-commits to satisfy the
        # always-on constraint and the dispatch cost balloons).
        #
        # Profiled (renewable) and candidate (TEP, expmod ≥ 0) gens
        # carry no Commitment, so for them the UC.jl pmin stays on
        # Generator (where the LP treats it as a true always-on
        # floor — matching UC.jl's renewable / candidate semantics).
        commitment_pmin = 0.0
        emit_gen_pmin = pmin
        if gtype == "thermal" and not is_candidate and profile_ts is None:
            commitment_pmin = pmin
            emit_gen_pmin = 0.0

        gen_entry = {
            "uid": gen_uid,
            "name": gname,
            "bus": bus_uid,
            "pmin": round(emit_gen_pmin, 6),
            "pmax": round(pmax, 6),
            "gcost": round(gcost, 6),
            "capacity": round(pmax, 6),
        }
        # Skip the ``type`` field: UC.jl's ``Type = "Profiled"`` /
        # ``"Thermal"`` are JSON-level classifiers that determine which
        # branch of *our* converter runs; gtopt's ``Generator.type`` is
        # a free-form tag with no semantic effect on the LP build, so
        # leaving it unset avoids accidentally selecting a future gtopt
        # type-based code path.
        # TEP: ``Investment cost ($)`` activates the expansion fields
        # (overrides ``capacity`` to 0 on candidate units — see
        # ``_expansion_fields``).
        gen_entry.update(
            _expansion_fields(invest_cost, pmax, integer=not relax_commitment)
        )
        generator_array.append(gen_entry)

        # GeneratorProfile entry for time-varying renewable capacity
        # (emitted only when ``_detect_time_varying_capacity`` returned
        # a non-flat profile above).  The profile is wrapped to
        # gtopt's ``STBRealFieldSched`` 3-D shape
        # ``[scenario][stage][block]`` — we have 1 scenario × 1 stage
        # × T blocks, so the outer two dims collapse to a single list.
        if profile_ts is not None:
            profile_uid += 1
            generator_profile_array.append(
                {
                    "uid": profile_uid,
                    "name": f"{gname}_profile",
                    "generator": gname,
                    "profile": [[[round(float(v), 6) for v in profile_ts]]],
                }
            )

        # Commitment is only meaningful for thermal generators with a
        # static capacity envelope.  Time-varying / renewable gens
        # dispatch within ``[0, pmax × profile(t)]`` and have no
        # on/off semantics — skip Commitment for them even when
        # UC.jl's ``Type`` field defaults to Thermal (RTS-GMLC v0.3
        # ships every gen without a Type field, so the profile-
        # detection signal is what tells us "this is renewable").
        #
        # Also skip when ``pmax == 0`` — gtopt's ``GeneratorLP`` early-
        # outs the per-block column at ``source/generator_lp.cpp:131``
        # when ``block_pmax == block_pmin == 0``, leaving no column
        # for a Commitment to reference.  Degenerate ``pmax = 0`` gens
        # turn up in RTS-GMLC v0.3 (e.g. ``212_CSP_1`` ships an all-
        # zero CSP profile) and tripped ``flat_map::at`` pre-fix.
        if gtype != "thermal" or profile_ts is not None or pmax <= 0.0:
            continue

        commitment_uid += 1
        c_entry = {
            "uid": commitment_uid,
            "name": f"{gname}_uc",
            "generator": gname,
            "relax": bool(relax_commitment),
        }
        # ``Commitment.pmin`` carries UC.jl's when-committed floor.
        # See the ``commitment_pmin`` derivation above for why this
        # field belongs on Commitment, not Generator.  Only emit
        # when non-zero (keeps the JSON tidy and matches the legacy
        # output for gens with ``Production cost curve (MW)[0] = 0``).
        if commitment_pmin > 0.0:
            c_entry["pmin"] = round(commitment_pmin, 6)

        # Map optional UC.jl fields → Commitment fields.
        for ucjl_key, gtopt_key in (
            ("Minimum uptime (h)", "min_up_time"),
            ("Minimum downtime (h)", "min_down_time"),
            ("Ramp up limit (MW)", "ramp_up"),
            ("Ramp down limit (MW)", "ramp_down"),
            ("Startup limit (MW)", "startup_ramp"),
            ("Shutdown limit (MW)", "shutdown_ramp"),
            ("Must run?", "must_run"),
        ):
            val = gdata.get(ucjl_key)
            if val is not None:
                c_entry[gtopt_key] = val

        # ``Commitment status`` (UC.jl ``case14/fixed.json``): per-block
        # forced commitment.  Each element is a tri-state ``true`` /
        # ``false`` / ``null`` — ``null`` leaves the block free.
        # gtopt's ``Commitment.fixed_status`` is a 2-D
        # ``OptTBRealFieldSched`` whose inner cells are plain ``Real``
        # (no per-cell nullable representation) — so partial pinning
        # cannot be expressed losslessly.  Strategy:
        #
        #   * If EVERY entry is a concrete ``true`` / ``false``, emit
        #     ``[[0.0, 1.0, ...]]`` with no sentinel values — the LP
        #     pins each ``u`` to 0 or 1 as specified.
        #   * If ANY entry is ``null`` (free), OMIT ``fixed_status``
        #     entirely.  This loses the partial pinning on the
        #     concrete entries but keeps the LP physically correct
        #     (every block is left free for the solver to choose).
        #     UC.jl's case14/fixed.json only uses partial-pinning on
        #     ``g6`` ([false, null, true, null]); dropping its pins
        #     simply gives gtopt the same degree of freedom UC.jl
        #     allows for the null blocks, while the concrete pins
        #     would otherwise need an out-of-band sentinel (e.g.
        #     ``-1.0``) that pollutes the emitted JSON.
        fixed_status_raw = gdata.get("Commitment status")
        if fixed_status_raw is not None:
            # Truncate / pad to T BEFORE checking for nulls so trailing
            # padding doesn't appear as a "free" entry that disables
            # otherwise-clean pinning.
            trimmed = list(fixed_status_raw[:T])
            if len(trimmed) < T:
                # Repeat the last concrete value when the list is short
                # — broadcast convention used by UC.jl's reader.
                pad_value = trimmed[-1] if trimmed else None
                trimmed += [pad_value] * (T - len(trimmed))
            # UC.jl uses Python ``bool`` (true / false) in modern
            # fixtures (case14/fixed.json) but plain ``int`` 1 / 0 in
            # older ones (issue-0057.json, schema v0.3).  Map both:
            # truthy → 1.0, falsy non-null → 0.0.  ``null`` (no-pin)
            # cells emit ``-1.0`` — gtopt's ``commitment_lp.cpp:263``
            # treats any value outside ``[0, 1]`` as the no-pin
            # sentinel, leaving ``u`` free on that (stage, block).
            mapped: list[float] = []
            for v in trimmed:
                if v is None:
                    mapped.append(-1.0)
                else:
                    mapped.append(1.0 if bool(v) else 0.0)
            c_entry["fixed_status"] = [mapped]  # outer dim = 1 stage

        # Initial status (h): +N online, -N offline.
        init_u, init_h = _initial_status_to_uc(gdata.get("Initial status (h)"))
        if init_u is not None:
            c_entry["initial_status"] = init_u
            c_entry["initial_hours"] = init_h

        # Initial power (MW): the gen's dispatch at t = -1.  Needed by
        # gtopt's first-block ramp-up / ramp-down rows so they enforce
        # ``p[0] - initial_power ≤ RU·u_init + SU·(1 - u_init)`` rather
        # than the looser ``p[0] ≤ RU·u_init + SU·(1 - u_init)``.
        # Without this, hot-start gens with ``pmin > ramp_up`` (e.g.
        # RTS-GMLC ``216_STEAM_1``: pmin = 62, ramp_up = 60,
        # ``Initial power (MW) = 62``) make the first-block LP
        # infeasible (pmin floor 62 clashes with ramp cap 60).
        init_p = gdata.get("Initial power (MW)")
        if init_p is not None:
            c_entry["initial_power"] = float(init_p)

        # Startup costs: scalar single tier or tiered hot/warm/cold.
        startup_costs = gdata.get("Startup costs ($)") or []
        startup_delays = gdata.get("Startup delays (h)") or []
        if len(startup_costs) >= 3 and len(startup_delays) >= 2:
            c_entry["hot_start_cost"] = float(startup_costs[0])
            c_entry["warm_start_cost"] = float(startup_costs[1])
            c_entry["cold_start_cost"] = float(startup_costs[2])
            c_entry["hot_start_time"] = float(startup_delays[0])
            c_entry["cold_start_time"] = float(startup_delays[-1])
        elif len(startup_costs) >= 2 and len(startup_delays) >= 1:
            # 2-tier: hot vs cold (use last delay as cold threshold).
            c_entry["hot_start_cost"] = float(startup_costs[0])
            c_entry["cold_start_cost"] = float(startup_costs[-1])
            c_entry["hot_start_time"] = float(startup_delays[0])
            c_entry["cold_start_time"] = float(startup_delays[-1])
        elif startup_costs:
            c_entry["startup_cost"] = float(startup_costs[0])

        # Piecewise heat-rate segmentation (when >=3 breakpoints).
        # After the 16fbdde45 Commitment refactor, piecewise cost segments
        # live on **Generator** (not Commitment). gtopt's piecewise cost
        # path (`generator_lp.cpp`) requires a fuel reference so the per-
        # segment slope is multiplied by `Fuel.price`; we wire a shared
        # `_ucjl_unit` Fuel with `price = 1.0` so the per-segment slopes
        # carried in `heat_rate_segments` act as direct $/MWh costs.
        # `noload_cost` stays on Commitment — it carries the cost-curve
        # intercept paid only when the unit is committed and dispatching
        # at pmin, matching ``f(p) = c_pmin + Σ h_k δ_k``.
        if pmax_segs and hr_segs:
            gen_entry["pmax_segments"] = pmax_segs
            gen_entry["heat_rate_segments"] = hr_segs
            gen_entry["fuel"] = _UCJL_UNIT_FUEL_NAME
            gen_entry["gcost"] = 0  # segments carry the slope cost
            if not _ucjl_fuel_emitted:
                fuel_array.append(
                    {
                        "uid": 1,
                        "name": _UCJL_UNIT_FUEL_NAME,
                        "price": 1.0,
                    }
                )
                _ucjl_fuel_emitted = True
            # ``noload`` is now the corrected value
            # ``cost[0] - slope0 × pmin`` (see ``_compute_pieces``), so
            # it can be NEGATIVE on hot-start gens where the over-counted
            # ``slope0 × pmin`` term exceeds ``cost[0]`` (e.g. case14/g1:
            # cost[0]=1400, slope0=20, pmin=100 → noload=-600).  The
            # negative coefficient on ``u`` is exactly the credit that
            # cancels the ``gcost × p`` over-charge on the
            # 0..pmin range when the gen is committed.  Emit on any
            # non-zero value (the LP-cost contribution is signed
            # already; suppressing negatives broke ``test_real_case14_*``
            # by leaving gtopt with the over-charge but no credit).
            if noload != 0.0:
                c_entry["noload_cost"] = round(noload, 6)

        commitment_array.append(c_entry)

        # ReserveProvision: one entry per generator listing the zones it
        # is eligible to serve.  UC.jl's ``Reserve eligibility`` is a list
        # of zone names; resolve against the map built above and silently
        # drop unknown zones.  When ANY of the eligible zones is
        # bidirectional (flexiramp / regulation), the provision needs
        # both up- and down-reserve fields — gtopt's
        # ``ReserveProvisionLP`` skips a direction silently when its
        # ``*_provision_factor`` is unset.
        eligible = gdata.get("Reserve eligibility") or []
        zones = [z for z in eligible if z in reserve_zone_name_to_uid]
        if zones:
            provision_uid += 1
            needs_dr = any(
                reserve_zone_direction.get(z) == "bidirectional" for z in zones
            )
            # ``urmax`` + ``ur_provision_factor`` must both be set or the
            # provision is silently skipped: ``ur_provision_factor``
            # gates the whole add_to_lp branch
            # (source/reserve_provision_lp.cpp:49); ``urmax`` (or an
            # available capacity-expansion column) is required per-block
            # at source/reserve_provision_lp.cpp:101-107.  Non-expandable
            # thermals like UC.jl's case14 don't ship a capacity_col, so
            # the fallback would silently ``continue``.  Setting
            # ``urmax = pmax`` matches UC.jl's implicit cap of "all
            # capacity is reservable" without needing a ``Reserve max``
            # field that UC.jl's schema doesn't carry.
            pmax_box = [[float(pmax)] * T]
            provision_entry: dict = {
                "uid": provision_uid,
                "name": f"{gname}_rp",
                "generator": gname,
                "reserve_zones": zones,
                "urmax": pmax_box,
                "ur_provision_factor": 1.0,
            }
            if needs_dr:
                provision_entry["drmax"] = pmax_box
                provision_entry["dr_provision_factor"] = 1.0
            reserve_provision_array.append(provision_entry)

    # --- Storage units (BESS) ------------------------------------------------
    # UC.jl ``Storage units`` map onto gtopt's unified ``Battery`` element:
    # System::expand_batteries() auto-generates the discharge Generator,
    # charge Demand, and linking Converter from a single ``Battery`` entry.
    # Per-block time-series fields (``Maximum level (MWh)`` as a list of T
    # values) round-trip naturally — gtopt's ``OptTRealFieldSched`` /
    # ``OptTBRealFieldSched`` accept both scalar and list shapes.
    #
    # Per-block hourly inputs honored end-to-end:
    #   * ``Discharge cost ($/MW)`` → ``Battery.discharge_cost`` (TB)
    #   * ``Charge cost ($/MW)``    → ``Battery.charge_cost`` (TB)
    #   * ``Maximum charge rate (MW)``     → ``Battery.pmax_charge`` (TB)
    #   * ``Maximum discharge rate (MW)``  → ``Battery.pmax_discharge`` (TB)
    #   * ``Minimum charge rate (MW)``     → ``Battery.pmin_charge`` (TB)
    #     — HARD floor on synthetic charge ``Demand.lmin``.
    #   * ``Minimum discharge rate (MW)``  → ``Battery.pmin_discharge`` (TB)
    #     — HARD floor on synthetic discharge ``Generator.pmin``.
    #   * ``Charge/Discharge efficiency``  → ``Battery.input/output_efficiency``
    #
    # Dropped (silently) — no native gtopt mapping needed:
    #   * ``Allow simultaneous charging and discharging`` — gtopt's
    #     synthetic charge Demand and discharge Generator are
    #     independent LP elements (linked only through the SoC row
    #     in the Converter+Battery pair), so simultaneous charge +
    #     discharge is allowed by construction.  UC.jl's flag
    #     forbids it via a binary; we ignore the flag (the LP almost
    #     always chooses one direction at optimum because both legs
    #     pay positive costs in the cost-minimisation objective).
    uc_storage = data.get("Storage units", {}) or {}
    battery_array = []
    bat_uid = 0
    for sname, sdata in uc_storage.items():
        bus_name = sdata.get("Bus")
        if bus_name not in name_to_uid:
            continue
        bat_uid += 1
        # gtopt's ``Battery`` defaults add a ``storage_close`` row that
        # equates final SoC with initial SoC when ``use_state_variable
        # = false`` (the default for batteries — see
        # include/gtopt/storage_lp.hpp:836-852).  UC.jl batteries with
        # ``Initial level (MWh)`` + ``Last period minimum level
        # (MWh)`` are NOT cyclic — they set INDEPENDENT start / end
        # SoC bounds (e.g. case14-storage's ``su3`` has eini=20,
        # efin=21 which is infeasible under the close row).  Setting
        # ``use_state_variable = true`` swaps the close row for an
        # SDDP-style state column publication — irrelevant in our
        # single-stage / single-phase setup, but cleanly disables the
        # cycle equality so eini and efin become independent bounds.
        b_entry: dict = {
            "uid": bat_uid,
            "name": sname,
            "bus": name_to_uid[bus_name],
            "use_state_variable": True,
            "daily_cycle": False,
        }
        # SoC bound mapping for ``emin`` / ``emax``: gtopt's Battery now
        # exposes both as ``OptTBRealFieldSched`` (per-(stage, block)).
        # Accepted JSON shapes:
        #   * scalar — broadcast to every (stage, block);
        #   * 2-D nested ``[[v_block0, v_block1, ..., v_blockT-1], ...]``
        #     indexed by stage then block (one outer entry per stage).
        # UC.jl ships three relevant fields:
        #   * ``Maximum/Minimum level (MWh)``: scalar OR per-block list of
        #     length T (e.g. ``case14/storage``'s ``su3``).
        #   * ``Last period maximum/minimum level (MWh)``: scalar that
        #     overrides the bound on the FINAL block of the horizon.
        # We expand to the 2-D ``[[..., last_period]]`` shape whenever
        # either (a) UC.jl ships a per-block list or (b) the last-period
        # override is present and differs from the broadcast base —
        # otherwise we keep the compact scalar form.  This is the
        # converter-side enabler for the per-block SoC bound LP feature
        # added 2026-05-18 on gtopt's side.
        for base_key, last_key, gtopt_key in (
            ("Maximum level (MWh)", "Last period maximum level (MWh)", "emax"),
            ("Minimum level (MWh)", "Last period minimum level (MWh)", "emin"),
        ):
            base = sdata.get(base_key)
            if base is None:
                continue
            if isinstance(base, list):
                # Already a per-block schedule — pad/truncate to T.
                base_list = [float(v) for v in base[:T]]
                if len(base_list) < T:
                    base_list += [base_list[-1]] * (T - len(base_list))
                row = list(base_list)
            else:
                # Scalar broadcast — promote to per-block only when the
                # last-period override is materially different.
                row = [float(base)] * T
            last_override = sdata.get(last_key)
            promote_to_2d = isinstance(base, list)
            if last_override is not None:
                lo = float(last_override)
                if lo != row[-1]:
                    row[-1] = lo
                    promote_to_2d = True
            if promote_to_2d:
                # Outer dim = 1 stage (the converter emits a single-stage
                # planning horizon — see ``stage_array`` setup above).
                b_entry[gtopt_key] = [row]
            else:
                # No override + no per-block list → compact scalar form
                # (round-trips identically to the legacy emission).
                b_entry[gtopt_key] = float(base)

        # Charge/discharge rate envelopes — gtopt's Battery exposes
        # all four as ``OptTBRealFieldSched`` (per-(stage, block)),
        # forwarded by ``System::expand_batteries()`` to the synthetic
        # ``Generator.pmin/pmax`` (discharge) and ``Demand.lmin/lmax``
        # (charge).  Scalars stay scalar; per-hour UC.jl lists emit
        # as 2-D ``[[h0, h1, ...]]``.
        for ucjl_key, gtopt_key in (
            ("Maximum charge rate (MW)", "pmax_charge"),
            ("Maximum discharge rate (MW)", "pmax_discharge"),
            ("Minimum charge rate (MW)", "pmin_charge"),
            ("Minimum discharge rate (MW)", "pmin_discharge"),
        ):
            value = sdata.get(ucjl_key)
            if value is None:
                continue
            if isinstance(value, list):
                b_entry[gtopt_key] = [list(_time_series(value, T))]
            else:
                b_entry[gtopt_key] = float(value)
        # ``input_efficiency`` / ``output_efficiency`` are TB since
        # PR-E (per-(stage, block)).  Hourly UC.jl lists round-trip
        # as 2-D ``[[h0, h1, ...]]``; scalar values stay scalar
        # (broadcast across every block).
        for ucjl_key, gtopt_key in (
            ("Charge efficiency", "input_efficiency"),
            ("Discharge efficiency", "output_efficiency"),
        ):
            value = sdata.get(ucjl_key)
            if value is None:
                continue
            if isinstance(value, list):
                b_entry[gtopt_key] = [list(_time_series(value, T))]
            else:
                b_entry[gtopt_key] = float(value)

        # UC.jl ``Loss factor`` models per-period SoC decay
        # ``SoC[t+1] = (1 − λ) × SoC[t] + η_in·charge − discharge/η_out``.
        # gtopt's Battery has no native SoC-decay term, but folding λ
        # into the existing efficiencies recovers the same round-trip
        # energy loss:
        #
        #   η_in_eff  = η_in  × √(1 − λ)
        #   η_out_eff = η_out × √(1 − λ)
        #
        # The square root splits the decay symmetrically across one
        # charge-discharge cycle.  For UC.jl ``Loss factor = 0.01``,
        # this shaves ~0.5 % off each efficiency — a faithful
        # approximation for BESS that cycles every block (the
        # case14/storage workload).  For long-horizon idle storage,
        # this would underestimate decay; a true ``Battery.loss_factor``
        # SoC-row term would be the semantic-pure fix (left as
        # follow-up).
        loss_factor_raw = sdata.get("Loss factor")
        if loss_factor_raw is not None:
            lf_scalar = float(_first_hour(loss_factor_raw))
            if lf_scalar > 0.0:
                damp = math.sqrt(max(1.0 - lf_scalar, 0.0))

                # Default efficiencies are 1.0 when UC.jl omitted them.
                # Efficiencies are TB since PR-E — the field may be a
                # scalar (broadcast) or a 2-D ``[[h0, h1, ...]]`` list.
                # When 2-D, multiply each cell by damp; when scalar,
                # multiply directly.
                def _scale_eff(raw: Any, damp_v: float) -> Any:
                    if raw is None:
                        return round(damp_v, 6)
                    if isinstance(raw, list):
                        # 2-D [[h0, h1, ...], ...] - scale each cell.
                        return [
                            [round(float(c) * damp_v, 6) for c in row] for row in raw
                        ]
                    return round(float(raw) * damp_v, 6)

                b_entry["input_efficiency"] = _scale_eff(
                    b_entry.get("input_efficiency"), damp
                )
                b_entry["output_efficiency"] = _scale_eff(
                    b_entry.get("output_efficiency"), damp
                )
        # Scalars (not field schedules).
        for ucjl_key, gtopt_key in (
            ("Initial level (MWh)", "eini"),
            ("Last period minimum level (MWh)", "efin"),
        ):
            value = sdata.get(ucjl_key)
            if value is not None:
                b_entry[gtopt_key] = float(value)

        # Discharge cost → ``Battery.discharge_cost``; Charge cost →
        # ``Battery.charge_cost``.  Both are ``OptTBRealFieldSched``
        # (per-(stage, block)) so per-hour UC.jl lists round-trip as
        # 2-D ``[[hour0, hour1, ...]]`` without collapse.  Scalar
        # values still emit as a plain number (broadcast everywhere).
        # ``System::expand_batteries()`` forwards ``discharge_cost``
        # onto the synthetic discharge ``Generator.gcost`` and
        # ``charge_cost`` (negated) onto the synthetic charge
        # ``Demand.fcost`` — both pipelines are TB-compatible.
        def _cost_value(raw: Any) -> Any:
            if isinstance(raw, list):
                return [list(_time_series(raw, T))]
            return float(raw)

        d_cost = sdata.get("Discharge cost ($/MW)")
        if d_cost is not None:
            b_entry["discharge_cost"] = _cost_value(d_cost)
        c_cost = sdata.get("Charge cost ($/MW)")
        if c_cost is not None:
            b_entry["charge_cost"] = _cost_value(c_cost)
        # Track installed capacity = max SoC so expansion / capacity-fail
        # bounds line up.
        emax_val = sdata.get("Maximum level (MWh)")
        if emax_val is not None:
            scalar_emax = (
                _first_hour(emax_val) if isinstance(emax_val, list) else emax_val
            )
            b_entry["capacity"] = float(scalar_emax)
        # Conditional rate floor: UC.jl's ``Minimum charge/discharge rate``
        # only fires when the battery is actively charging/discharging.
        # gtopt's static ``Demand.lmin`` / ``Generator.pmin`` are HARD
        # every-block floors by default; flipping ``Battery.commitment``
        # tells ``System::expand_batteries()`` to enable the per-block
        # binary gating on the synthetic ``Converter`` so the floors only
        # fire when the corresponding binary is 1 — matching UC.jl.
        # Commitment binaries are always integer (introduces a true MIP);
        # for LP-only solves leave ``Minimum charge/discharge rate`` out
        # of the UC.jl input so the static floor stays unconditional.
        if not relax_commitment and any(
            sdata.get(k) is not None
            for k in (
                "Minimum charge rate (MW)",
                "Minimum discharge rate (MW)",
            )
        ):
            b_entry["commitment"] = True
        battery_array.append(b_entry)

    # --- Lines ---------------------------------------------------------------
    uc_lines = data.get("Transmission lines", {})
    line_array = []
    line_uid = 0
    for lname, ldata in uc_lines.items():
        src = ldata.get("Source bus")
        tgt = ldata.get("Target bus")
        if src not in name_to_uid or tgt not in name_to_uid:
            continue
        # Accept any of UC.jl's three reactance encodings (TEP fixtures
        # ship ``Reactance (p.u.)``, case14 / case118 ship the
        # mislabelled ``Reactance (ohms)`` which is also already-pu;
        # case5 / TEP fall back to ``1 / Susceptance (S)``).
        x = _reactance_from_line(ldata)
        if x is None:
            continue

        # UC.jl line limits are intentionally NOT enforced on the
        # gtopt side.  UC.jl's default solver uses lazy
        # XavQiuWanThi2019 violation enforcement on top of a PTDF
        # matrix thresholded at ``isf_cutoff = 0.005`` (and ``0.001``
        # for LODF), which zeroes out most line-flow contributions
        # before the violation finder ever sees them.  The empirical
        # effect on the published case14/congested golden is that
        # ``Line overflow (MW) = 0`` on every line — i.e. no soft or
        # hard line constraint is ever added — even though a
        # Kirchhoff-exact DC OPF would route ~107 MW through ``l1``
        # (well above its Normal=15 target).
        #
        # gtopt's ``LineLP`` does support PLEXOS-style two-tier
        # ratings (``tmax_normal_{ab,ba}`` / ``overload_penalty``),
        # but emitting them here would charge real overload penalties
        # that UC.jl never pays.  The cleanest equivalence is the
        # PLEXOS config "``Overload Penalty = 0`` + ``Max Rating =
        # +inf``": every line ships with a 99999 sentinel cap and no
        # soft tier.  See ``tools/test_ucjl2gtopt.py`` golden tests
        # for the matching objective values.
        tmax = 99999.0

        line_uid += 1
        line_entry = {
            "uid": line_uid,
            "name": lname,
            "bus_a": name_to_uid[src],
            "bus_b": name_to_uid[tgt],
            "reactance": round(float(x), 6),
            "tmax_ab": round(tmax, 1),
            "tmax_ba": round(tmax, 1),
        }
        # TEP: ``Investment cost ($)`` activates the line-expansion fields
        # (overrides ``capacity`` to 0 on candidate lines — see
        # ``_expansion_fields``).
        line_entry.update(
            _expansion_fields(
                ldata.get("Investment cost ($)"), tmax, integer=not relax_commitment
            )
        )
        line_array.append(line_entry)

    # --- Interfaces (transmission-corridor flow limits) ----------------------
    # UC.jl ``Interfaces`` are linear combinations of line flows bounded
    # by upper / lower limits with an optional shortfall penalty.  No
    # native gtopt element exists, but the contract maps directly onto
    # ``UserConstraint`` (PAMPL): one entry per (interface × bound) with
    # the branch coefficients folded into a PAMPL expression.  Time-
    # varying RHS expands to one constraint per block (using PAMPL's
    # ``for(block in {N})`` scoping); scalar RHS stays as a single
    # all-blocks constraint.  ``Flow limit penalty ($/MW)`` becomes the
    # ``penalty`` on each emitted constraint so the LP can violate the
    # bound at a known cost rather than going infeasible.
    uc_interfaces = data.get("Interfaces", {}) or {}
    user_constraint_array = []
    uc_uid = 0

    def _flow_expression(branches):
        """Build the ``Σ coeff * line('name').flow`` expression body."""
        parts = []
        for line_name, coeff in branches.items():
            parts.append(f"{float(coeff)} * line('{line_name}').flow")
        return " + ".join(parts)

    def _emit_interface_bound(ifname, expr_body, op, limit, penalty_val, suffix):
        """Append one UserConstraint entry for an Interface bound.

        ``limit`` may be a scalar (one all-blocks constraint) or a
        length-T list (one per-block constraint per element).
        """
        nonlocal uc_uid
        if limit is None:
            return
        if isinstance(limit, list):
            for t_idx, val in enumerate(limit[:T]):
                uc_uid += 1
                entry = {
                    "uid": uc_uid,
                    "name": f"{ifname}_{suffix}_b{t_idx + 1}",
                    "expression": (
                        f"{expr_body} {op} {float(val)}, for(block in {{{t_idx + 1}}})"
                    ),
                }
                if penalty_val is not None:
                    entry["penalty"] = float(penalty_val)
                user_constraint_array.append(entry)
        else:
            uc_uid += 1
            entry = {
                "uid": uc_uid,
                "name": f"{ifname}_{suffix}",
                "expression": f"{expr_body} {op} {float(limit)}",
            }
            if penalty_val is not None:
                entry["penalty"] = float(penalty_val)
            user_constraint_array.append(entry)

    for ifname, ifdata in uc_interfaces.items():
        branches = ifdata.get("Branches", {})
        # Drop branches that reference lines we didn't emit (filtered
        # out by the line loop above).  No way to spot-check that
        # cleanly from the converter; assume UC.jl source is consistent.
        if not branches:
            continue
        expr_body = _flow_expression(branches)
        penalty_val = ifdata.get("Flow limit penalty ($/MW)")
        _emit_interface_bound(
            ifname,
            expr_body,
            "<=",
            ifdata.get("Net flow upper limit (MW)"),
            penalty_val,
            "upper",
        )
        _emit_interface_bound(
            ifname,
            expr_body,
            ">=",
            ifdata.get("Net flow lower limit (MW)"),
            penalty_val,
            "lower",
        )

    # --- Contingencies (N-1 security via LODF + PAMPL) -----------------------
    # UC.jl ``Contingencies`` are single-element line outages.  For each
    # contingency ``out`` and each remaining "monitored" line ``mon``,
    # emit a pair of soft ``UserConstraint`` rows enforcing::
    #
    #     |f_mon + LODF[mon, out] × f_out|  ≤  Emergency_mon
    #
    # ``LODF`` is computed off-line from the network reactance graph via
    # ``_compute_lodf``.  Coefficients below the 1e-6 magnitude
    # threshold are dropped to keep the row count manageable; on case14
    # this trims roughly 50 % of the (mon, out) pairs.  Two
    # ``UserConstraint`` rows per surviving pair (upper + lower).
    uc_contingencies = (
        {} if skip_contingencies else (data.get("Contingencies", {}) or {})
    )
    if uc_contingencies and line_array:
        # Build the (name, from_bus_idx, to_bus_idx, reactance) tuples.
        # gtopt's name_to_uid is 1-indexed; LODF math needs 0-indexed.
        line_specs = []
        line_emerg: dict[str, float] = {}
        line_penalty: dict[str, float] = {}
        for lname, ldata in uc_lines.items():
            src = ldata.get("Source bus")
            tgt = ldata.get("Target bus")
            if src not in name_to_uid or tgt not in name_to_uid:
                continue
            x = _reactance_from_line(ldata)
            if x is None:
                continue
            line_specs.append(
                (lname, name_to_uid[src] - 1, name_to_uid[tgt] - 1, float(x))
            )
            emerg = ldata.get("Emergency flow limit (MW)")
            if emerg is None:
                emerg = ldata.get("Normal flow limit (MW)", 99999.0)
            line_emerg[lname] = float(_first_hour(emerg))
            penalty = ldata.get("Flow limit penalty ($/MW)", 1000.0)
            line_penalty[lname] = float(penalty)

        lodf_table = _compute_lodf(
            line_specs, n_buses=len(bus_array), eps=contingency_eps
        )

        # Restrict the contingency set to the lines listed in
        # ``Contingencies`` (one outage per contingency).
        outaged_set = set()
        for cdata in uc_contingencies.values():
            for outaged in cdata.get("Affected lines", []) or []:
                outaged_set.add(outaged)

        for (mon, out), lodf_coef in lodf_table.items():
            if out not in outaged_set:
                continue
            emerg_mon = line_emerg.get(mon, 99999.0)
            penalty = line_penalty.get(mon, 1000.0)
            expr_body = (
                f"1.0 * line('{mon}').flow + {lodf_coef:.6f} * line('{out}').flow"
            )
            uc_uid += 1
            user_constraint_array.append(
                {
                    "uid": uc_uid,
                    "name": f"ctg_{out}_mon_{mon}_upper",
                    "expression": f"{expr_body} <= {emerg_mon}",
                    "penalty": penalty,
                }
            )
            uc_uid += 1
            user_constraint_array.append(
                {
                    "uid": uc_uid,
                    "name": f"ctg_{out}_mon_{mon}_lower",
                    "expression": f"{expr_body} >= {-emerg_mon}",
                    "penalty": penalty,
                }
            )

    output = {
        "options": {
            "method": "monolithic",
            "annual_discount_rate": 0.0,
            "output_format": "csv",
            "output_compression": "uncompressed",
            # gtopt is long-only since 2026-05-19: the `output_layout`
            # selector and wide output were removed (long drops exact-zero
            # rows), and gtopt's strict parser now rejects the key.  The
            # UC.jl cross-check reader reconstructs the full per-block grid
            # from the sparse long output (a missing (block) ⇒ zero), so no
            # layout option is emitted here.
            # Nested under model_options per the 2026-05-17 schema
            # reorganisation; the legacy top-level keys are still
            # accepted as deprecated aliases but emit a warning.
            "model_options": {
                "use_single_bus": bool(copperplate),
                "use_kirchhoff": not bool(copperplate),
                "demand_fail_cost": power_balance_penalty,
                "scale_objective": 1.0,  # Match UC.jl absolute $ values.
            },
        },
        "simulation": {
            "block_array": block_array,
            "stage_array": stage_array,
            "scenario_array": scenario_array,
        },
        "system": {
            "name": os.path.splitext(os.path.basename(ucjl_path))[0],
            "bus_array": bus_array,
            "fuel_array": fuel_array,
            "generator_array": generator_array,
            "demand_array": demand_array,
            "line_array": line_array,
            "commitment_array": commitment_array,
            "reserve_zone_array": reserve_zone_array,
            "reserve_provision_array": reserve_provision_array,
            "battery_array": battery_array,
            "user_constraint_array": user_constraint_array,
            "generator_profile_array": generator_profile_array,
        },
    }

    if output_path:
        with open(output_path, "w", encoding="utf-8") as f:
            json.dump(output, f, indent=2)
        print(f"Wrote {output_path}")
        print(f"  Buses: {len(bus_array)}")
        print(f"  Demands: {len(demand_array)}")
        print(f"  Generators: {len(generator_array)}")
        print(f"  Commitments: {len(commitment_array)}")
        print(f"  Lines: {len(line_array)}")
        print(f"  Reserve zones: {len(reserve_zone_array)}")
        print(f"  Reserve provisions: {len(reserve_provision_array)}")
        print(f"  Batteries: {len(battery_array)}")
        print(f"  Generator profiles: {len(generator_profile_array)}")
        print(f"  User constraints: {len(user_constraint_array)}")
        print(f"  Time horizon: {T} hours (1 scenario × 1 stage × {T} blocks)")
    return output


def main():
    parser = argparse.ArgumentParser(
        description="Convert UnitCommitment.jl JSON to gtopt JSON"
    )
    parser.add_argument("input", help="UC.jl JSON file path")
    parser.add_argument("-o", "--output", help="Output gtopt JSON path")
    parser.add_argument(
        "--mip",
        action="store_true",
        help="Emit MIP commitment (relax=false). Default: LP relaxation.",
    )
    parser.add_argument(
        "--copperplate",
        action="store_true",
        help="Drop the transmission network (use_single_bus=true).",
    )
    parser.add_argument(
        "--contingency-eps",
        type=float,
        default=0.10,
        help=(
            "Drop LODF coefficients below this magnitude when emitting "
            "N-1 contingency constraints (default 0.10 = 10%%).  Smaller "
            "values emit more soft constraints; 0 keeps every "
            "non-trivial sensitivity."
        ),
    )
    parser.add_argument(
        "--no-contingencies",
        action="store_true",
        help=(
            "Skip the UC.jl ``Contingencies`` block entirely (no LODF-"
            "based N-1 UserConstraints emitted).  Useful when the LP "
            "build time matters more than security analysis."
        ),
    )
    args = parser.parse_args()

    convert(
        args.input,
        args.output,
        relax_commitment=not args.mip,
        copperplate=args.copperplate,
        contingency_eps=args.contingency_eps,
        skip_contingencies=args.no_contingencies,
    )


if __name__ == "__main__":
    main()
