#!/usr/bin/env python3
"""Main entry point for PLP to GTOPT conversion."""

import argparse
import logging
import signal
import sys
from pathlib import Path

from gtopt_config import DEFAULT_CONFIG_PATH, get_version, load_config, save_section

from .plp2gtopt import (
    convert_plp_case,
    print_variable_scales_template,
    validate_plp_case,
)
from .info_display import display_plp_info

__version__ = get_version()

_DESCRIPTION = """\
Convert a PLP (PLPMAX/PLPOPT) case directory to gtopt JSON format.

Reads the standard PLP data files (plpblo.dat, plpbar.dat, plpcosce.dat,
plpcnfce.dat, plpcnfli.dat, plpdem.dat, plpeta.dat, …) from INPUT_DIR and
writes either:
  - (default) a self-contained gtopt JSON file + Parquet time-series files, or
  - (with -E) an igtopt-compatible Excel workbook that can later be converted
    using: igtopt <workbook>.xlsx
"""

_EPILOG = """
Quick start:
  plp2gtopt plp_dir                      Convert with default settings
  plp2gtopt plp_dir -y 1-16              Select 16 hydrologies
  plp2gtopt plp_dir -S sddp -a all       SDDP mode with all apertures
  plp2gtopt --info plp_dir               Inspect PLP case structure
  plp2gtopt --validate plp_dir           Validate PLP data only

examples:
  # Convert plp_case_2y using all simulations/apertures, stage 1 only
  plp2gtopt -i plp_case_2y -o gtopt_case_2y -s 1

  # All simulations and all apertures (defaults — same as above without -s)
  plp2gtopt -i plp_case_2y -o gtopt_case_2y

  # Specify directories explicitly
  plp2gtopt -i /data/plp_case -o /data/gtopt_case

  # Generate an igtopt Excel workbook instead of JSON + Parquet
  plp2gtopt -E -i plp_case -o gtopt_case -x plp_case.xlsx

  # Generate a ZIP archive compatible with gtopt_guisrv / gtopt_websrv
  plp2gtopt -z -i plp_case_2y -o gtopt_case_2y

  # Limit conversion to the first 5 stages
  plp2gtopt -i input/ -s 5

  # All simulations (explicit) with all apertures
  plp2gtopt -i input/ -y all -a all

  # Specific simulations: scenarios 1, 2, and 5 through 10 (1-based, Fortran)
  # When plpidsim.dat is present these are simulation indices mapped via idsim;
  # otherwise they are raw hydrology column indices.
  plp2gtopt -i input/ -y 1,2,5-10

  # Two simulation scenarios with explicit probability weights
  plp2gtopt -i input/ -y 1,2 -p 0.6,0.4

  # Select first 5 apertures explicitly
  plp2gtopt -i input/ -a 1-5

  # Group PLP stages 1–4 into phase 1, then one stage per phase after
  plp2gtopt -i input/ -g '1:4,5,6,7,8,9,10,...'

  # Apply a 10% annual discount rate
  plp2gtopt -i input/ -d 0.10

  # Reservoir/battery energy scaling: gtopt auto-scales from emax by default.
  # Override specific reservoirs:
  plp2gtopt -i input/ --reservoir-energy-scale 'RAPEL:500,COLBUN:15000'

  # Override specific battery energy scales:
  plp2gtopt -i input/ --battery-energy-scale 'BESS1:100'

  # Load additional variable scales from a JSON file (lowest priority):
  plp2gtopt -i input/ --variable-scales-file scales.json

  # Generate a variable_scales template, edit, and re-use:
  plp2gtopt -i input/ --variable-scales-template > scales.json
  # Edit scales.json to adjust specific scales...
  plp2gtopt -i input/ --variable-scales-file scales.json

  # Show verbose debug output
  plp2gtopt -i input/ -l DEBUG

  # Validate a PLP case without writing output files
  plp2gtopt --validate -i plp_case_2y
"""


def _parse_name_value_pairs(spec: str) -> dict[str, float]:
    """Parse a comma-separated 'name:value' specification into a dict.

    Example: ``"RAPEL:500,COLBUN:15000"`` returns
    ``{"RAPEL": 500.0, "COLBUN": 15000.0}``.

    Raises:
        ValueError: If a token cannot be parsed as ``name:number``.
    """
    result: dict[str, float] = {}
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        if ":" not in token:
            raise ValueError(
                f"Invalid name:value pair '{token}'; expected 'name:number'"
            )
        name, val_str = token.split(":", maxsplit=1)
        name = name.strip()
        try:
            result[name] = float(val_str.strip())
        except ValueError as exc:
            raise ValueError(f"Invalid numeric value in '{token}': {exc}") from exc
    return result


def signal_handler(sig, _frame):
    """Handle termination signals gracefully."""
    signame = signal.strsignal(sig)
    print(f"\nCaught signal {signame}. Exiting...")
    sys.exit(0)


_CONF_SECTION = "plp2gtopt"


