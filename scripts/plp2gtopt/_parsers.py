"""Argument-parser builders for plp2gtopt, grouped by domain.

Each ``add_*_arguments(parser, conf)`` function registers a group of related
CLI flags on the given *parser*.  The *conf* dict carries defaults read from
``~/.gtopt.conf`` (may be empty).

These helpers are composed by :func:`plp2gtopt.main.make_parser`.
"""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import TYPE_CHECKING

from gtopt_config import (
    add_color_argument,
    add_log_level_argument,
    add_version_argument,
)

if TYPE_CHECKING:
    pass


# ---------------------------------------------------------------------------
# Packaged data templates
# ---------------------------------------------------------------------------

# Default RoR-equivalence whitelist shipped inside the plp2gtopt package.
# Resolved relative to _parsers.py so it works for both editable installs
# (where __file__ points back into the source tree) and wheel installs
# (where __file__ points into site-packages/plp2gtopt/).  The file is
# declared in pyproject.toml under [tool.setuptools.package-data] so it is
# included in the built wheel.
DEFAULT_ROR_RESERVOIRS_FILE: Path = (
    Path(__file__).resolve().parent / "templates" / "ror_equivalence.csv"
)


# ---------------------------------------------------------------------------
# Utility type used by several argument groups
# ---------------------------------------------------------------------------


def _parse_time_arg(value: str) -> float:
    """Parse a time argument with optional suffix into hours.

    Supported formats:
        - Plain number: interpreted as hours (e.g. ``8760``)
        - ``Ny`` or ``Ny``: years (x 8760 h), e.g. ``1y``, ``1.5y``
        - ``Nm`` or ``Nm``: months (x 730 h), e.g. ``6m``, ``18m``

    Returns:
        Time value in hours.

    Raises:
        argparse.ArgumentTypeError: If the value cannot be parsed.
    """
    value = value.strip().lower()
    try:
        if value.endswith("y"):
            return float(value[:-1]) * 8760.0
        if value.endswith("m"):
            return float(value[:-1]) * 730.0
        return float(value)
    except ValueError:
        raise argparse.ArgumentTypeError(
            f"invalid time value '{value}': "
            "use a number (hours), or suffixed format like '1y', '6m', '1.5y'"
        ) from None


# ---------------------------------------------------------------------------
# 1. Input / output arguments
# ---------------------------------------------------------------------------


