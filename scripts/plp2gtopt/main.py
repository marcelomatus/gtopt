#!/usr/bin/env python3
"""Main entry point for PLP to GTOPT conversion."""

import argparse
import logging
import signal
import sys
from pathlib import Path

from .plp2gtopt import convert_plp_case
from .info_display import display_plp_info

try:
    from importlib.metadata import version as _pkg_version, PackageNotFoundError

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

_LOG_LEVEL_CHOICES = ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]

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

  # Set reservoir volume scaling for better LP numerics
  plp2gtopt -i input/ --vol-scale 'RAPEL:500,COLBUN:15000'

  # Auto-calculate vol_scale from PLP FEscala (plpplem1.dat / plpcnfce.dat)
  plp2gtopt -i input/ --auto-vol-scale

  # Set battery energy scaling
  plp2gtopt -i input/ --energy-scale 'BESS1:100'

  # Auto-set energy_scale=0.01 for all PLP batteries
  plp2gtopt -i input/ --auto-energy-scale

  # Show verbose debug output
  plp2gtopt -i input/ -l DEBUG
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


def make_parser() -> argparse.ArgumentParser:
    """Build and return the argument parser for plp2gtopt."""
    parser = argparse.ArgumentParser(
        prog="plp2gtopt",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )
    parser.add_argument(
        "positional_input",
        nargs="?",
        type=Path,
        default=None,
        metavar="INPUT_DIR",
        help="directory containing PLP input files (same as -i)",
    )
    parser.add_argument(
        "-i",
        "--input-dir",
        type=Path,
        metavar="DIR",
        default=None,
        help="directory containing PLP input files (default: input)",
    )
    parser.add_argument(
        "-o",
        "--output-dir",
        type=Path,
        metavar="DIR",
        default=Path("output"),
        help="directory for gtopt output files (default: %(default)s)",
    )
    parser.add_argument(
        "-f",
        "--output-file",
        type=Path,
        metavar="FILE",
        default=None,
        help=(
            "output JSON file path "
            "(default: <output-dir-name>.json in the current directory)"
        ),
    )
    parser.add_argument(
        "-s",
        "--last-stage",
        dest="last_stage",
        type=int,
        metavar="N",
        default=-1,
        help="last stage to include (default: all stages)",
    )
    parser.add_argument(
        "-d",
        "--discount-rate",
        dest="discount_rate",
        type=float,
        metavar="RATE",
        default=0.0,
        help="annual discount rate, e.g. 0.10 for 10%% (default: %(default)s)",
    )
    parser.add_argument(
        "-m",
        "--management-factor",
        dest="management_factor",
        type=float,
        metavar="FACTOR",
        default=0.0,
        help="demand management factor (default: %(default)s)",
    )
    parser.add_argument(
        "-t",
        "--last-time",
        dest="last_time",
        type=float,
        metavar="T",
        default=-1,
        help="last time value to extract (default: all time steps)",
    )
    parser.add_argument(
        "-n",
        "--name",
        dest="name",
        metavar="NAME",
        default=None,
        help=(
            "name for the system in the output JSON "
            "(default: basename of the output JSON file)"
        ),
    )
    parser.add_argument(
        "--sys-version",
        dest="sys_version",
        metavar="VERSION",
        default="",
        help="version string for the system in the output JSON (default: empty)",
    )
    parser.add_argument(
        "-F",
        "--output-format",
        dest="output_format",
        metavar="FORMAT",
        default="parquet",
        choices=["parquet", "csv"],
        help="output file format: parquet or csv (default: %(default)s)",
    )
    parser.add_argument(
        "-I",
        "--input-format",
        dest="input_format",
        metavar="FORMAT",
        default=None,
        choices=["parquet", "csv"],
        help=(
            "input format for gtopt to read time-series files "
            "(default: same as output-format)"
        ),
    )
    parser.add_argument(
        "-c",
        "--compression",
        dest="compression",
        metavar="ALG",
        default="zstd",
        help="compression codec for output files (default: %(default)s)",
    )
    parser.add_argument(
        "--demand-fail-cost",
        dest="demand_fail_cost",
        type=float,
        metavar="COST",
        default=1000.0,
        help="cost penalty for demand curtailment in $/MWh (default: %(default)s)",
    )
    parser.add_argument(
        "--reserve-fail-cost",
        dest="reserve_fail_cost",
        type=float,
        metavar="COST",
        default=None,
        help="cost penalty for reserve shortfall in $/MWh (default: not set)",
    )
    parser.add_argument(
        "--scale-objective",
        dest="scale_objective",
        type=float,
        metavar="FACTOR",
        default=1000.0,
        help="objective function scaling factor (default: %(default)s)",
    )
    parser.add_argument(
        "-b",
        "--use-single-bus",
        dest="use_single_bus",
        action="store_true",
        default=False,
        help="use single-bus (copper-plate) mode (default: %(default)s)",
    )
    parser.add_argument(
        "-k",
        "--use-kirchhoff",
        dest="use_kirchhoff",
        action="store_true",
        default=False,
        help="enable Kirchhoff voltage-law constraints (default: %(default)s)",
    )
    parser.add_argument(
        "-L",
        "--use-line-losses",
        dest="use_line_losses",
        action="store_true",
        default=None,
        help="model transmission line losses (omit to use gtopt default: true)",
    )
    parser.add_argument(
        "-y",
        "--hydrologies",
        dest="hydrologies",
        metavar="SPEC",
        default="all",
        help=(
            "Simulation/hydrology scenario selector using 1-based (Fortran) "
            "indices.  Accepts 'all' (default), a single index, "
            "comma-separated values, or ranges: '1', '1,2', '1,2,5-10'. "
            "When plpidsim.dat is present the indices are simulation numbers "
            "mapped to hydrology columns via plpidsim.dat; otherwise they are "
            "raw 1-based hydrology column indices.  (default: %(default)s)"
        ),
    )
    parser.add_argument(
        "-p",
        "--probability-factors",
        dest="probability_factors",
        metavar="P1[,P2,…]",
        default=None,
        help=(
            "comma-separated probability weights for each hydrology scenario "
            "(default: equal distribution 1/N)"
        ),
    )
    parser.add_argument(
        "-S",
        "--solver",
        dest="solver_type",
        metavar="TYPE",
        default="sddp",
        choices=["sddp", "mono", "monolithic"],
        help=(
            "solver type controlling the simulation structure: "
            "'sddp' produces one scene per scenario and one phase per stage "
            "(for Stochastic Dual Dynamic Programming); "
            "'mono'/'monolithic' produces a single scene with all scenarios and "
            "a single phase with all stages (for the monolithic solver). "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "-a",
        "--num-apertures",
        dest="num_apertures",
        metavar="SPEC",
        type=str,
        default="all",
        help=(
            "SDDP backward-pass aperture selector. "
            "Accepts 'all' (default), a single count N, a range '1-5', or a "
            "comma-separated list '1,2,3'. "
            "'all' auto-detects the count from plpidap2.dat; "
            "0 disables apertures; N > 0 uses the first N apertures. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "-A",
        "--aperture-directory",
        dest="aperture_directory",
        metavar="DIR",
        default=None,
        help=(
            "directory for aperture-specific scenario data files. "
            "When PLP aperture index files (plpidape.dat / plpidap2.dat) "
            "reference hydrology classes not in the forward-scenario set, "
            "the extra affluent data is written to this directory. "
            "If not set, defaults to <output-dir>/apertures when needed."
        ),
    )
    parser.add_argument(
        "--cut-sharing-mode",
        dest="cut_sharing_mode",
        metavar="MODE",
        default=None,
        choices=["none", "expected", "accumulate", "max"],
        help=(
            "SDDP cut sharing mode: "
            "'none' keeps cuts in their originating scene; "
            "'expected' computes a probability-weighted average cut; "
            "'accumulate' sums all cuts directly (correct when LP "
            "objectives include probability factors); "
            "'max' shares all cuts from all scenes to all scenes. "
            "(default: none)"
        ),
    )
    parser.add_argument(
        "--boundary-cuts-mode",
        dest="boundary_cuts_mode",
        metavar="MODE",
        default=None,
        choices=["noload", "separated", "combined"],
        help=(
            "Controls how PLP boundary cuts (plpplaem/plpplem files) are "
            "loaded into the SDDP solver. "
            "'noload' disables boundary-cut loading; "
            "'separated' loads each cut into the scene matching its ISimul; "
            "'combined' broadcasts all cuts to every scene. "
            "(default: separated)"
        ),
    )
    parser.add_argument(
        "--boundary-max-iterations",
        dest="boundary_max_iterations",
        metavar="N",
        type=int,
        default=None,
        help=(
            "Keep only boundary cuts from the last N SDDP iterations. "
            "0 means keep all iterations. "
            "(default: 0 = all)"
        ),
    )
    parser.add_argument(
        "--no-boundary-cuts",
        dest="no_boundary_cuts",
        action="store_true",
        default=False,
        help="Disable boundary-cut export entirely (equivalent to --boundary-cuts-mode=noload).",
    )
    parser.add_argument(
        "--hot-start-cuts",
        dest="hot_start_cuts",
        action="store_true",
        default=False,
        help=(
            "Export intermediate-stage cuts from plpplaem/plpplem files "
            "as a hot-start-cuts CSV (with named state variables and phase "
            "column).  The file is loaded by the SDDP solver via "
            "sddp_named_cuts_file to warm-start all phases."
        ),
    )
    parser.add_argument(
        "-g",
        "--stages-phase",
        dest="stages_phase",
        metavar="SPEC",
        default=None,
        help=(
            "Map PLP stages to gtopt phases using 1-based stage indices. "
            "Format: comma-separated tokens where each token is a single stage N "
            "or a range N:M, and the trailing token '...' auto-expands one stage "
            "per phase until all stages are covered. "
            "Example: '1:4,5,6,7,8,9,10,...' assigns stages 1-4 to phase 1, "
            "stages 5-10 each to their own phase, then one stage per phase for "
            "any remaining stages. "
            "When omitted, the phase layout is controlled by --solver. "
            "(default: not set)"
        ),
    )
    parser.add_argument(
        "-z",
        "--zip",
        dest="zip_output",
        action="store_true",
        default=False,
        help=(
            "create a ZIP archive <output-dir>.zip containing the JSON file "
            "and all Parquet/CSV data files (compatible with gtopt_guisrv and "
            "gtopt_websrv)"
        ),
    )
    parser.add_argument(
        "-E",
        "--excel-output",
        dest="excel_output",
        action="store_true",
        default=False,
        help=(
            "produce an igtopt-compatible Excel workbook instead of the "
            "JSON + Parquet files. The workbook can later be converted with: "
            "igtopt <workbook>.xlsx"
        ),
    )
    parser.add_argument(
        "-x",
        "--excel-file",
        dest="excel_file",
        type=Path,
        metavar="FILE",
        default=None,
        help=(
            "output Excel workbook path when -E/--excel-output is used "
            "(default: <output-file>.xlsx)"
        ),
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="INFO",
        choices=_LOG_LEVEL_CHOICES,
        metavar="LEVEL",
        help=(
            "logging verbosity: DEBUG, INFO, WARNING, ERROR, CRITICAL "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--info",
        dest="show_info",
        action="store_true",
        default=False,
        help=(
            "display a summary of the PLP case (buses, generators, stages, "
            "available hydrology classes, simulation-to-hydrology mapping from "
            "plpidsim.dat, aperture structure from plpidap2.dat) and exit. "
            "Use this to discover which -y / -a values to pass. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--vol-scale",
        dest="vol_scale",
        metavar="SPEC",
        default=None,
        help=(
            "Set reservoir volume scale values as comma-separated name:value pairs. "
            "Example: --vol-scale 'RAPEL:500,COLBUN:15000'. "
            "Emitted as variable_scales entries in the options section. "
            "The scale divides the LP volume variable for numerical conditioning. "
            "(default: not set — reservoirs use scale=1.0)"
        ),
    )
    parser.add_argument(
        "--auto-vol-scale",
        dest="auto_vol_scale",
        action="store_true",
        default=False,
        help=(
            "Automatically calculate vol_scale for each reservoir from the PLP "
            "FEscala field: vol_scale = 10^(FEscala - 6). "
            "FEscala is read from plpplem1.dat (CSV format, field 9) when available; "
            "otherwise falls back to plpcnfce.dat Escala (Escala / 1e6). "
            "Explicit --vol-scale entries override auto-calculated values. "
            "Scales are emitted as variable_scales entries in the options section. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--energy-scale",
        dest="energy_scale",
        metavar="SPEC",
        default=None,
        help=(
            "Set battery energy scale values as comma-separated name:value pairs. "
            "Example: --energy-scale 'BESS1:0.01,BESS2:100'. "
            "Emitted as variable_scales entries in the options section. "
            "The scale divides the LP energy variable for numerical conditioning. "
            "(default: not set — batteries use scale=1.0)"
        ),
    )
    parser.add_argument(
        "--auto-energy-scale",
        dest="auto_energy_scale",
        action="store_true",
        default=False,
        help=(
            "Set energy_scale=0.01 for all PLP batteries. This scales the LP "
            "energy variable for better solver numerics. "
            "Explicit --energy-scale entries override this default. "
            "Scales are emitted as variable_scales entries in the options section. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )
    return parser


def _resolve_input_dir(args: argparse.Namespace) -> Path:
    """Resolve input directory from positional and/or -i arguments.

    Priority: -i flag > positional argument > default 'input'.
    """
    if args.input_dir is not None:
        return args.input_dir
    if args.positional_input is not None:
        return args.positional_input
    return Path("input")


def build_options(args: argparse.Namespace) -> dict:
    """Convert parsed CLI arguments to a conversion options dict."""
    input_dir = _resolve_input_dir(args)
    output_file = args.output_file
    if output_file is None:
        output_file = Path(args.output_dir.name).with_suffix(".json")
    name = args.name if args.name is not None else Path(output_file).stem
    input_format = args.input_format if args.input_format else args.output_format
    opts = {
        "input_dir": input_dir,
        "output_dir": args.output_dir,
        "output_file": output_file,
        "last_stage": args.last_stage,
        "last_time": args.last_time,
        "compression": args.compression,
        "output_format": args.output_format,
        "input_format": input_format,
        "hydrologies": args.hydrologies,
        "probability_factors": args.probability_factors,
        "discount_rate": args.discount_rate,
        "management_factor": args.management_factor,
        "zip_output": args.zip_output,
        "excel_output": args.excel_output,
        "excel_file": args.excel_file,
        "name": name,
        "sys_version": args.sys_version,
        "demand_fail_cost": args.demand_fail_cost,
        "scale_objective": args.scale_objective,
        "use_single_bus": args.use_single_bus,
        "use_kirchhoff": args.use_kirchhoff,
        "solver_type": args.solver_type,
        "stages_phase": args.stages_phase,
        "num_apertures": args.num_apertures,
        "aperture_directory": args.aperture_directory,
    }
    if args.cut_sharing_mode is not None:
        opts["cut_sharing_mode"] = args.cut_sharing_mode
    if args.reserve_fail_cost is not None:
        opts["reserve_fail_cost"] = args.reserve_fail_cost
    if args.use_line_losses is not None:
        opts["use_line_losses"] = args.use_line_losses
    if args.boundary_cuts_mode is not None:
        opts["boundary_cuts_mode"] = args.boundary_cuts_mode
    if args.boundary_max_iterations is not None:
        opts["boundary_max_iterations"] = args.boundary_max_iterations
    if args.no_boundary_cuts:
        opts["no_boundary_cuts"] = True
    if args.hot_start_cuts:
        opts["hot_start_cuts"] = True
    if args.vol_scale is not None:
        opts["vol_scale"] = _parse_name_value_pairs(args.vol_scale)
    if args.auto_vol_scale:
        opts["auto_vol_scale"] = True
    if args.energy_scale is not None:
        opts["energy_scale"] = _parse_name_value_pairs(args.energy_scale)
    if args.auto_energy_scale:
        opts["auto_energy_scale"] = True
    return opts


def main():
    """Parse arguments and initiate conversion."""
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    no_args = len(sys.argv) == 1

    parser = make_parser()
    args = parser.parse_args()

    # Reconcile positional and -i input dir
    args.input_dir = _resolve_input_dir(args)

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

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

    try:
        convert_plp_case(build_options(args))
    except (RuntimeError, FileNotFoundError) as exc:
        if no_args:
            print(
                f"error: {exc}\n"
                "Usage: plp2gtopt [INPUT_DIR] -o <output_dir> [options]\n"
                "Run 'plp2gtopt -h' for the full list of options, "
                "or 'plp2gtopt --info <input_dir>' to inspect a case.",
                file=sys.stderr,
            )
            sys.exit(1)
        raise


if __name__ == "__main__":
    main()
