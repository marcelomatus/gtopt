"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
- Post-conversion validation via gtopt_check_json
"""

import json
import logging
import re
import sys
import time
import zipfile
from pathlib import Path
from typing import Any

from plp2gtopt.excel_writer import build_plp_excel
from plp2gtopt.gtopt_writer import GTOptWriter
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


def _log_stats(planning: dict, elapsed: float) -> None:
    """Print conversion statistics using styled terminal tables."""
    from gtopt_check_json._terminal import (  # noqa: PLC0415
        print_kv_table,
        print_section,
    )

    sys_data = planning.get("system", {})
    sim = planning.get("simulation", {})
    opts = planning.get("options", {})

    print_section("Conversion Results")

    print_kv_table(
        [
            ("System name", sys_data.get("name", "(unnamed)")),
            ("System version", sys_data.get("version", "")),
        ],
        title="System",
    )

    # Element counts (skip zero-count for cleaner output)
    all_elems = [
        ("Buses", len(sys_data.get("bus_array", []))),
        ("Generators", len(sys_data.get("generator_array", []))),
        ("Generator profiles", len(sys_data.get("generator_profile_array", []))),
        ("Demands", len(sys_data.get("demand_array", []))),
        ("Demand profiles", len(sys_data.get("demand_profile_array", []))),
        ("Lines", len(sys_data.get("line_array", []))),
        ("Batteries", len(sys_data.get("battery_array", []))),
        ("Converters", len(sys_data.get("converter_array", []))),
        ("Reserve zones", len(sys_data.get("reserve_zone_array", []))),
        ("Reserve provisions", len(sys_data.get("reserve_provision_array", []))),
        ("Junctions", len(sys_data.get("junction_array", []))),
        ("Waterways", len(sys_data.get("waterway_array", []))),
        ("Flows", len(sys_data.get("flow_array", []))),
        ("Reservoirs", len(sys_data.get("reservoir_array", []))),
        ("Filtrations", len(sys_data.get("filtration_array", []))),
        ("Turbines", len(sys_data.get("turbine_array", []))),
    ]
    elem_pairs = [(k, str(v)) for k, v in all_elems if v > 0]
    if not elem_pairs:
        elem_pairs = [(k, str(v)) for k, v in all_elems]
    print_kv_table(elem_pairs, title="Elements")

    print_kv_table(
        [
            ("Blocks", str(len(sim.get("block_array", [])))),
            ("Stages", str(len(sim.get("stage_array", [])))),
            ("Scenarios", str(len(sim.get("scenario_array", [])))),
        ],
        title="Simulation",
    )

    print_kv_table(
        [
            ("use_kirchhoff", str(opts.get("use_kirchhoff", False))),
            ("use_single_bus", str(opts.get("use_single_bus", False))),
            ("scale_objective", str(opts.get("scale_objective", 1000))),
            ("demand_fail_cost", str(opts.get("demand_fail_cost", 0))),
            ("input_directory", str(opts.get("input_directory", "(default)"))),
            ("output_directory", str(opts.get("output_directory", "(default)"))),
            ("output_format", str(opts.get("output_format", "csv"))),
        ],
        title="Options",
    )

    print_kv_table([("Elapsed", f"{elapsed:.3f}s")], title="Conversion Time")


def _plp_element_counts(parser: PLPParser) -> dict[str, int]:
    """Extract PLP element counts from the parser for comparison."""
    pdata = parser.parsed_data
    counts: dict[str, int] = {}

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
    aflce_parser = pdata.get("aflce_parser")
    if aflce_parser and aflce_parser.items:
        counts["hydrologies"] = aflce_parser.items[0].get("num_hydrologies", 0)

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

    # --- Total generation capacity from plpcnfce.dat ---
    central_parser = pd.get("central_parser")
    total_cap = 0.0
    hydro_cap = 0.0
    thermal_cap = 0.0
    if central_parser:
        hydro_types = {"embalse", "serie", "pasada"}
        for central in central_parser.centrals:
            ctype = str(central.get("type", "")).lower()
            if ctype == "falla":
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
    line_parser = pd.get("line_parser")
    total_line_cap = 0.0
    if line_parser:
        for line in getattr(line_parser, "lines", []):
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
            # Compute total energy
            for b_idx in range(num_blocks):
                blk = block_parser.get_item_by_number(b_idx + 1)
                duration = blk.get("duration", 1.0) if blk else 1.0
                total_energy += block_totals[b_idx] * duration

    first_blk_dem = block_totals[0] if block_totals else 0.0
    last_blk_dem = block_totals[-1] if block_totals else 0.0

    indicators["first_block_demand_mw"] = first_blk_dem
    indicators["last_block_demand_mw"] = last_blk_dem
    indicators["total_energy_mwh"] = total_energy

    # --- Accumulated affluent from plpaflce.dat ---
    aflce_parser = pd.get("aflce_parser")
    first_afl = 0.0
    last_afl = 0.0
    if aflce_parser:
        for flow in aflce_parser.flows:
            flow_data = flow.get("flow")  # numpy array (num_blocks, num_hydro)
            block_arr = flow.get("block")  # numpy array of block numbers
            if flow_data is None or block_arr is None or len(block_arr) == 0:
                continue
            num_hydro = flow.get("num_hydrologies", 1)
            if num_hydro <= 0:
                continue
            if hydrology_indices is not None:
                # Average across only the selected hydrology columns
                cols = [c for c in hydrology_indices if 0 <= c < num_hydro]
                if cols:
                    first_afl += float(flow_data[0, cols].mean())
                    last_afl += float(flow_data[-1, cols].mean())
            else:
                # Average across all hydrologies
                first_afl += float(flow_data[0].mean())
                last_afl += float(flow_data[-1].mean())

    indicators["first_block_affluent_avg"] = first_afl
    indicators["last_block_affluent_avg"] = last_afl

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
        "first_block_affluent_avg": ind.first_block_affluent_avg,
        "last_block_affluent_avg": ind.last_block_affluent_avg,
    }


def _gtopt_element_counts(planning: dict[str, Any]) -> dict[str, int]:
    """Extract gtopt element counts from the planning dict."""
    psys = planning.get("system", {})
    sim = planning.get("simulation", {})

    generators = psys.get("generator_array", [])
    counts: dict[str, int] = {
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
        raw = "✓"
    else:
        raw = f"{diff:+d}"
    padded = f"{raw:>6s}"
    if diff == 0:
        return _cc(_GREEN, padded, use_color)
    if diff < 0:
        return _cc(_YELLOW, padded, use_color)
    return _cc(_CYAN, padded, use_color)


def _log_comparison(
    plp_counts: dict[str, int],
    gtopt_counts: dict[str, int],
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
            return "[bold green]✓[/bold green]" if colr else "ok"
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
    table.add_column("Δ", justify="right", min_width=4)
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
    _row("embalse", p_embalse, g_gen_embalse or None, indent=1)
    _row("serie", p_serie, g_gen_serie or None, indent=1)
    _row("pasada", p_pasada, g_gen_pasada or None, indent=1)
    _row("termica", p_termica, g_gen_termica or None, indent=1)
    _row("bateria", p_bateria, note="→ batteries", indent=1)
    _row(
        "falla", p_falla, g_demands, note="→ gtopt demands (unserved energy)", indent=1
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
        note=(f"= pasada count ({p_pasada}) ✓" if g_gen_profiles == p_pasada else ""),
    )
    _row("demands", p_demands, g_demands, note="≈ PLP falla (unserved energy)")
    dem_delta = g_demands - p_demands
    if dem_delta != 0:
        _row("", note=f"delta {dem_delta:+d} demands with bus=0 or empty excluded")

    # separator row
    table.add_row("", "", "", "", "")

    # -- Hydro System --
    _row("Hydro System", section=True)
    _row("hydro centrals (emb+ser)", p_hydro)
    _row("turbines", p_hydro, g_turbines, note="only bus>0 with waterway")
    _row("junctions", None, g_junctions)
    _row("waterways", None, g_waterways)
    _row("flows", None, g_flows)
    _row(
        "reservoirs",
        p_embalse,
        g_reservoirs,
        note=(f"= embalse count ({p_embalse}) ✓" if g_reservoirs == p_embalse else ""),
    )
    _row(
        "stateless reservoirs",
        p_stateless_res,
        g_stateless_res,
        note="hid_indep=T → use_state_variable=False",
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
    _row("hydrologies / scenarios", p_hydrologies, g_scenarios)

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
        ind_table.add_column("Δ%", justify="right", min_width=6)

        def _ind_row(
            label: str,
            plp_val: float,
            gtopt_val: float,
            fmt: str = ".1f",
        ) -> None:
            if plp_val > 0:
                pct = (gtopt_val - plp_val) / plp_val * 100.0
                if abs(pct) < 0.5:
                    pct_s = "[bold green]✓[/bold green]" if colr else "ok"
                else:
                    tag = "bold yellow" if abs(pct) > 5 else "dim"
                    raw = f"{pct:+.0f}%"
                    pct_s = f"[{tag}]{raw}[/{tag}]" if colr else raw
            elif gtopt_val == 0.0:
                pct_s = "[bold green]✓[/bold green]" if colr else "ok"
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

        # Capacity adequacy: gen capacity / first block demand
        plp_dem1 = plp_ind.get("first_block_demand_mw", 0.0)
        gtopt_dem1 = gtopt_ind.get("first_block_demand_mw", 0.0)
        plp_cap = plp_ind.get("total_gen_capacity_mw", 0.0)
        gtopt_cap = gtopt_ind.get("total_gen_capacity_mw", 0.0)
        plp_ratio = plp_cap / plp_dem1 if plp_dem1 > 0 else float("inf")
        gtopt_ratio = gtopt_cap / gtopt_dem1 if gtopt_dem1 > 0 else float("inf")
        _ind_row("capacity adequacy", plp_ratio, gtopt_ratio, fmt=".3f")

        _ind_row(
            "first block affluent",
            plp_ind.get("first_block_affluent_avg", 0.0),
            gtopt_ind.get("first_block_affluent_avg", 0.0),
        )
        _ind_row(
            "last block affluent",
            plp_ind.get("last_block_affluent_avg", 0.0),
            gtopt_ind.get("last_block_affluent_avg", 0.0),
        )

        con.print(ind_table)


def run_post_check(
    planning: dict[str, Any],
    parser: PLPParser,
    output_dir: str | Path | None = None,
) -> None:
    """Run gtopt_check_json validation on the generated planning dict.

    Prints system statistics, a PLP-vs-gtopt element comparison, and
    runs basic validation checks.  Skips gracefully if gtopt_check_json
    is not importable.

    Parameters
    ----------
    planning
        The planning dict produced by GTOptWriter.
    parser
        The PLPParser instance with parsed PLP data.
    output_dir
        Absolute path to the case output directory.  Passed through to
        :func:`_gtopt_indicators` so that FieldSched file references
        can be resolved from Parquet/CSV files on disk.
    """
    base_dir = str(output_dir) if output_dir is not None else None

    # Derive the active hydrology indices from the PLP case data directly
    # (idsim_parser or aflce_parser), then verify consistency with what
    # the gtopt conversion selected as scenarios.
    hydrology_indices = _plp_active_hydrology_indices(parser)

    # Cross-check: the gtopt scenario_array should reference the same set
    sim = planning.get("simulation", {})
    scenarios = sim.get("scenario_array", [])
    if hydrology_indices is not None and scenarios:
        gtopt_hydrology_indices = sorted(
            s.get("hydrology") for s in scenarios if s.get("hydrology") is not None
        )
        plp_hydrology_indices = sorted(hydrology_indices)
        if gtopt_hydrology_indices != plp_hydrology_indices:
            logger.warning(
                "PLP active hydrologies %s differ from gtopt scenarios %s",
                plp_hydrology_indices,
                gtopt_hydrology_indices,
            )

    # --- PLP vs gtopt comparison (always available) ---
    plp_counts = _plp_element_counts(parser)
    gtopt_counts = _gtopt_element_counts(planning)
    plp_ind = _plp_indicators(parser, hydrology_indices=hydrology_indices)
    gtopt_ind = _gtopt_indicators(planning, base_dir=base_dir)
    _log_comparison(plp_counts, gtopt_counts, plp_ind, gtopt_ind)

    # --- gtopt_check_json integration (optional) ---
    try:
        from gtopt_check_json._checks import (  # noqa: PLC0415
            run_all_checks,
            Severity,
        )
        from gtopt_check_json._terminal import (  # noqa: PLC0415
            print_finding as _pf,
            print_status,
            print_summary,
        )
    except ImportError:
        logger.debug("gtopt_check_json not available; skipping JSON validation checks")
        return

    # Run validation checks (all non-AI checks)
    findings = run_all_checks(planning, enabled_checks=None, ai_options=None)

    if not findings:
        print_status("All checks passed — no issues found.", ok=True)
        return

    critical_count = 0
    warning_count = 0
    note_count = 0
    for finding in findings:
        _pf(finding.severity.name, finding.check_id, finding.message)
        if finding.severity == Severity.CRITICAL:
            critical_count += 1
        elif finding.severity == Severity.WARNING:
            warning_count += 1
        else:
            note_count += 1

    print_summary(critical_count, warning_count, note_count)


def create_zip_output(output_file: Path, output_dir: Path, zip_path: Path) -> None:
    """Create a ZIP archive containing the JSON file and all data files.

    The archive layout mirrors what gtopt_guisrv / gtopt_websrv expect:

    - ``{case_name}.json``  at the archive root
    - ``{input_directory}/{subdir}/{file}.parquet`` for data files

    Args:
        output_file: Path to the main JSON configuration file.
        output_dir:  Directory containing Parquet/CSV data files.
        zip_path:    Destination ZIP file path.
    """
    case_name = output_file.stem
    input_dir_name = output_dir.name

    logger.info("Creating ZIP archive: %s", zip_path)

    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        # Add main JSON config at archive root
        zf.write(output_file, arcname=f"{case_name}.json")

        # Add all data files under the input_directory prefix
        for data_file in sorted(output_dir.rglob("*")):
            if data_file.is_file():
                arcname = f"{input_dir_name}/{data_file.relative_to(output_dir)}"
                zf.write(data_file, arcname=arcname)

    logger.info(
        "ZIP archive written: %s (%d bytes)",
        zip_path,
        zip_path.stat().st_size,
    )


def validate_plp_case(options: dict[str, Any]) -> bool:
    """Validate PLP input files without writing any output.

    Parses all PLP files, builds the planning dict in memory, and reports
    element counts.  Returns True if the case is valid, False if errors
    were encountered.

    Args:
        options: Conversion options dict (same keys as convert_plp_case).

    Returns:
        True if the PLP case is valid, False otherwise.
    """
    input_dir = Path(options.get("input_dir", "input"))
    if not input_dir.exists():
        logger.error("Input directory does not exist: '%s'", input_dir)
        return False

    try:
        logger.info("Validating PLP input files from: %s", input_dir)
        parser = PLPParser(options)
        parser.parse_all()

        writer = GTOptWriter(parser)
        planning = writer.to_json(options)

        _log_stats(planning, 0.0)
        logger.info("Validation passed.")
        return True
    except (RuntimeError, FileNotFoundError, ValueError, OSError) as exc:
        logger.error("Validation failed: %s", exc)
        return False


def convert_plp_case(options: dict[str, Any]) -> None:
    """Convert PLP input files to GTOPT format.

    Args:
        options: Conversion options dict with keys:
            input_dir, output_dir, output_file, last_stage, last_time,
            compression, hydrologies, probability_factors, discount_rate,
            management_factor, zip_output (optional, default False),
            excel_output (optional, default False),
            excel_file (optional, defaults to output_file with .xlsx suffix),
            run_check (optional, default True) — run post-conversion
            validation via gtopt_check_json.

    Raises:
        RuntimeError: If any step of the conversion fails.
    """
    input_dir = Path(options.get("input_dir", "input"))
    if not input_dir.exists():
        raise RuntimeError(
            f"PLP to GTOPT conversion failed. "
            f"Details: Input directory does not exist: '{input_dir}'"
        )

    excel_output = options.get("excel_output", False)
    do_check = options.get("run_check", True)

    try:
        t0 = time.monotonic()

        # Parse all files
        logger.info("Parsing PLP input files from: %s", input_dir)
        parser = PLPParser(options)
        parser.parse_all()

        # Convert to GTOPT format (writes Parquet time-series to output_dir)
        writer = GTOptWriter(parser)
        output_dir = Path(options.get("output_dir", "output"))

        if excel_output:
            # Excel mode: build planning dict (writes Parquet to output_dir),
            # then produce the Excel workbook.  The JSON is NOT written.
            logger.info("Building planning data for Excel output...")
            planning = writer.to_json(options)

            excel_file = options.get("excel_file")
            if excel_file is None:
                # Default: place .xlsx next to output_dir (its parent) with
                # output_dir.name as the stem.  E.g. output_dir=/tmp/mycase
                # → excel_file=/tmp/mycase.xlsx
                excel_file = output_dir.parent / (output_dir.name + ".xlsx")
            else:
                excel_file = Path(excel_file)

            logger.info("Writing igtopt Excel workbook to: %s", excel_file)
            build_plp_excel(planning, output_dir, excel_file, options)
        else:
            # Normal mode: write JSON + Parquet
            logger.info("Writing GTOPT output to: %s", options["output_file"])
            writer.write(options)

        elapsed = time.monotonic() - t0

        # Log conversion statistics
        _log_stats(writer.planning, elapsed)

        if excel_output:
            logger.info(
                "Conversion successful! Excel workbook written to %s", excel_file
            )
        else:
            output_file = Path(options["output_file"])
            logger.info("Conversion successful! Output written to %s", output_file)

            # Optionally create a ZIP archive (JSON+Parquet mode only)
            if options.get("zip_output", False):
                zip_path = output_file.with_suffix(".zip")
                create_zip_output(output_file, output_dir, zip_path)
                print(f"ZIP archive created: {zip_path}")

        # Post-conversion validation
        if do_check:
            run_post_check(writer.planning, parser, output_dir=output_dir)

    except RuntimeError:
        raise
    except FileNotFoundError as e:
        raise RuntimeError(
            f"PLP to GTOPT conversion failed. Details: Required file not found: {e}"
        ) from e
    except ValueError as e:
        raise RuntimeError(
            f"PLP to GTOPT conversion failed. Details: Invalid data format: {e}"
        ) from e
    except Exception as e:
        raise RuntimeError(f"PLP to GTOPT conversion failed. Details: {e}") from e


def generate_variable_scales_template(options: dict[str, Any]) -> str:
    """Generate a pre-computed variable_scales JSON template from PLP case data.

    Parses the PLP case (same initial steps as convert_plp_case) and builds
    a JSON array of VariableScale objects for reservoirs and batteries.

    For each reservoir, the volume scale is computed from FEscala
    (``10^(FEscala - 6)``), falling back to the Escala field from
    plpcnfce.dat if FEscala is not available.

    For each battery/ESS, energy_scale defaults to 0.01.

    Informational fields prefixed with ``_`` (``_name``, ``_fescala``) are
    included as comments; gtopt ignores unknown fields starting with ``_``.

    Args:
        options: Conversion options dict (same keys as convert_plp_case).

    Returns:
        Pretty-printed JSON string of the variable_scales array.

    Raises:
        RuntimeError: If parsing or conversion fails.
    """
    input_dir = Path(options.get("input_dir", "input"))
    if not input_dir.exists():
        raise RuntimeError(f"Input directory does not exist: '{input_dir}'")

    # Parse PLP files
    parser = PLPParser(options)
    parser.parse_all()

    # Build planning dict to get reservoir/battery arrays with UIDs
    writer = GTOptWriter(parser)
    planning = writer.to_json(options)

    scales: list[dict[str, Any]] = []

    # --- Reservoir volume scales ---
    planos = parser.parsed_data.get("planos_parser")
    fescala_map: dict[str, int] = {}
    if planos is not None:
        fescala_map = planos.reservoir_fescala

    central_parser = parser.parsed_data.get("central_parser")
    central_vol_scale: dict[str, float] = {}
    if central_parser is not None:
        for central in central_parser.centrals:
            if central.get("type") == "embalse" and "vol_scale" in central:
                central_vol_scale[str(central["name"])] = central["vol_scale"]

    reservoirs = planning.get("system", {}).get("reservoir_array", [])
    for rsv in reservoirs:
        name = rsv["name"]
        uid = rsv["uid"]
        scale: float | None = None
        fescala_val: int | None = None

        # Priority 1: FEscala from plpplem1.dat
        fescala = fescala_map.get(name)
        if fescala is not None:
            scale = 10.0 ** (fescala - 6)
            fescala_val = fescala
        else:
            # Fallback: Escala from plpcnfce.dat (already divided by 1e6)
            cvs = central_vol_scale.get(name)
            if cvs is not None:
                scale = cvs

        entry: dict[str, Any] = {
            "class_name": "Reservoir",
            "variable": "volume",
            "uid": uid,
            "scale": scale if scale is not None else 1.0,
            "_name": name,
        }
        if fescala_val is not None:
            entry["_fescala"] = fescala_val
        scales.append(entry)

    # --- Battery energy scales ---
    batteries = planning.get("system", {}).get("battery_array", [])
    for bat in batteries:
        name = bat["name"]
        uid = bat["uid"]
        scales.append(
            {
                "class_name": "Battery",
                "variable": "energy",
                "uid": uid,
                "scale": 0.01,
                "_name": name,
            }
        )

    return json.dumps(scales, indent=2, ensure_ascii=False)


def print_variable_scales_template(options: dict[str, Any]) -> int:
    """Print a pre-computed variable_scales JSON template to stdout.

    Calls :func:`generate_variable_scales_template` and prints the result.

    Args:
        options: Conversion options dict (same keys as convert_plp_case).

    Returns:
        0 on success, 1 on error.
    """
    try:
        output = generate_variable_scales_template(options)
        print(output)
        return 0
    except (RuntimeError, FileNotFoundError, ValueError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
