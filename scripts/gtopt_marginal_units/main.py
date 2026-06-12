# SPDX-License-Identifier: BSD-3-Clause
"""CLI entry point for gtopt-marginal-units.

Wires together: input reader (gtopt or canonical-feed) → classifier
+ zone partition → §4.7 reconstruction (mode=real-reconstruct only)
→ recipe builders → merit ladder → parquet-dataset writer → optional
Markdown report.
"""

from __future__ import annotations

import argparse
import logging
import sys
from pathlib import Path
from typing import TYPE_CHECKING, Optional

import pandas as pd

from gtopt_canonical_feed import Cells, Topology
from gtopt_marginal_units._classify import classify
from gtopt_marginal_units._gtopt_reader import read_gtopt
from gtopt_marginal_units._io import write_dataset
from gtopt_marginal_units._ladder import build_ladder
from gtopt_marginal_units._lp_duals import (
    COL_GEN_RC,
    COL_GEN_SRMC,
    GtoptLpDuals,
    check_write_out_flags,
)
from gtopt_marginal_units._recipes import build_recipes_for_cell
from gtopt_marginal_units._reconstruct import reconstruct_all_zones
from gtopt_marginal_units._report import write_report
from gtopt_marginal_units._zones import partition_zones
from gtopt_marginal_units.constants import (
    DEFAULT_MERIT_LADDER_DEPTH,
    EXIT_INPUT_ERROR,
    EXIT_OK,
    EXIT_UNATTRIBUTED,
    PRICE_SETTER_KINDS,
    PROFILE_KINDS,
    STORAGE_KINDS,
    SYNTHETIC_GEN_NAME_SUFFIXES,
    SYNTHETIC_GEN_PMAX_MW,
    Confidence,
    Status,
    Tolerances,
)
from gtopt_marginal_units.errors import (
    AttributionError,
    InputValidationError,
    MarginalUnitsError,
)

if TYPE_CHECKING:  # avoid runtime cycle; only used in type hints
    from gtopt_marginal_units._reconstruct import ZoneR3Result  # noqa: F401


_LOG = logging.getLogger("gtopt_marginal_units")


# ---------------------------------------------------------------------------
# Argparser
# ---------------------------------------------------------------------------


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="gtopt-marginal-units",
        description=(
            "Identify marginal generating units and attribute the bus LMP "
            "and emission intensity. See "
            "docs/scripts/gtopt_marginal_units_plan.md for the full design.\n"
            "\n"
            "Recommended `gtopt` write_out for the source run:\n"
            "    --write-out 'sol,dual,rc:Generator,Line'\n"
            "Emits exactly the streams this tool consumes "
            "(Generator/generation_sol, Generator/generation_cost, "
            "Generator/srmc_sol, Bus/balance_dual, Junction/balance_dual, "
            "Battery/energy_dual, Reservoir/water_value_dual, "
            "Line/flowp_sol, Line/flown_sol, Demand/load_sol, "
            "Demand/fail_sol) and skips the per-element reduced costs "
            "no consumer reads — keeps the on-disk footprint lean."
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--input-kind",
        choices=("gtopt-dir", "feed-parquet", "auto"),
        default="auto",
        help="Source kind. 'auto' sniffs from --planning vs --feed presence.",
    )
    p.add_argument(
        "--mode",
        choices=("simulated", "real", "real-reconstruct", "compare"),
        default="simulated",
        help=(
            "Classification mode. 'simulated' requires --input-kind gtopt-dir; "
            "'real' and 'real-reconstruct' require --input-kind feed-parquet."
        ),
    )
    p.add_argument("--planning", type=Path, help="Planning JSON (gtopt-dir).")
    p.add_argument("--output", type=Path, help="gtopt output directory (gtopt-dir).")
    p.add_argument("--feed", type=Path, help="Canonical feed parquet (feed-parquet).")
    p.add_argument(
        "--out",
        type=Path,
        default=Path("./marginal_units.parquet"),
        help="Output parquet dataset directory (default: ./marginal_units.parquet).",
    )
    p.add_argument("--csv", action="store_true", help="Also write CSV views.")
    p.add_argument("--scenes", type=str, help="Comma- or range-list of scenario UIDs.")
    p.add_argument("--stages", type=str, help="Comma- or range-list of stage UIDs.")
    p.add_argument("--blocks", type=str, help="Comma- or range-list of block UIDs.")
    p.add_argument("--tol-price", type=float, default=Tolerances.default().tol_price)
    p.add_argument("--tol-flow", type=float, default=Tolerances.default().tol_flow)
    p.add_argument("--tol-mu", type=float, default=Tolerances.default().tol_mu)
    p.add_argument(
        "--tol-load-mw", type=float, default=Tolerances.default().tol_load_mw
    )
    p.add_argument("--eps", type=float, default=Tolerances.default().eps)
    p.add_argument(
        "--loss-factor-warn",
        type=float,
        default=Tolerances.default().loss_factor_warn,
        help=(
            "Per-bus emission scaling: raw bus_LMP/ref ratio above this "
            "value triggers a warning in the end-of-run summary "
            "(scaling is still applied). Default 2.0; empirical CEN "
            "2-year cascade peaks ~1.4."
        ),
    )
    p.add_argument(
        "--loss-factor-error",
        type=float,
        default=Tolerances.default().loss_factor_error,
        help=(
            "Per-bus emission scaling: raw bus_LMP/ref ratio above this "
            "value aborts the run (AttributionError — marginal unit "
            "likely lives in a different electrical island). "
            "Default 5.0."
        ),
    )
    p.add_argument(
        "--single-bus",
        action="store_true",
        help="Force copperplate (collapse zones to one).",
    )
    p.add_argument(
        "--zone-mode",
        choices=("congestion", "physical", "both"),
        default="congestion",
    )
    p.add_argument(
        "--merit-ladder-depth",
        type=int,
        default=DEFAULT_MERIT_LADDER_DEPTH,
        help="±K rungs above and below the anchor (default 3; 0 disables).",
    )
    p.add_argument(
        "--require-cdc-restriction",
        action="store_true",
        help="Refuse to infer line saturation; needs CDC declaration data.",
    )
    p.add_argument(
        "--require-regime-data",
        action="store_true",
        help="Refuse to run without commitment-regime feed columns.",
    )
    p.add_argument(
        "--emission-attribute",
        default="co2",
        help="Topology.generator emission column to use (v1: co2 only).",
    )
    p.add_argument(
        "--moer-compare",
        type=Path,
        help="Optional WattTime-style MOER CSV for back-testing.",
    )
    p.add_argument(
        "--demand-fail-cost",
        type=float,
        default=1000.0,
        help="Rationing cap [$/MWh] used by §4.7 R3.",
    )
    p.add_argument(
        "--carbon-price",
        type=float,
        default=0.0,
        help=(
            "Carbon price [USD per tCO2eq]. Persisted to the manifest and "
            "applied at consumer-API read time (bus_lmp, recompute_lmp, "
            "bus_lmp_and_emission); does NOT alter the saved recipes."
        ),
    )
    p.add_argument("--report", type=Path, help="Path to write a Markdown report.")
    p.add_argument(
        "--report-top-marginal",
        type=int,
        default=10,
        help=(
            "Truncate the 'top marginal units' table at N rows; "
            "the report includes an explicit elided-count note when "
            "the underlying data has > N rows. Default 10."
        ),
    )
    p.add_argument(
        "--report-top-lines",
        type=int,
        default=10,
        help=("Truncate the 'saturated lines' table at N rows. Default 10."),
    )
    p.add_argument(
        "--report-top-reasons",
        type=int,
        default=5,
        help=("Truncate the 'unattributed reasons' table at N rows. Default 5."),
    )
    p.add_argument(
        "--require-reduced-cost",
        action="store_true",
        default=True,
        help=(
            "Require gtopt reduced-cost output (Generator/generation_cost) "
            "under --output. Recommended source-run flag: "
            "`gtopt --write-out 'sol,dual,rc:Generator,Line'` — emits only "
            "the streams this tool consumes. Default: on."
        ),
    )
    p.add_argument(
        "--no-require-reduced-cost",
        action="store_false",
        dest="require_reduced_cost",
        help=(
            "Skip the reduced-cost check and fall back to declared_MC from "
            "the planning JSON. Useful for legacy outputs."
        ),
    )
    p.add_argument("-v", "--verbose", action="count", default=0)
    p.add_argument("-q", "--quiet", action="store_true")
    return p


