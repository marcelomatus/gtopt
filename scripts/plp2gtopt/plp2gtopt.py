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
# ANSI colour helpers for the comparison table
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
    """Return True when the root logger streams to an interactive terminal."""
    for handler in logging.getLogger().handlers:
        stream = getattr(handler, "stream", None)
        if stream is not None and hasattr(stream, "isatty") and stream.isatty():
            return True
    return False


def _vis_len(text: str) -> int:
    """Visible length of *text*, excluding ANSI escape sequences."""
    return len(re.sub(r"\033\[[0-9;]*m", "", text))


def _log_stats(planning: dict, elapsed: float) -> None:
    """Log conversion statistics similar to gtopt_main log_pre_solve_stats."""
    sys = planning.get("system", {})
    sim = planning.get("simulation", {})
    opts = planning.get("options", {})

    logger.info("=== System statistics ===")
    logger.info("  System name     : %s", sys.get("name", "(unnamed)"))
    logger.info("  System version  : %s", sys.get("version", ""))
    logger.info("=== System elements  ===")
    logger.info("  Buses           : %d", len(sys.get("bus_array", [])))
    logger.info("  Generators      : %d", len(sys.get("generator_array", [])))
    logger.info("  Generator profs : %d", len(sys.get("generator_profile_array", [])))
    logger.info("  Demands         : %d", len(sys.get("demand_array", [])))
    logger.info("  Demand profs    : %d", len(sys.get("demand_profile_array", [])))
    logger.info("  Lines           : %d", len(sys.get("line_array", [])))
    logger.info("  Batteries       : %d", len(sys.get("battery_array", [])))
    logger.info("  Converters      : %d", len(sys.get("converter_array", [])))
    logger.info("  Reserve zones   : %d", len(sys.get("reserve_zone_array", [])))
    logger.info(
        "  Reserve provisions   : %d",
        len(sys.get("reserve_provision_array", [])),
    )
    logger.info("  Junctions       : %d", len(sys.get("junction_array", [])))
    logger.info("  Waterways       : %d", len(sys.get("waterway_array", [])))
    logger.info("  Flows           : %d", len(sys.get("flow_array", [])))
    logger.info("  Reservoirs      : %d", len(sys.get("reservoir_array", [])))
    logger.info("  Filtrations     : %d", len(sys.get("filtration_array", [])))
    logger.info("  Turbines        : %d", len(sys.get("turbine_array", [])))
    logger.info("=== Simulation statistics ===")
    logger.info("  Blocks          : %d", len(sim.get("block_array", [])))
    logger.info("  Stages          : %d", len(sim.get("stage_array", [])))
    logger.info("  Scenarios       : %d", len(sim.get("scenario_array", [])))
    logger.info("=== Key options ===")
    logger.info("  use_kirchhoff   : %s", opts.get("use_kirchhoff", False))
    logger.info("  use_single_bus  : %s", opts.get("use_single_bus", False))
    logger.info("  scale_objective : %s", opts.get("scale_objective", 1000))
    logger.info("  demand_fail_cost: %s", opts.get("demand_fail_cost", 0))
    logger.info("  input_directory : %s", opts.get("input_directory", "(default)"))
    logger.info("  output_directory: %s", opts.get("output_directory", "(default)"))
    logger.info("  output_format   : %s", opts.get("output_format", "csv"))
    logger.info("=== Conversion time ===")
    logger.info("  Elapsed         : %.3fs", elapsed)


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

    return counts