def add_io_arguments(parser: argparse.ArgumentParser, conf: dict[str, str]) -> None:
    """Register input-dir, output-dir, file, format, compression, zip, excel."""
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
        default=None,
        help=(
            "directory for gtopt output files "
            "(default: output, or gtopt_NAME if input is plp_NAME)"
        ),
    )
    parser.add_argument(
        "-f",
        "--output-file",
        type=Path,
        metavar="FILE",
        default=None,
        help="output JSON file path (default: <output-dir>/<output-dir-name>.json)",
    )
    parser.add_argument(
        "-F",
        "--output-format",
        dest="output_format",
        metavar="FORMAT",
        default=conf.get("output_format", "parquet"),
        choices=["parquet", "csv"],
        help="output file format: parquet or csv (default: %(default)s)",
    )
    parser.add_argument(
        "-I",
        "--input-format",
        dest="input_format",
        metavar="FORMAT",
        default=conf.get("input_format"),
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
        default=conf.get("compression", "zstd"),
        help="compression codec for output files (default: %(default)s)",
    )
    parser.add_argument(
        "--compression-level",
        dest="compression_level",
        type=int,
        metavar="N",
        default=int(conf.get("compression_level", "1")) or None,
        help=(
            "compression level for the codec, e.g. 1-22 for zstd "
            "(default: %(default)s; 0 or omitted = codec default)"
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


# ---------------------------------------------------------------------------
# 2. Stage / time selection arguments
# ---------------------------------------------------------------------------


def add_stage_arguments(parser: argparse.ArgumentParser, _conf: dict[str, str]) -> None:
    """Register stage/time selection arguments."""
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
        "-t",
        "--last-time",
        dest="last_time",
        type=_parse_time_arg,
        metavar="TIME",
        default=-1,
        help=(
            "include stages up to this accumulated time.  "
            "Accepts hours (plain number), or suffixed values: "
            "'1y' = 1 year (8760 h), '6m' = 6 months (4380 h), "
            "'1.5y' = 1.5 years (13140 h).  "
            "The last stage whose cumulative block duration reaches "
            "this threshold is included.  "
            "Use -s N for stage-count selection instead. "
            "(default: all stages)"
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


# ---------------------------------------------------------------------------
# 3. Scenario / hydrology arguments
# ---------------------------------------------------------------------------


def add_scenario_arguments(
    parser: argparse.ArgumentParser, _conf: dict[str, str]
) -> None:
    """Register hydrology/scenario selection and aperture arguments."""
    parser.add_argument(
        "-y",
        "--hydrologies",
        dest="hydrologies",
        metavar="SPEC",
        default="all",
        help=(
            "Simulation/hydrology scenario selector using 1-based (Fortran) "
            "indices.  Accepts 'all' (default), 'first' (first available), "
            "a single index, comma-separated values, or ranges: "
            "'51', '51,52', '51,52,55-60'. "
            "Use '--info' to list available scenarios. "
            "When plpidsim.dat is present the indices are the hydrology "
            "columns from plpidsim.dat; otherwise raw 1-based hydrology "
            "column indices from plpaflce.dat.  (default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--first-scenario",
        action="store_true",
        default=False,
        help=(
            "Select only the first available scenario (equivalent to "
            "'-y first').  Safe shortcut that works regardless of the "
            "case's hydrology numbering."
        ),
    )
    parser.add_argument(
        "--show-simulation",
        action="store_true",
        default=False,
        help=(
            "After conversion, print a detailed summary of the simulation "
            "structure: scenarios, stages, phases, apertures, and their "
            "relationships."
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


# ---------------------------------------------------------------------------
# 4. Solver / SDDP arguments
# ---------------------------------------------------------------------------


def add_solver_arguments(parser: argparse.ArgumentParser, conf: dict[str, str]) -> None:
    """Register solver type, cut-sharing, boundary cuts, convergence args."""
    parser.add_argument(
        "-S",
        "--solver",
        dest="solver_type",
        metavar="TYPE",
        default=conf.get("solver_type", "sddp"),
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
        help=(
            "Disable boundary-cut export entirely "
            "(equivalent to --boundary-cuts-mode=noload)."
        ),
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
            "named_cuts_file to warm-start all phases."
        ),
    )
    parser.add_argument(
        "--stationary-tol",
        dest="stationary_tol",
        metavar="TOL",
        type=float,
        default=None,
        help=(
            "Secondary convergence tolerance for stationary-gap detection. "
            "When the relative change in the SDDP gap over the last "
            "--stationary-window iterations falls below this value, the "
            "solver declares convergence even if gap > --convergence-tol. "
            "This handles problems where the gap converges to a non-zero "
            "stationary value rather than to 0. "
            "Example: 0.01 declares convergence when the gap improves by "
            "less than 1%% over the look-back window. "
            "Default: not set (secondary criterion disabled)."
        ),
    )
    parser.add_argument(
        "--stationary-window",
        dest="stationary_window",
        metavar="N",
        type=int,
        default=None,
        help=(
            "Number of iterations to look back when checking gap stationarity "
            "(secondary convergence criterion). "
            "Only used when --stationary-tol is set. "
            "Default: 10."
        ),
    )


# ---------------------------------------------------------------------------
# 5. Model / scaling arguments
# ---------------------------------------------------------------------------


def add_model_arguments(parser: argparse.ArgumentParser, conf: dict[str, str]) -> None:
    """Register demand-fail-cost, scale factors, kirchhoff, line losses, etc."""
    parser.add_argument(
        "--demand-fail-cost",
        dest="demand_fail_cost",
        type=float,
        metavar="COST",
        default=float(conf.get("demand_fail_cost", "1000.0")),
        help="cost penalty for demand curtailment in $/MWh (default: %(default)s)",
    )
    parser.add_argument(
        "--state-fail-cost",
        dest="state_fail_cost",
        type=float,
        metavar="COST",
        default=float(conf.get("state_fail_cost", "1000.0")),
        help="penalty for state variable deviations in $/MWh (default: %(default)s)",
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
        default=float(conf.get("scale_objective", "1000.0")),
        help=("objective function scaling factor. (default: %(default)s)"),
    )
    parser.add_argument(
        "--scale-theta",
        dest="scale_theta",
        type=float,
        metavar="FACTOR",
        default=None,
        help=(
            "voltage-angle scale factor (1/ScaleAng). "
            "Only emitted when explicitly set; C++ auto_scale_theta "
            "computes from median line reactance by default."
        ),
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
        action=argparse.BooleanOptionalAction,
        default=True,
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


# ---------------------------------------------------------------------------
# 6. Reservoir / battery / variable-scale arguments
# ---------------------------------------------------------------------------


def add_reservoir_battery_arguments(
    parser: argparse.ArgumentParser, conf: dict[str, str]
) -> None:
    """Register reservoir-scale, battery-scale, variable-scales, soft-emin."""
    parser.add_argument(
        "--reservoir-scale-mode",
        dest="reservoir_scale_mode",
        choices=["plp", "auto"],
        default=conf.get("reservoir_scale_mode", "auto"),
        help=(
            "How to determine the reservoir energy_scale factor. "
            "'auto' (default): delegate to C++ auto_scale mode which computes "
            "energy_scale = pow(10, floor(log10(capacity))). "
            "'plp': compute from PLP FEscala field (= EmbFEsc / 1E6) "
            "and set explicit energy_scale per reservoir. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--reservoir-energy-scale",
        dest="reservoir_energy_scale",
        metavar="SPEC",
        default=None,
        help=(
            "Override reservoir energy scale for specific reservoirs as "
            "comma-separated name:value pairs. "
            "Example: --reservoir-energy-scale 'RAPEL:500,COLBUN:15000'. "
            "These explicit values override auto-calculated scales. "
            "Emitted as variable_scales entries in the options section. "
            "(default: not set — gtopt auto-scales from emax)"
        ),
    )
    parser.add_argument(
        "--auto-reservoir-energy-scale",
        dest="auto_reservoir_energy_scale",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "Emit reservoir energy_scale as variable_scales entries computed "
            "from PLP FEscala / Escala fields. "
            "OFF by default — gtopt auto-scales reservoirs from emax. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--battery-energy-scale",
        dest="battery_energy_scale",
        metavar="SPEC",
        default=None,
        help=(
            "Override battery energy scale for specific batteries as "
            "comma-separated name:value pairs. "
            "Example: --battery-energy-scale 'BESS1:0.01,BESS2:100'. "
            "These explicit values override auto-calculated scales. "
            "Emitted as variable_scales entries in the options section. "
            "(default: not set — gtopt auto-scales from emax)"
        ),
    )
    parser.add_argument(
        "--auto-battery-energy-scale",
        dest="auto_battery_energy_scale",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "Emit battery energy_scale=0.01 as variable_scales entries. "
            "OFF by default — gtopt auto-scales from emax. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--clamp-battery-efficiency",
        dest="clamp_battery_efficiency",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "Clamp battery input_efficiency and output_efficiency values "
            "to a maximum of 1.0. Values above 1.0 are always warned about; "
            "this option controls whether they are also clamped in the output. "
            "Use --no-clamp-battery-efficiency to pass through raw values. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "-X",
        "--variable-scales-file",
        dest="variable_scales_file",
        type=Path,
        metavar="FILE",
        default=None,
        help=(
            "JSON file containing an array of VariableScale objects to merge "
            "into the variable_scales option. Each object must have: "
            "class_name, variable, uid, scale. "
            "File entries have LOWEST priority: auto-calculated and "
            "--reservoir-energy-scale/--battery-energy-scale values override them. "
            "(default: not set)"
        ),
    )
    parser.add_argument(
        "--variable-scales-template",
        action="store_true",
        default=False,
        help=(
            "print a JSON template of variable_scales entries computed from "
            "the PLP case (FEscala for reservoirs, 0.01 for batteries). "
            "The template includes _name and _fescala comment fields. "
            "Edit the output and pass it back via --variable-scales-file. "
            "Example workflow:\n"
            "  plp2gtopt -i plp_case --variable-scales-template > scales.json\n"
            "  # edit scales.json to adjust specific scales\n"
            "  plp2gtopt -i plp_case --variable-scales-file scales.json"
        ),
    )
    parser.add_argument(
        "--soft-emin-cost",
        dest="soft_emin_cost",
        type=float,
        metavar="COST",
        default=0.1,
        help=(
            "default penalty cost [$/dam3] for the soft minimum volume "
            "constraint (plpminembh.dat).  Per-stage costs from the file "
            "override this default.  Set to 0 to disable soft emin. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--embed-reservoir-constraints",
        dest="embed_reservoir_constraints",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "embed seepage, discharge_limit, and production_factor arrays "
            "inside each reservoir definition instead of using system-level "
            "reservoir_seepage_array / reservoir_discharge_limit_array / "
            "reservoir_production_factor_array.  The embedded form requires "
            "expand_reservoir_constraints() at load time. "
            "(default: %(default)s)"
        ),
    )


# ---------------------------------------------------------------------------
# 6b. RoR-as-reservoirs equivalence arguments
# ---------------------------------------------------------------------------


def add_ror_arguments(parser: argparse.ArgumentParser, _conf: dict[str, str]) -> None:
    """Register ``--ror-as-reservoirs`` and ``--ror-as-reservoirs-file``.

    These options let a user promote selected ``pasada`` / ``serie`` PLP
    centrals to **daily-cycle reservoirs** (mirroring the ESS DCMod=2
    regulation-tank pattern).  The feature is strictly whitelist-gated:
    a central can only be promoted if its name appears in the CSV file
    passed via ``--ror-as-reservoirs-file`` (so we never invent a vmax).
    """
    parser.add_argument(
        "--ror-as-reservoirs",
        dest="ror_as_reservoirs",
        metavar="SELECTION",
        default=None,
        help=(
            "promote run-of-river (pasada/serie) centrals to daily-cycle "
            "reservoirs.  SELECTION is 'all', 'none', or a comma-separated "
            "list of central names (e.g. 'CentralA,CentralB').  Requires "
            "--ror-as-reservoirs-file; only centrals whose vmax is listed "
            "in that CSV are eligible.  (default: feature disabled)"
        ),
    )
    parser.add_argument(
        "--ror-as-reservoirs-file",
        dest="ror_as_reservoirs_file",
        type=Path,
        metavar="FILE",
        default=DEFAULT_ROR_RESERVOIRS_FILE,
        help=(
            "CSV file mapping central names to daily-cycle vmax [hm3]. "
            "Required columns: name, vmax_hm3.  Optional columns: "
            "enabled (true/false), comment.  Only centrals whose vmax "
            "is known should be listed here — this file is the sole "
            "source of truth for --ror-as-reservoirs.  See "
            "docs/templates/ror_equivalence.example.csv for the schema. "
            "(default: packaged template at plp2gtopt/templates/"
            "ror_equivalence.csv)"
        ),
    )


# ---------------------------------------------------------------------------
# 7. Technology detection arguments
# ---------------------------------------------------------------------------


def add_tech_arguments(parser: argparse.ArgumentParser, _conf: dict[str, str]) -> None:
    """Register pasada-mode, tech-detect, tech-overrides, tech-list."""
    parser.add_argument(
        "--pasada-mode",
        dest="pasada_mode",
        choices=["auto", "hydro", "flow-turbine", "profile"],
        default="auto",
        help=(
            "how to model pasada (run-of-river) centrals: "
            "'auto' = per-central: solar/wind -> profile, hydro -> flow+turbine; "
            "'hydro' = full topology (junctions, waterways, turbines, flows); "
            "'flow-turbine' = all pasada as simplified flow + turbine; "
            "'profile' = all pasada as generator profiles. "
            "(default: %(default)s)"
        ),
    )
    # Backward compatibility aliases
    parser.add_argument(
        "--pasada-hydro",
        dest="pasada_mode",
        action="store_const",
        const="hydro",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--no-pasada-hydro",
        dest="pasada_mode",
        action="store_const",
        const="profile",
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--tech-detect",
        dest="auto_detect_tech",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "auto-detect generator technology from central names. "
            "Refines PLP types (termica, pasada) into specific types "
            "(solar, wind, gas, coal, etc.) by scanning names for "
            "keywords. Use --tech-detect to enable. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--tech-overrides",
        dest="tech_overrides",
        metavar="SPEC",
        default=None,
        help=(
            "override generator technology types as comma-separated "
            "name:type pairs (e.g. 'SolarAlmeyda:solar,Canela:wind') "
            "or a path to a .json/.csv file with overrides. "
            "These take priority over auto-detection."
        ),
    )
    parser.add_argument(
        "--tech-list",
        action="store_true",
        default=False,
        help="list known technology types and exit",
    )


# ---------------------------------------------------------------------------
# 8. General / miscellaneous arguments
# ---------------------------------------------------------------------------


def add_general_arguments(
    parser: argparse.ArgumentParser, conf: dict[str, str]
) -> None:
    """Register name, discount-rate, management-factor, logging, misc flags."""
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
        "-d",
        "--discount-rate",
        dest="discount_rate",
        type=float,
        metavar="RATE",
        default=float(conf.get("discount_rate", "0.0")),
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
    add_log_level_argument(parser)
    parser.add_argument(
        "--log",
        dest="log_file",
        metavar="FILE",
        default=None,
        help=(
            "write detailed DEBUG-level log to FILE "
            "(default: <output_dir>/logs/conversion.log)"
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
        "--validate",
        action="store_true",
        default=False,
        help=(
            "parse all PLP files and report element counts and any errors, "
            "without writing any output files; exits with code 0 if valid, "
            "1 if errors are found"
        ),
    )
    parser.add_argument(
        "--emit-water-rights",
        dest="emit_water_rights",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "emit canonical Stage-1 irrigation agreement JSON files "
            "(laja.json, maule.json) from plplajam.dat / plpmaulen.dat. "
            "The Stage-2 transform (FlowRight/VolumeRight/UserConstraint "
            "entities and the companion PAMPL file) is now handled "
            "exclusively by `gtopt_irrigation` — run it on the dumped "
            "JSON to get the rights entities. (default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--check",
        dest="run_check",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "run post-conversion validation via gtopt_check_json: prints "
            "system statistics, a PLP-vs-gtopt element comparison, and "
            "basic consistency checks. Use --no-check to disable. "
            "(default: enabled)"
        ),
    )
    add_color_argument(parser)
    parser.add_argument(
        "--init-config",
        action="store_true",
        default=False,
        help="initialize [plp2gtopt] section in ~/.gtopt.conf with defaults",
    )
    add_version_argument(parser)
