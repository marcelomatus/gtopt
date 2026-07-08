"""Argument-parser builders for plp2gtopt, grouped by domain.

Each ``add_*_arguments(parser, conf)`` function registers a group of related
CLI flags on the given *parser*.  The *conf* dict carries defaults read from
``~/.gtopt.conf`` (may be empty).

These helpers are composed by :func:`plp2gtopt.main.make_parser`.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from gtopt_config import (
    add_color_argument,
    add_log_level_argument,
    add_version_argument,
)
from gtopt_shared.cli_flags import (
    add_aperture_chunk_size_argument,
    add_demand_fail_cost_argument,
    add_lift_line_caps_argument,
    add_line_losses_mode_argument,
    add_loss_cost_eps_argument,
    add_scale_objective_argument,
    add_soft_storage_bounds_argument,
    add_use_kirchhoff_argument,
    add_use_single_bus_argument,
    add_write_out_argument,
)


# ---------------------------------------------------------------------------
# Packaged data templates
# ---------------------------------------------------------------------------

# Default RoR-equivalence whitelist shipped inside the gtopt_expand package.
# The canonical copy now lives in gtopt_expand/templates/ (moved as part of
# the gtopt_irrigation → gtopt_expand rename).  The plp2gtopt/templates/
# copy is kept as a fallback for standalone plp2gtopt installs.
DEFAULT_ROR_RESERVOIRS_FILE: Path = (
    Path(__file__).resolve().parent / "templates" / "ror_equivalence.csv"
)

# Default whitelist for the ``--pmin-as-flowright`` transform.  The
# canonical copy ships inside the gtopt_expand package so plp2gtopt
# and ``gtopt_expand pmin_as_flowright`` agree on the central list.
DEFAULT_PMIN_FLOWRIGHT_FILE: Path = (
    Path(__file__).resolve().parent.parent
    / "gtopt_expand"
    / "templates"
    / "pmin_as_flowright.csv"
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
        "--layout",
        dest="layout",
        choices=["wide", "long"],
        default=conf.get("layout", "long"),
        help=(
            "on-disk layout for per-element field Parquet files: 'long' "
            "(default) emits tidy [<index cols>, uid, value] tables (read "
            "natively by gtopt, ideal for Power BI); 'wide' emits the "
            "legacy one-column-per-uid (uid:N) shape (default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--compression-level",
        dest="compression_level",
        type=int,
        metavar="N",
        default=int(conf.get("compression_level", "0")) or None,
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
    # Emissions flags live in gtopt_shared.cli_flags so plexos2gtopt
    # picks up exactly the same surface (issue #507 Phase 2).
    from gtopt_shared.cli_flags import (  # noqa: PLC0415
        add_emissions_arguments,
    )

    add_emissions_arguments(parser)

    # PLEXOS overlay — adopt heat-rate / Fuel / per-generator metadata
    # from a converter-side PLEXOS planning JSON (typically the output
    # of ``plexos2gtopt`` on a recent CEN PCP bundle).  The overlay
    # rewires matching PLP-side generators with the richer PLEXOS data
    # so the LP can apply per-fuel cost + emission accounting.  When
    # the overlay JSON doesn't carry the data for a specific generator
    # (cogen / geothermal / waste-heat units PLEXOS leaves at
    # ``HR = 0``), the converter falls back to the ``--emissions-file``
    # (see :class:`gtopt_shared.emissions.GeneratorOverride`) to apply
    # the canonical ``Generator.type`` tag and drop the spurious fuel
    # ref.  Pass ``latest`` to auto-pick the most recent registered
    # plexos2gtopt run (see ``plp2gtopt._plexos_overlay``).
    parser.add_argument(
        "--plexos-overlay",
        dest="plexos_overlay",
        metavar="PATH",
        default=None,
        help=(
            "PLEXOS planning JSON (or a directory containing one) to "
            "overlay on top of the PLP-derived planning.  Adopts "
            "heat_rate / Fuel attachments / per-Fuel emission_factors "
            "for generators matched by name.  Pass the literal "
            "'latest' to auto-pick the most recent plexos2gtopt run "
            "(see _plexos_overlay._PLEXOS_RUN_REGISTRY)."
        ),
    )
    parser.add_argument(
        "--plexos-overlay-report",
        dest="plexos_overlay_report",
        metavar="FILE",
        type=Path,
        default=None,
        help=(
            "Write a JSON audit of the PLEXOS overlay (matched / "
            "unmatched generators, fuels added / reused) to FILE.  "
            "Defaults to <output-dir>/plexos_overlay_report.json when "
            "--plexos-overlay is set."
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
            "When omitted, the phase layout is controlled by --method. "
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
        "--apertures",
        "--num-apertures",  # deprecated alias — the `num-` prefix
        # implied an integer but the flag accepts 'all', '1-5', '1,2,3'.
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
            "(--num-apertures is kept as a deprecated alias) "
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
    add_aperture_chunk_size_argument(parser)
    # ``--write-out`` injects ``options.write_out`` into the planning
    # JSON so the standalone ``gtopt`` binary writes the right output
    # streams (Generator/generation_cost, Bus/balance_dual, etc.) for
    # downstream tools (``gtopt_marginal_units``, loss-audit, the
    # LMP-attribution pipeline).  Default is the canonical
    # ``DEFAULT_WRITE_OUT`` (``sol,dual,rc:Generator,Line``); pass
    # ``--write-out all`` for the full audit-grade dump or
    # ``--write-out sol`` for primal-only.
    add_write_out_argument(parser)


# ---------------------------------------------------------------------------
# 4. Solver / SDDP arguments
# ---------------------------------------------------------------------------


def add_solver_arguments(parser: argparse.ArgumentParser, conf: dict[str, str]) -> None:
    """Register solver type, cut-sharing, boundary cuts, convergence args."""
    parser.add_argument(
        "-M",
        "--method",
        dest="method",
        metavar="METHOD",
        # Default `cascade` since 2026-05-15: the 4-level multi-fidelity
        # ladder (warmup/uninodal/transport/full_network) is the
        # production-grade path on juan/IPLP-scale problems — plain
        # `sddp` is retained as a CLI choice for diagnostic / one-level
        # runs but no longer the default.  `--plp-legacy` still pins
        # `sddp` to match the legacy PLP equivalence shim.
        default=conf.get("method", "cascade"),
        # `mono` is kept as a deprecated alias for `monolithic` —
        # simulation_writer.py:58 normalises it back to `monolithic`.
        choices=[
            "sddp",
            "monolithic",
            "mono",
            "cascade",
            "cascade-reduced",
            "cascade_reduced",
        ],
        help=(
            "planning method controlling the simulation structure: "
            "'sddp' (default) produces one scene per scenario and one phase "
            "per stage (for Stochastic Dual Dynamic Programming); "
            "'cascade' uses a 4-level cascade with the full topology at "
            "every level (warmup, uninodal, transport, full_network); "
            "'cascade-reduced' uses a 4-level multi-fidelity cascade where "
            "L1 and L2 are reduced grids produced by the gtopt_reduce_network "
            "package (L1: ONB/6 buses, transport-only; L2: ONB/3 buses, "
            "Kirchhoff on, demand uplifted via per-demand lossfactor); "
            "'monolithic' produces a single scene with all scenarios and a "
            "single phase with all stages (for the monolithic solver; "
            "'mono' is kept as a deprecated alias for 'monolithic'). "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--cut-sharing-mode",
        dest="cut_sharing_mode",
        metavar="MODE",
        default=None,
        choices=[
            "none",
            "multicut",
        ],
        help=(
            "SDDP cut sharing mode (default for sddp/cascade: 'multicut'; "
            "unset for monolithic): "
            "'multicut' is the PLP-faithful mechanism — each scene-LP "
            "carries N dedicated future-cost columns (varphi_0..N-1), and "
            "scenario-s's backward cut is broadcast onto varphi_s in every "
            "scene-LP, priced 1/N (matches `plp-agrespd.f:94` source "
            "indexing + `defprbpd.f:810` 1/N averaging); "
            "'none' keeps cuts in their originating scene (no sharing). "
            "The legacy broadcast_mean/expected/accumulate/max modes were "
            "REMOVED from gtopt on 2026-07-08 (invalid broadcasts; see "
            "docs/formulation/sddp-cut-validity.md section 7). "
            "(default: multicut for sddp/cascade)"
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
            "Keep only boundary cuts from the last N SDDP iterations "
            "(the most-converged ones); 0 means keep all.  Bounding this "
            "limits a large tail of stale / foreign-system boundary cuts "
            "(which over-tighten the terminal alpha and drive LB > UB). "
            "(default: not set; gtopt uses 0 = all)"
        ),
    )
    parser.add_argument(
        "--no-boundary-cuts",
        dest="no_boundary_cuts",
        action="store_true",
        default=False,
        help=(
            "Disable boundary-cut export entirely: skip writing "
            "boundary_cuts.csv AND force boundary_cuts_mode=noload "
            "in the gtopt JSON.  Strictly stronger than "
            "--boundary-cuts-mode=noload, which still emits the CSV "
            "but tells gtopt to ignore it on load."
        ),
    )
    # ``--hot-start-cuts`` / ``--no-hot-start-cuts`` retired 2026-05
    # alongside ``write_hot_start_cuts_csv``.  Hot-start cuts are an
    # internal gtopt format and now use the typed Parquet path
    # (``--cuts-input-file`` on the gtopt side).
    parser.add_argument(
        "--alias-file",
        dest="alias_file",
        metavar="JSON",
        type=Path,
        default=None,
        help=(
            "Path to a JSON file containing a flat {old_name: new_name} map "
            "of state-variable renames applied when writing the "
            "boundary_cuts.csv header.  Use this to reconcile PLP "
            "reservoir/junction names with gtopt names without editing data "
            "files.  Unknown keys are ignored; missing keys pass through "
            'unchanged.  Example: {"CANUTILLAR": "CHAPO"}.'
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
            "Default: same as --convergence-tol."
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
            "(default: not set; gtopt uses 4)"
        ),
    )
    parser.add_argument(
        "--min-iterations",
        dest="min_iterations",
        metavar="N",
        type=int,
        default=None,
        help=(
            "Minimum SDDP iterations before any convergence test fires.  "
            "Forces the solver to train at least N iterations regardless of "
            "gap / CI / stationary criteria.  "
            "(default: emit 3 when not set; gtopt's own default differs)"
        ),
    )
    parser.add_argument(
        "--convergence-confidence",
        dest="convergence_confidence",
        metavar="P",
        type=float,
        default=None,
        help=(
            "Confidence level for the statistical CI convergence test (0-1).  "
            "When > 0 with multiple scenes, declare convergence when "
            "UB - LB <= z_{α/2} * σ where α = 1 - P.  P=0.95 → z=1.96, "
            "P=0.99 → z=2.576 — note higher P gives a *wider* CI, so the "
            "test fires more easily (i.e. higher P = looser, lower P = "
            "tighter).  Set to 0 to disable the CI test entirely.  "
            "Also see --stationary-gap-ceiling for an absolute gap guard.  "
            "(default: emit 0.99 when not set)"
        ),
    )
    parser.add_argument(
        "--stationary-gap-ceiling",
        dest="stationary_gap_ceiling",
        metavar="G",
        type=float,
        default=None,
        help=(
            "Absolute gap ceiling (0-1) for the secondary convergence "
            "tests (stationary + statistical CI).  When the relative gap "
            "is at or above G, neither stationary nor CI convergence will "
            "fire — only the primary `convergence_tol` test can declare "
            "convergence.  Defends against heterogeneous-scene σ explosion "
            "(juan run 2026-05-02 converged at iter 2 with gap=25%% via the "
            "CI test because σ=77 M dominated the 38 M absolute gap).  "
            "Use 1.0 to disable the ceiling.  "
            "(default: emit 0.05 when not set; gtopt's own default is 0.5)"
        ),
    )

    # ── cascade-reduced knobs ────────────────────────────────────────────
    # All optional; only consumed when --method=cascade-reduced.  Defaults
    # reproduce the design in docs/network_reduction_proposal.md.
    parser.add_argument(
        "--cascade-l1-reduce-ratio",
        dest="cascade_l1_reduce_ratio",
        type=int,
        default=6,
        metavar="R",
        help=(
            "cascade-reduced L1 reduction ratio: K1 = max(ONB // R, "
            "--cascade-l1-min-buses).  Only consumed when "
            "--method=cascade-reduced.  (default: 6)"
        ),
    )
    parser.add_argument(
        "--cascade-l2-reduce-ratio",
        dest="cascade_l2_reduce_ratio",
        type=int,
        default=3,
        metavar="R",
        help=(
            "cascade-reduced L2 reduction ratio: K2 = max(ONB // R, "
            "--cascade-l2-min-buses).  (default: 3)"
        ),
    )
    parser.add_argument(
        "--cascade-l1-min-buses",
        dest="cascade_l1_min_buses",
        type=int,
        default=4,
        metavar="N",
        help="cascade-reduced L1 floor on K1.  (default: 4)",
    )
    parser.add_argument(
        "--cascade-l2-min-buses",
        dest="cascade_l2_min_buses",
        type=int,
        default=8,
        metavar="N",
        help="cascade-reduced L2 floor on K2.  (default: 8)",
    )
    parser.add_argument(
        "--cascade-l1-uplift-pct",
        dest="cascade_l1_uplift_pct",
        type=float,
        default=3.0,
        metavar="PCT",
        help=(
            "cascade-reduced L1 per-demand lossfactor value (= PCT / 100).  "
            "Same semantics as --cascade-l2-uplift-pct but applied to the "
            "L1 (transport-only) reduced grid.  (default: 3.0)"
        ),
    )
    parser.add_argument(
        "--cascade-l2-uplift-pct",
        dest="cascade_l2_uplift_pct",
        type=float,
        default=3.0,
        metavar="PCT",
        help=(
            "cascade-reduced L2 per-demand lossfactor value (= PCT / 100).  "
            "Models transport losses as a lumped demand uplift instead of "
            "explicit line losses.  (default: 3.0)"
        ),
    )
    parser.add_argument(
        "--cascade-l1-uplift-collision",
        dest="cascade_l1_uplift_collision",
        choices=["replace", "add", "compound"],
        default="replace",
        help=(
            "cascade-reduced L1 collision rule when a demand already has a "
            "scalar lossfactor.  (default: replace)"
        ),
    )
    parser.add_argument(
        "--cascade-l2-uplift-collision",
        dest="cascade_l2_uplift_collision",
        choices=["replace", "add", "compound"],
        default="replace",
        help=(
            "cascade-reduced L2 collision rule when a demand already has a "
            "scalar lossfactor: 'replace' overwrites; 'add' sums; "
            "'compound' applies (1+old)(1+new)-1.  (default: replace)"
        ),
    )
    parser.add_argument(
        "--cascade-l1-aperture-ratio",
        dest="cascade_l1_aperture_ratio",
        type=int,
        default=4,
        metavar="R",
        help=(
            "cascade-reduced L1 num_apertures = max(ONA // R, 1) where ONA "
            "is the max apertures referenced by any phase.  L1 is coarser "
            "than L2 so the default ratio is larger.  (default: 4)"
        ),
    )
    parser.add_argument(
        "--cascade-l2-aperture-ratio",
        dest="cascade_l2_aperture_ratio",
        type=int,
        default=2,
        metavar="R",
        help=(
            "cascade-reduced L2 num_apertures = max(ONA // R, 1).  L2 is "
            "finer than L1 so the default ratio is smaller (more apertures).  "
            "(default: 2)"
        ),
    )
    parser.add_argument(
        "--cascade-l1-distance",
        dest="cascade_l1_distance",
        choices=["reactance-shortest-path", "zbus", "ptdf"],
        default="reactance-shortest-path",
        help=(
            "cascade-reduced L1 clustering metric.  (default: reactance-shortest-path)"
        ),
    )
    parser.add_argument(
        "--cascade-l2-distance",
        dest="cascade_l2_distance",
        choices=["reactance-shortest-path", "zbus", "ptdf"],
        default="ptdf",
        help=(
            "cascade-reduced L2 clustering metric.  PTDF is preferred when "
            "Kirchhoff is on.  (default: ptdf)"
        ),
    )
    parser.add_argument(
        "--cascade-disable-l1",
        dest="cascade_disable_l1",
        action="store_true",
        default=False,
        help=(
            "skip L1 (reduced_transport) in the cascade-reduced mode; "
            "useful when corridor structure isn't expected to help."
        ),
    )
    parser.add_argument(
        "--cascade-disable-l2",
        dest="cascade_disable_l2",
        action="store_true",
        default=False,
        help=(
            "skip L2 (reduced_dcopf) in the cascade-reduced mode; "
            "useful for early screening with only L0+L1+L3."
        ),
    )


# ---------------------------------------------------------------------------
# 5. Model / scaling arguments
# ---------------------------------------------------------------------------


def add_model_arguments(parser: argparse.ArgumentParser, conf: dict[str, str]) -> None:
    """Register demand-fail-cost, scale factors, kirchhoff, line losses, etc."""
    # `default=None` signals "auto-derive from plpcnfce.dat's falla
    # centrals" (CentralParser.avg_falla_cost — see gtopt_writer.py).
    # Explicit `--demand-fail-cost NNNN` or `demand_fail_cost = NNNN` in
    # the conf still overrides.
    _default_dfc = conf.get("demand_fail_cost")
    add_demand_fail_cost_argument(
        parser,
        default=float(_default_dfc) if _default_dfc is not None else None,
        help_text=(
            "cost penalty for demand curtailment in $/MWh "
            "(default: average first-tier FALLA gcost from plpcnfce.dat)"
        ),
    )
    parser.add_argument(
        "--state-fail-cost",
        dest="state_fail_cost",
        type=float,
        metavar="COST",
        default=float(conf.get("state_fail_cost", "1000.0")),
        help="penalty for state variable deviations in $/MWh (default: %(default)s)",
    )
    # PLP-faithful soft volume bounds: when enabled (the default), each
    # reservoir's hard ``efin >=`` row becomes soft via the C++
    # ``Reservoir.efin_cost`` slack, AND the reservoir-maintenance per-stage
    # emin is routed through the soft_emin / soft_emin_cost slack mechanism
    # instead of a hard variable bound.  The slack costs are inherited from
    # plpvrebemb.dat (per-reservoir Costo de Rebalse) when the reservoir is
    # in vrebemb, falling back to plpmat.dat ``CVert`` (global), then a
    # hard 1000 $/hm³ default.  Disable with ``--no-soft-storage-bounds``
    # for the legacy hard-constraint behaviour.  ``--plp-legacy`` also
    # enables this flag (PLP itself uses these as soft).
    _default_ssb = conf.get("soft_storage_bounds")
    add_soft_storage_bounds_argument(
        parser,
        default=(
            _default_ssb.lower() not in ("false", "0", "no")
            if _default_ssb is not None
            else True
        ),
    )
    # Cap on the per-reservoir spillage cost (``Costo de Rebalse`` from
    # plpvrebemb.dat / ``CVert`` from plpmat.dat) used as ``efin_cost``
    # / ``soft_emin_cost`` when ``--soft-storage-bounds`` is on.  PLP
    # production cases sometimes carry vrebemb costs of 5000 \$/hm³,
    # which dominates the SDDP objective on iter-0 forward passes and
    # produces an enormous UB (~10⁹) until enough Benders cuts steer
    # the trajectory to avoid the slack.  Capping the cost lets the
    # gap close in fewer iterations at the price of allowing slightly
    # more spillage in the LP optimum.  The cap is INCLUSIVE — costs
    # at or below it pass through unchanged.  Set to 0 to disable.
    _default_vcc = conf.get("vert_cost_cap")
    parser.add_argument(
        "--spillway-cost-cap",
        "--vert-cost-cap",  # deprecated alias — "vert" is the PLP
        # Spanish abbreviation (vertimiento) and is opaque to users
        # outside the CEN ecosystem.
        dest="vert_cost_cap",
        type=float,
        default=(float(_default_vcc) if _default_vcc is not None else 500.0),
        help=(
            "cap ($/hm³) for the spillway / vrebemb / CVert cost emitted as "
            "Reservoir.efin_cost / soft_emin_cost (only effective when "
            "--soft-storage-bounds is on; 0 disables the cap; "
            "obsolete when --auto-water-fail-cost is on; "
            "default: %(default)s)"
        ),
    )
    # ``--drop-spillway-waterway`` (default False, opt-in): when enabled,
    # suppress every ``_ver`` (spillway / vert) waterway emission and let
    # excess water leave the system through the central's own junction
    # (``drain = True``).  The trade-off is physical accuracy: PLP routes
    # spill water to the downstream central named in ``ser_ver`` (the water
    # can be reused) and charges per-flow ``CVert`` / ``Costo de Rebalse``.
    # Dropping the arc loses the routing AND the cost — all spillover
    # becomes a free leak — but in exchange every ``_ver`` arc and its
    # associated ``fcost`` disappears from the LP, which improves scaling
    # and removes a class of spurious binding-bound duals.
    #
    # Default flipped to False after the gtopt_iplp investigation
    # (2026-04-28): the suppress-mode topology was implicated in the
    # SDDP elastic-cut degeneracy chain at LMAULE / ELTORO that produced
    # ``no recoverable feasibility cut`` failures.  PLP-faithful spillway
    # topology is the safer default; opt into suppress mode only when LP
    # scaling outweighs routing fidelity for the case at hand.
    _default_drop_spillway = conf.get("drop_spillway_waterway")
    parser.add_argument(
        "--drop-spillway-waterway",
        dest="drop_spillway_waterway",
        action=argparse.BooleanOptionalAction,
        default=(
            _default_drop_spillway.lower() not in ("false", "0", "no")
            if _default_drop_spillway is not None
            else False
        ),
        help=(
            "suppress ``_ver`` (spillway/vert) waterway emission and "
            "rely on a junction-level drain to discharge surplus water "
            "(default: %(default)s; opt-in — disables ``fcost`` on "
            "spillways and improves LP scaling at the cost of routing "
            "fidelity)"
        ),
    )
    # ``--vrebemb-as-sink`` (default False, opt-in): for centrals listed in
    # ``plpvrebemb.dat``, route their ``_ver`` waterway to the synthetic
    # ``<name>_ocean`` drain (rather than to ``ser_ver`` central) and drop
    # both ``fmax`` and ``fcost`` on the arc.  This restores PLP semantics
    # for the qrb (sink-bound, costed) rebalse mechanism that gtopt
    # previously translated as a parallel-pipe `_ver` to `ser_ver`,
    # producing "fictitious water" feeding downstream demand via cap
    # arbitrage.  Non-vrebemb centrals are untouched.  See
    # JunctionWriter._process_central.
    _default_vas = conf.get("vrebemb_as_sink")
    parser.add_argument(
        "--vrebemb-as-sink",
        dest="vrebemb_as_sink",
        action=argparse.BooleanOptionalAction,
        default=(
            _default_vas.lower() not in ("false", "0", "no")
            if _default_vas is not None
            else False
        ),
        help=(
            "for centrals in plpvrebemb.dat, route the ``_ver`` waterway to a "
            "synthetic ``<name>_ocean`` drain and drop ``fmax``/``fcost``. "
            "Restores PLP qrb-to-sink semantics; eliminates cap-arbitrage "
            "fictitious-water generation through the spillway arc. "
            "(default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--reserve-fail-cost",
        dest="reserve_fail_cost",
        type=float,
        metavar="COST",
        default=None,
        help="cost penalty for reserve shortfall in $/MWh (default: not set)",
    )
    # ``--reservoir-flow-estimate`` (default True): replace the generic
    # C++ ReservoirLP -9000/6000 m³/s extraction defaults with tight,
    # per-reservoir fmin/fmax estimated from the hydraulic network
    # (downstream waterway + turbine release caps for fmax; a NetworkX
    # max-flow bottleneck on natural inflows for fmin).  Tight bounds
    # remove free-below extraction columns that block GPU first-order /
    # heuristic LP solvers.  Disable with ``--no-reservoir-flow-estimate``.
    _default_rfe = conf.get("reservoir_flow_estimate")
    parser.add_argument(
        "--reservoir-flow-estimate",
        dest="reservoir_flow_estimate",
        action=argparse.BooleanOptionalAction,
        default=(
            _default_rfe.lower() not in ("false", "0", "no")
            if _default_rfe is not None
            else True
        ),
        help=(
            "estimate per-reservoir extraction-flow bounds (fmin/fmax) from "
            "the hydraulic network topology instead of the generic gtopt "
            "-9000/6000 m³/s defaults (default: %(default)s)"
        ),
    )
    add_scale_objective_argument(
        parser, default=float(conf.get("scale_objective", "1.0"))
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
    add_use_single_bus_argument(parser)
    add_use_kirchhoff_argument(parser)
    parser.add_argument(
        "--kirchhoff-mode",
        dest="kirchhoff_mode",
        metavar="MODE",
        default="node_angle",
        choices=["node_angle", "cycle_basis"],
        help=(
            "Kirchhoff Voltage Law (KVL) formulation: "
            "'node_angle' = classical B–θ form (one θ per bus + one KVL "
            "per line, gauge-pinned reference bus per island); "
            "'cycle_basis' = loop-flow form (one KVL per fundamental cycle, "
            "no θ, no theta-scale tuning).  Default: %(default)s — the B–θ "
            "form mirrors PLP's voltage-angle model and, despite a slightly "
            "larger LP, solves faster under CPLEX (sparse per-line KVL rows "
            "vs dense per-cycle loop rows; ~3x faster dual simplex on the "
            "CEN65 full-network case)."
        ),
    )
    parser.add_argument(
        "-L",
        "--use-line-losses",
        dest="use_line_losses",
        action=argparse.BooleanOptionalAction,
        default=None,
        help=(
            "model transmission line losses; pass --no-use-line-losses to "
            "explicitly disable (omit to inherit the gtopt default: true)"
        ),
    )
    # plp2gtopt defaults to ``piecewise_direct`` — the PLP-faithful loss model
    # (PLP ``genpdlin.f``): per-segment bus stamps, no loss-tracking rows, the
    # most compact LP and a row count matching PLP.  PLP cases operate in PLP's
    # historical regime (no negative receiver LMPs), where piecewise_direct's
    # phantom-flow caveat does not bite.  Override with --line-losses-mode for
    # the safer bidirectional/adaptive model on cases that can see negative LMPs.
    add_line_losses_mode_argument(parser, default="piecewise_direct")
    # ``loss_cost_eps`` defaults to 0.1 $/MWh for plp2gtopt — well below
    # any thermal SRMC so dispatch is unchanged, but large enough (vs the
    # historical 1e-6 LP-tolerance-level value) that the bidirectional-
    # flow degeneracy is strictly broken even under aggressive presolve
    # / Ruiz scaling, eliminating phantom A→B + B→A flow on every
    # PWL/bidirectional line.  Pass ``--loss-cost-eps 0`` to disable.
    add_loss_cost_eps_argument(parser, dialect="plp", default=0.1)
    add_lift_line_caps_argument(parser, dialect="plp")
    parser.add_argument(
        "--plp-legacy",
        dest="plp_legacy",
        action="store_true",
        default=False,
        help=(
            "bundle PLP-compatibility defaults that make gtopt outputs "
            "closer to PLP even when that is not the highest-quality "
            "or smallest-LP choice. Adjusts the defaults of: "
            "--line-losses-mode (→piecewise_direct; PLP `genpdlin.f`), "
            "--use-line-losses (→true, emitted explicitly). "
            "method, pasada_mode, use_kirchhoff, discount_rate are already "
            "PLP-aligned by default so no bundle change is needed. "
            "reservoir_scale_mode is intentionally left alone. "
            "Explicit flags still win over the bundle."
        ),
    )
    parser.add_argument(
        "--disable-discharge-limit-for",
        dest="disable_discharge_limit_for",
        metavar="NAMES",
        default=None,
        help=(
            "comma-separated list of reservoir names whose plpralco-derived "
            "ReservoirDischargeLimit constraint should NOT be emitted. "
            "Example: 'RALCO,RAPEL'. The discharge-limit row is currently a "
            "hard inequality with no slack; for cases where PLP relies on "
            "the soft `vrbp/vrbn` slacks on this row (which gtopt does not "
            "yet model) the gtopt LP can become spuriously infeasible at "
            "iter-0 of the SDDP cascade. Use this flag to skip the row for "
            "the offending reservoirs. (default: emit all)"
        ),
    )
    # ``--soft-min-flows`` is ON by default (uses the bundled
    # whitelist) because PLP-faithful runs need MACHICURA / PANGUE /
    # PILMAIQUEN / ABANICO / ANTUCO / PALMUCHO routed as FlowRight
    # discharge obligations to keep ``reservoir_efin >= eini`` rows
    # feasible at iter-0 of the SDDP cascade (same root cause as the
    # plp_case_2y aperture regression and the support/juan/IPLP_uninodal
    # investigation).  Pass ``--no-soft-min-flows`` to opt out.
    #
    # The flag covers BOTH conversions:
    #   * generator pmin → FlowRight (whitelist-driven; flow = pmin/Rendi)
    #   * waterway fmin → FlowRight (auto-detected from
    #     ``Waterway/fmin.parquet``; flow = fmin in m³/s)
    # Both are minimum-flow obligations that, left as hard variable
    # bounds, can render an SDDP phase infeasible when the predecessor
    # trial leaves the upstream reservoir empty.  Routing them through
    # ``FlowRight.fail_cost`` slack converts the hard floor into a
    # priced soft constraint.
    #
    # The internal options key remains ``pmin_as_flowright`` for legacy
    # compatibility (config files, tests, programmatic callers).
    # ``--pmin-as-flowright`` / ``--no-pmin-as-flowright`` continue to
    # work as deprecated aliases of the new flag.
    _default_pmf = conf.get("soft_min_flows")
    if _default_pmf is None:
        _default_pmf = conf.get("pmin_as_flowright")  # legacy key fallback
    if _default_pmf is None:
        _default_pmf_value = ""  # use bundled CSV
    elif _default_pmf.lower() in ("false", "0", "no"):
        _default_pmf_value = None
    else:
        _default_pmf_value = _default_pmf
    pmf_group = parser.add_mutually_exclusive_group()
    pmf_group.add_argument(
        "--soft-min-flows",
        dest="pmin_as_flowright",
        metavar="PATH_OR_NAMES",
        nargs="?",
        const="",  # sentinel: flag passed without value -> use bundled CSV
        default=_default_pmf_value,
        help=(
            "Convert minimum-flow obligations into soft FlowRight "
            "rights.  Covers generator pmin (whitelist-driven; "
            "flow = pmin/Rendi) and waterway fmin (auto-detected from "
            "Waterway/fmin.parquet; flow = fmin m³/s).  The optional "
            "argument is either a CSV path (same schema as "
            "gtopt_expand/templates/pmin_as_flowright.csv) or a "
            "comma-separated list of central names; it filters the "
            "generator-pmin path only — the waterway-fmin path is "
            "all-or-nothing.  Without an argument, uses the bundled "
            "default whitelist (MACHICURA, PANGUE, PILMAIQUEN, ABANICO, "
            "ANTUCO, PALMUCHO).  ON by default; pass "
            "--no-soft-min-flows to disable both transforms."
        ),
    )
    pmf_group.add_argument(
        "--no-soft-min-flows",
        dest="pmin_as_flowright",
        action="store_const",
        const=None,
        help="disable the minimum-flow → FlowRight conversion.",
    )
    # Deprecated aliases — kept for backwards compatibility with existing
    # config files, scripts, and CI invocations.  Same semantics as the
    # ``--soft-min-flows`` form above (and the same internal options key
    # ``pmin_as_flowright``).
    pmf_group.add_argument(
        "--pmin-as-flowright",
        dest="pmin_as_flowright",
        metavar="PATH_OR_NAMES",
        nargs="?",
        const="",
        help="deprecated alias of --soft-min-flows.",
    )
    pmf_group.add_argument(
        "--no-pmin-as-flowright",
        dest="pmin_as_flowright",
        action="store_const",
        const=None,
        help="deprecated alias of --no-soft-min-flows.",
    )
    parser.add_argument(
        "--flow-right-fail-cost",
        dest="flow_right_fail_cost",
        type=float,
        metavar="COST",
        default=None,
        help=(
            "Override the FlowRight fail_cost in $/Hm³ (PLP convention; "
            "matches plpmat.dat CCauFal value).  When unset (default), the "
            "value is auto-resolved from plpmat.dat CCauFal.  Internally "
            "multiplied by FactTiempoH=3.6 to convert to gtopt's "
            "$/(m³/s·h) per-block coefficient.  Use this flag to tune the "
            "FlowRight slack penalty empirically; for juan/gtopt_iplp the "
            "auto value is 7000 $/Hm³ → 25200 $/(m³/s·h) internal."
        ),
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
            "default penalty cost ($/dam³ — note: --vert-cost-cap is "
            "$/hm³, 1 hm³ = 1000 dam³) for the soft minimum volume "
            "constraint (plpminembh.dat).  Per-stage costs from the file "
            "override this default.  Set to 0 to disable soft emin. "
            "(default: %(default)s)"
        ),
    )
    # ``--auto-water-fail-cost`` (default True since 2026-05-11): replace
    # the legacy vrebemb/CVert cascade and the ``2 × max(rebalse_cost)``
    # heuristic with a single principled formula derived from the case's
    # own demand-failure prices.  See ``plp2gtopt._water_value`` for the
    # formula and rationale.  When enabled, ``--vert-cost-cap`` becomes
    # obsolete (the new helper does not consult vrebemb cost); leave it
    # in place for backward compat but it has no effect on the
    # soft-storage / FlowRight pricing path.  Disable with
    # ``--no-auto-water-fail-cost`` to fall back to legacy paths.
    parser.add_argument(
        "--auto-water-fail-cost",
        dest="auto_water_fail_cost",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "auto-derive Reservoir.efin_cost / soft_emin_cost and "
            "FlowRight.fail_cost from the case's own demand-failure "
            "prices instead of vrebemb / CVert / hard-coded fallbacks.  "
            "When on, the anchor is max(falla.gcost) + 1 ($/MWh) so "
            "water obligations strictly dominate energy obligations in "
            "the LP objective.  Use --water-fail-cost to override the "
            "anchor by hand (e.g. 500 instead of the auto value).  Note: "
            "--vert-cost-cap becomes obsolete when this is on.  Pass "
            "--no-auto-water-fail-cost to disable. "
            "(default: %(default)s)"
        ),
    )
    # ``--water-fail-cost`` (manual override in $/MWh): when set, use
    # this value directly as the water-shortfall anchor and enable the
    # unified pricing pipeline (taking priority over
    # ``--auto-water-fail-cost``).  When ``None`` (default), the value
    # is auto-derived from ``max(falla.gcost) × (1 + losses) + 1`` if
    # ``--auto-water-fail-cost`` is on, else legacy paths are used.
    # Mirrors ``--demand-fail-cost``; intended for users who want
    # explicit control or to compare the auto-derived value against a
    # hand-tuned one.
    parser.add_argument(
        "--water-fail-cost",
        dest="water_fail_cost",
        type=float,
        metavar="$/MWh",
        default=None,
        help=(
            "manual override in $/MWh for the unified water-shortfall "
            "anchor.  When set, drives Reservoir.efin_cost / "
            "soft_emin_cost (path B) and FlowRight.fail_cost via "
            "lost_pf scaling.  Implicitly enables the new pipeline "
            "(takes priority over --auto-water-fail-cost). "
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
        default=True,
        help=(
            "auto-detect generator technology from central names. "
            "Refines PLP types (termica, pasada) into specific types "
            "(solar, wind, gas, coal, etc.) by scanning names for "
            "keywords (CEN suffix convention: ``_FV``→solar, "
            "``_EO``→wind, etc.).  Without this, many PLP renewables "
            "end up tagged ``type=thermal`` with HR=0 and no fuel, "
            "which breaks downstream emission attribution (#524). "
            "(default: %(default)s — enabled by default since 2026-06)"
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
        "--case-version",
        "--sys-version",  # deprecated alias — too easily confused with
        # the global --version flag (which prints the tool's version).
        dest="sys_version",
        metavar="VERSION",
        default="",
        help=(
            "version string for the planning case stored in the output JSON "
            "(--sys-version is kept as a deprecated alias) (default: empty)"
        ),
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
        help=(
            "demand management fraction in [0, 1) — final demand is "
            "scaled by (1 - factor); 0.05 ≈ 5%% demand reduction "
            "(default: %(default)s)"
        ),
    )
    add_log_level_argument(parser)
    parser.add_argument(
        "--log-file",
        "--log",  # backward-compat alias; the bare `--log` was confusing
        # because it sounded like `--log-level`.
        dest="log_file",
        metavar="FILE",
        default=None,
        help=(
            "write detailed DEBUG-level log to FILE; auto-redirects to "
            "<output_dir>/logs/conversion.log when omitted "
            "(--log is kept as a deprecated alias for --log-file)"
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
        "--expand-water-rights",
        dest="expand_water_rights",
        action=argparse.BooleanOptionalAction,
        default=False,
        help=(
            "run the gtopt_expand laja|maule Stage-2 transforms from "
            "plplajam.dat / plpmaulen.dat (opt-in).  When set, the "
            "resulting FlowRight / VolumeRight / UserConstraint entities "
            "are merged into planning.json, companion laja.pampl / "
            "maule.pampl files are written next to it, and per-agreement "
            "system fragments (laja_water_rights.json / "
            "maule_water_rights.json) are emitted for the manifest.  "
            "Parser-side *_dat.json intermediates are NOT written to "
            "disk (never shipped).  Fully independent of --expand-lng "
            "and --ror-as-reservoirs; the latter is complementary "
            "because promoting MACHICURA lets the Maule agreement pick "
            "its richer embalse template variant.  A no-op when the PLP "
            "case has no plplajam.dat / plpmaulen.dat. (default: "
            "%(default)s)"
        ),
    )
    parser.add_argument(
        "--expand-lng",
        dest="expand_lng",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "run the gtopt_expand lng Stage-2 transform from "
            "plpcnfgnl.dat: the resulting LngTerminal entities are "
            "merged into planning.json.  Fully independent of "
            "--expand-water-rights and --ror-as-reservoirs.  A no-op "
            "when the PLP case has no plpcnfgnl.dat. (default: "
            "%(default)s)"
        ),
    )
    parser.add_argument(
        "--expand-ror",
        dest="expand_ror",
        action=argparse.BooleanOptionalAction,
        default=True,
        help=(
            "also emit ror_promoted.json (the gtopt_expand ror audit "
            "artifact) listing every central promoted by "
            "--ror-as-reservoirs.  Independent of --expand-water-rights "
            "and --expand-lng, but complementary to the former: when "
            "MACHICURA is among the promoted RoRs, the Maule agreement "
            "picks its richer embalse template variant instead of the "
            "default pasada.  Has no effect when --ror-as-reservoirs "
            "is disabled. (default: %(default)s)"
        ),
    )
    parser.add_argument(
        "--pumped-storage",
        dest="pumped_storage_files",
        type=Path,
        action="append",
        metavar="FILE",
        default=None,
        help=(
            "run the ``gtopt_expand pumped_storage`` transform for each "
            "FILE: emit a reversible pumped-storage unit (Turbine + Pump "
            "between an upper and a lower reservoir).  May be repeated "
            "to expand several units in one run.  Each FILE is a "
            "canonical config JSON (name/vmin/vmax/PFs/pump_factor — "
            "see --pumped-storage-template).  ``vmin`` / ``vmax`` at 0 "
            "or absent fall back to the upper reservoir's ``emin`` / "
            "``emax`` in plpcnfce.dat.  The unit name defaults to the "
            "filename stem (e.g. ``hb_maule.json`` → ``hb_maule``).  "
            "Writes one ``{name}.json`` per unit and merges the "
            "entities into the planning JSON.  Requires each unit's "
            "``lower_reservoir`` to be a reservoir — real embalse or "
            "RoR-promoted via --ror-as-reservoirs. (default: disabled)"
        ),
    )
    parser.add_argument(
        "--pumped-storage-template",
        action="store_true",
        default=False,
        help=(
            "print a JSON template of pumped-storage parameters to "
            "stdout, populated with HB Maule reference values "
            "(pump.pdf §4).  Edit the output and pass it back via "
            "--pumped-storage.  Example workflow:\n"
            "  plp2gtopt --pumped-storage-template > hb_maule.json\n"
            "  # edit hb_maule.json to tune specific values\n"
            "  plp2gtopt -i plp_case --pumped-storage hb_maule.json"
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