def _plp_indicators(parser: PLPParser) -> dict[str, float]:
    """Compute aggregate PLP indicators from parsed data for comparison.

    Returns a dict with keys matching those from :func:`_gtopt_indicators`:

    * ``total_gen_capacity_mw`` — sum of ``pmax`` across all non-failure
      centrals in ``plpcnfce.dat``.  Failure centrals are identified by
      ``type == "falla"``.
    * ``first_block_demand_mw`` — total system demand at the first block.
    * ``last_block_demand_mw`` — total system demand at the last block.
    * ``total_energy_mwh`` — Σ (demand × duration) across all blocks.
    * ``first_block_affluent_avg`` — average (across hydrologies) total
      affluent at the first block from ``plpaflce.dat``.
    * ``last_block_affluent_avg`` — same for the last block.
    """
    pd = parser.parsed_data
    indicators: dict[str, float] = {}

    # --- Total generation capacity from plpcnfce.dat ---
    central_parser = pd.get("central_parser")
    total_cap = 0.0
    if central_parser:
        for central in central_parser.centrals:
            ctype = str(central.get("type", "")).lower()
            if ctype == "falla":
                continue
            pmax = central.get("pmax", 0.0)
            if isinstance(pmax, (int, float)):
                total_cap += float(pmax)
    indicators["total_gen_capacity_mw"] = total_cap

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
            # First block: mean across hydrologies
            first_afl += float(flow_data[0].mean()) if num_hydro > 0 else 0.0
            # Last block: mean across hydrologies
            last_afl += float(flow_data[-1].mean()) if num_hydro > 0 else 0.0

    indicators["first_block_affluent_avg"] = first_afl
    indicators["last_block_affluent_avg"] = last_afl

    return indicators


def _gtopt_indicators(
    planning: dict[str, Any],
    base_dir: str | None = None,
) -> dict[str, float]:
    """Compute aggregate gtopt indicators from the planning dict.

    Uses :func:`gtopt_check_json._info.compute_indicators` when available,
    otherwise falls back to a simplified local computation.

    Parameters
    ----------
    planning
        The planning dict produced by GTOptWriter.
    base_dir
        Absolute path to the case output directory so that FieldSched
        file references (e.g. ``"lmax"``) can be resolved from
        Parquet/CSV files on disk.
    """
    try:
        from gtopt_check_json._info import compute_indicators  # noqa: PLC0415

        ind = compute_indicators(planning, base_dir=base_dir)
        return {
            "total_gen_capacity_mw": ind.total_gen_capacity_mw,
            "first_block_demand_mw": ind.first_block_demand_mw,
            "last_block_demand_mw": ind.last_block_demand_mw,
            "total_energy_mwh": ind.total_energy_mwh,
            "first_block_affluent_avg": ind.first_block_affluent_avg,
            "last_block_affluent_avg": ind.last_block_affluent_avg,
        }
    except ImportError:
        # Fallback: compute locally using type attribute
        sys_data = planning.get("system", {})
        total_cap = 0.0
        for gen in sys_data.get("generator_array", []):
            if str(gen.get("type", "")).lower() == "falla":
                continue
            cap = gen.get("capacity", gen.get("pmax", 0))
            if isinstance(cap, (int, float)):
                total_cap += float(cap)
        return {
            "total_gen_capacity_mw": total_cap,
            "first_block_demand_mw": 0.0,
            "last_block_demand_mw": 0.0,
            "total_energy_mwh": 0.0,
            "first_block_affluent_avg": 0.0,
            "last_block_affluent_avg": 0.0,
        }


