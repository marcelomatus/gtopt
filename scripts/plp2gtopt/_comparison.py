"""PLP vs gtopt comparison and indicator logic.

Handles:
- Extracting element counts from PLP parser and gtopt planning dicts
- Computing aggregate indicators (capacity, demand, energy, affluent)
- Formatting side-by-side comparison tables for terminal display
"""

import logging
import re
import sys
from typing import Any

from plp2gtopt.plp_parser import PLPParser

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# ANSI colour helpers (kept for _log_comparison backward compat + tests)
# ---------------------------------------------------------------------------
_BOLD = "\033[1m"
_DIM = "\033[2m"
_RED = "\033[91m"
_GREEN = "\033[92m"
_YELLOW = "\033[93m"
_CYAN = "\033[96m"
_MAGENTA = "\033[95m"
_RESET = "\033[0m"


def _use_color() -> bool:
    """Return True when stderr is connected to an interactive terminal."""
    return hasattr(sys.stderr, "isatty") and sys.stderr.isatty()


def _vis_len(text: str) -> int:
    """Visible length of *text*, excluding ANSI escape sequences."""
    return len(re.sub(r"\033\[[0-9;]*m", "", text))


def _plp_element_counts(parser: PLPParser) -> dict[str, Any]:
    """Extract PLP element counts from the parser for comparison."""
    pdata = parser.parsed_data
    counts: dict[str, Any] = {}

    bus_parser = pdata.get("bus_parser")
    if bus_parser:
        counts["buses"] = getattr(bus_parser, "num_buses", 0)

    central_parser = pdata.get("central_parser")
    if central_parser:
        counts["centrals"] = getattr(central_parser, "num_centrals", 0)
        for ctype, clist in central_parser.centrals_of_type.items():
            counts[f"sub_{ctype}"] = len(clist)

    demand_parser = pdata.get("demand_parser")
    if demand_parser:
        counts["demands"] = getattr(demand_parser, "num_demands", 0)

    line_parser = pdata.get("line_parser")
    if line_parser:
        counts["lines"] = getattr(line_parser, "num_lines", 0)

    battery_parser = pdata.get("battery_parser")
    if battery_parser:
        counts["batteries"] = len(getattr(battery_parser, "batteries", []))

    ess_parser = pdata.get("ess_parser")
    if ess_parser:
        counts["ess"] = len(getattr(ess_parser, "items", []))

    block_parser = pdata.get("block_parser")
    if block_parser:
        counts["blocks"] = getattr(block_parser, "num_blocks", 0)

    stage_parser = pdata.get("stage_parser")
    if stage_parser:
        counts["stages"] = getattr(stage_parser, "num_stages", 0)

    # Hydrology count: use active simulations from plpidsim.dat when
    # available (matches process_scenarios "all" logic), otherwise fall
    # back to raw hydrology columns in plpaflce.dat.
    aflce_parser = pdata.get("aflce_parser")
    active_hydros = _plp_active_hydrology_indices(parser)
    if active_hydros is not None:
        counts["hydrologies"] = len(active_hydros)
    elif aflce_parser and aflce_parser.items:
        counts["hydrologies"] = aflce_parser.items[0].get("num_hydrologies", 0)
    if aflce_parser and aflce_parser.items:
        hydro_types = {"embalse", "serie", "pasada"}
        hydro_affluent_names: list[str] = []
        for flow in aflce_parser.flows:
            name = flow.get("name", "")
            if central_parser:
                cdata = central_parser.get_central_by_name(name)
                if not cdata or cdata.get("type", "") not in hydro_types:
                    continue
                # Skip isolated pasada centrals (bus<=0, no electrical output)
                if cdata.get("type") == "pasada" and cdata.get("bus", 0) <= 0:
                    continue
                hydro_affluent_names.append(name)
            else:
                hydro_affluent_names.append(name)
        counts["affluents"] = len(hydro_affluent_names)
        counts["_affluent_names"] = sorted(hydro_affluent_names)

    # ReservoirSeepages from plpfilemb.dat (primary) or plpcenfi.dat (legacy)
    filemb_parser = pdata.get("filemb_parser")
    cenfi_parser = pdata.get("cenfi_parser")
    if filemb_parser:
        counts["seepages"] = getattr(filemb_parser, "num_seepages", 0)
    elif cenfi_parser:
        counts["seepages"] = getattr(cenfi_parser, "num_seepages", 0)

    # Reservoir efficiencies from plpcenre.dat
    cenre_parser = pdata.get("cenre_parser")
    if cenre_parser:
        counts["reservoir_efficiencies"] = getattr(cenre_parser, "num_efficiencies", 0)

    # Reservoir discharge limits from plplajam.dat
    ralco_parser = pdata.get("ralco_parser")
    if ralco_parser:
        counts["discharge_limits"] = getattr(
            ralco_parser, "num_reservoir_discharge_limits", 0
        )

    # Simulation-independent hydrology centrals (PLP ``Hid_Indep=T``,
    # read as ``EstocFIndep`` in leecnfce.f:283).  ONLY changes which
    # stochastic class each aperture reads in the backward pass
    # (plp-fasedual.f:605-620: ApertInd2 instead of ApertInd); it does
    # NOT remove inter-stage storage — these reservoirs keep full state
    # in both PLP and gtopt.  Informational count.
    if central_parser:
        embalses = central_parser.centrals_of_type.get("embalse", [])
        counts["hid_indep_centrals"] = sum(
            1 for c in embalses if c.get("hid_indep", False)
        )

    return counts


def _plp_active_hydrology_indices(parser: PLPParser) -> list[int] | None:
    """Return the 0-based hydrology column indices that PLP uses to run.

    The selection mirrors the logic in
    :meth:`~.gtopt_writer.GTOptWriter.process_scenarios` with ``spec="all"``:

    * When ``plpidsim.dat`` is present, collects the unique hydrology
      classes from the stage-1 mappings (preserving simulation order).
    * When absent, uses all hydrology columns ``0..N-1`` from
      ``plpaflce.dat``.
    * Returns ``None`` when neither data source is available (no
      filtering needed).
    """
    pd = parser.parsed_data

    idsim_parser = pd.get("idsim_parser")
    if idsim_parser is not None and idsim_parser.num_simulations > 0:
        indices_0based: list[int] = []
        seen: set[int] = set()
        for sim_idx in range(idsim_parser.num_simulations):
            hydro_idx = idsim_parser.get_index(sim_idx, 1)  # 1-based hydrology
            if hydro_idx is not None and hydro_idx not in seen:
                seen.add(hydro_idx)
                indices_0based.append(hydro_idx - 1)  # convert to 0-based
        return indices_0based if indices_0based else None

    aflce_parser = pd.get("aflce_parser")
    if aflce_parser and aflce_parser.items:
        num_hydro = aflce_parser.items[0].get("num_hydrologies", 0)
        if num_hydro > 0:
            return list(range(num_hydro))

    return None


