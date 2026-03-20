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

    # Hydrology count (number of raw hydrology columns in plpaflce.dat)
    # and affluent count/names — only embalse/serie/pasada centrals that
    # have flow data in plpaflce.dat (excludes termica, falla, bateria)
    aflce_parser = pdata.get("aflce_parser")
    if aflce_parser and aflce_parser.items:
        counts["hydrologies"] = aflce_parser.items[0].get("num_hydrologies", 0)
        hydro_types = {"embalse", "serie", "pasada"}
        hydro_affluent_names: list[str] = []
        for flow in aflce_parser.flows:
            name = flow.get("name", "")
            if central_parser:
                cdata = central_parser.get_central_by_name(name)
                if cdata and cdata.get("type", "") in hydro_types:
                    hydro_affluent_names.append(name)
            else:
                hydro_affluent_names.append(name)
        counts["affluents"] = len(hydro_affluent_names)
        counts["_affluent_names"] = sorted(hydro_affluent_names)

    # Filtrations from plpfilemb.dat (primary) or plpcenfi.dat (legacy)
    filemb_parser = pdata.get("filemb_parser")
    cenfi_parser = pdata.get("cenfi_parser")
    if filemb_parser:
        counts["filtrations"] = getattr(filemb_parser, "num_filtrations", 0)
    elif cenfi_parser:
        counts["filtrations"] = getattr(cenfi_parser, "num_filtrations", 0)

    # Reservoir efficiencies from plpcenre.dat
    cenre_parser = pdata.get("cenre_parser")
    if cenre_parser:
        counts["reservoir_efficiencies"] = getattr(cenre_parser, "num_efficiencies", 0)

    # Stateless reservoirs: embalse centrals with hid_indep=True
    if central_parser:
        embalses = central_parser.centrals_of_type.get("embalse", [])
        counts["stateless_reservoirs"] = sum(
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

    # --- Total generation capacity from plpcnfce.dat (bus > 0 only) ---
    # Exclude "falla" (failure generators) to match gtopt.  Batteries
    # (type "bateria") are included because gtopt counts their discharge
    # capacity via battery_array.pmax_discharge.
    total_cap = 0.0
    hydro_cap = 0.0
    thermal_cap = 0.0
    if central_parser:
        hydro_types = {"embalse", "serie", "pasada"}
        for central in central_parser.centrals:
            ctype = str(central.get("type", "")).lower()
            if ctype == "falla":
                continue
            if central.get("bus", 0) <= 0:
                continue
            pmax = central.get("pmax", 0.0)
            if isinstance(pmax, (int, float)):
                pmw = float(pmax)
                total_cap += pmw
                if ctype in hydro_types:
                    hydro_cap += pmw
                elif ctype == "termica":
                    thermal_cap += pmw
    indicators["total_gen_capacity_mw"] = total_cap
    indicators["hydro_capacity_mw"] = hydro_cap
    indicators["thermal_capacity_mw"] = thermal_cap

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
        for flow in aflce_parser.flows:
            # Skip flows that have no central definition in plpcnfce.dat
            flow_name = flow.get("name", "")
            if central_parser and central_parser.get_central_by_name(flow_name) is None:
                continue
            flow_data = flow.get("flow")  # numpy array (num_blocks, num_hydro)
            block_arr = flow.get("block")  # numpy array of block numbers
            if flow_data is None or block_arr is None or len(block_arr) == 0:
                continue
            num_hydro = flow.get("num_hydrologies", 1)
            if num_hydro <= 0:
                continue
            num_active_flows += 1
            # Pre-compute valid hydrology columns outside the block loop
            if hydrology_indices is not None:
                valid_cols = [c for c in hydrology_indices if 0 <= c < num_hydro]
                if not valid_cols:
                    continue
            else:
                valid_cols = None
            for b_idx, blk_num_raw in enumerate(block_arr):
                blk_num = int(blk_num_raw)
                blk = block_parser.get_item_by_number(blk_num)
                duration = blk.get("duration", 1.0) if blk else 1.0
                if valid_cols is not None:
                    avg_flow_val = float(flow_data[b_idx, valid_cols].mean())
                else:
                    avg_flow_val = float(flow_data[b_idx].mean())
                total_water_vol_hm3 += avg_flow_val * duration * _M3S_TO_HM3_PER_H
                if b_idx == 0:
                    first_afl += avg_flow_val
                if b_idx == len(block_arr) - 1:
                    last_afl += avg_flow_val

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
        "total_line_capacity_mw": ind.total_line_capacity_mw,
        "first_block_demand_mw": ind.first_block_demand_mw,
        "last_block_demand_mw": ind.last_block_demand_mw,
        "total_energy_mwh": ind.total_energy_mwh,
        "avg_annual_energy_mwh": ind.avg_annual_energy_mwh,
        "first_block_affluent_avg": ind.first_block_affluent_avg,
        "last_block_affluent_avg": ind.last_block_affluent_avg,
        "total_water_volume_hm3": ind.total_water_volume_hm3,
        "avg_flow_m3s": ind.avg_flow_m3s,
    }