# ---------------------------------------------------------------------------
# Public entry
# ---------------------------------------------------------------------------


def cli(argv: Optional[list[str]] = None) -> int:
    """Console-script entry. Returns process exit code."""
    parser = _build_parser()
    args = parser.parse_args(argv)
    _setup_logging(args.verbose, args.quiet)
    try:
        return _run(args)
    except InputValidationError as exc:
        _LOG.error("input error: %s", exc)
        return EXIT_INPUT_ERROR
    except AttributionError as exc:
        _LOG.error("writer-side invariant violation: %s", exc)
        return EXIT_INPUT_ERROR
    except MarginalUnitsError as exc:
        _LOG.error("%s", exc)
        return EXIT_INPUT_ERROR


def _setup_logging(verbose: int, quiet: bool) -> None:
    level = logging.WARNING
    if quiet:
        level = logging.ERROR
    elif verbose >= 2:
        level = logging.DEBUG
    elif verbose == 1:
        level = logging.INFO
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )


def _run(args: argparse.Namespace) -> int:
    # 1. Resolve input kind.
    kind = _resolve_input_kind(args)
    _LOG.info("resolved --input-kind=%s, --mode=%s", kind, args.mode)

    # 2. Validate (--input-kind, --mode) combination.
    _validate_kind_mode(kind, args.mode)

    # 3. Load Topology + Cells (+ LP-dual extras when input is a gtopt run).
    lp_duals: GtoptLpDuals = GtoptLpDuals.empty()
    if kind == "gtopt-dir":
        if args.planning is None or args.output is None:
            raise InputValidationError(
                "--input-kind gtopt-dir requires both --planning and --output"
            )
        # Fail-fast on missing reduced-cost stems before we melt huge parquets.
        check_write_out_flags(
            args.output,
            require_reduced_cost=args.require_reduced_cost,
        )
        topology, cells, lp_duals = read_gtopt(args.planning, args.output)
    else:  # feed-parquet
        from gtopt_marginal_units._feed_reader import read_canonical_feed  # noqa: PLC0415

        if args.feed is None:
            raise InputValidationError("--input-kind feed-parquet requires --feed")
        drop_lmp = args.mode == "real-reconstruct"
        topology, cells = read_canonical_feed(args.feed, drop_lmp=drop_lmp)

    if args.loss_factor_warn < 1.0:
        raise InputValidationError(
            f"--loss-factor-warn must be ≥ 1.0; got {args.loss_factor_warn}"
        )
    if args.loss_factor_error <= args.loss_factor_warn:
        raise InputValidationError(
            f"--loss-factor-error ({args.loss_factor_error}) must be "
            f"> --loss-factor-warn ({args.loss_factor_warn})"
        )
    tol = Tolerances(
        eps=args.eps,
        tol_price=args.tol_price,
        tol_flow=args.tol_flow,
        tol_mu=args.tol_mu,
        tol_lmp=Tolerances.default().tol_lmp,
        tol_load_mw=args.tol_load_mw,
        loss_factor_warn=args.loss_factor_warn,
        loss_factor_error=args.loss_factor_error,
    )

    if args.carbon_price < 0.0:
        raise InputValidationError(
            f"--carbon-price must be non-negative; got {args.carbon_price}"
        )

    # B2: build per-bus demand_fail_cost lookup when we have a
    # planning JSON (gtopt-dir mode).  Resolution order matches the
    # C++ LP: per-Demand ``fcost`` > model_options.demand_fail_cost >
    # CLI ``--demand-fail-cost`` fallback.  Feed-parquet inputs lack
    # the planning JSON so we fall back to the CLI scalar.
    dfc_arg: float | dict[int, float] = float(args.demand_fail_cost)
    if kind == "gtopt-dir":
        from gtopt_check_output._reader import load_planning as _lp  # noqa: PLC0415

        from gtopt_marginal_units._gtopt_reader import (  # noqa: PLC0415
            demand_fail_cost_by_bus,
        )

        dfc_arg = demand_fail_cost_by_bus(
            _lp(args.planning), float(args.demand_fail_cost)
        )

    # 4. Per-cell driver (single-bus collapse if asked).
    summary = _process_cells(
        topology=topology,
        cells=cells,
        lp_duals=lp_duals,
        tol=tol,
        merit_ladder_depth=max(0, int(args.merit_ladder_depth)),
        single_bus=args.single_bus,
        zone_mode=args.zone_mode,
        demand_fail_cost=dfc_arg,
        out_root=args.out,
        carbon_price=float(args.carbon_price),
    )

    # 5. Optional Markdown report.
    if args.report:
        per_bus = pd.read_parquet(args.out / "attribution/per_bus.parquet")
        per_zone = pd.read_parquet(args.out / "attribution/per_zone.parquet")
        unattributed_path = args.out / "audit/unattributed.parquet"
        unattributed = (
            pd.read_parquet(unattributed_path) if unattributed_path.exists() else None
        )
        write_report(
            args.report,
            per_bus=per_bus,
            per_zone=per_zone,
            unattributed=unattributed,
            top_n_marginal=int(args.report_top_marginal),
            top_n_lines=int(args.report_top_lines),
            top_n_reasons=int(args.report_top_reasons),
        )
        _LOG.info("report written: %s", args.report)

    if summary.has_unattributed_cells:
        _LOG.warning(
            "%d unattributed cell(s) emitted; exit code %d.",
            summary.rows_audit_unattributed,
            EXIT_UNATTRIBUTED,
        )
        return EXIT_UNATTRIBUTED
    return EXIT_OK


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _resolve_input_kind(args: argparse.Namespace) -> str:
    if args.input_kind != "auto":
        return args.input_kind
    have_gtopt = args.planning is not None or args.output is not None
    have_feed = args.feed is not None
    if have_gtopt and have_feed:
        raise InputValidationError(
            "--input-kind auto: both gtopt-dir and feed-parquet args provided; "
            "specify --input-kind explicitly to disambiguate."
        )
    if have_gtopt:
        return "gtopt-dir"
    if have_feed:
        return "feed-parquet"
    raise InputValidationError(
        "no input provided. Supply either (--planning + --output) or --feed."
    )


