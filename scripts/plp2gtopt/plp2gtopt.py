"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
- Post-conversion validation via gtopt_check_json
"""

import logging
import time
import zipfile
from pathlib import Path
from typing import Any

from plp2gtopt.excel_writer import build_plp_excel
from plp2gtopt.gtopt_writer import GTOptWriter
from plp2gtopt.plp_parser import PLPParser

logger = logging.getLogger(__name__)


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
    pd = parser.parsed_data
    counts: dict[str, int] = {}

    bus_parser = pd.get("bus_parser")
    if bus_parser:
        counts["buses"] = getattr(bus_parser, "num_buses", 0)

    central_parser = pd.get("central_parser")
    if central_parser:
        counts["centrals"] = getattr(central_parser, "num_centrals", 0)
        for ctype, clist in central_parser.centrals_of_type.items():
            counts[f"  {ctype}"] = len(clist)

    demand_parser = pd.get("dem_parser")
    if demand_parser:
        counts["demands"] = getattr(demand_parser, "num_demands", 0)

    line_parser = pd.get("line_parser")
    if line_parser:
        counts["lines"] = getattr(line_parser, "num_lines", 0)

    battery_parser = pd.get("battery_parser")
    if battery_parser:
        counts["batteries (plpcenbat)"] = len(getattr(battery_parser, "batteries", []))

    ess_parser = pd.get("ess_parser")
    if ess_parser:
        counts["ESS (plpess)"] = len(getattr(ess_parser, "items", []))

    block_parser = pd.get("block_parser")
    if block_parser:
        counts["blocks"] = getattr(block_parser, "num_blocks", 0)

    stage_parser = pd.get("stage_parser")
    if stage_parser:
        counts["stages"] = getattr(stage_parser, "num_stages", 0)

    return counts


def _gtopt_element_counts(planning: dict[str, Any]) -> dict[str, int]:
    """Extract gtopt element counts from the planning dict."""
    sys = planning.get("system", {})
    sim = planning.get("simulation", {})
    return {
        "buses": len(sys.get("bus_array", [])),
        "generators": len(sys.get("generator_array", [])),
        "generator_profiles": len(sys.get("generator_profile_array", [])),
        "demands": len(sys.get("demand_array", [])),
        "demand_profiles": len(sys.get("demand_profile_array", [])),
        "lines": len(sys.get("line_array", [])),
        "batteries": len(sys.get("battery_array", [])),
        "converters": len(sys.get("converter_array", [])),
        "junctions": len(sys.get("junction_array", [])),
        "waterways": len(sys.get("waterway_array", [])),
        "flows": len(sys.get("flow_array", [])),
        "reservoirs": len(sys.get("reservoir_array", [])),
        "filtrations": len(sys.get("filtration_array", [])),
        "turbines": len(sys.get("turbine_array", [])),
        "blocks": len(sim.get("block_array", [])),
        "stages": len(sim.get("stage_array", [])),
        "scenarios": len(sim.get("scenario_array", [])),
    }


def _log_comparison(
    plp_counts: dict[str, int],
    gtopt_counts: dict[str, int],
) -> None:
    """Log a side-by-side comparison of PLP vs gtopt element counts."""
    logger.info("=== PLP vs gtopt element comparison ===")
    logger.info("  %-25s %8s %8s", "Element", "PLP", "gtopt")
    logger.info("  %-25s %8s %8s", "-" * 25, "-" * 8, "-" * 8)

    # PLP counts
    for key, val in plp_counts.items():
        logger.info("  %-25s %8d %8s", key, val, "")

    logger.info("  %-25s %8s %8s", "", "", "")

    # gtopt counts (skip zero counts for cleanliness)
    for key, val in gtopt_counts.items():
        if val > 0:
            logger.info("  %-25s %8s %8d", key, "", val)


def run_post_check(
    planning: dict[str, Any],
    parser: PLPParser,
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
    """
    # --- PLP vs gtopt comparison (always available) ---
    plp_counts = _plp_element_counts(parser)
    gtopt_counts = _gtopt_element_counts(planning)
    _log_comparison(plp_counts, gtopt_counts)

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
    for line in format_info(planning).splitlines():
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
            run_post_check(writer.planning, parser)

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