def _plp_indicators(
    parser: PLPParser,
    hydrology_indices: list[int] | None = None,
    gtopt_planning: dict[str, Any] | None = None,
) -> dict[str, float]:
    """Compute aggregate PLP indicators from parsed data for comparison.

    Returns a dict with keys matching those from :func:`_gtopt_indicators`:

    * ``total_gen_capacity_mw`` — sum of ``pmax`` across all non-failure
      centrals in ``plpcnfce.dat``.  Failure centrals are identified by
      ``type == "falla"``.
    * ``first_block_demand_mw`` — total system demand at the first block.
    * ``last_block_demand_mw`` — total system demand at the last block.
    * ``total_energy_mwh`` — Σ (demand × duration) across all blocks.
    * ``first_block_affluent_avg`` — average (across selected hydrologies)
      total affluent at the first block from ``plpaflce.dat``.
    * ``last_block_affluent_avg`` — same for the last block.

    Parameters
    ----------
    parser
        The PLPParser instance with parsed PLP data.
    hydrology_indices
        Optional list of 0-based hydrology column indices to average
        across when computing affluent indicators.  When ``None``, all
        hydrologies in ``plpaflce.dat`` are used.  Pass the selected
        scenario hydrology indices to match the gtopt conversion.
    """
    pd = parser.parsed_data
    indicators: dict[str, float] = {}

    # --- Build set of central names with bus > 0 for filtering ---
    central_parser = pd.get("central_parser")
    _bus_gt0_names: set[str] = set()
    if central_parser:
        for central in central_parser.centrals:
            if central.get("bus", 0) > 0:
                _bus_gt0_names.add(str(central.get("name", "")))

    # --- Symmetric bucketing: prefer the gtopt JSON's type field over PLP's ---
    # The gtopt converter reclassifies PLP centrals (e.g. small distributed
    # ROR → ``renewable:hydro``, solar/wind via name patterns, BAT_*_LOAD
    # absorbed into batteries).  When a planning dict is supplied we use
    # that name-keyed type map so the PLP-side hydro / thermal / renewable
    # buckets tie out against gtopt's :func:`compute_indicators` by
    # construction.  ``absorbed_names`` lists PLP centrals that gtopt's
    # converter folded into a Battery (twin ``<name>_LOAD`` pmax=0 entries)
    # — these must be dropped from gen_min_stable so PLP's catch-all
    # ``termica`` sum doesn't double-count battery-charge ghosts.
    name_to_gtopt_type: dict[str, str] = {}
    name_to_gtopt_pmin: dict[str, Any] = {}
    res_name_to_gtopt_emin: dict[str, Any] = {}
    absorbed_names: set[str] = set()
    if gtopt_planning is not None:
        psys = gtopt_planning.get("system", {})
        for g in psys.get("generator_array", []):
            gname = str(g.get("name", ""))
            name_to_gtopt_type[gname] = str(g.get("type", ""))
            name_to_gtopt_pmin[gname] = g.get("pmin")
        for r in psys.get("reservoir_array", []):
            rname = str(r.get("name", ""))
            res_name_to_gtopt_emin[rname] = r.get("emin")
        for b in psys.get("battery_array", []):
            bname = str(b.get("name", ""))
            absorbed_names.add(bname)
            absorbed_names.add(bname + "_LOAD")

    # --- Total generation capacity from plpcnfce.dat (bus > 0 only) ---
    # Exclude "falla" (failure generators) to match gtopt.  Batteries
    # (type "bateria") are included because gtopt counts their discharge
    # capacity via battery_array.pmax_discharge.  When a planning dict is
    # supplied, use the gtopt-side type classifier for hydro / thermal /
    # renewable bucketing so the sub-totals tie out exactly.
    from .tech_classify import classify_type  # noqa: PLC0415

    total_cap = 0.0
    hydro_cap = 0.0
    thermal_cap = 0.0
    renewable_cap = 0.0
    if central_parser:
        hydro_types = {"embalse", "serie", "pasada"}
        for central in central_parser.centrals:
            ctype = str(central.get("type", "")).lower()
            if ctype == "falla":
                continue
            if central.get("bus", 0) <= 0:
                continue
            pmax = central.get("pmax", 0.0)
            if not isinstance(pmax, (int, float)):
                continue
            pmw = float(pmax)
            total_cap += pmw
            cname = str(central.get("name", ""))
            if cname in name_to_gtopt_type:
                # Authoritative: classify by gtopt's emitted type
                category = classify_type(name_to_gtopt_type[cname])
            elif ctype in hydro_types:
                category = "hydro"
            elif ctype == "termica":
                category = "thermal"
            else:
                category = "other"
            if category == "hydro":
                hydro_cap += pmw
            elif category == "thermal":
                thermal_cap += pmw
            elif category == "renewable":
                renewable_cap += pmw
    indicators["total_gen_capacity_mw"] = total_cap
    indicators["hydro_capacity_mw"] = hydro_cap
    indicators["thermal_capacity_mw"] = thermal_cap
    indicators["renewable_capacity_mw"] = renewable_cap

    # Read receipts for input columns that no other indicator reflects:
    #   * plpcnfce PotMin → Generator.pmin (gen min-stable level)
    #   * plpcnfce Vmin   → Reservoir.emin (minimum storage volume)
    # Without these, a PotMin / Vmin column silently read as zero would be
    # invisible in the comparison.  Both tie out against the gtopt side's
    # Tier-5 aggregates (compute_indicators), which sum the same JSON fields.
    # ``absorbed_names`` filters out PLP BAT_*_LOAD twins absorbed by the
    # battery converter — their large negative pmin (charge max) is already
    # represented on the gtopt side as Battery.pmax_charge.  For centrals
    # that gtopt does emit, prefer the emitted pmin so the sum mirrors
    # what the LP actually sees — ``pmin_as_flowright`` and similar soft
    # constraints zero the generator pmin field and re-express the floor
    # as a FlowRight, so summing PLP raw pmin would over-count.
    gen_min_stable = 0.0
    if central_parser:
        for central in central_parser.centrals:
            if str(central.get("type", "")).lower() == "falla":
                continue
            if central.get("bus", 0) <= 0:
                continue
            cname = str(central.get("name", ""))
            if cname in absorbed_names:
                continue
            if cname in name_to_gtopt_pmin:
                gpmin = name_to_gtopt_pmin[cname]
                if isinstance(gpmin, (int, float)):
                    gen_min_stable += float(gpmin)
                # else: profile / Parquet reference → 0 (mirrors gtopt
                # _info.py:_first_scalar which yields None for refs).
            else:
                pmin = central.get("pmin", 0.0)
                if isinstance(pmin, (int, float)):
                    gen_min_stable += float(pmin)
    indicators["total_gen_min_stable_mw"] = gen_min_stable

    # Reservoir min vol: when gtopt has the reservoir but its emin field
    # is a string profile / Parquet reference (time-varying min vol from
    # plpminembh.dat), gtopt's ``compute_indicators`` uses ``_first_scalar``
    # which yields None for refs and silently excludes the reservoir from
    # the sum.  Mirror that here so the row ties out — skip the PLP
    # central's scalar emin when its gtopt twin has a non-scalar emin.
    reservoir_min_vol = 0.0
    if central_parser:
        for central in central_parser.centrals:
            emin = central.get("emin")
            if not isinstance(emin, (int, float)):
                continue
            rname = str(central.get("name", ""))
            if rname in res_name_to_gtopt_emin:
                gemin = res_name_to_gtopt_emin[rname]
                if not isinstance(gemin, (int, float)):
                    continue  # profile ref → mirror gtopt's silent skip
            reservoir_min_vol += float(emin)
    indicators["total_reservoir_min_vol"] = reservoir_min_vol

    # --- Total line capacity from plplin.dat ---
    # Only include lines whose both endpoints are bus > 0 (matching
    # line_writer filtering) AND that do NOT have maintenance schedules
    # (plpmanli.dat).  Lines with maintenance become FieldSched file
    # references in the gtopt JSON; compute_indicators skips those
    # string values, so we must exclude them here too for a fair match.
    line_parser = pd.get("line_parser")
    manli_parser = pd.get("manli_parser")
    _manli_names: set[str] = set()
    if manli_parser:
        for m in getattr(manli_parser, "manlis", []):
            mname = m.get("name", "")
            if mname:
                _manli_names.add(mname)
    total_line_cap = 0.0
    if line_parser:
        for line in getattr(line_parser, "lines", []):
            bus_a = line.get("bus_a", 0)
            bus_b = line.get("bus_b", 0)
            if bus_a <= 0 or bus_b <= 0 or bus_a == bus_b:
                continue
            line_name = line.get("name", "")
            if line_name in _manli_names:
                continue  # maintenance → Parquet ref; skipped by compute_indicators
            tmax_ab = line.get("tmax_ab", 0.0)
            tmax_ba = line.get("tmax_ba", 0.0)
            if isinstance(tmax_ab, (int, float)):
                total_line_cap += float(tmax_ab)
            if isinstance(tmax_ba, (int, float)):
                total_line_cap += float(tmax_ba)
    indicators["total_line_capacity_mw"] = total_line_cap

    # --- Total demand per block from plpdem.dat ---
    demand_parser = pd.get("demand_parser")
    block_parser = pd.get("block_parser")

    total_energy = 0.0
    total_hours = 0.0
    has_demand = False
    block_totals: list[float] = []

    if demand_parser and block_parser:
        num_blocks = getattr(block_parser, "num_blocks", 0)
        block_totals = [0.0] * num_blocks

        for dem in demand_parser.demands:
            blocks = dem.get("blocks")
            values = dem.get("values")
            if blocks is not None and values is not None:
                for i, blk_num in enumerate(blocks):
                    if i >= len(values):
                        break
                    idx = int(blk_num) - 1  # block numbers are 1-based
                    if 0 <= idx < num_blocks:
                        block_totals[idx] += float(values[i])
                        has_demand = True

        if has_demand and num_blocks > 0:
            # Compute total energy and total hours
            for b_idx in range(num_blocks):
                blk = block_parser.get_item_by_number(b_idx + 1)
                duration = blk.get("duration", 1.0) if blk else 1.0
                total_energy += block_totals[b_idx] * duration
                total_hours += duration

    first_blk_dem = block_totals[0] if block_totals else 0.0
    last_blk_dem = block_totals[-1] if block_totals else 0.0

    # Avg annual energy: total_energy × 8760 / total_hours
    _HOURS_PER_YEAR = 8760.0
    avg_annual_energy = (
        total_energy * _HOURS_PER_YEAR / total_hours if total_hours > 0 else 0.0
    )

    indicators["first_block_demand_mw"] = first_blk_dem
    indicators["last_block_demand_mw"] = last_blk_dem
    indicators["total_energy_mwh"] = total_energy
    indicators["avg_annual_energy_mwh"] = avg_annual_energy

    # --- Accumulated affluent from plpaflce.dat ---
    # Only include centrals that also exist in plpcnfce.dat (central_parser).
    # Flows in plpaflce.dat without a matching central definition are orphan
    # data and should be ignored — they have no uid for Parquet output.
    # Conversion factor: 1 m³/s flowing for 1 h = 0.0036 Hm³
    _M3S_TO_HM3_PER_H = 0.0036
    aflce_parser = pd.get("aflce_parser")
    central_parser = pd.get("central_parser")
    first_afl = 0.0
    last_afl = 0.0
    total_water_vol_hm3 = 0.0
    num_active_flows = 0
    if aflce_parser and block_parser:
        import numpy as np  # noqa: PLC0415

        for flow in aflce_parser.flows:
            # Skip flows that have no central definition in plpcnfce.dat
            flow_name = flow.get("name", "")
            fill_value: float | None = None
            if central_parser:
                cdata = central_parser.get_central_by_name(flow_name)
                if cdata is None:
                    continue
                # Skip isolated pasada centrals (bus<=0, no electrical output)
                if cdata.get("type") == "pasada" and cdata.get("bus", 0) <= 0:
                    continue
                fill_value = cdata.get("afluent")
            flow_data = flow.get("flow")  # numpy array (num_blocks, num_hydro)
            block_arr = flow.get("block")  # numpy array of block numbers
            if flow_data is None or block_arr is None or len(block_arr) == 0:
                continue
            num_hydro = flow.get("num_hydrologies", 1)
            if num_hydro <= 0:
                continue
            # Pre-compute valid hydrology columns outside the block loop
            if hydrology_indices is not None:
                valid_cols = [c for c in hydrology_indices if 0 <= c < num_hydro]
                if not valid_cols:
                    continue
            else:
                valid_cols = None

            # Skip flows whose active-hydrology data all equals the fill
            # value — these are excluded from the Parquet by AflceWriter's
            # global pre-filter and should not inflate the flow count.
            if fill_value is not None and flow_data.ndim == 2:
                cols = flow_data[:, valid_cols] if valid_cols is not None else flow_data
                if np.allclose(cols, fill_value, rtol=1e-8, atol=1e-11):
                    continue

            num_active_flows += 1

            # Vectorized: compute mean flow per block across hydrologies
            if valid_cols is not None:
                avg_flows = flow_data[:, valid_cols].mean(axis=1)
            else:
                avg_flows = flow_data.mean(axis=1)

            # Build durations array for all blocks
            durations = np.array(
                [
                    (block_parser.get_item_by_number(int(bn)) or {}).get(
                        "duration", 1.0
                    )
                    for bn in block_arr
                ],
                dtype=np.float64,
            )
            total_water_vol_hm3 += float(
                (avg_flows * durations).sum() * _M3S_TO_HM3_PER_H
            )
            first_afl += float(avg_flows[0])
            if len(avg_flows) > 0:
                last_afl += float(avg_flows[-1])

    # Average flow per affluent (m³/s) = total_volume / total_time / num_flows
    avg_flow_m3s = (
        total_water_vol_hm3 / (total_hours * _M3S_TO_HM3_PER_H) / num_active_flows
        if total_hours > 0 and num_active_flows > 0
        else 0.0
    )

    indicators["first_block_affluent_avg"] = first_afl
    indicators["last_block_affluent_avg"] = last_afl
    indicators["total_water_volume_hm3"] = total_water_vol_hm3
    indicators["avg_flow_m3s"] = avg_flow_m3s

    # --- Average failure cost from falla centrals (min gcost per bus) ---
    # Single source of truth: CentralParser.avg_falla_cost().  The same
    # value is used by gtopt_writer.py as the default for the JSON's
    # model_options.demand_fail_cost so the diagnostic row shown in this
    # table matches what gtopt's LP actually prices curtailment at.
    if central_parser:
        indicators["avg_fcost"] = central_parser.avg_falla_cost()

    return indicators