def _validate_kind_mode(kind: str, mode: str) -> None:
    """Reject illegal kind×mode combinations per master §4.8.3."""
    if mode == "simulated" and kind != "gtopt-dir":
        raise InputValidationError("--mode simulated requires --input-kind gtopt-dir.")
    if mode in ("real", "real-reconstruct") and kind != "feed-parquet":
        raise InputValidationError(f"--mode {mode} requires --input-kind feed-parquet.")


def _process_cells(
    *,
    topology: Topology,
    cells: Cells,
    lp_duals: GtoptLpDuals,
    tol: Tolerances,
    merit_ladder_depth: int,
    single_bus: bool,
    zone_mode: str,
    demand_fail_cost: float | dict[int, float],
    out_root: Path,
    carbon_price: float = 0.0,
):
    """Execute the per-cell loop and write the dataset.

    Returns the WriteSummary from ``write_dataset``.
    """
    if cells.dispatch.empty:
        raise InputValidationError("cells/dispatch is empty; nothing to attribute.")

    cell_keys = cells.cell_keys()
    per_bus_rows: list[dict] = []
    per_zone_rows: list[dict] = []
    ladder_rows = []
    price_recipe: list = []
    emission_recipe: list = []
    audit_unattributed: list[dict] = []

    bus_uids = topology.bus_uids()
    profile_uids = {
        g.uid for g in topology.generators if g.kind in {k.value for k in PROFILE_KINDS}
    }

    # Pre-compute merit-eligibility (v1: only profile-exclusion).
    merit_eligible = {g.uid: g.uid not in profile_uids for g in topology.generators}

    # Index dispatch / lmp / flow / load by cell-key tuple.
    dispatch_by_cell = _group_by_cell(cells.dispatch, "gen_uid", "dispatch")
    lmp_by_cell = _group_by_cell(cells.lmp, "bus_uid", "lmp") if cells.has_lmp() else {}
    load_by_cell = (
        _group_by_cell(cells.load, "bus_uid", "load") if cells.load is not None else {}
    )
    flow_by_cell = (
        _group_by_cell(cells.flow, "line_uid", "flow") if cells.has_flow() else {}
    )
    # ``Line/flow_cost`` carries the LP-textbook saturation signal: the
    # SIGNED reduced cost on the line's flow column (negative ⇔ at
    # +tmax_ab, positive ⇔ at −tmax_ba, zero ⇔ interior basic).  When
    # available, this is strictly cleaner than the primal-magnitude
    # test ``|flow| ≥ (1−eps) × tmax`` (which produces false positives
    # on lines whose flow lands at e.g. 99.79 % of tmax without the
    # cap actually binding).
    flow_cost_by_cell = (
        _group_by_cell(cells.flow_dual, "line_uid", "flow_dual")
        if cells.has_flow_dual()
        else {}
    )

    # Per-line thermal limit ``max(tmax_ab, tmax_ba)`` used as the
    # primal-fallback saturation test (when ``flow_cost`` is absent —
    # older gtopt outputs without the unified-flow stems).  Pre-built
    # once outside the cell loop so each cell's saturation check is an
    # O(nlines) hash lookup.
    line_tmax: dict[int, float] = {
        ln.uid: max(float(ln.tmax_ab), float(ln.tmax_ba)) for ln in topology.lines
    }

    # Index LP-dual extras. Empty dict iff the parquet was not written
    # (legacy run or --no-require-reduced-cost path).
    gen_rc_by_cell = (
        _group_by_cell(lp_duals.gen_reduced_cost, "gen_uid", COL_GEN_RC)
        if lp_duals.has_gen_reduced_cost()
        else {}
    )
    gen_srmc_by_cell = (
        _group_by_cell(lp_duals.gen_srmc, "gen_uid", COL_GEN_SRMC)
        if lp_duals.has_gen_srmc()
        else {}
    )

    for _, key_row in cell_keys.iterrows():
        cell_key = tuple(
            key_row[c]
            for c in ("scenario", "stage", "block", "date_utc", "hour", "data_source")
        )
        dispatch_by_uid = dispatch_by_cell.get(cell_key, {})
        lmp_by_bus = lmp_by_cell.get(cell_key, {})
        load_by_bus = load_by_cell.get(cell_key, {})
        gen_rc_by_uid = gen_rc_by_cell.get(cell_key, {})
        gen_srmc_by_uid = gen_srmc_by_cell.get(cell_key, {})

        # Zone partition.  A line is "saturated" iff its cap row in
        # the LP is binding — the LP-textbook test.  Two signals:
        #
        # Primary: ``Line/flow_cost`` (signed rc on the flow column).
        # ``|rc| > eps`` ⇔ at upper or lower cap (basic-feasible-
        # solution theory).  Strictly correct because it reads the LP
        # basis, not a primal-magnitude heuristic.  Available since
        # the unified line-flow output (``Line/flow_sol`` + ``flow_cost``).
        #
        # Fallback: primal magnitude ``|flow| ≥ (1−eps) × tmax`` for
        # older gtopt outputs that don't emit ``flow_cost``.  Has known
        # false positives — a line landing at e.g. 99.79 % of tmax
        # with no congestion price gets flagged, splitting a network
        # that's actually still connected and producing spurious
        # "no-marginal-unit" islands (jan18 / Diego-de-Almagro pattern).
        if single_bus or not topology.lines:
            zone_of = {u: 0 for u in bus_uids}
        else:
            saturated_uids: set[int] = set()
            if flow_cost_by_cell:
                # rc test (preferred).  Note we gate on the GLOBAL
                # presence of the ``flow_cost`` stream (``flow_cost_by_cell``),
                # not on the per-cell dict ``flow_cost_by_cell.get(cell_key)``:
                # gtopt's writer filters all-zero rc rows, so a cell with
                # NO saturated lines produces ZERO rows — a perfectly
                # valid "all rc = 0" answer that means "no saturation in
                # this cell".  Falling back to the primal test in that
                # case false-positives on lines landing at e.g. 99.79 %
                # of tmax without the cap actually binding.
                cost_by_uid = flow_cost_by_cell.get(cell_key, {})
                eps_rc = max(tol.tol_price, tol.tol_rc_floor)
                for uid, rc in cost_by_uid.items():
                    if abs(float(rc)) > eps_rc:
                        saturated_uids.add(int(uid))
            else:
                # Primal-fallback for legacy parquet outputs.
                flow_by_uid = flow_by_cell.get(cell_key, {})
                if flow_by_uid and line_tmax:
                    eps_sat = max(tol.tol_flow, tol.tol_sat_floor)
                    for uid, fval in flow_by_uid.items():
                        tmax = line_tmax.get(int(uid))
                        if tmax is None or tmax <= 0.0:
                            continue
                        if abs(float(fval)) >= (1.0 - eps_sat) * tmax:
                            saturated_uids.add(int(uid))
            zone_of = partition_zones(topology, saturated_line_uids=saturated_uids)

        if lmp_by_bus:
            # LP duals are the source of truth for the bus price.
            # Pass the topology-driven ``zone_of`` (connected components
            # after dropping saturated lines) so the marginal-unit
            # search uses **physical islands**, not LMP-value buckets:
            # a bus with no local generator inherits its island's
            # marginal unit via the un-saturated tie lines.
            zone_of, zone_results = _zone_results_from_lp_duals(
                topology=topology,
                lmp_by_bus=lmp_by_bus,
                dispatch_by_uid=dispatch_by_uid,
                gen_rc_by_uid=gen_rc_by_uid,
                gen_srmc_by_uid=gen_srmc_by_uid,
                tol=tol,
                merit_eligible=merit_eligible,
                demand_fail_cost=demand_fail_cost,
                topology_zone_of=zone_of,
            )
        else:
            # mode=real-reconstruct path: no LP duals, run §4.7 R3.
            # ``reconstruct_all_zones`` uses ``demand_fail_cost`` as a
            # zone-level rationing cap (the clamp ``λ_z ≤ DFC``); when
            # we have per-Demand caps, the binding cap is the MAX
            # across all per-bus values (above the highest, every
            # demand is rationed at its own price).  Resolve to a
            # scalar before calling.
            dfc_scalar: float = (
                max(demand_fail_cost.values())
                if isinstance(demand_fail_cost, dict)
                else float(demand_fail_cost)
            )
            zone_results = reconstruct_all_zones(
                topology=topology,
                zone_of=zone_of,
                dispatch_by_uid=dispatch_by_uid,
                load_by_bus=load_by_bus,
                demand_fail_cost=dfc_scalar,
                tol=tol,
                merit_eligible_by_uid=merit_eligible,
            )

        # Per-bus rows.
        for bus_uid in bus_uids:
            # In LP-duals mode the zone partition only covers buses with
            # a balance_dual entry: an orphan bus (no demand AND no
            # generation, common in PLEXOS imports where every node is
            # materialised) has no LMP, so no zone, so no marginal-unit
            # row to emit.  Skip silently.
            zid = zone_of.get(bus_uid)
            if zid is None:
                continue
            zres = zone_results.get(zid)
            if zres is None:
                continue
            zone_lmp = float(zres.lambda_z)
            # Emit one row per generator at this bus that contributed.
            gens_at_bus = [g for g in topology.generators if g.bus_uid == bus_uid]
            if not gens_at_bus:
                # Buses with no generators still get a row tagging the zone.
                per_bus_rows.append(
                    _per_bus_row(
                        cell_key,
                        bus_uid,
                        zid,
                        zone_lmp,
                        gen=None,
                        zres=zres,
                    )
                )
                continue
            for g in gens_at_bus:
                d = dispatch_by_uid.get(g.uid, 0.0)
                status = classify(
                    dispatch=d,
                    pmin=g.pmin,
                    pmax=g.pmax,
                    marginal_cost=g.declared_MC,
                    lmp=zone_lmp,
                    kind=g.kind,
                    tol=tol,
                )
                is_marginal = (
                    status
                    in {
                        Status.MARGINAL,
                        Status.HYDRO_MARGINAL,
                        Status.FORCED_PMIN,
                    }
                    and g.uid in zres.marginal_gen_uids
                )
                per_bus_rows.append(
                    _per_bus_row(
                        cell_key,
                        bus_uid,
                        zid,
                        zone_lmp,
                        gen=g,
                        dispatch=d,
                        status=status,
                        is_marginal=is_marginal,
                        zres=zres,
                    )
                )

        # Per-zone rows.
        for zid, zres in zone_results.items():
            zone_buses = sorted(u for u, z in zone_of.items() if z == zid)
            zone_load = sum(load_by_bus.get(u, 0.0) for u in zone_buses)
            zone_disp = sum(
                dispatch_by_uid.get(g.uid, 0.0)
                for g in topology.generators
                if g.bus_uid in zone_buses
            )
            per_zone_rows.append(
                {
                    **_unpack_cell_key(cell_key),
                    "zone_id": zid,
                    "zone_lmp": float(zres.lambda_z),
                    "bus_uids": zone_buses,
                    "bus_count": len(zone_buses),
                    "marginal_gen_uids": list(zres.marginal_gen_uids),
                    "marginal_gen_names": [
                        next((g.name for g in topology.generators if g.uid == u), "")
                        for u in zres.marginal_gen_uids
                    ],
                    "zone_load_mw": float(zone_load),
                    "zone_dispatch_mw": float(zone_disp),
                    "saturated_line_uids": [],
                    "status": zres.formula_kind,
                    "degenerate": zres.degenerate,
                    "confidence": zres.confidence.value,
                    "data_source": cell_key[5],
                }
            )
            if zres.formula_kind == "unattributed":
                audit_unattributed.append(
                    {
                        **_unpack_cell_key(cell_key),
                        "zone_id": zid,
                        "reason": zres.reason,
                    }
                )

        # Recipes.
        p_rows, e_rows = build_recipes_for_cell(
            cell_key=cell_key,
            topology=topology,
            zone_of=zone_of,
            zone_results=zone_results,
            dispatch_by_uid=dispatch_by_uid,
            lmp_by_bus=lmp_by_bus,
            srmc_by_uid=gen_srmc_by_uid,
            demand_fail_cost=demand_fail_cost,
            tol=tol,
        )
        price_recipe.extend(p_rows)
        emission_recipe.extend(e_rows)

        # Merit ladder per zone.
        if merit_ladder_depth > 0:
            for zid, zres in zone_results.items():
                gens_in_zone = [
                    g for g in topology.generators if zone_of.get(g.bus_uid) == zid
                ]
                ladder_rows.extend(
                    build_ladder(
                        cell_key=cell_key,
                        zone_id=zid,
                        generators_in_zone=gens_in_zone,
                        dispatch_by_uid=dispatch_by_uid,
                        zone_result=zres,
                        depth=merit_ladder_depth,
                        merit_eligible_by_uid=merit_eligible,
                    )
                )

    # Write dataset.
    extras: dict[str, object] = {}
    if carbon_price > 0.0:
        extras["carbon_price_usd_per_ton"] = float(carbon_price)
    summary = write_dataset(
        out_root,
        per_bus=pd.DataFrame(per_bus_rows),
        per_zone=pd.DataFrame(per_zone_rows),
        merit_ladder=ladder_rows,
        price_recipe=price_recipe,
        emission_recipe=emission_recipe,
        unattributed=(pd.DataFrame(audit_unattributed) if audit_unattributed else None),
        extras=extras or None,
    )
    _LOG.info("dataset written: %s", out_root)
    _LOG.info(
        "rows per_bus=%d per_zone=%d ladder=%d price_recipe=%d",
        summary.rows_per_bus,
        summary.rows_per_zone,
        summary.rows_merit_ladder,
        summary.rows_price_recipe,
    )

    # End-of-run loss-factor scaling summary (#523).  Counts cells whose
    # raw bus_LMP / ref ratio fell outside the physical envelope.  ``warn``
    # cells were scaled but flagged; ``negative`` and ``missing`` cells
    # had scale=1.0; ``error`` cells would have aborted the run earlier.
    # ``negative_lmp_kind`` is tracked separately so the two regimes
    # (no_marginal vs storage_clamped) get distinct warnings.
    status_counts: dict[str, int] = {}
    neg_lmp_counts: dict[str, int] = {}
    raw_max = float("-inf")
    raw_min = float("inf")
    for r in emission_recipe:
        status_counts[r.loss_factor_status] = (
            status_counts.get(r.loss_factor_status, 0) + 1
        )
        if r.loss_factor_status in (
            "ok",
            "warn_radial",
            "warn_meshed",
            "critical_radial",
            "critical_meshed",
        ):
            raw_max = max(raw_max, r.loss_factor_raw)
            raw_min = min(raw_min, r.loss_factor_raw)
        if r.negative_lmp_kind:
            neg_lmp_counts[r.negative_lmp_kind] = (
                neg_lmp_counts.get(r.negative_lmp_kind, 0) + 1
            )
    if status_counts:
        n_ok = status_counts.get("ok", 0)
        n_na = status_counts.get("n/a", 0)
        n_wr = status_counts.get("warn_radial", 0)
        n_wm = status_counts.get("warn_meshed", 0)
        n_cr = status_counts.get("critical_radial", 0)
        n_cm = status_counts.get("critical_meshed", 0)
        n_ph = status_counts.get("phantom_bus", 0)
        n_xi = status_counts.get("cross_island", 0)
        n_zh = status_counts.get("zero_srmc_hydro", 0)
        n_ne_data = status_counts.get("no_emission_data", 0)
        n_neg = status_counts.get("negative", 0)
        n_miss = status_counts.get("missing", 0)
        _LOG.info(
            "loss-factor scaling: ok=%d  n/a=%d  warn_radial=%d  "
            "warn_meshed=%d  critical_radial=%d  critical_meshed=%d  "
            "phantom_bus=%d  cross_island=%d  zero_srmc_hydro=%d  "
            "no_emission_data=%d  negative=%d  missing=%d  "
            "raw range [%s, %s]",
            n_ok,
            n_na,
            n_wr,
            n_wm,
            n_cr,
            n_cm,
            n_ph,
            n_xi,
            n_zh,
            n_ne_data,
            n_neg,
            n_miss,
            f"{raw_min:.3f}" if raw_min != float("inf") else "n/a",
            f"{raw_max:.3f}" if raw_max != float("-inf") else "n/a",
        )
        if n_zh:
            _LOG.warning(
                "loss-factor scaling: %d cells had a hydro/storage "
                "marginal at near-zero LMP (tol_lmp=%g). Cannot derive "
                "a meaningful loss factor — scale=1 applied. Common "
                "cause: LP elects one zero-MC hydro as basic-in-LP for "
                "a country-spanning connected component.",
                n_zh,
                tol.tol_lmp,
            )
        if n_ne_data:
            _LOG.warning(
                "loss-factor scaling: %d cells had a thermal marginal "
                "with no emission_rate (e.g. cogen biomass). EF=0 means "
                "no carbon to scale — scale=1 applied. Recipe row's "
                "recomputed_emission_intensity stays 0 either way.",
                n_ne_data,
            )
        if n_wr:
            _LOG.warning(
                "loss-factor scaling: %d cells on a RADIAL path had "
                "raw bus_LMP / ref ∈ (%g, %g] — applied as-is (R "
                "drives the loss on a single-path corridor).  Filter "
                "on loss_factor_status == 'warn_radial'.",
                n_wr,
                tol.loss_factor_warn,
                tol.loss_factor_error,
            )
        if n_wm:
            _LOG.warning(
                "loss-factor scaling: %d cells on a MESHED path had "
                "raw bus_LMP / ref ∈ (%g, %g] — applied as-is but "
                "review marginal-unit selection (high ratios on "
                "meshed networks usually indicate congestion or a "
                "wrong reference price).  Filter on loss_factor_status "
                "== 'warn_meshed'.",
                n_wm,
                tol.loss_factor_warn,
                tol.loss_factor_error,
            )
        if n_cr:
            _LOG.warning(
                "loss-factor scaling: %d cells on a RADIAL path had "
                "raw > --loss-factor-error (%g) — applied as-is.  "
                "Likely a very long / high-R radial corridor (Aysén / "
                "Chiloé pattern); cross-check the line impedance "
                "against the source PLEXOS XML.  Filter on "
                "loss_factor_status == 'critical_radial'.",
                n_cr,
                tol.loss_factor_error,
            )
        if n_cm:
            _LOG.warning(
                "loss-factor scaling: %d cells on a MESHED path had "
                "raw > --loss-factor-error (%g) — applied as-is.  "
                "This is the most suspicious bucket: meshed + very "
                "high ratio almost always indicates a wrong marginal-"
                "unit selection or LP-stress artefact.  Filter on "
                "loss_factor_status == 'critical_meshed' and review.",
                n_cm,
                tol.loss_factor_error,
            )
        if n_xi:
            _LOG.warning(
                "loss-factor scaling: %d cells had cross-island "
                "marginal (bus and marginal-unit live in different "
                "connected components after dropping saturated lines). "
                "Scale=1.0 was applied; see loss_factor_status == "
                "'cross_island' rows.",
                n_xi,
            )
        if n_neg:
            _LOG.warning(
                "loss-factor scaling: %d cells had negative raw "
                "(oversupply / negative LMP); scale=1.0 applied.",
                n_neg,
            )
    # Negative-LMP regime summary (two distinct cases) — both clamped
    # the recipe's r_lmp and r_em to 0, but for different physical
    # reasons.  Downstream consumers (arbitrage / battery balance)
    # should filter the cells out.
    n_neg_no_marg = neg_lmp_counts.get("no_marginal", 0)
    n_neg_storage = neg_lmp_counts.get("storage_clamped", 0)
    if n_neg_no_marg:
        _LOG.warning(
            "negative-LMP: %d cells had zone lambda_z < 0 with NO "
            "storage marginal (reactance loop, energy constraint, "
            "spillover penalty — no real marginal unit). Recipe rows "
            "carry formula_kind == 'no_marginal_neg_lmp' and "
            "negative_lmp_kind == 'no_marginal'; both LMP and "
            "emission factor are clamped to 0. Downstream arbitrage "
            "MUST filter these cells (no guaranteed payment).",
            n_neg_no_marg,
        )
    if n_neg_storage:
        _LOG.warning(
            "negative-LMP: %d cells had zone lambda_z < 0 with a "
            "STORAGE marginal (spillover / regulation / energy-"
            "constraint binding on the battery / reservoir itself). "
            "Recipe rows keep their formula_kind but carry "
            "negative_lmp_kind == 'storage_clamped'; both LMP and "
            "emission factor are clamped to 0. The storage IS a real "
            "marginal at zero price for this block.",
            n_neg_storage,
        )
    return summary