def _gtopt_element_counts(planning: dict[str, Any]) -> dict[str, Any]:
    """Extract gtopt element counts from the planning dict."""
    psys = planning.get("system", {})
    sim = planning.get("simulation", {})

    generators = psys.get("generator_array", [])
    counts: dict[str, Any] = {
        "buses": len(psys.get("bus_array", [])),
        "generators": len(generators),
        "generator_profiles": len(psys.get("generator_profile_array", [])),
        "demands": len(psys.get("demand_array", [])),
        "demand_profiles": len(psys.get("demand_profile_array", [])),
        "lines": len(psys.get("line_array", [])),
        "batteries": len(psys.get("battery_array", [])),
        "converters": len(psys.get("converter_array", [])),
        "junctions": len(psys.get("junction_array", [])),
        "waterways": len(psys.get("waterway_array", [])),
        "flows": len(psys.get("flow_array", [])),
        "reservoirs": len(psys.get("reservoir_array", [])),
        "reservoir_efficiencies": len(psys.get("reservoir_efficiency_array", [])),
        "filtrations": len(psys.get("filtration_array", [])),
        "turbines": len(psys.get("turbine_array", [])),
        "blocks": len(sim.get("block_array", [])),
        "stages": len(sim.get("stage_array", [])),
        "scenarios": len(sim.get("scenario_array", [])),
    }

    # Generator count by type attribute
    type_counts: dict[str, int] = {}
    for gen in generators:
        gtype = str(gen.get("type", "unknown")).lower()
        type_counts[gtype] = type_counts.get(gtype, 0) + 1
    for gtype, gcount in type_counts.items():
        counts[f"gen_{gtype}"] = gcount

    # Stateless reservoirs: use_state_variable == False
    reservoirs = psys.get("reservoir_array", [])
    counts["stateless_reservoirs"] = sum(
        1 for r in reservoirs if r.get("use_state_variable") is False
    )

    # Flow + generator profile names for comparison with PLP affluents
    # (embalse/serie → Flow objects; pasada → GeneratorProfile objects)
    flow_names = [f.get("name", "") for f in psys.get("flow_array", [])]
    profile_names = [p.get("name", "") for p in psys.get("generator_profile_array", [])]
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
    """Extract the set of PLP central names from gtopt ``flow_array``.

    Each flow created by :class:`JunctionWriter` stores the original PLP
    central name in the ``plp_central`` field.  This function is used by
    integration tests to verify that ``plp_central`` traceability is
    present on every flow.

    Returns ``None`` when no ``plp_central`` field is found (e.g. hand-
    written JSON that predates the field).
    """
    flows = planning.get("system", {}).get("flow_array", [])
    if not flows:
        return None
    names: set[str] = set()
    for fl in flows:
        pc = fl.get("plp_central")
        if isinstance(pc, str) and pc:
            names.add(pc)
    # Only return the set if at least one flow had plp_central
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
    )
    gtopt_ind = _gtopt_indicators(planning, base_dir=base_dir)
    return plp_ind, gtopt_ind