def _gtopt_element_counts(planning: dict[str, Any]) -> dict[str, int]:
    """Extract gtopt element counts from the planning dict."""
    psys = planning.get("system", {})
    sim = planning.get("simulation", {})
    return {
        "buses": len(psys.get("bus_array", [])),
        "generators": len(psys.get("generator_array", [])),
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
        "filtrations": len(psys.get("filtration_array", [])),
        "turbines": len(psys.get("turbine_array", [])),
        "blocks": len(sim.get("block_array", [])),
        "stages": len(sim.get("stage_array", [])),
        "scenarios": len(sim.get("scenario_array", [])),
    }


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
    """Log a formatted side-by-side PLP vs gtopt element comparison.

    The table is grouped by category (network, generation, hydro, storage,
    loads, simulation) and includes derived analysis rows such as *generators
    excluding falla+batería* and *hydro centrals (embalse+serie) vs turbines*.
    ANSI colours are used when stderr is connected to a terminal.
    """
    colr = _use_color()

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
    g_filtrations = gtopt_counts.get("filtrations", 0)
    g_turbines = gtopt_counts.get("turbines", 0)
    g_blocks = gtopt_counts.get("blocks", 0)
    g_stages = gtopt_counts.get("stages", 0)
    g_scenarios = gtopt_counts.get("scenarios", 0)

    # --- row helper ---
    def _val(val: int | None) -> str:
        return f"{val:>8d}" if val is not None else " " * 8

    def _row(
        label: str,
        plp: int | None = None,
        gtopt: int | None = None,
        note: str = "",
        *,
        indent: int = 0,
    ) -> None:
        prefix = "  " + "  " * indent
        lbl_width = 26 - 2 * indent
        delta = _delta_str(plp, gtopt, colr)
        note_part = f"  {_cc(_DIM, note, colr)}" if note else ""
        logger.info(
            "%s%-*s %s %s %s%s",
            prefix,
            lbl_width,
            label,
            _val(plp),
            _val(gtopt),
            delta,
            note_part,
        )

    def _section(title: str) -> None:
        logger.info("  %s", _cc(_BOLD, title, colr))

    def _blank() -> None:
        logger.info("")

    # --- render table ---
    _blank()
    logger.info(
        "%s",
        _cc(_BOLD + _CYAN, "═══ PLP vs gtopt Element Comparison ═══", colr),
    )
    _blank()

    hdr = f"  {'Element':<26s} {'PLP':>8s} {'gtopt':>8s} {'Δ':>6s}  Notes"
    logger.info("%s", _cc(_BOLD, hdr, colr))
    logger.info("  %s %s %s %s  %s", "─" * 26, "─" * 8, "─" * 8, "─" * 6, "─" * 24)

    # -- Network --
    _section("Network")
    _row("buses", p_buses, g_buses)
    _row("lines", p_lines, g_lines)
    _blank()

    # -- Generation --
    _section("Generation")
    _row("centrals (total)", p_centrals)
    _row("embalse", p_embalse, note="→ turbines + generators", indent=1)
    _row("serie", p_serie, note="→ turbines + generators", indent=1)
    _row("pasada", p_pasada, note="→ generators + profiles", indent=1)
    _row("termica", p_termica, note="→ generators", indent=1)
    _row("bateria", p_bateria, note="→ batteries", indent=1)
    _row(
        "falla",
        p_falla,
        note="excluded from gtopt",
        indent=1,
    )
    _row("gen (excl falla+bat)", p_gen_excl, g_generators)
    gen_delta = g_generators - p_gen_excl
    if gen_delta != 0:
        _row(
            "",
            note=f"delta {gen_delta:+d} centrals with bus<=0 excluded",
        )
    _row(
        "generator profiles",
        None,
        g_gen_profiles,
        note=(f"= pasada count ({p_pasada}) ✓" if g_gen_profiles == p_pasada else ""),
    )
    _blank()

    # -- Hydro System --
    _section("Hydro System")
    _row("hydro centrals (emb+ser)", p_hydro)
    _row(
        "turbines",
        p_hydro,
        g_turbines,
        note="only bus>0 with waterway",
    )
    _row("junctions", None, g_junctions)
    _row("waterways", None, g_waterways)
    _row("flows", None, g_flows)
    _row(
        "reservoirs",
        p_embalse,
        g_reservoirs,
        note=(f"= embalse count ({p_embalse}) ✓" if g_reservoirs == p_embalse else ""),
    )
    _row("filtrations", None, g_filtrations)
    _blank()

    # -- Storage --
    _section("Storage")
    _row("batteries (plpcenbat)", p_batteries, g_batteries)
    if p_ess > 0:
        _row("ESS (plpess)", p_ess)
    _blank()

    # -- Loads --
    _section("Loads")
    _row("demands", p_demands, g_demands)
    _blank()

    # -- Simulation --
    _section("Simulation")
    _row("blocks", p_blocks, g_blocks)
    _row("stages", p_stages, g_stages)
    _row("scenarios", None, g_scenarios)
    _blank()

    # --- Global indicators side-by-side ---
    if plp_ind and gtopt_ind:
        logger.info("")
        logger.info("=== PLP vs gtopt global indicators ===")
        logger.info("  %-25s %12s %12s", "Indicator", "PLP", "gtopt")
        logger.info("  %-25s %12s %12s", "-" * 25, "-" * 12, "-" * 12)

        for key in (
            "total_gen_capacity_mw",
            "first_block_demand_mw",
            "last_block_demand_mw",
            "total_energy_mwh",
            "first_block_affluent_avg",
            "last_block_affluent_avg",
        ):
            plp_val = plp_ind.get(key, 0.0)
            gtopt_val = gtopt_ind.get(key, 0.0)
            label = (
                key.replace("_", " ")
                .replace(" mw", " (MW)")
                .replace(" mwh", " (MWh)")
                .replace(" avg", " avg (m³/s)")
            )
            logger.info("  %-25s %12.1f %12.1f", label, plp_val, gtopt_val)

        # Capacity adequacy ratio (using first block demand)
        plp_dem1 = plp_ind.get("first_block_demand_mw", 0.0)
        gtopt_dem1 = gtopt_ind.get("first_block_demand_mw", 0.0)
        plp_cap = plp_ind.get("total_gen_capacity_mw", 0.0)
        gtopt_cap = gtopt_ind.get("total_gen_capacity_mw", 0.0)
        plp_ratio = plp_cap / plp_dem1 if plp_dem1 > 0 else float("inf")
        gtopt_ratio = gtopt_cap / gtopt_dem1 if gtopt_dem1 > 0 else float("inf")
        logger.info(
            "  %-25s %12.3f %12.3f", "capacity adequacy ratio", plp_ratio, gtopt_ratio
        )


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

    # --- PLP vs gtopt comparison (always available) ---
    plp_counts = _plp_element_counts(parser)
    gtopt_counts = _gtopt_element_counts(planning)
    plp_ind = _plp_indicators(parser)
    gtopt_ind = _gtopt_indicators(planning, base_dir=base_dir)
    _log_comparison(plp_counts, gtopt_counts, plp_ind, gtopt_ind)

    # --- gtopt_check_json integration (optional) ---
    try:
        from gtopt_check_json._info import format_info  # noqa: PLC0415
        from gtopt_check_json._checks import (  # noqa: PLC0415
            run_all_checks,
            Severity,
        )
    except ImportError:
        logger.debug("gtopt_check_json not available; skipping JSON validation checks")
        return

    # Print system statistics
    logger.info("=== gtopt_check_json: system info ===")
    for line in format_info(planning, base_dir=base_dir).splitlines():
        logger.info("  %s", line)

    # Run validation checks (all non-AI checks)
    findings = run_all_checks(planning, enabled_checks=None, ai_options=None)

    if not findings:
        logger.info("gtopt_check_json: all checks passed — no issues found.")
        return

    critical_count = 0
    warning_count = 0
    note_count = 0
    for finding in findings:
        if finding.severity == Severity.CRITICAL:
            logger.error("[CRITICAL] (%s) %s", finding.check_id, finding.message)
            critical_count += 1
        elif finding.severity == Severity.WARNING:
            logger.warning("[WARNING] (%s) %s", finding.check_id, finding.message)
            warning_count += 1
        else:
            logger.info("[NOTE] (%s) %s", finding.check_id, finding.message)
            note_count += 1

    logger.info(
        "gtopt_check_json summary: %d critical, %d warnings, %d notes",
        critical_count,
        warning_count,
        note_count,
    )


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
