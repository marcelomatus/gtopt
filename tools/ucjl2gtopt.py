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
  block and per-generator ``Reserve eligibility``: spinning-reserve
  ``Amount (MW)`` → ``urreq``, ``Shortfall penalty ($/MW)`` → ``urcost``.
* **Price-sensitive loads** (``Price-sensitive loads``):
  ``{Bus, Revenue ($/MW), Demand (MW)}`` → extra ``Demand`` with
  ``lmax = Demand (MW)`` and per-element ``fcost = Revenue``.
  gtopt's ``demand_fail_cost`` resolution honors per-Demand ``fcost``
  (source/demand_lp.cpp:100), so this entry's curtailment cost is the
  marginal revenue earned by serving the load — the LP serves up to
  ``Demand`` only when ``Revenue`` exceeds the marginal cost of
  generation, matching UC.jl's elastic-demand semantics.
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
  ``Discharge cost ($/MW)`` → ``gcost``.  gtopt's
  ``System::expand_batteries()`` auto-instantiates the discharge
  Generator / charge Demand / linking Converter at LP-build time.
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

* Profiled / renewable generators with time-varying capacity
  (``generator_profile_array`` would be the gtopt-side target).
* Non-spinning reserves (UC.jl ``Type != "Spinning"``).
* Contingencies (N-1 outage constraints, would need pre-computed
  LODFs + one UserConstraint per (monitored line × outage)).
* AC-specific fields: ``Emergency flow limit`` per line is not mapped
  (gtopt is DC-only).  UC.jl's per-line ``Flow limit penalty`` is not
  mapped — gtopt uses ``demand_fail_cost`` via Power balance penalty
  instead.
* Storage charge cost (``Charge cost ($/MW)``), per-block min charge /
  discharge rates, last-period max level, ``Loss factor``, and
  ``Allow simultaneous charging and discharging`` — see the inline
  comment at the Storage block for the dropped fields.
"""

import argparse
import json
import os


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


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
    """Compute ``pmin, pmax, gcost, (pmax_segments, heat_rate_segments)``.

    UC.jl piecewise breakpoints are scalar lists; v0.3 profiled gens wrap
    each breakpoint in a time series.  We collapse to hour 0 for the
    static fields, then build a heat-rate-style segmentation that gtopt
    consumes via ``Commitment.pmax_segments`` + ``heat_rate_segments``
    when the curve has >=3 breakpoints (else fall back to single slope).

    Returned ``gcost`` is the cost slope between the first and second
    breakpoint — gtopt's per-unit gcost is overridden by the per-segment
    heat-rate cost when both segment arrays are present.
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

    return pmin, pmax, slope0, pmax_segments, heat_rate_segments


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
    # TEP / Profiled flat-cost schema.  ``Minimum power (MW)`` is
    # forced to 0: the gtopt invariant is "pmin lives on Commitment,
    # not on Generator" — ``CommitmentLP::add_to_lp`` reads
    # ``gen_pmin = lp.get_col_lowb(gcol)`` and then resets
    # ``gcol.lowb = 0`` (source/commitment_lp.cpp:347-352), migrating
    # the floor onto a C2 row gated by the binary status ``u`` so it
    # only binds when the unit is committed.  Profiled / renewable
    # generators have NO Commitment in our converter, so emitting
    # ``Generator.pmin > 0`` would leave the static col lower bound
    # in place and force dispatch ≥ pmin every block regardless of
    # whether the unit is "running" — that's the wrong semantics for
    # renewable curtailment (and breaks TEP candidate units where
    # ``capainst = 0`` forces dispatch = 0 when not built).
    pmax = float(gdata.get("Maximum power (MW)", 0.0))
    gcost = float(gdata.get("Cost ($/MW)", 0.0))
    return 0.0, pmax, gcost, [], []


# ---------------------------------------------------------------------------
# Main conversion
# ---------------------------------------------------------------------------