def _conf_defaults() -> dict[str, str]:
    """Read ``[plp2gtopt]`` from ``~/.gtopt.conf`` (empty dict if missing)."""
    cfg = load_config()
    if not cfg.has_section(_CONF_SECTION):
        return {}
    return dict(cfg.items(_CONF_SECTION))


def make_parser() -> argparse.ArgumentParser:
    """Build and return the argument parser for plp2gtopt."""
    from ._parsers import (  # noqa: PLC0415
        add_general_arguments,
        add_io_arguments,
        add_model_arguments,
        add_reservoir_battery_arguments,
        add_ror_arguments,
        add_scenario_arguments,
        add_solver_arguments,
        add_stage_arguments,
        add_tech_arguments,
    )

    conf = _conf_defaults()
    parser = argparse.ArgumentParser(
        prog="plp2gtopt",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )

    add_io_arguments(parser, conf)
    add_stage_arguments(parser, conf)
    add_scenario_arguments(parser, conf)
    add_solver_arguments(parser, conf)
    add_model_arguments(parser, conf)
    add_reservoir_battery_arguments(parser, conf)
    add_ror_arguments(parser, conf)
    add_tech_arguments(parser, conf)
    add_general_arguments(parser, conf)

    return parser


# Keys and hard-coded defaults for the [plp2gtopt] config section.
_SECTION_DEFAULTS: dict[str, str] = {
    "compression": "zstd",
    "compression_level": "1",
    "output_format": "parquet",
    "input_format": "parquet",
    "solver_type": "sddp",
    "demand_fail_cost": "1000.0",
    "state_fail_cost": "1000.0",
    "scale_objective": "1000.0",
    "discount_rate": "0.0",
    "reservoir_scale_mode": "auto",
}


def _init_config() -> None:
    """Write ``[plp2gtopt]`` defaults to ``~/.gtopt.conf``."""
    save_section(DEFAULT_CONFIG_PATH, _CONF_SECTION, _SECTION_DEFAULTS)
    print(f"Initialized [{_CONF_SECTION}] in {DEFAULT_CONFIG_PATH}")


def _resolve_input_dir(args: argparse.Namespace) -> Path:
    """Resolve input directory from positional and/or -i arguments.

    Priority: -i flag > positional argument > default 'input'.
    """
    if args.input_dir is not None:
        return args.input_dir
    if args.positional_input is not None:
        return args.positional_input
    return Path("input")


def _infer_output_dir(input_dir: Path, explicit_output: Path) -> Path:
    """Infer the output directory from the input directory name.

    When only a directory name starting with ``plp_`` is given as the
    positional argument and ``-o`` was not set, the output directory is
    derived by replacing the ``plp_`` prefix with ``gtopt_``.

    For example, ``plp_case_2y`` → ``gtopt_case_2y``.

    If the input directory does not start with ``plp_``, the original
    output directory is returned unchanged.
    """
    dir_name = input_dir.name
    if dir_name.startswith("plp_"):
        return input_dir.parent / ("gtopt_" + dir_name[4:])
    return explicit_output