def _log_comparison(
    plp_counts: dict[str, Any],
    gtopt_counts: dict[str, Any],
    plp_ind: dict[str, float] | None = None,
    gtopt_ind: dict[str, float] | None = None,
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
    p_filtrations = plp_counts.get("filtrations", 0)
    p_res_eff = plp_counts.get("reservoir_efficiencies", 0)
    p_stateless_res = plp_counts.get("stateless_reservoirs", 0)

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
    g_filtrations = gtopt_counts.get("filtrations", 0)
    g_turbines = gtopt_counts.get("turbines", 0)
    g_blocks = gtopt_counts.get("blocks", 0)
    g_stages = gtopt_counts.get("stages", 0)
    g_scenarios = gtopt_counts.get("scenarios", 0)
    g_stateless_res = gtopt_counts.get("stateless_reservoirs", 0)

    # gtopt generator type breakdown
    g_gen_embalse = gtopt_counts.get("gen_embalse", 0)
    g_gen_serie = gtopt_counts.get("gen_serie", 0)
    g_gen_pasada = gtopt_counts.get("gen_pasada", 0)
    g_gen_termica = gtopt_counts.get("gen_termica", 0)

    # --- helpers ---
    def _v(val: int | None) -> str:
        return str(val) if val is not None else ""

    def _delta(plp: int | None, gtopt: int | None) -> str:
        if plp is None or gtopt is None:
            return ""
        diff = gtopt - plp
        if diff == 0:
            return "[bold green]\u2713[/bold green]" if colr else "ok"
        sign = "+" if diff > 0 else ""
        style = "[bold yellow]" if diff < 0 else "[bold cyan]"
        end = "[/bold yellow]" if diff < 0 else "[/bold cyan]"
        return f"{style}{sign}{diff}{end}" if colr else f"{sign}{diff}"

    box_style = ROUNDED if con.is_terminal else ASCII

    # --- Build the comparison table ---
    print_section("PLP vs gtopt Element Comparison")

    table = Table(
        box=box_style,
        show_lines=False,
        padding=(0, 1),
        title_justify="left",
    )
    table.add_column("Element", no_wrap=True, min_width=26)
    table.add_column("PLP", justify="right", min_width=6)
    table.add_column("gtopt", justify="right", min_width=6)
    table.add_column("\u0394", justify="right", min_width=4)
    table.add_column("Notes", no_wrap=True, style="dim")

    def _row(
        label: str,
        plp: int | None = None,
        gtopt: int | None = None,
        note: str = "",
        *,
        indent: int = 0,
        section: bool = False,
    ) -> None:
        lbl = ("  " * indent) + label
        if section:
            table.add_row(f"[bold]{lbl}[/bold]" if colr else lbl, "", "", "", "")
        else:
            table.add_row(lbl, _v(plp), _v(gtopt), _delta(plp, gtopt), note)

    # -- Network & Generation --
    _row("Network & Generation", section=True)
    _row("buses", p_buses, g_buses)
    _row("lines", p_lines, g_lines)
    _row("centrals (total)", p_centrals)
    emb_note = ""
    if g_gen_embalse is not None and g_gen_embalse != p_embalse:
        emb_note = (
            f"{p_embalse - g_gen_embalse} embalse with bus<=0 (dam only, no turbine)"
        )
    _row("embalse", p_embalse, g_gen_embalse or None, note=emb_note, indent=1)
    serie_note = ""
    if g_gen_serie is not None and g_gen_serie != p_serie:
        serie_note = "bus<=0 or no generation waterway excluded"
    _row("serie", p_serie, g_gen_serie or None, note=serie_note, indent=1)
    _row("pasada", p_pasada, g_gen_pasada or None, indent=1)
    _row("termica", p_termica, g_gen_termica or None, indent=1)
    _row("bateria", p_bateria, note=f"{_arrow} batteries", indent=1)
    _row(
        "falla",
        p_falla,
        g_demands,
        note=f"{_arrow} gtopt demands (unserved energy)",
        indent=1,
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
    _row(
        "generator profiles",
        p_pasada,
        g_gen_profiles,
        note=(
            f"= pasada count ({p_pasada}) {_check}"
            if g_gen_profiles == p_pasada
            else ""
        ),
    )
    _approx = "\u2248" if colr else "~"
    _row("demands", p_demands, g_demands, note=f"{_approx} PLP falla (unserved energy)")
    dem_delta = g_demands - p_demands
    if dem_delta != 0:
        _row("", note=f"delta {dem_delta:+d} demands with bus=0 or empty excluded")

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
    _row("affluents / flows+profiles", p_affluents or None, g_flow_profile_count)
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
    _row(
        "stateless reservoirs",
        p_stateless_res,
        g_stateless_res,
        note=f"hid_indep=T {_arrow} use_state_variable=False",
        indent=1,
    )
    _row("reservoir efficiencies", p_res_eff, g_res_eff)
    _row("filtrations", p_filtrations, g_filtrations)
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
        hydro_note = "PLP=raw columns; gtopt=active from plpidsim"
    _row("hydrologies / scenarios", p_hydrologies, g_scenarios, note=hydro_note)

    con.print(table)

    # --- Global indicators side-by-side ---
    if plp_ind and gtopt_ind:
        ind_table = Table(
            box=box_style,
            show_lines=False,
            padding=(0, 1),
            title="[title]Global Indicators[/title]" if colr else "Global Indicators",
            title_justify="left",
        )
        ind_table.add_column("Indicator", no_wrap=True, min_width=26)
        ind_table.add_column("PLP", justify="right", min_width=8)
        ind_table.add_column("gtopt", justify="right", min_width=8)
        ind_table.add_column("\u0394%", justify="right", min_width=6)

        def _ind_row(
            label: str,
            plp_val: float,
            gtopt_val: float,
            fmt: str = ".1f",
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
            ind_table.add_row(label, f"{plp_val:{fmt}}", f"{gtopt_val:{fmt}}", pct_s)

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
        _ind_row(
            "  thermal capacity (MW)",
            plp_ind.get("thermal_capacity_mw", 0.0),
            gtopt_ind.get("thermal_capacity_mw", 0.0),
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
            fmt=".3f",
        )
        _ind_row(
            "avg annual energy (TWh)",
            plp_ind.get("avg_annual_energy_mwh", 0.0) * _MWH_TO_TWH,
            gtopt_ind.get("avg_annual_energy_mwh", 0.0) * _MWH_TO_TWH,
            fmt=".3f",
        )

        # Capacity adequacy: gen capacity / first block demand
        plp_dem1 = plp_ind.get("first_block_demand_mw", 0.0)
        gtopt_dem1 = gtopt_ind.get("first_block_demand_mw", 0.0)
        plp_cap = plp_ind.get("total_gen_capacity_mw", 0.0)
        gtopt_cap = gtopt_ind.get("total_gen_capacity_mw", 0.0)
        plp_ratio = plp_cap / plp_dem1 if plp_dem1 > 0 else float("inf")
        gtopt_ratio = gtopt_cap / gtopt_dem1 if gtopt_dem1 > 0 else float("inf")
        _ind_row("capacity adequacy", plp_ratio, gtopt_ratio, fmt=".3f")

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
            fmt=".1f",
        )
        _ind_row(
            "avg flow/affluent (m\u00b3/s)",
            plp_ind.get("avg_flow_m3s", 0.0),
            gtopt_ind.get("avg_flow_m3s", 0.0),
            fmt=".2f",
        )

        con.print(ind_table)