def convert(  # pylint: disable=too-many-locals,too-many-statements,too-many-branches
    ucjl_path, output_path=None, relax_commitment=True, copperplate=False
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
        # ``Revenue`` is collapsed to its first-hour value: gtopt's
        # ``Demand.fcost`` is ``OptTRealFieldSched`` (per-stage, not
        # per-block), so a UC.jl per-hour revenue list can't be
        # honored cell-by-cell.  ``Demand (MW)`` itself can be
        # time-varying (lmax accepts a per-block list).
        revenue_scalar = _first_hour(revenue) if isinstance(revenue, list) else revenue
        # Skip entries where both demand and revenue are zero — UC.jl
        # base_with_storage ships a degenerate ps3 with {Revenue: 0,
        # Demand: 0} that would add a no-op Demand otherwise.
        demand_max = (
            max(_time_series(demand_mw, T))
            if isinstance(demand_mw, list)
            else float(demand_mw)
        )
        if revenue_scalar == 0.0 and demand_max == 0.0:
            continue
        demand_uid += 1
        ps_ts = _time_series(demand_mw, T)
        demand_array.append(
            {
                "uid": demand_uid,
                "name": psname,
                "bus": name_to_uid[bus_name],
                "lmax": [ps_ts],
                "fcost": float(revenue_scalar),
            }
        )

    # --- Reserve zones -------------------------------------------------------
    # UC.jl: ``Reserves = {r1: {Type, Amount (MW), Shortfall penalty ($/MW)}}``.
    # We map only ``Type == "Spinning"`` zones onto ReserveZone.urreq /
    # ReserveZone.urcost.  Other types (Non-spinning, Flexiramp, ...) are
    # silently dropped — extending the mapping is a follow-up scope.
    uc_reserves = data.get("Reserves", {}) or {}
    reserve_zone_array = []
    reserve_zone_name_to_uid: dict[str, int] = {}
    for zname, zdata in uc_reserves.items():
        if str(zdata.get("Type", "Spinning")).lower() != "spinning":
            continue
        zuid = len(reserve_zone_array) + 1
        reserve_zone_name_to_uid[zname] = zuid
        # ``Amount (MW)`` may be a scalar OR a per-hour list (UC.jl v0.3
        # case118-initcond ships a length-T time series).  Always emit
        # the 2-D ``[stage][block]`` shape that ``OptTBRealFieldSched``
        # broadcasts cleanly across the LP build (the per-block lookup
        # at source/reserve_zone_lp.cpp:40 would otherwise skip blocks
        # with no explicit value).
        amount = zdata.get("Amount (MW)", 0.0)
        urreq_ts = _time_series(amount, T)
        reserve_zone_array.append(
            {
                "uid": zuid,
                "name": zname,
                "urreq": [urreq_ts],
                "urcost": float(zdata.get("Shortfall penalty ($/MW)", 1000.0)),
            }
        )

    # --- Generators + Commitments + ReserveProvisions ------------------------
    uc_gens = data.get("Generators", {})
    generator_array = []
    commitment_array = []
    reserve_provision_array = []
    gen_uid = 0
    commitment_uid = 0
    provision_uid = 0

    for gname, gdata in uc_gens.items():
        bus_name = gdata.get("Bus")
        if bus_name not in name_to_uid:
            continue
        bus_uid = name_to_uid[bus_name]
        gtype = str(gdata.get("Type", "Thermal")).lower()

        pmin, pmax, gcost, pmax_segs, hr_segs = _gen_bounds(gdata)
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

        gen_uid += 1
        gen_entry = {
            "uid": gen_uid,
            "name": gname,
            "bus": bus_uid,
            "pmin": round(pmin, 6),
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

        # Commitment is only meaningful for thermal generators.
        if gtype != "thermal":
            continue

        commitment_uid += 1
        c_entry = {
            "uid": commitment_uid,
            "name": f"{gname}_uc",
            "generator": gname,
            "relax": bool(relax_commitment),
        }

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
        # ``OptTBRealFieldSched`` ``[[v_1, v_2, ..., v_T]]`` with the
        # convention ``-1.0 = no pin`` (any value outside ``[0, 1]``).
        fixed_status_raw = gdata.get("Commitment status")
        if fixed_status_raw is not None:
            mapped = []
            for v in fixed_status_raw:
                if v is True:
                    mapped.append(1.0)
                elif v is False:
                    mapped.append(0.0)
                else:
                    mapped.append(-1.0)  # null / unset → no pin sentinel
            # Broadcast to T blocks if the list is shorter; truncate if longer.
            if len(mapped) < T:
                mapped += [-1.0] * (T - len(mapped))
            elif len(mapped) > T:
                mapped = mapped[:T]
            c_entry["fixed_status"] = [mapped]  # outer dim = 1 stage

        # Initial status (h): +N online, -N offline.
        init_u, init_h = _initial_status_to_uc(gdata.get("Initial status (h)"))
        if init_u is not None:
            c_entry["initial_status"] = init_u
            c_entry["initial_hours"] = init_h

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
        if pmax_segs and hr_segs:
            c_entry["pmax_segments"] = pmax_segs
            c_entry["heat_rate_segments"] = hr_segs

        commitment_array.append(c_entry)

        # ReserveProvision: one entry per generator listing the zones it
        # is eligible to serve.  UC.jl's ``Reserve eligibility`` is a list
        # of zone names; we resolve them against the spinning-zone map
        # built above and silently drop unknown / non-spinning zones.
        eligible = gdata.get("Reserve eligibility") or []
        zones = [z for z in eligible if z in reserve_zone_name_to_uid]
        if zones:
            provision_uid += 1
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
            reserve_provision_array.append(
                {
                    "uid": provision_uid,
                    "name": f"{gname}_rp",
                    "generator": gname,
                    "reserve_zones": zones,
                    # ``urmax`` is ``OptTBRealFieldSched`` (2-D [stage][block]).
                    # Match the explicit shape used for ``urreq`` and
                    # ``Demand.lmax`` to avoid silent scalar-broadcast
                    # variance across FieldSched call sites.
                    "urmax": [[float(pmax)] * T],
                    "ur_provision_factor": 1.0,
                }
            )

    # --- Storage units (BESS) ------------------------------------------------
    # UC.jl ``Storage units`` map onto gtopt's unified ``Battery`` element:
    # System::expand_batteries() auto-generates the discharge Generator,
    # charge Demand, and linking Converter from a single ``Battery`` entry.
    # Per-block time-series fields (``Maximum level (MWh)`` as a list of T
    # values) round-trip naturally — gtopt's ``OptTRealFieldSched`` /
    # ``OptTBRealFieldSched`` accept both scalar and list shapes.
    #
    # Dropped (silently) — no native gtopt mapping:
    #   * ``Minimum charge rate (MW)`` / ``Minimum discharge rate (MW)`` —
    #     gtopt's pmax_charge / pmax_discharge are upper bounds only.
    #   * ``Charge cost ($/MW)`` — gtopt charges via the auto-generated
    #     Demand's load cost, which the unified Battery doesn't expose
    #     as a direct field; rolled into the round-trip ``gcost``
    #     (discharge cost) approximation below.
    #   * ``Last period maximum level`` — gtopt has efin (min) but not efmax.
    #   * ``Allow simultaneous charging and discharging`` — gtopt's
    #     formulation disallows simultaneous flows by construction.
    #   * ``Loss factor`` — gtopt's ``annual_loss`` is in different units
    #     (annual %/year vs UC.jl's per-block factor); deferred.
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
        # Field mappings — all are ``OptTRealFieldSched`` (per-stage,
        # not per-block) on gtopt's Battery, so per-block UC.jl lists
        # (e.g. ``case14-storage.json``'s ``su3`` ships per-hour lists
        # for emin/emax/pmax_charge/.../efficiency) are collapsed to
        # the first-hour scalar.  A clean per-block Battery LP would
        # need ``OptTBRealFieldSched`` upgrades on the gtopt side
        # (left as a follow-up — see the module docstring's
        # "Not yet mapped" list).
        for ucjl_key, gtopt_key in (
            ("Maximum level (MWh)", "emax"),
            ("Minimum level (MWh)", "emin"),
            ("Maximum charge rate (MW)", "pmax_charge"),
            ("Maximum discharge rate (MW)", "pmax_discharge"),
            ("Charge efficiency", "input_efficiency"),
            ("Discharge efficiency", "output_efficiency"),
        ):
            value = sdata.get(ucjl_key)
            if value is None:
                continue
            b_entry[gtopt_key] = (
                float(_first_hour(value)) if isinstance(value, list) else value
            )
        # Scalars (not field schedules).
        for ucjl_key, gtopt_key in (
            ("Initial level (MWh)", "eini"),
            ("Last period minimum level (MWh)", "efin"),
        ):
            value = sdata.get(ucjl_key)
            if value is not None:
                b_entry[gtopt_key] = float(value)
        # Round-trip cost approximation: UC.jl bills charging and
        # discharging separately, gtopt's unified Battery only exposes
        # ``gcost`` (discharge).  Use it for the discharge side; the
        # charge side is implicit in the energy-balance constraint.
        # ``Discharge cost ($/MW)`` may be a per-hour list (case14-storage)
        # — collapse to the first hour since ``Battery.gcost`` is
        # ``OptTRealFieldSched`` (per-stage, NOT per-block).
        d_cost = sdata.get("Discharge cost ($/MW)")
        if d_cost is not None:
            b_entry["gcost"] = float(_first_hour(d_cost))
        # Track installed capacity = max SoC so expansion / capacity-fail
        # bounds line up.
        emax_val = sdata.get("Maximum level (MWh)")
        if emax_val is not None:
            scalar_emax = (
                _first_hour(emax_val) if isinstance(emax_val, list) else emax_val
            )
            b_entry["capacity"] = float(scalar_emax)
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

        tmax_raw = ldata.get("Normal flow limit (MW)")
        if tmax_raw is None:
            tmax = 99999.0
        elif isinstance(tmax_raw, list):
            tmax = min(float(_first_hour(tmax_raw)), 99999.0)
        else:
            tmax = min(float(tmax_raw), 99999.0)

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

    output = {
        "options": {
            "method": "monolithic",
            "annual_discount_rate": 0.0,
            "output_format": "csv",
            "output_compression": "uncompressed",
            "use_single_bus": bool(copperplate),
            "use_kirchhoff": not bool(copperplate),
            "demand_fail_cost": power_balance_penalty,
            "scale_objective": 1.0,  # Match UC.jl absolute $ values.
        },
        "simulation": {
            "block_array": block_array,
            "stage_array": stage_array,
            "scenario_array": scenario_array,
        },
        "system": {
            "name": os.path.splitext(os.path.basename(ucjl_path))[0],
            "bus_array": bus_array,
            "generator_array": generator_array,
            "demand_array": demand_array,
            "line_array": line_array,
            "commitment_array": commitment_array,
            "reserve_zone_array": reserve_zone_array,
            "reserve_provision_array": reserve_provision_array,
            "battery_array": battery_array,
            "user_constraint_array": user_constraint_array,
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
    args = parser.parse_args()

    convert(
        args.input,
        args.output,
        relax_commitment=not args.mip,
        copperplate=args.copperplate,
    )


if __name__ == "__main__":
    main()