def _matches_any_demand_fail_cost(
    lam: float,
    dfc: float | dict[int, float],
    zid: int,
    zone_of: dict[int, int],
    tol_price: float,
) -> bool:
    """Match zone LMP against demand_fail_cost (per-Demand, B2).

    A zone hits the rationing cap when ``lambda_z`` equals the fcost of
    ANY demand in that zone — different buses in the same zone may
    carry different per-Demand fcost.  The zone-level test must accept
    any of them; we tolerance-match against the set of per-bus values
    that belong to this zone.  Falls back to scalar match when called
    with a float (legacy / test path).
    """
    if isinstance(dfc, dict):
        for bus_uid, bz in zone_of.items():
            if bz != zid:
                continue
            v = dfc.get(bus_uid)
            if v is not None and abs(lam - v) <= tol_price:
                return True
        return False
    return abs(lam - float(dfc)) <= tol_price


def _zone_results_from_lp_duals(
    *,
    topology,
    lmp_by_bus: dict[int, float],
    dispatch_by_uid: dict[int, float],
    gen_rc_by_uid: Optional[dict[int, float]] = None,
    gen_srmc_by_uid: Optional[dict[int, float]] = None,
    tol: Tolerances,
    merit_eligible: dict[int, bool],
    demand_fail_cost: float | dict[int, float],
    topology_zone_of: Optional[dict[int, int]] = None,
) -> tuple[dict[int, int], dict[int, "ZoneR3Result"]]:
    """Build (zone_of, zone_results) directly from LP duals.

    Zone partition strategy:

    * **Topology islands (preferred)** — when ``topology_zone_of`` is
      supplied, use it as the partition.  Each island (connected
      component of the topology graph after dropping saturated lines)
      shares one marginal unit; per-bus ``λ_b`` within an island can
      still vary (loss-driven micro-variation), but the marginal-unit
      attribution is the same across the island.  This is the LP-
      textbook correct view: a bus with no local generator inherits
      its island's marginal unit via tie lines.

    * **Bus-LMP bucketing (fallback)** — when ``topology_zone_of`` is
      None, fall back to grouping buses whose ``λ_b`` are within
      ``tol_lmp`` of each other.  This was the v1 default and creates
      singleton zones for every bus with a slightly unique LMP — fine
      for tiny test cases but produces ~80 % "no-local-gen"
      unattributed cells on real PLEXOS imports.

    When ``gen_rc_by_uid`` is populated (gtopt was run with
    ``--write-out ...,rc``), the primary basic-vs-bound test is the
    min-|rc| ranking on dispatched gens — the LP-textbook definition
    of "basic at its current value".  See
    :func:`_select_marginal_candidates`.

    Imports lazy to keep the main module's import surface small.
    """
    from gtopt_marginal_units._reconstruct import ZoneR3Result  # noqa: PLC0415

    rc_by_uid: dict[int, float] = gen_rc_by_uid or {}
    srmc_by_uid: dict[int, float] = gen_srmc_by_uid or {}

    if topology_zone_of:
        # Topology-driven islands.  Each zone's representative λ is
        # the MEAN of its buses' LMPs — for non-congested islands all
        # buses share λ exactly so the mean equals every per-bus value;
        # for islands with loss-driven micro-variation the mean is a
        # reasonable single number to record on the zone, while the
        # per-bus output rows still carry the exact ``lmp_by_bus[b]``.
        bus_to_zone = {
            b: int(topology_zone_of[b]) for b in lmp_by_bus if b in topology_zone_of
        }
        zid_to_lams: dict[int, list[float]] = {}
        for b, zid in bus_to_zone.items():
            zid_to_lams.setdefault(zid, []).append(float(lmp_by_bus[b]))
        rep_lmps = [sum(lams) / len(lams) for _, lams in sorted(zid_to_lams.items())]
        # Remap to dense zid sequence for downstream iteration.
        old_to_new = {
            old: new for new, (old, _) in enumerate(sorted(zid_to_lams.items()))
        }
        bus_to_zone = {b: old_to_new[z] for b, z in bus_to_zone.items()}
    else:
        # Bucket bus uids by LMP value (within tol_price).
        sorted_buses = sorted(lmp_by_bus.keys())
        rep_lmps = []
        bus_to_zone = {}
        for b in sorted_buses:
            lam = float(lmp_by_bus[b])
            zid = -1
            for i, r in enumerate(rep_lmps):
                if abs(lam - r) <= max(tol.tol_lmp, tol.tol_lmp * abs(r)):
                    zid = i
                    break
            if zid == -1:
                rep_lmps.append(lam)
                zid = len(rep_lmps) - 1
            bus_to_zone[b] = zid

    # For each zone, find candidate marginal units via min-|rc|
    # ranking (preferred) or declared_MC fallback.
    zone_results: dict[int, ZoneR3Result] = {}
    for zid, lam in enumerate(rep_lmps):
        zone_bus_uids = {b for b, z in bus_to_zone.items() if z == zid}
        candidates, reason = _select_marginal_candidates(
            topology=topology,
            zone_bus_uids=zone_bus_uids,
            lam=lam,
            dispatch_by_uid=dispatch_by_uid,
            rc_by_uid=rc_by_uid,
            srmc_by_uid=srmc_by_uid,
            tol=tol,
            merit_eligible=merit_eligible,
        )
        if candidates:
            # Renewable-curtailment detection (#526): when LMP ≈ 0 AND
            # every candidate is a profile (solar / wind) generator,
            # the marginal is "free renewable on the margin" — classify
            # as renewable_curtailment so consumers can distinguish
            # this case from a thermal-marginal "tied_units at $0" and
            # report dispatch / emissions accordingly.
            profile_kinds = {"profile", "solar", "wind"}
            all_profile = all(g.kind in profile_kinds for g in candidates)
            if abs(lam) <= tol.tol_price and all_profile:
                kind = "renewable_curtailment"
                reason_tag = (reason or "") + ";renewable_at_zero_lmp"
            else:
                kind = "single_unit" if len(candidates) == 1 else "tied_units"
                reason_tag = reason
            zone_results[zid] = ZoneR3Result(
                zone_id=zid,
                lambda_z=lam,
                formula_kind=kind,
                marginal_gen_uids=[g.uid for g in candidates],
                confidence=Confidence.LP_DUAL,
                degenerate=False,
                reason=reason_tag,
                clamped=False,
            )
        elif _matches_any_demand_fail_cost(
            lam, demand_fail_cost, zid, bus_to_zone, tol.tol_price
        ):
            zone_results[zid] = ZoneR3Result(
                zone_id=zid,
                lambda_z=lam,
                formula_kind="demand_fail",
                marginal_gen_uids=[],
                confidence=Confidence.LP_DUAL,
                degenerate=True,
                reason="lp_dual_at_demand_fail_cost",
                clamped=False,
            )
        else:
            # No interior merit candidate found.  Before falling through
            # to ``unattributed``, check three more cases:
            #
            #   (a) Phantom-bus / storage-marginal (#525) — a zone whose
            #       buses are all ``*_int_bus`` AND/OR whose only
            #       generators are synthetic ``BAT_*_LOAD`` / ``_gen``
            #       wrappers.  Classified as ``hydro_marginal``.
            #
            #   (b) Empty island — one or more buses sharing a zone
            #       with no demand, no merit candidate, and no
            #       generator with positive pmax at this cell.
            #       Nothing is happening on the island this hour; LP
            #       gives LMP=0 by free-vertex choice.  Classified as
            #       ``empty_island`` (#43, follow-up to #526).
            #       Singleton case = Ralco220 (tie lines inactive +
            #       gens decommissioned); multi-bus case = a whole
            #       sub-network disconnected from the main grid.
            from gtopt_marginal_units._zones import is_phantom_bus  # noqa: PLC0415

            bus_by_uid = {b.uid: b for b in topology.buses}
            gens_by_bus: dict[int, list] = {}
            for g in topology.generators:
                gens_by_bus.setdefault(g.bus_uid, []).append(g)
            all_phantom = bool(zone_bus_uids) and all(
                is_phantom_bus(
                    str(bus_by_uid.get(b).name if bus_by_uid.get(b) else ""),
                    gens_by_bus.get(b, []),
                )
                for b in zone_bus_uids
            )

            # Empty-island check (#43): every bus in the zone has
            # neither demand nor pmax-bearing gens AND LMP ≈ 0.
            # We don't have ``load_by_bus`` in this function, but if
            # there were unserved demand the LP would have lifted LMP
            # to demand_fail_cost (handled by the branch above), so
            # |LMP|≈0 is the safe demand-side filter.  On the supply
            # side we check that no gen in any zone bus carries a
            # positive ``pmax`` — the LP can only export from a bus
            # with capacity.  Together: nothing flowing on the
            # island this hour.  Works for singleton (Ralco220 tie-
            # lines inactive + decommissioned gens) and multi-bus
            # (whole sub-network disconnected from main grid) cases.
            if not all_phantom and abs(lam) <= tol.tol_price:
                zone_has_capacity = any(
                    (g.pmax or 0) > tol.eps
                    for b in zone_bus_uids
                    for g in gens_by_bus.get(b, [])
                )
                if not zone_has_capacity:
                    zone_results[zid] = ZoneR3Result(
                        zone_id=zid,
                        lambda_z=lam,
                        formula_kind="empty_island",
                        marginal_gen_uids=[],
                        confidence=Confidence.LP_DUAL,
                        degenerate=False,
                        reason="island_has_no_active_capacity",
                        clamped=False,
                    )
                    continue

            if all_phantom:
                # Pick a representative synthetic gen as marginal_gen_uid
                # — informational only; the consumer keys on
                # formula_kind="hydro_marginal" to know the bus is a
                # storage internal.  em = 0 by physics for these.
                synth_gens = sorted(
                    (g for b in zone_bus_uids for g in gens_by_bus.get(b, [])),
                    key=lambda g: g.uid,
                )
                zone_results[zid] = ZoneR3Result(
                    zone_id=zid,
                    lambda_z=lam,
                    formula_kind="hydro_marginal",
                    marginal_gen_uids=[synth_gens[0].uid] if synth_gens else [],
                    confidence=Confidence.LP_DUAL,
                    degenerate=False,
                    reason="phantom_bus_storage_marginal",
                    clamped=False,
                )
                continue

            # Congested or degenerate — λ matches no interior MC.
            # Emit unattributed; recipe will mark it explicitly.
            zone_results[zid] = ZoneR3Result(
                zone_id=zid,
                lambda_z=lam,
                formula_kind="unattributed",
                marginal_gen_uids=[],
                confidence=Confidence.LP_DUAL,
                degenerate=True,
                reason="lp_dual_no_interior_match",
                clamped=False,
            )
    return bus_to_zone, zone_results


