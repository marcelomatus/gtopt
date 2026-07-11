#!/usr/bin/env python3
"""Main entry point for PLP to GTOPT conversion."""

import argparse
import logging
import sys
from pathlib import Path

from gtopt_config import DEFAULT_CONFIG_PATH, get_version, load_config, save_section
from gtopt_shared.cli_signals import (  # pylint: disable=unused-import
    install_termination_handlers,
    signal_handler,  # noqa: F401  re-export for back-compat / tests
)
from gtopt_shared.state_snapshot import (
    write_plp2gtopt_readme,
    write_state_snapshot,
)

from ._fast_path import FAST_PATH_METHODS, apply_iterative_fast_path
from .plp2gtopt import (
    convert_plp_case,
    print_pumped_storage_template,
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


DEFAULT_LIFT_LINE_CAPS_FACTOR = 2.0


def _parse_lift_line_caps(spec: str) -> dict[str, float]:
    """Parse a --lift-line-caps spec into ``{line_name: factor}``.

    Accepts either ``L1:F1,L2:F2`` (per-line override factor) or
    ``L1,L2`` (uses the default factor 2.0). The factor multiplies
    ``tmax_ab`` to produce ``loss_envelope``.

    Raises ``ValueError`` if a numeric factor cannot be parsed.
    """
    result: dict[str, float] = {}
    for token in spec.split(","):
        token = token.strip()
        if not token:
            continue
        if ":" in token:
            name, val_str = token.split(":", maxsplit=1)
            name = name.strip()
            try:
                result[name] = float(val_str.strip())
            except ValueError as exc:
                raise ValueError(
                    f"Invalid factor in --lift-line-caps token '{token}': {exc}"
                ) from exc
        else:
            result[token] = DEFAULT_LIFT_LINE_CAPS_FACTOR
    return result


# ``signal_handler`` is re-exported from gtopt_shared.cli_signals at the
# top of this module.  Existing imports from ``plp2gtopt.main`` continue
# to work.


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

    parser.add_argument(
        "--solver-invariant",
        dest="solver_invariant",
        action="store_true",
        help=(
            "SDDP/cascade only: configure the forward+backward LP solves for "
            "barrier WITHOUT crossover (crossover=false) and retune the "
            "bundled cplex.prm to match.  Makes the solve reproducible across "
            "memory-saving modes (low_memory off == compress): barrier reaches "
            "the unique analytic-center point and the cut path keeps the unique "
            "interior duals, so the degenerate-vertex selection that dual "
            "simplex suffers (different equal-cost trajectory off vs compress) "
            "cannot occur.  ~2x slower per solve and yields a different but "
            "equally valid solution than the dual-simplex default."
        ),
    )
    parser.add_argument(
        "--from-state",
        dest="from_state",
        type=Path,
        default=None,
        metavar="PATH",
        help=(
            "load every parsed argument from a prior run's "
            "``plp2gtopt_state.json`` snapshot (written automatically to "
            "every output directory).  Reproduces the original invocation "
            "byte-for-byte unless overriden by an explicit flag on the "
            "current command line (e.g. ``--from-state foo/plp2gtopt_state.json "
            "-o new_output_dir`` reuses every option except output dir).  "
            "See <output_dir>/README.md for the snapshot format."
        ),
    )

    return parser


# Keys and hard-coded defaults for the [plp2gtopt] config section.
#
# IMPORTANT: do NOT add ``demand_fail_cost`` here.  When unset (``None``)
# the parser auto-derives the value from the average first-tier FALLA
# gcost in plpcnfce.dat.  Writing ``demand_fail_cost = 0.0`` to the conf
# disables that auto-detection silently — see _parsers.py:add_model_arguments.
_SECTION_DEFAULTS: dict[str, str] = {
    "compression": "zstd",
    # "zstd" matches the gtopt C++ default
    # (`PlanningOptionsLP::default_output_compression`) and is a single
    # unambiguous codec every downstream reader (pandas, polars, duckdb,
    # Power BI / Power Query) opens natively — unlike "lz4", whose
    # deprecated Hadoop-framed variant is frequently unreadable.  We do
    # NOT set a default compression_level here (0 / omitted = the codec's
    # own default); callers who want a higher zstd archival level must
    # pass `--compression-level 1..22` explicitly.
    "output_format": "parquet",
    "input_format": "parquet",
    # On-disk layout for the per-element field Parquet files.  "long"
    # (default) emits the tidy `[<index cols>, uid, value]` shape — read
    # natively by gtopt (auto-sniffed) and ideal for Power BI / Power
    # Query, which prefer long tables and need no unpivot step.  "wide"
    # restores the legacy one-column-per-uid (`uid:N`) shape.
    "layout": "long",
    # `cascade` is the production-grade default since 2026-05-15.  See
    # ``_parsers.py``'s ``--method`` default for the rationale.
    "method": "cascade",
    "state_fail_cost": "1000.0",
    # Default 1.0 (was 1000): with the production-default `cascade` method,
    # scale_objective=1 keeps the LP basis well-conditioned (it is the dominant
    # kappa contributor once Benders cuts accumulate) and matches the
    # method-aware C++ default for sddp/cascade.  The writer still remaps a
    # legacy explicit 1000 to 1.0 for sddp/cascade and preserves 1000 for
    # explicit monolithic requests.
    "scale_objective": "1.0",
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


def _apply_plp_legacy_bundle(args: argparse.Namespace) -> None:
    """Apply --plp-legacy defaults in place, honouring explicit flags.

    The bundle substitutes defaults so that gtopt output is as close to
    PLP as possible, at the cost of LP quality / size.  Explicit user
    flags take precedence — we only override values that the user
    did not set on the command line.

    Bundle table (see --plp-legacy help):

      | Option           | Default (normal)       | --plp-legacy       |
      |------------------|------------------------|--------------------|
      | method           | sddp                   | sddp (=)           |
      | line_losses_mode | unset (→adaptive)      | piecewise_direct   |
      | use_line_losses  | unset (→gtopt true)    | true (explicit)    |
      | pasada_mode      | auto                   | flow-turbine       |
      | use_kirchhoff    | true                   | true (=)           |
      | discount_rate    | 0.0                    | 0.0 (=)            |

    `=` marks no-op bundle entries: gtopt's normal default already
    matches PLP so no change is needed.  `reservoir_scale_mode` is
    intentionally left alone (user preference).

    Why pasada_mode = flow-turbine under --plp-legacy: PLP models every
    `pasada` (run-of-river) central as an independent turbine driven by
    its own afluent — there is no per-RoR junction / waterway / reservoir
    chain because `pasada` centrals have no upstream/downstream hydro
    relations in PLP.  The `auto` default runs name-based tech detection
    and may divert some pasadas to generator-profile mode (solar/wind
    look-alikes); under PLP-compat we always want flow+turbine so the
    LP topology matches PLP exactly.
    """
    if not args.plp_legacy:
        return

    # argparse stores the final parsed value regardless of whether it
    # came from the CLI or the default.  Inspect sys.argv directly to
    # tell "user typed --method" apart from "default was sddp".
    explicit_flags = {a.split("=", 1)[0] for a in sys.argv[1:]}
    explicit_method = {"-M", "--method"} & explicit_flags
    explicit_losses = {"--line-losses-mode"} & explicit_flags
    explicit_use_losses = {"-L", "--use-line-losses"} & explicit_flags
    explicit_pasada = {"--pasada-mode", "--pasada-hydro", "--no-pasada-hydro"} & (
        explicit_flags
    )

    applied: list[str] = []
    if not explicit_method and args.method != "sddp":
        args.method = "sddp"
        applied.append("method=sddp")
    if not explicit_losses and args.line_losses_mode != "piecewise_direct":
        args.line_losses_mode = "piecewise_direct"
        applied.append("line_losses_mode=piecewise_direct")
    if not explicit_use_losses and args.use_line_losses is None:
        # Force explicit `true` in the JSON so the PLP-compat intent is
        # self-documenting, even though the gtopt default is also true.
        args.use_line_losses = True
        applied.append("use_line_losses=true")
    if not explicit_pasada and args.pasada_mode != "flow-turbine":
        # PLP has no tech detection — every pasada is a hydro turbine
        # driven by its afluent, regardless of the central's name.
        args.pasada_mode = "flow-turbine"
        applied.append("pasada_mode=flow-turbine")

    # PLP's EmbVMin / EmbVFin are HARD bounds (leemanem.f / volfinem.f,
    # verified against the PLP-written last-stage LP), so --plp-legacy keeps
    # `soft_storage_bounds` OFF — hard ``vol_end >= efin`` / ``vol >= emin``.
    explicit_ssb = {
        "--soft-storage-bounds",
        "--no-soft-storage-bounds",
    } & explicit_flags
    if not explicit_ssb and args.soft_storage_bounds:
        args.soft_storage_bounds = False
        applied.append("soft_storage_bounds=false")

    if applied:
        logging.info("--plp-legacy: applying %s", ", ".join(applied))


def build_options(args: argparse.Namespace) -> dict:
    """Convert parsed CLI arguments to a conversion options dict."""
    _apply_plp_legacy_bundle(args)
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
        "layout": getattr(args, "layout", "long"),
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
        "method": args.method,
        # plp2gtopt-meta (consumed at install + fast-path; never emitted to
        # the gtopt JSON, which is built explicitly from this config dict).
        "solver_invariant": getattr(args, "solver_invariant", False),
        # Forwarded to ``options.write_out`` (see
        # ``gtopt_writer.write_planning_options``).  ``getattr`` with the
        # canonical default keeps in-tree fixtures that build a minimal
        # Namespace (test_main_coverage.py) working without rewiring.
        "write_out": getattr(args, "write_out", None),
        # cascade-reduced runtime knobs.  ``getattr`` with defaults makes
        # this resilient to test fixtures that build a minimal Namespace
        # by hand (e.g. test_main_coverage.py) — the actual CLI parser
        # always populates every attr from add_solver_arguments.
        "cascade_reduced_opts": {
            "l1_reduce_ratio": getattr(args, "cascade_l1_reduce_ratio", 6),
            "l2_reduce_ratio": getattr(args, "cascade_l2_reduce_ratio", 3),
            "l1_min_buses": getattr(args, "cascade_l1_min_buses", 4),
            "l2_min_buses": getattr(args, "cascade_l2_min_buses", 8),
            "l1_uplift_pct": getattr(args, "cascade_l1_uplift_pct", 3.0),
            "l2_uplift_pct": getattr(args, "cascade_l2_uplift_pct", 3.0),
            "l1_uplift_collision": getattr(
                args, "cascade_l1_uplift_collision", "replace"
            ),
            "l2_uplift_collision": getattr(
                args, "cascade_l2_uplift_collision", "replace"
            ),
            "l1_aperture_ratio": getattr(args, "cascade_l1_aperture_ratio", 4),
            "l2_aperture_ratio": getattr(args, "cascade_l2_aperture_ratio", 2),
            "l1_distance": getattr(
                args, "cascade_l1_distance", "reactance-shortest-path"
            ),
            "l2_distance": getattr(args, "cascade_l2_distance", "ptdf"),
            "disable_l1": getattr(args, "cascade_disable_l1", False),
            "disable_l2": getattr(args, "cascade_disable_l2", False),
        },
        "stages_phase": args.stages_phase,
        "num_apertures": args.num_apertures,
        "aperture_directory": args.aperture_directory,
        # ``--inflow-model ar1`` (default None = off): estimate a
        # per-central AR(1) inflow model from the plpaflce hydrology
        # ensemble and attach it to each generated Flow element.  See
        # inflow_model.py and docs/formulation/sddp-ar-inflows.md.
        "inflow_model": getattr(args, "inflow_model", None),
        # PLP-faithful soft volume bounds: routes per-reservoir efin
        # through the C++ ``Reservoir.efin_cost`` slack and per-stage
        # maintenance emin through the soft_emin / soft_emin_cost slack
        # mechanism instead of hard constraints.  See add_model_arguments
        # in _parsers.py for cost-source priority.  Default True;
        # ``--plp-legacy`` also enforces True.
        "soft_storage_bounds": args.soft_storage_bounds,
        # Cap on the spillage-cost source used for ``efin_cost`` /
        # ``soft_emin_cost`` when ``--soft-storage-bounds`` is on.
        # Caps the per-reservoir vrebemb / global CVert before it
        # becomes a slack price; 0 disables the cap.
        "vert_cost_cap": args.vert_cost_cap,
        # ``--drop-spillway-waterway`` (default False, opt-in): when on,
        # suppress every ``_ver`` waterway and rely on junction-level
        # drain to shed surplus water.  See JunctionWriter._process_central.
        "drop_spillway_waterway": args.drop_spillway_waterway,
        # ``--vrebemb-as-sink`` (default False, opt-in): for centrals in
        # plpvrebemb.dat, route ``_ver`` to a synthetic ocean drain and drop
        # ``fmax``/``fcost``.  See JunctionWriter._process_central.
        "vrebemb_as_sink": args.vrebemb_as_sink,
        # ``--reservoir-flow-estimate`` (default True): topology-driven
        # per-reservoir extraction-flow bounds (gtopt_shared.reservoir_flow).
        # ``getattr`` keeps minimal-Namespace test fixtures working.
        "reservoir_flow_estimate": getattr(args, "reservoir_flow_estimate", True),
        # Plexos overlay: source of heat-rate / Fuel data to merge into
        # the PLP-derived planning.  Resolved by _plexos_overlay.
        "plexos_overlay": getattr(args, "plexos_overlay", None),
        "plexos_overlay_report": getattr(args, "plexos_overlay_report", None),
        # IPCC defaults fill-in for missing CO2 emission factors on Fuel
        # elements (after the PLEXOS overlay).  Master switch +
        # optional file / report overrides.  Resolved by
        # ``gtopt_shared.emissions``.
        "emissions": getattr(args, "emissions", False),
        "emissions_file": getattr(args, "emissions_file", None),
        "emissions_report": getattr(args, "emissions_report", None),
        # ``--only-emissions`` (issue #519) implies ``--emissions``
        # and stamps the carbon price + objective_mode = "emissions"
        # on the planning JSON so gtopt runs the pure-emissions LP.
        "only_emissions": getattr(args, "only_emissions", False),
        "carbon_price": getattr(args, "carbon_price", None),
        # Synthetic emissions ray (#520) — used by gtopt_writer when
        # --only-emissions is set to build the boundary_cuts.csv.
        "emissions_discount_rate": getattr(args, "emissions_discount_rate", 0.05),
        "emissions_horizon_years": getattr(args, "emissions_horizon_years", None),
    }
    # Model-specific options nested under model_options.
    model_opts: dict = {
        "demand_fail_cost": args.demand_fail_cost,
        # Renamed per §11.10 (docs/analysis/naming-conventions.md): the
        # gtopt canonical option is `state_violation_cost`; the legacy
        # `state_fail_cost` JSON key is still accepted via the
        # naming-dialects registry for back-compat. CLI arg keeps the
        # legacy `--state-fail-cost` spelling for unchanged user
        # invocation.
        "state_violation_cost": args.state_fail_cost,
        "scale_objective": args.scale_objective,
        "use_single_bus": args.use_single_bus,
        "use_kirchhoff": args.use_kirchhoff,
        # Default to the node-angle (B–θ) KVL formulation: it mirrors PLP's
        # voltage-angle model and, although the LP is slightly larger (one θ
        # per bus + one KVL row per line vs one per fundamental cycle), its
        # sparse per-line rows solve faster under CPLEX than the dense
        # per-cycle loop rows of cycle_basis (see --kirchhoff-mode help).
        "kirchhoff_mode": args.kirchhoff_mode,
    }
    if args.scale_theta is not None:
        model_opts["scale_theta"] = args.scale_theta
    elif args.use_kirchhoff and args.kirchhoff_mode == "node_angle":
        # node_angle default: pin scale_theta=1.0 (no θ auto-scaling).  On the
        # CEN65 full-network LP the auto θ-scale (median X/V² ≈ 2.7e-4) shrinks
        # the angle columns ~3700×, which skews the matrix and forces ruiz
        # equilibration to compensate — net slower.  The raw per-line KVL
        # matrix is already well-conditioned (X/V² ~ 1e-4…1e-2), so leaving θ
        # unscaled lets CPLEX barrier solve it fastest (0.27s vs 0.44s scaled).
        model_opts["scale_theta"] = 1.0
    if args.reserve_fail_cost is not None:
        # §11.10 rename: gtopt canonical is `reserve_shortage_cost`;
        # legacy `reserve_fail_cost` JSON key still accepted via the
        # naming-dialects registry.  CLI arg keeps the legacy spelling.
        model_opts["reserve_shortage_cost"] = args.reserve_fail_cost
    if args.use_line_losses is not None:
        model_opts["use_line_losses"] = args.use_line_losses
    if args.line_losses_mode is not None:
        model_opts["line_losses_mode"] = args.line_losses_mode
    if getattr(args, "loss_cost_eps", None) is not None:
        model_opts["loss_cost_eps"] = args.loss_cost_eps
    opts["model_options"] = model_opts

    if getattr(args, "aperture_chunk_size", None) is not None:
        sddp_opts = opts.setdefault("sddp_options", {})
        sddp_opts["aperture_chunk_size"] = args.aperture_chunk_size

    # Iterative fast-path defaults (benchmarked plp2gtopt -> gtopt pipeline,
    # ~PLP parity on the CEN65 2-year case): provably-zero LP-column elision
    # (``lp_reduction``, ~-19% wall) + dual aperture warm-start
    # (``aperture_solve_mode = warm`` with ``aperture_chunk_size = 0`` — auto,
    # parallel per-aperture chunks) + dual simplex on the forward/backward
    # passes with an advanced (warm) basis.  The ``piecewise_direct`` loss
    # model is the default already (see ``line_losses_mode`` above).  All are
    # overridable.  Applied to BOTH ``sddp`` and ``cascade``: these top-level
    # ``sddp_options`` become the cascade base options (``m_base_opts_`` in
    # cascade_method.cpp), which every cascade level copies wholesale via
    # ``build_level_sddp_opts`` (it never overrides aperture/solver fields), so
    # the same warm/dual/reduction config flows through L0..L3.  The bundled
    # ``cplex.prm`` is rewritten to dual / no-presolve for these methods (see
    # ``install_solver_param_files``); without that the prm's ``LPMethod 4``
    # (barrier) would override the JSON algorithm since it is read last.
    if args.method in FAST_PATH_METHODS:
        sddp_opts = opts.setdefault("sddp_options", {})
        apply_iterative_fast_path(
            model_opts,
            sddp_opts,
            invariant=getattr(args, "solver_invariant", False),
        )
        # PLP-faithful cut sharing: each scene-LP carries N dedicated
        # future-cost columns (varphi_0..N-1) and scenario-s's backward
        # cut lands on varphi_s in every scene-LP, priced 1/N — matching
        # PLP's source-scenario indexing (plp-agrespd.f:94) + 1/N
        # averaging (defprbpd.f:810).  Replaces the prior `none` default
        # (no sharing).  An explicit --cut-sharing-mode wins (applied
        # below, where args.cut_sharing_mode is not None).  Set on the
        # top-level opts dict (CLI plumbing); the writer threads it into
        # sddp_options.cut_sharing_mode.
        opts.setdefault("cut_sharing_mode", "multicut")
    if getattr(args, "lift_line_caps", None):
        opts["lift_line_caps"] = _parse_lift_line_caps(args.lift_line_caps)

    if args.cut_sharing_mode is not None:
        opts["cut_sharing_mode"] = args.cut_sharing_mode
    # `getattr` defensively — hand-built Namespaces in tests may omit it.
    if getattr(args, "forward_sampling_mode", None) is not None:
        opts["forward_sampling_mode"] = args.forward_sampling_mode
    if getattr(args, "integer_cuts_mode", None) is not None:
        opts["integer_cuts_mode"] = args.integer_cuts_mode
    if args.boundary_cuts_mode is not None:
        opts["boundary_cuts_mode"] = args.boundary_cuts_mode
    if args.boundary_max_iterations is not None:
        opts["boundary_max_iterations"] = args.boundary_max_iterations
    opts["cuts_govern_terminal"] = getattr(args, "cuts_govern_terminal", True)
    if args.no_boundary_cuts:
        opts["no_boundary_cuts"] = True
        # Keep the gtopt JSON consistent with the missing CSV: an
        # explicit user-set boundary_cuts_mode wins; otherwise force
        # `noload` so the solver doesn't try to load a file we never
        # wrote.
        opts.setdefault("boundary_cuts_mode", "noload")
    # ``--hot-start-cuts`` was retired in 2026-05 alongside
    # ``write_hot_start_cuts_csv``; internal hot-start cuts now travel
    # via the typed Parquet path (``cuts_input_file``).
    if args.alias_file is not None:
        opts["alias_file"] = args.alias_file
    if args.stationary_tol is not None:
        opts["stationary_tol"] = args.stationary_tol
    if args.stationary_window is not None:
        opts["stationary_window"] = args.stationary_window
    # `getattr` defensively — `test_main_coverage` builds Namespaces by
    # hand and may not declare these attrs.  Defaults flow through the
    # writer-side fallback (gtopt_writer.process_options).
    if getattr(args, "min_iterations", None) is not None:
        opts["min_iterations"] = args.min_iterations
    if getattr(args, "convergence_confidence", None) is not None:
        opts["convergence_confidence"] = args.convergence_confidence
    if getattr(args, "stationary_gap_ceiling", None) is not None:
        opts["stationary_gap_ceiling"] = args.stationary_gap_ceiling
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
    opts["plp_legacy"] = args.plp_legacy
    # Auto water-shortfall pricing (see plp2gtopt._water_value).
    opts["auto_water_fail_cost"] = getattr(args, "auto_water_fail_cost", False)
    opts["water_fail_cost"] = getattr(args, "water_fail_cost", None)
    if getattr(args, "disable_discharge_limit_for", None):
        opts["disable_discharge_limit_for"] = args.disable_discharge_limit_for
    # ``--pmin-as-flowright`` may be:
    #   - absent (None): feature disabled
    #   - flag without value (""): use bundled default CSV
    #   - a CSV path on disk: use that whitelist
    #   - a comma-separated list of names: use those names
    if getattr(args, "pmin_as_flowright", None) is not None:
        opts["pmin_as_flowright"] = args.pmin_as_flowright
    if getattr(args, "flow_right_fail_cost", None) is not None:
        opts["flow_right_fail_cost"] = args.flow_right_fail_cost
    opts["expand_water_rights"] = args.expand_water_rights
    opts["irrigation_couplings"] = getattr(args, "irrigation_couplings", True)
    opts["expand_lng"] = args.expand_lng
    opts["expand_ror"] = args.expand_ror
    ps_files = getattr(args, "pumped_storage_files", None)
    if ps_files:
        opts["pumped_storage_files"] = [Path(p) for p in ps_files]
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
    install_termination_handlers()

    no_args = len(sys.argv) == 1

    parser = make_parser()
    args = parser.parse_args(argv)

    # ``--from-state PATH`` reload: rebuild args from a prior snapshot
    # so the current invocation reproduces the original byte-for-byte
    # (modulo any explicit overrides on the current CLI line).  The
    # snapshot file is written at the END of every plp2gtopt run to
    # ``<output_dir>/plp2gtopt_state.json`` — see the per-output
    # ``README.md`` for the format.
    if getattr(args, "from_state", None) is not None:
        from gtopt_shared.state_snapshot import (  # noqa: PLC0415
            apply_state_to_args,
            load_state_snapshot,
        )

        try:
            payload = load_state_snapshot(Path(args.from_state))
        except (FileNotFoundError, ValueError) as exc:
            parser.error(f"--from-state: {exc}")
        else:
            args = apply_state_to_args(
                parser,
                payload["args"],
                list(argv) if argv is not None else sys.argv[1:],
            )

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

    if getattr(args, "pumped_storage_template", False):
        sys.exit(print_pumped_storage_template())

    # Resolve options once so the snapshot + the converter share the
    # exact same parsed-options dict (incl. resolved output_dir).
    _opts_for_snapshot = build_options(args)
    try:
        critical_count = convert_plp_case(_opts_for_snapshot)
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

    # Post-conversion state snapshot + README.  Written AFTER
    # ``convert_plp_case`` because the converter atomically replaces
    # the entire output directory via temp-dir + rename, which would
    # wipe any pre-conversion writes.  Best-effort — snapshot failure
    # must not turn a successful conversion into an error.  See
    # ``gtopt_shared.state_snapshot`` for the file format +
    # reproducibility contract.
    try:
        _snapshot_out_dir = Path(_opts_for_snapshot["output_dir"])
        write_state_snapshot(
            output_dir=_snapshot_out_dir,
            tool_name="plp2gtopt",
            args=args,
            tool_version=__version__,
            extra=_opts_for_snapshot,
        )
        write_plp2gtopt_readme(_snapshot_out_dir)
    except (OSError, KeyError, ValueError) as exc:
        print(f"warning: failed to write state snapshot: {exc}", file=sys.stderr)

    # A structural bug in the generated planning (duplicate entity names,
    # missing references, etc.) surfaces as a CRITICAL finding from
    # gtopt_check_json.  Exit nonzero so CI and shell callers notice —
    # a silent success on a broken conversion is worse than a hard error.
    if critical_count > 0:
        print(
            f"error: conversion completed with {critical_count} CRITICAL "
            f"finding(s) — fix the underlying issue before using the output.",
            file=sys.stderr,
        )
        sys.exit(1)


if __name__ == "__main__":
    main()
