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
from gtopt_marginal_units._recipes import build_recipes_for_cell
from gtopt_marginal_units._reconstruct import reconstruct_all_zones
from gtopt_marginal_units._report import write_report
from gtopt_marginal_units._zones import partition_zones
from gtopt_marginal_units.constants import (
    DEFAULT_MERIT_LADDER_DEPTH,
    EXIT_INPUT_ERROR,
    EXIT_OK,
    EXIT_UNATTRIBUTED,
    PROFILE_KINDS,
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
            "docs/scripts/gtopt_marginal_units_plan.md for the full design."
        ),
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
    p.add_argument("--report", type=Path, help="Path to write a Markdown report.")
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

    # 3. Load Topology + Cells.
    if kind == "gtopt-dir":
        if args.planning is None or args.output is None:
            raise InputValidationError(
                "--input-kind gtopt-dir requires both --planning and --output"
            )
        topology, cells = read_gtopt(args.planning, args.output)
    else:  # feed-parquet
        from gtopt_marginal_units._feed_reader import read_canonical_feed  # noqa: PLC0415

        if args.feed is None:
            raise InputValidationError("--input-kind feed-parquet requires --feed")
        drop_lmp = args.mode == "real-reconstruct"
        topology, cells = read_canonical_feed(args.feed, drop_lmp=drop_lmp)

    tol = Tolerances(
        eps=args.eps,
        tol_price=args.tol_price,
        tol_flow=args.tol_flow,
        tol_mu=args.tol_mu,
        tol_lmp=Tolerances.default().tol_lmp,
        tol_load_mw=args.tol_load_mw,
    )

    # 4. Per-cell driver (single-bus collapse if asked).
    summary = _process_cells(
        topology=topology,
        cells=cells,
        tol=tol,
        merit_ladder_depth=max(0, int(args.merit_ladder_depth)),
        single_bus=args.single_bus,
        zone_mode=args.zone_mode,
        demand_fail_cost=float(args.demand_fail_cost),
        out_root=args.out,
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
    tol: Tolerances,
    merit_ladder_depth: int,
    single_bus: bool,
    zone_mode: str,
    demand_fail_cost: float,
    out_root: Path,
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

    for _, key_row in cell_keys.iterrows():
        cell_key = tuple(
            key_row[c]
            for c in ("scenario", "stage", "block", "date_utc", "hour", "data_source")
        )
        dispatch_by_uid = dispatch_by_cell.get(cell_key, {})
        lmp_by_bus = lmp_by_cell.get(cell_key, {})
        load_by_bus = load_by_cell.get(cell_key, {})

        # Zone partition.
        if single_bus or not topology.lines:
            zone_of = {u: 0 for u in bus_uids}
        else:
            zone_of = partition_zones(
                topology
            )  # v1: no saturation detection in main loop

        if lmp_by_bus:
            # LP duals are the source of truth for the bus price.
            # Repartition by LMP-value buckets so each "zone" has a
            # uniform λ, then build ZoneR3Result entries by finding
            # the interior unit at that bus whose MC matches λ.
            zone_of, zone_results = _zone_results_from_lp_duals(
                topology=topology,
                lmp_by_bus=lmp_by_bus,
                dispatch_by_uid=dispatch_by_uid,
                tol=tol,
                merit_eligible=merit_eligible,
                demand_fail_cost=demand_fail_cost,
            )
        else:
            # mode=real-reconstruct path: no LP duals, run §4.7 R3.
            zone_results = reconstruct_all_zones(
                topology=topology,
                zone_of=zone_of,
                dispatch_by_uid=dispatch_by_uid,
                load_by_bus=load_by_bus,
                demand_fail_cost=demand_fail_cost,
                tol=tol,
                merit_eligible_by_uid=merit_eligible,
            )

        # Per-bus rows.
        for bus_uid in bus_uids:
            zid = zone_of[bus_uid]
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
    summary = write_dataset(
        out_root,
        per_bus=pd.DataFrame(per_bus_rows),
        per_zone=pd.DataFrame(per_zone_rows),
        merit_ladder=ladder_rows,
        price_recipe=price_recipe,
        emission_recipe=emission_recipe,
        unattributed=(pd.DataFrame(audit_unattributed) if audit_unattributed else None),
    )
    _LOG.info("dataset written: %s", out_root)
    _LOG.info(
        "rows per_bus=%d per_zone=%d ladder=%d price_recipe=%d",
        summary.rows_per_bus,
        summary.rows_per_zone,
        summary.rows_merit_ladder,
        summary.rows_price_recipe,
    )
    return summary


def _zone_results_from_lp_duals(
    *,
    topology,
    lmp_by_bus: dict[int, float],
    dispatch_by_uid: dict[int, float],
    tol: Tolerances,
    merit_eligible: dict[int, bool],
    demand_fail_cost: float,
) -> tuple[dict[int, int], dict[int, "ZoneR3Result"]]:
    """Build (zone_of, zone_results) directly from LP duals.

    In simulated mode the LP has already solved the price; we just
    need to (a) bucket buses by λ_b value and (b) find the marginal
    unit(s) at each bucket so the recipe table is consistent.

    Imports lazy to keep the main module's import surface small.
    """
    from gtopt_marginal_units._reconstruct import ZoneR3Result  # noqa: PLC0415

    # Bucket bus uids by LMP value (within tol_price).
    sorted_buses = sorted(lmp_by_bus.keys())
    rep_lmps: list[float] = []
    bus_to_zone: dict[int, int] = {}
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

    # For each zone, find candidate marginal units: interior gens
    # at that zone's buses whose MC matches λ_z.
    zone_results: dict[int, ZoneR3Result] = {}
    for zid, lam in enumerate(rep_lmps):
        zone_bus_uids = {b for b, z in bus_to_zone.items() if z == zid}
        candidates = [
            g
            for g in topology.generators
            if g.bus_uid in zone_bus_uids
            and merit_eligible.get(g.uid, True)
            and g.declared_MC is not None
            and (g.pmin + tol.eps)
            < dispatch_by_uid.get(g.uid, 0.0)
            < (g.pmax - tol.eps)
            and abs(float(g.declared_MC) - lam)
            <= max(tol.tol_price, tol.tol_price * abs(lam))
        ]
        candidates.sort(key=lambda g: g.uid)
        if candidates:
            kind = "single_unit" if len(candidates) == 1 else "tied_units"
            zone_results[zid] = ZoneR3Result(
                zone_id=zid,
                lambda_z=lam,
                formula_kind=kind,
                marginal_gen_uids=[g.uid for g in candidates],
                confidence=Confidence.LP_DUAL,
                degenerate=False,
                reason="interior_match_lp_dual",
                clamped=False,
            )
        elif abs(lam - demand_fail_cost) <= tol.tol_price:
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