def _is_synthetic_gen(g) -> bool:
    """Return True for PLEXOS / CEN-PCP placeholder generators that
    cannot physically set the LMP at grid scale.

    Two signatures match (both empirically derived from CEN-PCP bundles):

    1. **``pmax < SYNTHETIC_GEN_PMAX_MW``** — CCGT alternative-dispatch
       entries that PLEXOS ships with ``Gen_Rating = (missing)``;
       plexos2gtopt clamps them at ``0.01 MW`` so they always carry a
       defined ``pmax`` but never participate meaningfully in dispatch.
       ~50 such gens in jan18 (``ATA_CC1_TGA_DIE``, ``KELAR-TG1_GNL_X``,
       ``MEJILLONES_3-TG+TV_GNL_X``, …).
    2. **Name ending in ``_INF`` / ``_GNL_INF`` / ``_NOGNL_INF``** —
       PLEXOS infinity / inflexible-LNG placeholders used for gas-import
       accounting (``TAMAKAYA_NOGNL_INF`` and similar).

    These can never be the "real" marginal but the LP basis still
    picks them at tie-break corners (``gcost = 0 → rc ≈ 0``).  Skipping
    them here lets the cascade fall through to a real-capacity thermal
    that genuinely sets the price.
    """
    try:
        if float(g.pmax) < SYNTHETIC_GEN_PMAX_MW:
            return True
    except (TypeError, ValueError):
        pass
    name = getattr(g, "name", "") or ""
    return any(name.endswith(suffix) for suffix in SYNTHETIC_GEN_NAME_SUFFIXES)


