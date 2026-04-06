"""PLP to GTOPT conversion functions.

Handles:
- Coordinating all parser modules
- Validating input data consistency
- Managing conversion process
- Post-conversion validation via gtopt_check_json
"""

import json
import logging
import shutil
import sys
import tempfile
import time
import zipfile
from pathlib import Path
from typing import Any

from plp2gtopt._progress import ConversionProgress, setup_file_logging
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
    from gtopt_check_json._terminal import print_table  # noqa: PLC0415

    sys_data = planning.get("system", {})
    sim = planning.get("simulation", {})
    opts = planning.get("options", {})

    # Build all rows for a single results table
    rows: list[tuple[str, str]] = [
        ("System name", sys_data.get("name", "(unnamed)")),
    ]
    version = sys_data.get("version", "")
    if version:
        rows.append(("System version", version))

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
        ("Turbines", len(sys_data.get("turbine_array", []))),
        ("Flow rights", len(sys_data.get("flow_right_array", []))),
        ("Volume rights", len(sys_data.get("volume_right_array", []))),
        ("User constraints", len(sys_data.get("user_constraint_array", []))),
        ("User params", len(sys_data.get("user_param_array", []))),
    ]
    for name, count in all_elems:
        if count > 0:
            rows.append((name, str(count)))

    # User constraint file
    uc_file = sys_data.get("user_constraint_file")
    if uc_file:
        rows.append(("Constraint file", uc_file))

    # Simulation
    blocks = sim.get("block_array", [])
    rows.append(("Blocks", str(len(blocks))))
    rows.append(("Stages", str(len(sim.get("stage_array", [])))))
    rows.append(("Scenarios", str(len(sim.get("scenario_array", [])))))
    apertures = sim.get("aperture_array", [])
    if apertures:
        rows.append(("Apertures", str(len(apertures))))

    # Total period from block durations
    total_hours = sum(b.get("duration", 0) for b in blocks)
    if total_hours > 0:
        months = total_hours / (30.0 * 24.0)
        rows.append(("Total period", f"{total_hours:,.0f} h ({months:.1f} months)"))

    # Boundary cuts
    bc_count = planning.get("_boundary_cuts_count", 0)
    bc_vars = planning.get("_boundary_state_variables", 0)
    if bc_count:
        rows.append(("Boundary cuts", f"{bc_count:,} ({bc_vars} state variables)"))

    # Falla centrals / demand fail cost summary
    demands = sys_data.get("demand_array", [])
    fcost_values = [
        d["fcost"] for d in demands if isinstance(d.get("fcost"), (int, float))
    ]
    if fcost_values:
        lo, hi = min(fcost_values), max(fcost_values)
        cost_str = f"{lo:.2f}" if lo == hi else f"{lo:.2f}\u2013{hi:.2f}"
        rows.append(
            (
                "Falla centrals",
                f"{len(fcost_values)} bus(es), fcost {cost_str} $/MWh",
            )
        )

    # Options
    mo = opts.get("model_options", {})
    rows.append(("use_kirchhoff", str(mo.get("use_kirchhoff", False))))
    rows.append(("use_single_bus", str(mo.get("use_single_bus", False))))
    rows.append(("scale_objective", str(mo.get("scale_objective", 1_000))))
    rows.append(("demand_fail_cost", str(mo.get("demand_fail_cost", 0))))

    # Skipped centrals
    skipped = planning.get("_skipped_isolated", [])
    if skipped:
        for name in sorted(skipped):
            rows.append((f"  skip: {name}", "isolated (bus<=0)"))

    rows.append(("Elapsed", f"{elapsed:.3f}s"))

    print_table(
        headers=["Property", "Value"],
        rows=rows,
        aligns=["left", "right"],
        title="Conversion Results",
    )