def build_options(args: argparse.Namespace) -> dict:
    """Convert parsed CLI arguments to a conversion options dict."""
    input_dir = _resolve_input_dir(args)

    # When -o is not given, infer the output dir:
    # - If input dir starts with "plp_", replace prefix with "gtopt_"
    # - Otherwise, default to "output"
    output_dir = args.output_dir
    if output_dir is None:
        output_dir = _infer_output_dir(input_dir, Path("output"))

    output_file = args.output_file
    if output_file is None:
        output_file = output_dir / Path(output_dir.name).with_suffix(".json")
    name = args.name if args.name is not None else Path(output_file).stem
    input_format = args.input_format if args.input_format else args.output_format
    opts = {
        "input_dir": input_dir,
        "output_dir": output_dir,
        "output_file": output_file,
        "last_stage": args.last_stage,
        "last_time": args.last_time,
        "compression": args.compression,
        "compression_level": args.compression_level,
        "output_format": args.output_format,
        "input_format": input_format,
        "hydrologies": "first" if args.first_scenario else args.hydrologies,
        "show_simulation": args.show_simulation,
        "probability_factors": args.probability_factors,
        "discount_rate": args.discount_rate,
        "management_factor": args.management_factor,
        "zip_output": args.zip_output,
        "excel_output": args.excel_output,
        "excel_file": args.excel_file,
        "name": name,
        "sys_version": args.sys_version,
        "solver_type": args.solver_type,
        "stages_phase": args.stages_phase,
        "num_apertures": args.num_apertures,
        "aperture_directory": args.aperture_directory,
    }
    # Model-specific options nested under model_options.
    model_opts: dict = {
        "demand_fail_cost": args.demand_fail_cost,
        "state_fail_cost": args.state_fail_cost,
        "scale_objective": args.scale_objective,
        "use_single_bus": args.use_single_bus,
        "use_kirchhoff": args.use_kirchhoff,
    }
    if args.scale_theta is not None:
        model_opts["scale_theta"] = args.scale_theta
    if args.reserve_fail_cost is not None:
        model_opts["reserve_fail_cost"] = args.reserve_fail_cost
    if args.use_line_losses is not None:
        model_opts["use_line_losses"] = args.use_line_losses
    opts["model_options"] = model_opts

    if args.cut_sharing_mode is not None:
        opts["cut_sharing_mode"] = args.cut_sharing_mode
    if args.boundary_cuts_mode is not None:
        opts["boundary_cuts_mode"] = args.boundary_cuts_mode
    if args.boundary_max_iterations is not None:
        opts["boundary_max_iterations"] = args.boundary_max_iterations
    if args.no_boundary_cuts:
        opts["no_boundary_cuts"] = True
    if args.hot_start_cuts:
        opts["hot_start_cuts"] = True
    if args.stationary_tol is not None:
        opts["stationary_tol"] = args.stationary_tol
    if args.stationary_window is not None:
        opts["stationary_window"] = args.stationary_window
    opts["reservoir_scale_mode"] = args.reservoir_scale_mode
    if args.reservoir_energy_scale is not None:
        opts["reservoir_energy_scale"] = _parse_name_value_pairs(
            args.reservoir_energy_scale
        )
    opts["auto_reservoir_energy_scale"] = args.auto_reservoir_energy_scale
    if args.battery_energy_scale is not None:
        opts["battery_energy_scale"] = _parse_name_value_pairs(
            args.battery_energy_scale
        )
    opts["auto_battery_energy_scale"] = args.auto_battery_energy_scale
    if args.variable_scales_file is not None:
        opts["variable_scales_file"] = args.variable_scales_file
    opts["soft_emin_cost"] = args.soft_emin_cost
    opts["embed_reservoir_constraints"] = args.embed_reservoir_constraints
    opts["emit_water_rights"] = args.emit_water_rights
    if args.ror_as_reservoirs is not None:
        opts["ror_as_reservoirs"] = args.ror_as_reservoirs
    if args.ror_as_reservoirs_file is not None:
        opts["ror_as_reservoirs_file"] = args.ror_as_reservoirs_file
    opts["run_check"] = args.run_check
    # Technology detection
    opts["auto_detect_tech"] = args.auto_detect_tech
    if args.tech_overrides is not None:
        from .tech_detect import load_overrides  # noqa: PLC0415

        opts["tech_overrides"] = load_overrides(args.tech_overrides)
    # Pasada mode: "hydro", "flow-turbine", or "profile"
    pasada_mode = getattr(args, "pasada_mode", "flow-turbine") or "flow-turbine"
    opts["pasada_mode"] = pasada_mode
    # Backward compat: pasada_hydro = True when mode is "hydro", "flow-turbine", or "auto"
    opts["pasada_hydro"] = pasada_mode in ("hydro", "flow-turbine", "auto")
    opts["log_file"] = getattr(args, "log_file", None)
    return opts


def main(argv: list[str] | None = None) -> None:
    """Parse arguments and initiate conversion."""
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    no_args = len(sys.argv) == 1

    parser = make_parser()
    args = parser.parse_args(argv)

    if args.init_config:
        _init_config()
        return

    # Reconcile positional and -i input dir
    args.input_dir = _resolve_input_dir(args)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    # Use clean formatter for non-DEBUG levels (no timestamps on INFO lines)
    try:
        from gtopt_check_json._terminal import CleanFormatter, init  # noqa: PLC0415

        if args.log_level != "DEBUG":
            for handler in logging.getLogger().handlers:
                handler.setFormatter(CleanFormatter())
        init(force_color=None)
    except ImportError:
        pass

    if args.show_info:
        try:
            display_plp_info(
                {
                    "input_dir": args.input_dir,
                    "last_stage": args.last_stage,
                    "hydrologies": args.hydrologies,
                }
            )
        except (RuntimeError, FileNotFoundError, OSError) as exc:
            print(
                f"error: {exc}\n"
                "Usage: plp2gtopt --info -i <input_dir>\n"
                "       plp2gtopt --info <input_dir>",
                file=sys.stderr,
            )
            sys.exit(1)
        return

    if args.tech_list:
        from .tech_classify import type_label  # noqa: PLC0415
        from .tech_detect import available_types  # noqa: PLC0415

        print("Known generator technology types:")
        for t in available_types():
            label = type_label(t)
            if label != t:
                print(f"  {t:<25s} {label}")
            else:
                print(f"  {t}")
        return

    if args.validate:
        valid = validate_plp_case(build_options(args))
        sys.exit(0 if valid else 1)

    if args.variable_scales_template:
        sys.exit(print_variable_scales_template(build_options(args)))

    try:
        convert_plp_case(build_options(args))
    except (RuntimeError, FileNotFoundError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        if no_args:
            print(
                "Usage: plp2gtopt [INPUT_DIR] -o <output_dir> [options]\n"
                "Run 'plp2gtopt -h' for the full list of options, "
                "or 'plp2gtopt --info <input_dir>' to inspect a case.",
                file=sys.stderr,
            )
        sys.exit(1)


if __name__ == "__main__":
    main()