def _gtopt_indicators(
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> dict[str, float]:
    """Compute aggregate gtopt indicators from the planning dict.

    Delegates to :func:`gtopt_check_json._info.compute_indicators` which
    computes all indicators (capacity, demand, energy, affluent, capacity
    breakdown by type, line capacity) from the gtopt planning dict.

    Parameters
    ----------
    planning
        The planning dict produced by GTOptWriter.
    base_dir
        Absolute path to the case output directory so that FieldSched
        file references (e.g. ``"lmax"``) can be resolved from
        Parquet/CSV files on disk.
    """
    from gtopt_check_json._info import compute_indicators  # noqa: PLC0415

    ind = compute_indicators(planning, base_dir=base_dir)
    return {
        "total_gen_capacity_mw": ind.total_gen_capacity_mw,
        "hydro_capacity_mw": ind.hydro_capacity_mw,
        "thermal_capacity_mw": ind.thermal_capacity_mw,
        "renewable_capacity_mw": ind.renewable_capacity_mw,
        "total_line_capacity_mw": ind.total_line_capacity_mw,
        "first_block_demand_mw": ind.first_block_demand_mw,
        "last_block_demand_mw": ind.last_block_demand_mw,
        "total_energy_mwh": ind.total_energy_mwh,
        "avg_annual_energy_mwh": ind.avg_annual_energy_mwh,
        "first_block_affluent_avg": ind.first_block_affluent_avg,
        "last_block_affluent_avg": ind.last_block_affluent_avg,
        "total_water_volume_hm3": ind.total_water_volume_hm3,
        "avg_flow_m3s": ind.avg_flow_m3s,
        "avg_fcost": ind.avg_fcost,
        # Tier-5 input-file read receipts (shared compute_indicators).
        "total_gen_min_stable_mw": ind.total_gen_min_stable_mw,
        "total_reservoir_min_vol": ind.total_reservoir_min_vol,
    }


def _gtopt_element_counts(planning: dict[str, Any]) -> dict[str, Any]:
    """Extract gtopt element counts from the planning dict."""
    psys = planning.get("system", {})
    sim = planning.get("simulation", {})

    generators = psys.get("generator_array", [])
    counts: dict[str, Any] = {
        "buses": len(psys.get("bus_array", [])),
        "generators": len(generators),
        # Accept both legacy and unified profile shapes — C++ folds them
        # together at parse time, so for comparison purposes the totals
        # are what matters.  Counted as a single bucket.
        "capacity_profiles": (
            len(psys.get("capacity_profile_array", []))
            + len(psys.get("generator_profile_array", []))
            + len(psys.get("demand_profile_array", []))
        ),
        "demands": len(psys.get("demand_array", [])),
        "lines": len(psys.get("line_array", [])),
        "batteries": len(psys.get("battery_array", [])),
        "converters": len(psys.get("converter_array", [])),
        "junctions": len(psys.get("junction_array", [])),
        "waterways": len(psys.get("waterway_array", [])),
        "flows": len(psys.get("flow_array", [])),
        "reservoirs": len(psys.get("reservoir_array", [])),
        "reservoir_efficiencies": (
            len(psys.get("reservoir_production_factor_array", []))
            + sum(
                len(r.get("production_factor", []))
                for r in psys.get("reservoir_array", [])
            )
        ),
        "seepages": (
            len(psys.get("reservoir_seepage_array", []))
            + sum(len(r.get("seepage", [])) for r in psys.get("reservoir_array", []))
        ),
        "discharge_limits": (
            len(psys.get("reservoir_discharge_limit_array", []))
            + sum(
                len(r.get("discharge_limit", []))
                for r in psys.get("reservoir_array", [])
            )
        ),
        "turbines": len(psys.get("turbine_array", [])),
        "blocks": len(sim.get("block_array", [])),
        "stages": len(sim.get("stage_array", [])),
        "scenarios": sum(
            1 for s in sim.get("scenario_array", []) if "input_directory" not in s
        ),
    }

    # Generator count by type attribute
    type_counts: dict[str, int] = {}
    for gen in generators:
        gtype = str(gen.get("type", "unknown")).lower()
        type_counts[gtype] = type_counts.get(gtype, 0) + 1
    for gtype, gcount in type_counts.items():
        counts[f"gen_{gtype}"] = gcount

    # Stateless reservoirs: entries without an inter-stage state
    # variable — either ``use_state_variable=False`` or
    # ``daily_cycle=True`` (the C++ ``StorageOptions`` forces
    # ``use_state_variable=False`` whenever ``daily_cycle`` is true).
    # Since the ``hid_indep`` → ``daily_cycle`` mapping was removed
    # (PLP's ``EstocFIndep`` never touches storage dynamics), only
    # deliberate daily-cycle devices remain here (e.g.
    # ``--ror-as-reservoirs`` promotions).
    reservoirs = psys.get("reservoir_array", [])
    counts["stateless_reservoirs"] = sum(
        1
        for r in reservoirs
        if r.get("use_state_variable") is False or r.get("daily_cycle") is True
    )

    # Flow + capacity-profile names for comparison with PLP affluents
    # (embalse/serie → Flow objects; pasada → CapacityProfile objects).
    # Accept both the unified `capacity_profile_array` and the legacy
    # `generator_profile_array` for backward compatibility with older
    # planning.json files.
    flow_names = [f.get("name", "") for f in psys.get("flow_array", [])]
    profile_names = [
        p.get("name", "")
        for p in psys.get("capacity_profile_array", [])
        + psys.get("generator_profile_array", [])
    ]
    counts["_flow_names"] = sorted(flow_names + profile_names)

    return counts


# ---------------------------------------------------------------------------
# Formatted comparison table
# ---------------------------------------------------------------------------


def _cc(code: str, text: str, use_color: bool) -> str:
    """Wrap *text* in ANSI *code* when colour is enabled."""
    return f"{code}{text}{_RESET}" if use_color else str(text)


def _delta_str(
    plp_val: int | None,
    gtopt_val: int | None,
    use_color: bool,
) -> str:
    """Return a 6-char right-aligned delta string with optional colour."""
    if plp_val is None or gtopt_val is None:
        return " " * 6
    diff = gtopt_val - plp_val
    if diff == 0:
        raw = "\u2713"
    else:
        raw = f"{diff:+d}"
    padded = f"{raw:>6s}"
    if diff == 0:
        return _cc(_GREEN, padded, use_color)
    if diff < 0:
        return _cc(_YELLOW, padded, use_color)
    return _cc(_CYAN, padded, use_color)


def _extract_flow_central_names(planning: dict[str, Any]) -> set[str] | None:
    """Extract the set of flow names from gtopt ``flow_array``.

    In plp2gtopt output the flow name equals the PLP central name.

    Returns ``None`` when ``flow_array`` is empty.
    """
    flows = planning.get("system", {}).get("flow_array", [])
    if not flows:
        return None
    names: set[str] = set()
    for fl in flows:
        name = fl.get("name")
        if isinstance(name, str) and name:
            names.add(name)
    return names if names else None


def compute_comparison_indicators(
    parser: PLPParser,
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> tuple[dict[str, float], dict[str, float]]:
    """Compute matched PLP and gtopt indicators for comparison.

    Extracts hydrology indices from the planning dict's
    ``scenario_array`` and ``plp_central`` names from ``flow_array``
    to ensure the PLP indicators are computed over exactly the same
    subset of data that was converted to gtopt.

    Parameters
    ----------
    parser
        The PLPParser instance with parsed PLP data.
    planning
        The planning dict produced by the conversion.
    base_dir
        Absolute path to the case output directory so that FieldSched
        file references can be resolved from Parquet/CSV files on disk.

    Returns
    -------
    tuple[dict[str, float], dict[str, float]]
        ``(plp_indicators, gtopt_indicators)`` — both dicts use the
        same key names for direct comparison.
    """
    # Hydrology indices from the converted scenarios
    sim = planning.get("simulation", {})
    scenarios = sim.get("scenario_array", [])
    hydrology_indices: list[int] | None = (
        sorted(s["hydrology"] for s in scenarios if s.get("hydrology") is not None)
        or None
    )

    # NOTE: _plp_indicators now includes ALL centrals from aflce_parser
    # (no bus>0 or flow_central_names filtering) because AflceWriter writes
    # all aflce centrals to discharge.parquet, and compute_indicators sums
    # all uid columns from that Parquet file.
    plp_ind = _plp_indicators(
        parser,
        hydrology_indices=hydrology_indices,
        gtopt_planning=planning,
    )
    gtopt_ind = _gtopt_indicators(planning, base_dir=base_dir)
    return plp_ind, gtopt_ind


# Every PLP input ``.dat`` file the converter reads, mapped to the report
# indicator / element-count that reflects it — the "did we read it" ledger.
# Each row's linked indicator is non-zero only when the file was parsed, so a
# zero in the comparison flags a missing / empty input.  Keep in sync with the
# *_parser.py read-sites.  Columns: (file, what it carries, linked indicator).
_INPUT_FILE_INDICATORS: tuple[tuple[str, str, str], ...] = (
    # -- topology / time --
    ("plpbar.dat", "buses", "buses (#)"),
    ("plpcnfli.dat", "lines (reactance, rating)", "lines (#) / line capacity (MW)"),
    ("plpblo.dat", "block durations", "blocks (#)"),
    ("plpeta.dat", "stages", "stages (#)"),
    ("indhor.csv", "block↔hour mapping", "blocks (#)"),
    # -- generation & cost --
    (
        "plpcnfce.dat",
        "centrals: PotMax/PotMin, Vmin/Vmax, falla",
        "gen capacity, gen min-stable, reservoirs, batteries",
    ),
    ("plpcosce.dat", "marginal fuel cost", "generators (#) (gcost; --info avg gcost)"),
    ("plpmance.dat", "gen pmin/pmax maintenance profile", "capacity profiles (#)"),
    ("plpcenpmax.dat", "volume-dependent turbine pmax", "turbines (#) / gen capacity"),
    # -- demand --
    ("plpdem.dat", "bus demand profiles", "first/last/peak demand, total energy"),
    ("plpextrac.dat", "bus external extractions", "demand (net load; --info)"),
    ("plpmat.dat", "failure / spill cost params", "avg failure cost ($/MWh)"),
    # -- network maintenance --
    ("plpmanli.dat", "line maintenance windows", "lines (#) / line capacity"),
    # -- hydro topology & water --
    (
        "plpaflce.dat",
        "inflows (hydrology scenarios)",
        "first block flow / total water vol",
    ),
    (
        "plpmanem.dat",
        "reservoir emin/emax profile",
        "reservoir min vol (Σ) / reservoirs",
    ),
    ("plpminembh.dat", "soft per-stage min volume", "reservoir min vol (Σ)"),
    ("plpcenre.dat", "reservoir production factor", "reservoir efficiencies (#)"),
    (
        "plpcenpmax.dat",
        "volume-dependent Pmax/head curve",
        "reservoir efficiencies (#) [+ to plpcenre]",
    ),
    ("plpcenfi.dat / plpfilemb.dat", "reservoir seepage/filtration", "seepages (#)"),
    ("plpralco.dat", "volume→max-discharge curve", "discharge limits (#)"),
    ("plpvrebemb.dat", "spill threshold & cost (efin)", "reservoirs (#)"),
    (
        "plplajam.dat / plpmaulen.dat",
        "Laja/Maule water rights",
        "flows / waterways (#)",
    ),
    # -- storage --
    ("plpess.dat / plpcenbat.dat", "battery emax / efficiency", "batteries (#)"),
    ("plpmanbat.dat / plpmaness.dat", "battery maintenance", "batteries (#)"),
    # -- SDDP / scenarios --
    ("plpidsim.dat", "sim→hydrology mapping", "scenarios (#)"),
    (
        "plpidape.dat / plpidap2.dat",
        "SDDP aperture indices",
        "scenarios (#) / apertures",
    ),
    ("plpplaem1/2.dat", "future-cost (FCF) cuts", "boundary_cuts.csv (--info)"),
    ("plpcnfgnl.dat", "LNG terminals", "generators (#) (--expand-lng)"),
)


def _log_input_files() -> None:
    """Print the PLP input-file ledger: every ``.dat`` the converter reads,
    what it carries, and the report indicator that proves it was read.

    Mirrors the plexos2gtopt ledger: each file maps to an indicator or element
    count that is non-zero only when the file was parsed, so a zero there flags
    a missing / empty input.
    """
    from gtopt_check_json._terminal import (  # noqa: PLC0415
        console,
        print_section,
        table_width,
    )
    from rich.box import ASCII, ROUNDED  # noqa: PLC0415
    from rich.table import Table  # noqa: PLC0415

    con = console()
    colr = con.is_terminal

    print_section("PLP Input Files → Indicators")
    table = Table(
        box=ROUNDED if colr else ASCII,
        show_lines=False,
        padding=(0, 1),
        width=table_width(),
    )
    table.add_column("Input file", no_wrap=True, min_width=26)
    table.add_column("Carries", min_width=24)
    table.add_column("Linked indicator", style="dim")
    for fname, carries, indicator in _INPUT_FILE_INDICATORS:
        table.add_row(fname, carries, indicator)
    con.print(table)
    con.print(
        "[dim]A zero / blank in a linked indicator means that file was not "
        "read (missing or empty in the case).[/dim]"
        if colr
        else "Note: a zero/blank linked indicator means the file was not read."
    )


def _log_comparison(
    plp_counts: dict[str, Any],
    gtopt_counts: dict[str, Any],
    plp_ind: dict[str, float] | None = None,
    gtopt_ind: dict[str, float] | None = None,
    gtopt_options: dict[str, Any] | None = None,
) -> None:
    """Print a formatted side-by-side PLP vs gtopt element comparison.

    The table is grouped by category (network & generation, hydro, storage,
    loads, simulation, global indicators) and includes derived analysis rows
    such as *generators excluding falla+batería*, gtopt generators by type,
    and per-block demand/affluent indicators.

    Output goes to stderr via :mod:`rich` with automatic colour and
    Unicode/ASCII detection.
    """
    from gtopt_check_json._terminal import (  # noqa: PLC0415
        console,
        print_section,
        table_width,
    )
    from rich.table import Table  # noqa: PLC0415
    from rich.box import ASCII, ROUNDED  # noqa: PLC0415

    con = console()
    colr = con.is_terminal

    # ASCII substitutions for non-terminal / non-UTF8 environments
    _arrow = "\u2192" if colr else "->"
    _check = "\u2713" if colr else "ok"
    _ellipsis = "\u2026" if colr else "..."

    # --- extract PLP values ---
    p_buses = plp_counts.get("buses", 0)
    p_lines = plp_counts.get("lines", 0)
    p_centrals = plp_counts.get("centrals", 0)
    p_embalse = plp_counts.get("sub_embalse", 0)
    p_serie = plp_counts.get("sub_serie", 0)
    p_pasada = plp_counts.get("sub_pasada", 0)
    p_termica = plp_counts.get("sub_termica", 0)
    p_bateria = plp_counts.get("sub_bateria", 0)
    p_falla = plp_counts.get("sub_falla", 0)
    p_demands = plp_counts.get("demands", 0)
    p_batteries = plp_counts.get("batteries", 0)
    p_ess = plp_counts.get("ess", 0)
    p_blocks = plp_counts.get("blocks", 0)
    p_stages = plp_counts.get("stages", 0)
    p_hydrologies = plp_counts.get("hydrologies", 0)
    p_seepages = plp_counts.get("seepages", 0)
    p_res_eff = plp_counts.get("reservoir_efficiencies", 0)
    p_discharge_limits = plp_counts.get("discharge_limits", 0)
    p_hid_indep = plp_counts.get("hid_indep_centrals", 0)

    # derived
    p_gen_excl = p_centrals - p_falla - p_bateria
    p_hydro = p_embalse + p_serie

    # --- extract gtopt values ---
    g_buses = gtopt_counts.get("buses", 0)
    g_generators = gtopt_counts.get("generators", 0)
    g_gen_profiles = gtopt_counts.get("generator_profiles", 0)
    g_demands = gtopt_counts.get("demands", 0)
    g_lines = gtopt_counts.get("lines", 0)
    g_batteries = gtopt_counts.get("batteries", 0)
    g_junctions = gtopt_counts.get("junctions", 0)
    g_waterways = gtopt_counts.get("waterways", 0)
    g_flows = gtopt_counts.get("flows", 0)
    g_reservoirs = gtopt_counts.get("reservoirs", 0)
    g_res_eff = gtopt_counts.get("reservoir_efficiencies", 0)
    g_seepages = gtopt_counts.get("seepages", 0)
    g_discharge_limits = gtopt_counts.get("discharge_limits", 0)
    g_turbines = gtopt_counts.get("turbines", 0)
    g_blocks = gtopt_counts.get("blocks", 0)
    g_stages = gtopt_counts.get("stages", 0)
    g_scenarios = gtopt_counts.get("scenarios", 0)
    g_stateless_res = gtopt_counts.get("stateless_reservoirs", 0)

    # gtopt generator type breakdown — group new tech types back to PLP
    # categories using the shared classification map.  ``plp_category``
    # handles hierarchical ``"<top>:<sub>"`` types (e.g. ``renewable:hydro``)
    # by falling back to the prefix.
    from .tech_classify import plp_category  # noqa: PLC0415

    _plp_cat_counts: dict[str, int] = {
        "embalse": 0,
        "serie": 0,
        "pasada": 0,
        "termica": 0,
    }
    for key, val in gtopt_counts.items():
        if key.startswith("gen_"):
            tech = key[4:]
            plp_cat = plp_category(tech)
            if plp_cat and plp_cat in _plp_cat_counts:
                _plp_cat_counts[plp_cat] += val
    g_gen_embalse = _plp_cat_counts["embalse"]
    g_gen_serie = _plp_cat_counts["serie"]
    g_gen_pasada = _plp_cat_counts["pasada"]
    g_gen_termica = _plp_cat_counts["termica"]

    # --- helpers ---
    def _v(val: int | None) -> str:
        return str(val) if val is not None else ""

    def _delta(plp: int | None, gtopt: int | None, *, force_ok: bool = False) -> str:
        if plp is None or gtopt is None:
            return ""
        diff = gtopt - plp
        if diff == 0 or force_ok:
            return "[bold green]\u2713[/bold green]" if colr else "ok"
        sign = "+" if diff > 0 else ""
        style = "[bold yellow]" if diff < 0 else "[bold cyan]"
        end = "[/bold yellow]" if diff < 0 else "[/bold cyan]"
        return f"{style}{sign}{diff}{end}" if colr else f"{sign}{diff}"

    box_style = ROUNDED if con.is_terminal else ASCII

    # --- Build the comparison table ---
    print_section("PLP vs gtopt Element Comparison")

    tw = table_width()

    table = Table(
        box=box_style,
        show_lines=False,
        padding=(0, 1),
        title_justify="left",
        width=tw,
    )
    table.add_column("Element", no_wrap=True, min_width=26)
    table.add_column("PLP", justify="right", min_width=6, no_wrap=True)
    table.add_column("gtopt", justify="right", min_width=6, no_wrap=True)
    table.add_column("\u0394", justify="right", min_width=4, no_wrap=True)
    table.add_column("Notes", style="dim")

    def _row(
        label: str,
        plp: int | None = None,
        gtopt: int | None = None,
        note: str = "",
        *,
        indent: int = 0,
        section: bool = False,
        force_ok: bool = False,
    ) -> None:
        lbl = ("  " * indent) + label
        if section:
            table.add_row(f"[bold]{lbl}[/bold]" if colr else lbl, "", "", "", "")
        else:
            table.add_row(
                lbl, _v(plp), _v(gtopt), _delta(plp, gtopt, force_ok=force_ok), note
            )

    # -- Network & Generation --
    _row("Network & Generation", section=True)
    _row("buses", p_buses, g_buses)
    _row("lines", p_lines, g_lines)
    _row("centrals (total)", p_centrals)
    emb_note = ""
    emb_explained = False
    if g_gen_embalse is not None and g_gen_embalse != p_embalse:
        emb_note = (
            f"{p_embalse - g_gen_embalse} embalse with bus<=0 (dam only, no turbine)"
        )
        # The mismatch is fully explained — dam-only embalses have no
        # turbine so they are not emitted as generators; show as OK.
        emb_explained = True
    _row(
        "embalse",
        p_embalse,
        g_gen_embalse or None,
        note=emb_note,
        indent=1,
        force_ok=emb_explained,
    )
    serie_note = ""
    serie_explained = False
    if g_gen_serie is not None and g_gen_serie != p_serie:
        serie_note = "bus<=0 or no generation waterway excluded"
        serie_explained = True
    _row(
        "serie",
        p_serie,
        g_gen_serie or None,
        note=serie_note,
        indent=1,
        force_ok=serie_explained,
    )
    _row("pasada", p_pasada, g_gen_pasada or None, indent=1)
    termica_note = ""
    termica_explained = False
    if g_gen_termica is not None and g_gen_termica != p_termica:
        termica_note = "bat_LOAD phantoms and bus<=0 excluded"
        termica_explained = True
    _row(
        "termica",
        p_termica,
        g_gen_termica or None,
        note=termica_note,
        indent=1,
        force_ok=termica_explained,
    )
    _row("bateria", p_bateria, note=f"{_arrow} batteries", indent=1)
    falla_note = f"{_arrow} gtopt demands (unserved energy)"
    if g_demands != p_falla and p_falla > 0:
        falla_note += f"; bus>0: {g_demands} of {p_falla}"
    _row(
        "falla",
        p_falla,
        g_demands,
        note=falla_note,
        indent=1,
        force_ok=g_demands <= p_falla,
    )
    # gtopt auto-creates 1 auxiliary generator per battery (via converter)
    g_gen_with_bat_aux = g_generators + g_batteries
    _row("gen (excl falla+bat)", p_gen_excl, g_gen_with_bat_aux)
    gen_delta = g_gen_with_bat_aux - p_gen_excl
    if gen_delta != 0:
        _row("", note=f"delta {gen_delta:+d} centrals with bus<=0 excluded")
    if g_batteries > 0:
        _row(
            "",
            note=f"gtopt incl. {g_batteries} battery aux gen (auto-created)",
        )
    if g_gen_profiles == p_pasada:
        gp_note = f"from {p_pasada} pasada centrals"
    elif g_gen_profiles == 0 and p_pasada > 0:
        gp_note = f"pasada {_arrow} flows+turbines (not profiles)"
    else:
        gp_note = ""
    _row("generator profiles", None, g_gen_profiles, note=gp_note)
    _row(
        "bus demand profiles",
        p_demands,
        note="PLP plpdem.dat (bus load profiles)",
    )
    _row(
        "demands (fail nodes)",
        p_falla,
        g_demands,
        note=f"from falla centrals (bus>0: {g_demands} of {p_falla})",
        force_ok=g_demands <= p_falla,
    )

    # separator row
    table.add_row("", "", "", "", "")

    # -- Hydro System --
    _row("Hydro System", section=True)
    _row("hydro centrals (emb+ser)", p_hydro)
    turb_note = "only bus>0 with generation waterway"
    _row("turbines", p_hydro, g_turbines, note=turb_note)
    _row("junctions", None, g_junctions)
    _row("waterways", None, g_waterways)
    p_affluents = plp_counts.get("affluents", 0)
    p_affluent_names: list[str] = plp_counts.get("_affluent_names", [])
    g_flow_names: list[str] = gtopt_counts.get("_flow_names", [])
    g_flow_profile_count = g_flows + gtopt_counts.get("generator_profiles", 0)
    afl_note = ""
    afl_explained = False
    if p_affluents and g_flow_profile_count != p_affluents:
        afl_note = "bus<=0 hydro centrals excluded"
        afl_explained = True
    _row(
        "affluents / flows+profiles",
        p_affluents or None,
        g_flow_profile_count,
        note=afl_note,
        force_ok=afl_explained,
    )
    if p_affluents > 0 and p_affluents != g_flows:
        missing = sorted(set(p_affluent_names) - set(g_flow_names))
        extra = sorted(set(g_flow_names) - set(p_affluent_names))
        if missing:
            names = ", ".join(missing[:8])
            suffix = (
                f", {_ellipsis} (+{len(missing) - 8} more)" if len(missing) > 8 else ""
            )
            _row("", note=f"{len(missing)} PLP-only: {names}{suffix}")
        if extra:
            names = ", ".join(extra[:8])
            suffix = f", {_ellipsis} (+{len(extra) - 8} more)" if len(extra) > 8 else ""
            _row("", note=f"{len(extra)} gtopt-only: {names}{suffix}")
    _row(
        "reservoirs",
        p_embalse,
        g_reservoirs,
        note=(
            f"= embalse count ({p_embalse}) {_check}"
            if g_reservoirs == p_embalse
            else ""
        ),
    )
    # hid_indep (EstocFIndep) only switches the backward-pass aperture
    # class indexing in PLP (plp-fasedual.f:605-620); the reservoirs keep
    # their inter-stage state on both sides.  gtopt-side stateless
    # entries are deliberate daily-cycle devices (--ror-as-reservoirs).
    _row(
        "hid_indep reservoirs",
        p_hid_indep,
        g_stateless_res,
        note=f"aperture-class indexing only {_arrow} state kept "
        f"(gtopt column counts daily_cycle devices)",
        indent=1,
    )
    # The PLP column counts only plpcenre.dat (rendimiento) entries, but
    # gtopt's production factors also include plpcenpmax.dat volume-dependent
    # Pmax/head curves.  A reservoir with a Pmax curve but no rendimiento row
    # (e.g. CIPRESES) therefore shows up only on the gtopt side — an expected
    # difference, not a conversion error.
    if g_res_eff > p_res_eff:
        _pfac_note = (
            f"+{g_res_eff - p_res_eff} from plpcenpmax.dat "
            "(Pmax/head curve, no plpcenre rendimiento)"
        )
    elif p_res_eff and g_res_eff == p_res_eff:
        _pfac_note = f"= plpcenre.dat {_check}"
    else:
        _pfac_note = ""
    _row("production factors", p_res_eff, g_res_eff, note=_pfac_note, indent=1)
    _row("seepages", p_seepages, g_seepages, indent=1)
    _row("discharge limits", p_discharge_limits, g_discharge_limits, indent=1)
    table.add_row("", "", "", "", "")

    # -- Storage --
    _row("Storage", section=True)
    _row("batteries (plpcenbat)", p_batteries, g_batteries)
    if p_ess > 0:
        _row("ESS (plpess)", p_ess)
    table.add_row("", "", "", "", "")

    # -- Simulation --
    _row("Simulation", section=True)
    _row("blocks", p_blocks, g_blocks)
    _row("stages", p_stages, g_stages)
    hydro_note = ""
    if p_hydrologies != g_scenarios and g_scenarios > 0:
        hydro_note = "PLP=active from plpidsim; gtopt=selected via -y"
    _row("hydrologies / scenarios", p_hydrologies, g_scenarios, note=hydro_note)

    # -- Scaling Options --
    if gtopt_options:
        table.add_row("", "", "", "", "")
        _row("Scaling Options", section=True)
        model_opts = gtopt_options.get("model_options", {})

        def _scale_row(
            label: str, plp_default: str, gtopt_val: Any, note: str = ""
        ) -> None:
            g_str = (
                f"{gtopt_val:g}"
                if isinstance(gtopt_val, (int, float))
                else str(gtopt_val)
            )
            table.add_row(label, plp_default, g_str, "", note)

        _scale_row(
            "scale_objective",
            "1e7",
            model_opts.get("scale_objective", ""),
            note="PLP ScaleObj",
        )
        _scale_row(
            "scale_theta",
            "1e4",
            model_opts.get("scale_theta", ""),
            note="PLP ScaleAng",
        )
        _scale_row(
            "demand_fail_cost",
            "1000",
            model_opts.get("demand_fail_cost", ""),
            note="$/MWh unserved",
        )

    con.print(table)

    # --- Global indicators side-by-side ---
    if plp_ind and gtopt_ind:
        ind_table = Table(
            box=box_style,
            show_lines=False,
            padding=(0, 1),
            title="[title]Global Indicators[/title]" if colr else "Global Indicators",
            title_justify="left",
            width=tw,
        )
        ind_table.add_column("Indicator", no_wrap=True, min_width=26)
        ind_table.add_column("PLP", justify="right", min_width=8)
        ind_table.add_column("gtopt", justify="right", min_width=8)
        ind_table.add_column("\u0394%", justify="right", min_width=6)

        from gtopt_check_json._terminal import fmt_num  # noqa: PLC0415

        def _ind_row(
            label: str,
            plp_val: float,
            gtopt_val: float,
            decimals: int = 2,
        ) -> None:
            if plp_val > 0:
                pct = (gtopt_val - plp_val) / plp_val * 100.0
                if abs(pct) < 0.5:
                    pct_s = "[bold green]\u2713[/bold green]" if colr else "ok"
                else:
                    tag = "bold yellow" if abs(pct) > 5 else "dim"
                    raw = f"{pct:+.0f}%"
                    pct_s = f"[{tag}]{raw}[/{tag}]" if colr else raw
            elif gtopt_val == 0.0:
                pct_s = "[bold green]\u2713[/bold green]" if colr else "ok"
            else:
                pct_s = ""
            ind_table.add_row(
                label, fmt_num(plp_val, decimals), fmt_num(gtopt_val, decimals), pct_s
            )

        _ind_row(
            "gen capacity (MW)",
            plp_ind.get("total_gen_capacity_mw", 0.0),
            gtopt_ind.get("total_gen_capacity_mw", 0.0),
        )
        _ind_row(
            "  hydro capacity (MW)",
            plp_ind.get("hydro_capacity_mw", 0.0),
            gtopt_ind.get("hydro_capacity_mw", 0.0),
        )
        # Both PLP and gtopt indicators bucket centrals using the
        # gtopt-side type classifier (``_plp_indicators`` consults the
        # planning dict), so the hydro / thermal / renewable rows tie
        # out by construction.
        _ind_row(
            "  thermal capacity (MW)",
            plp_ind.get("thermal_capacity_mw", 0.0),
            gtopt_ind.get("thermal_capacity_mw", 0.0),
        )
        _ind_row(
            "  renewable capacity (MW)",
            plp_ind.get("renewable_capacity_mw", 0.0),
            gtopt_ind.get("renewable_capacity_mw", 0.0),
        )
        _ind_row(
            "gen min-stable (Σ MW)",
            plp_ind.get("total_gen_min_stable_mw", 0.0),
            gtopt_ind.get("total_gen_min_stable_mw", 0.0),
        )
        _ind_row(
            "line capacity (MW)",
            plp_ind.get("total_line_capacity_mw", 0.0),
            gtopt_ind.get("total_line_capacity_mw", 0.0),
        )
        _ind_row(
            "first block demand (MW)",
            plp_ind.get("first_block_demand_mw", 0.0),
            gtopt_ind.get("first_block_demand_mw", 0.0),
        )
        _ind_row(
            "last block demand (MW)",
            plp_ind.get("last_block_demand_mw", 0.0),
            gtopt_ind.get("last_block_demand_mw", 0.0),
        )

        # Convert MWh to TWh (÷ 1e6) for display
        _MWH_TO_TWH = 1e-6
        _ind_row(
            "total energy (TWh)",
            plp_ind.get("total_energy_mwh", 0.0) * _MWH_TO_TWH,
            gtopt_ind.get("total_energy_mwh", 0.0) * _MWH_TO_TWH,
            decimals=3,
        )
        _ind_row(
            "avg annual energy (TWh)",
            plp_ind.get("avg_annual_energy_mwh", 0.0) * _MWH_TO_TWH,
            gtopt_ind.get("avg_annual_energy_mwh", 0.0) * _MWH_TO_TWH,
            decimals=3,
        )

        # Capacity adequacy: gen capacity / first block demand
        plp_dem1 = plp_ind.get("first_block_demand_mw", 0.0)
        gtopt_dem1 = gtopt_ind.get("first_block_demand_mw", 0.0)
        plp_cap = plp_ind.get("total_gen_capacity_mw", 0.0)
        gtopt_cap = gtopt_ind.get("total_gen_capacity_mw", 0.0)
        plp_ratio = plp_cap / plp_dem1 if plp_dem1 > 0 else float("inf")
        gtopt_ratio = gtopt_cap / gtopt_dem1 if gtopt_dem1 > 0 else float("inf")
        _ind_row("capacity adequacy", plp_ratio, gtopt_ratio, decimals=3)

        _ind_row(
            "first block flow (m\u00b3/s)",
            plp_ind.get("first_block_affluent_avg", 0.0),
            gtopt_ind.get("first_block_affluent_avg", 0.0),
        )
        _ind_row(
            "last block flow (m\u00b3/s)",
            plp_ind.get("last_block_affluent_avg", 0.0),
            gtopt_ind.get("last_block_affluent_avg", 0.0),
        )
        _ind_row(
            "total water vol (Hm\u00b3)",
            plp_ind.get("total_water_volume_hm3", 0.0),
            gtopt_ind.get("total_water_volume_hm3", 0.0),
        )
        _ind_row(
            "reservoir min vol (\u03a3)",
            plp_ind.get("total_reservoir_min_vol", 0.0),
            gtopt_ind.get("total_reservoir_min_vol", 0.0),
        )
        _ind_row(
            "avg flow/affluent (m\u00b3/s)",
            plp_ind.get("avg_flow_m3s", 0.0),
            gtopt_ind.get("avg_flow_m3s", 0.0),
        )
        _ind_row(
            "avg failure cost ($/MWh)",
            plp_ind.get("avg_fcost", 0.0),
            gtopt_ind.get("avg_fcost", 0.0),
        )

        con.print(ind_table)

    _log_input_files()
