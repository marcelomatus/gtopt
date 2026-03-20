"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
- Post-conversion validation via gtopt_check_json
"""

import json
import logging
import sys
import time
import zipfile
from pathlib import Path
from typing import Any

from plp2gtopt.excel_writer import build_plp_excel
from plp2gtopt.gtopt_writer import GTOptWriter
from plp2gtopt.plp_parser import PLPParser

# pylint: disable=unused-import
from ._comparison import (  # noqa: F401  — re-exported for backward compat
    _BOLD,
    _CYAN,
    _DIM,
    _GREEN,
    _MAGENTA,
    _RED,
    _RESET,
    _YELLOW,
    _cc,
    _delta_str,
    _extract_flow_central_names,
    _gtopt_element_counts,
    _gtopt_indicators,
    _log_comparison,
    _plp_active_hydrology_indices,
    _plp_element_counts,
    _plp_indicators,
    _use_color,
    _vis_len,
    compute_comparison_indicators,
)

# pylint: enable=unused-import

logger = logging.getLogger(__name__)


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
    plp_ind, gtopt_ind = compute_comparison_indicators(
        parser, planning, base_dir=base_dir
    )
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

    if critical_count > 0:
        logger.info(
            "  Tip: Run 'gtopt_check_json <case.json>' for detailed case statistics."
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
        logger.info("Building gtopt planning model...")
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
            logger.info("Writing output files...")
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
            logger.info("Running post-conversion checks...")
            run_post_check(writer.planning, parser, output_dir=output_dir)

    except RuntimeError:
        raise
    except FileNotFoundError as e:
        raise RuntimeError(
            f"PLP to GTOPT conversion failed. Details: Required file not found: {e}\n"
            f"  Hint: Run 'plp2gtopt --validate -i {input_dir}' to check which "
            f"files are present, or 'plp2gtopt --info -i {input_dir}' for case "
            f"information."
        ) from e
    except ValueError as e:
        raise RuntimeError(
            f"PLP to GTOPT conversion failed. Details: Invalid data format: {e}\n"
            f"  Hint: Check that input files use the expected encoding and "
            f"format (PLP .dat text files)."
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
            "name": name,
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
                "name": name,
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