def _select_marginal_candidates(
    *,
    topology,
    zone_bus_uids: set[int],
    lam: float,
    dispatch_by_uid: dict[int, float],
    rc_by_uid: dict[int, float],
    srmc_by_uid: dict[int, float],
    tol: Tolerances,
    merit_eligible: dict[int, bool],
):
    """Pick the marginal-unit candidates for one zone.

    Priority:
      1. If reduced costs are available, return interior generators
         (``pmin + ε < dispatch < pmax − ε``) with ``|rc| ≤ tol.tol_price``.
         This is the LP-textbook definition of "basic at its current
         value".  Two reasons it beats the legacy match:
           - Piecewise generators on a non-primary segment have a
             ``declared_MC`` from the JSON that does not equal the
             active-segment slope; the rc test does not care.
           - Hydro / battery units have JSON ``gcost ≈ 0`` and their
             true MC is the reservoir / battery shadow price.  Their
             ``rc`` is still ≈ 0 when they set the price, even though
             ``declared_MC ≠ lam``.
      2. If reduced costs are missing, fall back to the legacy
         ``|declared_MC − lam| ≤ tol_price`` match on interior gens.

    Returns ``(candidates, reason)`` where ``reason`` tags which
    branch was taken.  Sorted by uid for determinism.
    """
    # Candidate pool: dispatched gens at this zone's buses.  We
    # deliberately do NOT pre-filter on ``(pmin+eps, pmax-eps)``
    # against the topology nameplate ``pmax`` — the LP's effective
    # per-block upper bound is typically below nameplate
    # (lossfactor-adjusted, profile-driven, fuel-cap, or heat-rate-
    # segment binding), so a gen dispatching at e.g.  ``131 MW``
    # against a nameplate ``152 MW`` may already be saturated at a
    # per-block cap.  Trust the rc instead: if the column is basic in
    # the LP, its reduced cost is ≈ 0.
    #
    # Profile-eligibility fallback: prefer non-profile (thermal /
    # hydro / battery) candidates when present, but allow profiles
    # as a last-resort marginal when no other dispatched gen exists
    # in the zone.  Without this fallback, zones with ONLY FV / wind
    # dispatching (small isolated pockets that import via saturated
    # lines, e.g. CEN northern desert in daytime) fall through to
    # ``unattributed`` even though there IS a basic-in-LP gen
    # available — just one that the merit-eligible filter discards
    # for "intermittents don't set the price" reasons.  The
    # consequential-MOER picker downstream walks past the chosen
    # profile gen automatically (its emission_rate = 0), so the
    # consumer still gets the carbon-opportunity value from the
    # next-up thermal in the zone or in the topology.
    dispatched = [
        g
        for g in topology.generators
        if g.bus_uid in zone_bus_uids and dispatch_by_uid.get(g.uid, 0.0) > tol.eps
    ]
    # Pre-filter — drop *synthetic* placeholder gens (CEN-PCP CCGT mode-
    # variants with ``pmax = 0.01 MW`` + ``gcost = 0`` + no Fuels link,
    # and PLEXOS ``_INF`` infinity placeholders).  They can never
    # physically set the LMP at grid scale but the LP's tie-break
    # corners do elect them at the basis (``rc ≈ 0`` because they
    # carry no cost coefficient), which then trips Guard B
    # ``no_emission_data`` downstream when they're picked over a
    # real-capacity thermal.  Filter them here with a graceful
    # fallback: if every dispatched gen in this zone is synthetic, the
    # cascade proceeds on the unfiltered set (the recipe still needs
    # *something* to anchor the consequential walk-up).
    real_capacity = [g for g in dispatched if not _is_synthetic_gen(g)]
    candidate_input = real_capacity or dispatched
    # Filter cascade — exclude price-takers from the LP-marginal pool,
    # but never return empty (the recipe layer's consequential walk-up
    # depends on getting *some* dispatched gen back).  The four kinds
    # of price-takers we exclude:
    #
    #   1. **cogens** (``is_cogen=True``) — self-dispatching biomass /
    #      refinery / paper-mill units; the LP sees them as fixed
    #      injections, not market participants.
    #   2. **batteries** (``kind == battery``) — bid on stored-energy
    #      shadow price, not a thermodynamic MC.
    #   3. **reservoir hydro** (``kind == hydro``) — bid on water value
    #      (reservoir dual), not MC.
    #   4. **profiles** (``kind == profile``, captured by
    #      ``merit_eligible``) — FV / wind / RoR with zero MC.
    #
    # All four can be basic-in-LP (``|rc| ≈ 0``) at tie-break corners,
    # which then poisons the downstream emission attribution.  The
    # canonical example on jan18 is PAS_MEJILLONES (cogen) being
    # elected as marginal at 41 cells → ``no_emission_data`` bucket.
    # Excluding them here lets the recipe layer's
    # ``_compute_consequential_moer`` walk the merit ladder up to the
    # next-up thermal unit (the "real" marginal that responds to
    # demand) for both the price and the CO2 attribution.
    #
    # The cascade is graceful: if a zone has *only* price-takers
    # dispatching (small islanded BESS zone, hydro-only basin, cogen-
    # only refinery node), we fall through to wider tiers so the LP
    # selection still returns *something* and the consumer doesn't
    # see ``unattributed`` purely due to local merit-pool emptiness.
    #
    # Cascade priority (each tier strictly inside the next):
    #   1. real setters         — thermal ∧ ¬cogen           (the LP price-setter)
    #   2. non-storage non-cogen — thermal ∨ profile, ¬cogen  (allow profile fallback)
    #   3. non-cogen            — anything except cogens     (drop storage filter)
    #   4. non-profile          — anything except profiles   (drop cogen filter)
    #   5. dispatched           — last resort                (recipe walks up downstream)
    setter_kinds = {k.value for k in PRICE_SETTER_KINDS}
    storage_kinds = {k.value for k in STORAGE_KINDS}
    real_setters = [
        g for g in candidate_input if not g.is_cogen and g.kind in setter_kinds
    ]
    nc_nostorage = [
        g for g in candidate_input if not g.is_cogen and g.kind not in storage_kinds
    ]
    nc = [g for g in candidate_input if not g.is_cogen]
    nonprofile = [g for g in candidate_input if merit_eligible.get(g.uid, True)]
    candidates_pool = (
        real_setters or nc_nostorage or nc or nonprofile or candidate_input
    )
    # Backward-compatible alias used by the legacy declared-MC fallback
    # below; that path is informational only (kept for runs with no rc).
    interior = candidates_pool

    if rc_by_uid and interior:
        # LP-textbook marginal-unit test (inverted from the old absolute
        # ``|rc| ≤ tol_price`` filter): pick the dispatched interior gens
        # with the **smallest** ``|rc|`` in this zone.  By the LP basic-
        # feasible-solution definition, a column is "basic" (the marginal
        # unit setting the zone price) iff its reduced cost is ≈ 0; the
        # ones with ``|rc| > 0`` are at their bounds (pmin/pmax) or on a
        # non-primary piecewise segment.
        #
        # The previous absolute threshold (default ``0.01 $/MWh``) was too
        # tight against actual LP basis noise (~``0.4 $/MWh`` on CEN-scale
        # cases), so 88 % of cells fell into ``lp_dual_no_interior_match``.
        # The relative band below — gens within ``tol_price``
        # absolute + ``tol_price`` × max(1, |λ|) relative slack of the
        # zone's min-|rc| — is robust to both extremes (tight on
        # well-conditioned slack, generous when the basis is degenerate).
        #
        # ``rc_by_uid`` may not carry every dispatched gen (some
        # parquet stems are sharded by (scene, phase) and the read
        # may miss a class).  Exclude gens with missing rc rather than
        # default to 0.0: a fake-zero rc would always win the min.
        with_rc = [g for g in interior if g.uid in rc_by_uid]
        if with_rc:

            def _abs_rc(g):
                return abs(rc_by_uid[g.uid])

            min_abs_rc = min(_abs_rc(g) for g in with_rc)
            band = max(tol.tol_price, tol.tol_price * abs(lam))
            rc_match = sorted(
                (g for g in with_rc if _abs_rc(g) <= min_abs_rc + band),
                key=lambda g: g.uid,
            )
            if rc_match:
                return rc_match, "interior_rc_zero_lp_dual"

    # Legacy / fallback: declared_MC match.
    mc_match = [
        g
        for g in interior
        if g.declared_MC is not None
        and abs(float(g.declared_MC) - lam)
        <= max(tol.tol_price, tol.tol_price * abs(lam))
    ]
    mc_match.sort(key=lambda g: g.uid)
    return mc_match, "interior_match_lp_dual"