def show_simulation_summary(planning: dict) -> None:
    """Print a detailed summary of the simulation structure."""
    from gtopt_check_json._terminal import (  # noqa: PLC0415
        print_kv_table,
        print_section,
    )

    sim = planning.get("simulation", {})
    opts = planning.get("options", {})
    sddp_opts = opts.get("sddp_options", {})

    print_section("Simulation Structure")

    # --- Scenarios ---
    scenarios = sim.get("scenario_array", [])
    scen_rows = []
    for s in scenarios:
        scen_rows.append(
            (
                f"uid={s['uid']}",
                f"hydrology={s.get('hydrology', '?')} "
                f"prob={s.get('probability_factor', 1.0):.4f}",
            )
        )
    if len(scenarios) > 10:
        scen_rows = scen_rows[:10]
        scen_rows.append(("...", f"({len(scenarios) - 10} more)"))
    print_kv_table(scen_rows, title=f"Scenarios ({len(scenarios)})")

    # --- Stages ---
    stages = sim.get("stage_array", [])
    if stages:
        first = stages[0]
        last = stages[-1]
        print_kv_table(
            [
                ("Count", str(len(stages))),
                (
                    "First",
                    f"uid={first.get('uid')} block_start={first.get('first_block')}",
                ),
                (
                    "Last",
                    f"uid={last.get('uid')} block_start={last.get('first_block')}",
                ),
            ],
            title="Stages",
        )

    # --- Phases ---
    phases = sim.get("phase_array", [])
    if phases:
        print_kv_table(
            [
                ("Count", str(len(phases))),
                ("Stages/phase", str(phases[0].get("count_stage", 1))),
            ],
            title="Phases",
        )

    # --- Scenes ---
    scenes = sim.get("scene_array", [])
    if scenes:
        scene_rows = []
        for sc in scenes[:5]:
            scene_rows.append(
                (
                    f"uid={sc.get('uid')}",
                    f"scenarios={sc.get('first_scenario')}"
                    f"..{sc.get('first_scenario', 0) + sc.get('count_scenario', 1) - 1}"
                    f" (count={sc.get('count_scenario')})",
                )
            )
        if len(scenes) > 5:
            scene_rows.append(("...", f"({len(scenes) - 5} more scenes)"))
        print_kv_table(scene_rows, title=f"Scenes ({len(scenes)})")

    # --- Apertures ---
    apertures = sim.get("aperture_array", [])
    if apertures:
        # Group by source_scenario availability
        scen_uids = {s["uid"] for s in scenarios}
        valid = [a for a in apertures if a["source_scenario"] in scen_uids]
        missing = [a for a in apertures if a["source_scenario"] not in scen_uids]
        ap_rows = [
            ("Total apertures", str(len(apertures))),
            ("Valid (source in scenarios)", str(len(valid))),
            ("Missing (source not found)", str(len(missing))),
        ]
        if sddp_opts.get("aperture_directory"):
            ap_rows.append(("aperture_directory", sddp_opts["aperture_directory"]))
        print_kv_table(ap_rows, title="Apertures")

        # Show per-phase aperture distribution
        phases_with_ap = [p for p in phases if "apertures" in p]
        if phases_with_ap:
            ap_set_sizes = [len(p["apertures"]) for p in phases_with_ap]
            print_kv_table(
                [
                    ("Phases with apertures", str(len(phases_with_ap))),
                    ("Min apertures/phase", str(min(ap_set_sizes))),
                    ("Max apertures/phase", str(max(ap_set_sizes))),
                ],
                title="Phase Aperture Distribution",
            )

    # --- SDDP options ---
    if sddp_opts:
        sddp_rows = [
            (k, str(v))
            for k, v in sddp_opts.items()
            if k not in ("aperture_directory",)
        ]
        if sddp_rows:
            print_kv_table(sddp_rows, title="SDDP Options")


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
    base_dir = str(output_dir) if output_dir is not None else ""

    # Derive the active hydrology indices from the PLP case data directly
    # (idsim_parser or aflce_parser), then verify consistency with what
    # the gtopt conversion selected as scenarios.
    hydrology_indices = _plp_active_hydrology_indices(parser)

    # Cross-check: the gtopt scenario_array should reference the same set
    sim = planning.get("simulation", {})
    scenarios = sim.get("scenario_array", [])
    if hydrology_indices is not None and scenarios:
        # Only compare forward scenarios (exclude aperture-only ones that
        # have their own input_directory).
        forward_scenarios = [s for s in scenarios if "input_directory" not in s]
        gtopt_hydrology_indices = sorted(
            s.get("hydrology")
            for s in forward_scenarios
            if s.get("hydrology") is not None
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
    _log_comparison(
        plp_counts,
        gtopt_counts,
        plp_ind,
        gtopt_ind,
        gtopt_options=planning.get("options"),
    )

    # --- gtopt_check_json integration (optional) ---
    try:
        from gtopt_check_json._checks import (  # noqa: PLC0415
            run_all_checks,
            Severity,
        )
        from gtopt_check_json._terminal import (  # noqa: PLC0415
            print_status,
            print_summary,
            print_table,
        )
    except ImportError:
        logger.debug("gtopt_check_json not available; skipping JSON validation checks")
        return

    # Run validation checks (all non-AI checks)
    findings = run_all_checks(
        planning, enabled_checks=None, ai_options=None, base_dir=base_dir
    )

    # Inject conversion-time battery efficiency warnings (the values were
    # clamped to 1.0, so check_battery_efficiency cannot detect them).
    from gtopt_check_json._checks import Finding  # noqa: PLC0415

    for msg in planning.get("_clamped_battery_warnings", []):
        findings.append(
            Finding(
                check_id="battery_efficiency",
                severity=Severity.WARNING,
                message=msg,
                action="Check plpess.dat / plpcenbat.dat efficiency values",
            )
        )

    if not findings:
        print_status("All checks passed — no issues found.", ok=True)
        return

    # Collect all findings into the warnings table
    rows: list[tuple[str, str, str]] = []
    critical_count = 0
    warning_count = 0
    note_count = 0
    for finding in findings:
        if finding.severity == Severity.CRITICAL:
            critical_count += 1
        elif finding.severity == Severity.WARNING:
            warning_count += 1
        else:
            note_count += 1
        rows.append(
            (
                finding.severity.name,
                finding.message,
                finding.action,
            )
        )

    if rows:
        print_table(
            headers=["Severity", "Description", "Action / Notes"],
            rows=rows,
            aligns=["left", "left", "left"],
            title="Warnings",
            styles=["warn", "", "dim"],
            min_widths=[8, None, None],
            wraps=[False, True, True],
            show_lines=len(rows) > 1,
        )

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

    output_dir = Path(options.get("output_dir", "output"))
    output_file = Path(options.get("output_file", output_dir / "gtopt.json"))
    output_dir.parent.mkdir(parents=True, exist_ok=True)

    # Work in a temporary directory next to the final output, then swap
    # atomically on success.  This prevents orphaned files from previous
    # conversions and ensures the output is always consistent.
    tmp_dir = None
    log_handler: logging.Handler | None = None
    try:
        with ConversionProgress() as progress:
            t0 = time.monotonic()

            # Parse all files
            progress.step("parse")
            logger.debug("Parsing PLP input files from: %s", input_dir)
            parser = PLPParser(options)
            parser.parse_all()

            # Create temp dir in the same parent as output_dir so rename
            # is atomic
            tmp_dir = Path(
                tempfile.mkdtemp(
                    prefix=f".{output_dir.name}.tmp.",
                    dir=output_dir.parent,
                )
            )

            # Set up file logging into the temp dir (moved to final dir on
            # success)
            log_handler = setup_file_logging(tmp_dir, log_file=options.get("log_file"))

            # Redirect output_dir to temp; keep output_file location as-is
            # when it lives outside the output_dir (e.g. -f flag).
            json_inside = output_file.parent == output_dir
            tmp_output_file = tmp_dir / output_file.name if json_inside else output_file
            tmp_options = {
                **options,
                "output_dir": tmp_dir,
                "output_file": tmp_output_file,
                "_progress": progress,
            }

            # Convert to GTOPT format (writes Parquet time-series to
            # tmp_dir).  The to_json / write methods call
            # progress.step() for each sub-step.
            logger.debug("Building gtopt planning model...")
            writer = GTOptWriter(parser)

            if excel_output:
                logger.debug("Building planning data for Excel output...")
                planning = writer.to_json(tmp_options)

                excel_file = options.get("excel_file")
                if excel_file is None:
                    excel_file = output_dir.parent / (output_dir.name + ".xlsx")
                else:
                    excel_file = Path(excel_file)

                progress.step("write")
                logger.debug("Writing igtopt Excel workbook to: %s", excel_file)
                build_plp_excel(planning, tmp_dir, excel_file, tmp_options)
            else:
                logger.debug("Writing GTOPT output to: %s", output_file)
                writer.write(tmp_options)

            elapsed = time.monotonic() - t0

            # --- Conversion succeeded: swap temp → final output_dir ---
            # When logging to the default temp-dir path, close the handler
            # before renaming.  When --log points to a persistent file,
            # keep it open so post-check output is also logged.
            if log_handler is not None and options.get("log_file") is None:
                logging.getLogger().removeHandler(log_handler)
                log_handler.close()
                log_handler = None

            if output_dir.exists():
                shutil.rmtree(output_dir)
            tmp_dir.rename(output_dir)
            tmp_dir = None  # Prevent cleanup in finally

            # If JSON was written inside the temp dir, it's now at the
            # final path.
            if json_inside:
                output_file = output_dir / output_file.name

            # Fix temp dir paths in the planning dict and re-write the
            # JSON.
            if not json_inside:
                tmp_str = str(tmp_options["output_dir"])
                final_str = str(output_dir)
                plan_json = json.dumps(writer.planning)
                if tmp_str in plan_json:
                    plan_json = plan_json.replace(tmp_str, final_str)
                    writer.planning = json.loads(plan_json)
                with open(output_file, "w", encoding="utf-8") as f:
                    json.dump(writer.planning, f, indent=4)

            # Mark last build step done before printing tables
            progress.step("validate")
            progress.done()

        # --- Outside the progress context: print tables ---
        _log_stats(writer.planning, elapsed)

        if options.get("show_simulation", False):
            show_simulation_summary(writer.planning)

        if not excel_output and options.get("zip_output", False):
            zip_path = output_file.with_suffix(".zip")
            create_zip_output(output_file, output_dir, zip_path)

        # Post-conversion validation
        if do_check:
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
    finally:
        if log_handler is not None:
            logging.getLogger().removeHandler(log_handler)
            log_handler.close()
        if tmp_dir is not None and tmp_dir.exists():
            shutil.rmtree(tmp_dir, ignore_errors=True)


def generate_variable_scales_template(options: dict[str, Any]) -> str:
    """Generate a pre-computed variable_scales JSON template from PLP case data.

    Parses the PLP case (same initial steps as convert_plp_case) and builds
    a JSON array of VariableScale objects for reservoirs and batteries.

    For each reservoir, the energy scale is computed from FEscala
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
    central_energy_scale: dict[str, float] = {}
    if central_parser is not None:
        for central in central_parser.centrals:
            if central.get("type") == "embalse" and "energy_scale" in central:
                central_energy_scale[str(central["name"])] = central["energy_scale"]

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
            cvs = central_energy_scale.get(name)
            if cvs is not None:
                scale = cvs

        resolved_scale = scale if scale is not None else 1.0
        entry: dict[str, Any] = {
            "class_name": "Reservoir",
            "variable": "energy",
            "uid": uid,
            "scale": resolved_scale,
            "name": name,
        }
        if fescala_val is not None:
            entry["_fescala"] = fescala_val
        scales.append(entry)
        # Scale flow (extraction) variables: flow_scale = energy_scale / 1000
        scales.append(
            {
                "class_name": "Reservoir",
                "variable": "flow",
                "uid": uid,
                "scale": resolved_scale / 1000.0,
                "name": name,
            }
        )

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
        # Scale flow (finp/fout) with the same factor so
        # energy-balance coefficients stay O(1).
        scales.append(
            {
                "class_name": "Battery",
                "variable": "flow",
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
