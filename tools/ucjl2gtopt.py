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
* **relax: true** by default so the LP relaxation solves with any LP
  backend (HiGHS / CLP / CPLEX).  Drop ``--relax`` from the CLI to emit
  MIP commitment (requires a MIP solver loaded at run time).

Not yet mapped (scoped out, will silently no-op when present):

* Profiled / renewable generators with time-varying capacity.
* Non-spinning reserves (UC.jl ``Type != "Spinning"``).
* Price-sensitive loads, contingencies, AC fields (``Susceptance``,
  emergency limits).  UC.jl's per-line ``Flow limit penalty`` is not
  mapped — gtopt uses ``demand_fail_cost`` via Power balance penalty
  instead.
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
    T = int(t_h)
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
    block_array = [{"uid": t + 1, "duration": 1} for t in range(T)]

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
        urreq = float(zdata.get("Amount (MW)", 0.0))
        # ``urreq`` is parsed as ``OptTBRealFieldSched`` — a 2-D
        # ``[stage][block]`` schedule.  A bare scalar is NOT broadcast
        # in ``ReserveZoneLP::add_requirement`` (source/reserve_zone_lp.cpp:40
        # calls ``optval(stage_uid, block_uid)`` and skips blocks with no
        # explicit value).  Emit the same shape as ``Demand.lmax``.
        reserve_zone_array.append(
            {
                "uid": zuid,
                "name": zname,
                "urreq": [[urreq] * T],
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

        curve_mw = gdata.get("Production cost curve (MW)", [0, 100])
        curve_cost = gdata.get("Production cost curve ($)", [0, 1000])
        pmin, pmax, gcost, pmax_segs, hr_segs = _compute_pieces(curve_mw, curve_cost)

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
        if gtype not in ("thermal", ""):
            gen_entry["type"] = gtype
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

    # --- Lines ---------------------------------------------------------------
    uc_lines = data.get("Transmission lines", {})
    line_array = []
    line_uid = 0
    for lname, ldata in uc_lines.items():
        src = ldata.get("Source bus")
        tgt = ldata.get("Target bus")
        if src not in name_to_uid or tgt not in name_to_uid:
            continue
        x = ldata.get("Reactance (ohms)", 0.0)
        if x is None or float(x) <= 0:
            continue

        tmax_raw = ldata.get("Normal flow limit (MW)")
        if tmax_raw is None:
            tmax = 99999.0
        elif isinstance(tmax_raw, list):
            tmax = min(float(_first_hour(tmax_raw)), 99999.0)
        else:
            tmax = min(float(tmax_raw), 99999.0)

        line_uid += 1
        line_array.append(
            {
                "uid": line_uid,
                "name": lname,
                "bus_a": name_to_uid[src],
                "bus_b": name_to_uid[tgt],
                "reactance": round(float(x), 6),
                "tmax_ab": round(tmax, 1),
                "tmax_ba": round(tmax, 1),
            }
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