def _per_bus_row(
    cell_key: tuple,
    bus_uid: int,
    zid: int,
    zone_lmp: float,
    *,
    gen=None,
    dispatch: float = 0.0,
    status=None,
    is_marginal: bool = False,
    zres=None,
) -> dict:
    return {
        **_unpack_cell_key(cell_key),
        "zone_id": zid,
        "zone_lmp": zone_lmp,
        "bus_uid": bus_uid,
        "gen_uid": gen.uid if gen is not None else None,
        "gen_name": gen.name if gen is not None else None,
        "status": status.value
        if status is not None
        else (zres.formula_kind if zres is not None else "unattributed"),
        "dispatch": float(dispatch) if gen is not None else None,
        "pmin": gen.pmin if gen is not None else None,
        "pmax": gen.pmax if gen is not None else None,
        "marginal_cost": gen.declared_MC if gen is not None else None,
        "reduced_cost": None,
        "active_segment": -1,
        "is_marginal": bool(is_marginal),
        "data_source": cell_key[5],
        "confidence": zres.confidence.value if zres is not None else "fallback",
        "degenerate": bool(zres.degenerate) if zres is not None else False,
        "reason": zres.reason if zres is not None else "",
    }


def _unpack_cell_key(cell_key: tuple) -> dict:
    scenario, stage, block, date_utc, hour, data_source = cell_key
    return {
        "scenario": scenario,
        "stage": stage,
        "block": block,
        "date_utc": date_utc,
        "hour": hour,
        "data_source": data_source,
    }


def _group_by_cell(
    df: Optional[pd.DataFrame],
    uid_col: str,
    value_col: str,
) -> dict[tuple, dict[int, float]]:
    """Group a long-form Cells frame by cell_key tuple → {uid: value}."""
    if df is None or df.empty:
        return {}
    out: dict[tuple, dict[int, float]] = {}
    key_cols = ("scenario", "stage", "block", "date_utc", "hour", "data_source")
    for _, row in df.iterrows():
        key = tuple(row.get(c) for c in key_cols)
        out.setdefault(key, {})[int(row[uid_col])] = float(row[value_col])
    return out


if __name__ == "__main__":
    sys.exit(cli())
