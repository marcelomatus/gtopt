#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""gtopt-net — bus/line reducer + solution projector for gtopt cases.

Subcommands:

* ``reduce``               collapse a gtopt JSON case to K buses
* ``project-results``      map reduced dispatch parquets back to nodal
* ``project-investment``   map per-cluster expansion deltas back to nodal
* ``cascade-run``          orchestrate reduce → run gtopt → project
"""

from __future__ import annotations

import argparse
import json
import logging
import sys
from pathlib import Path
from typing import Any

from gtopt_reduce_network._busmap import (
    save_aggregator,
    save_busmap,
    save_linemap,
    save_reducer_config,
)
from gtopt_reduce_network._cascade_run import cascade_run
from gtopt_reduce_network._io import load_case, save_case
from gtopt_reduce_network._project_dispatch import project_results
from gtopt_reduce_network._project_investment import (
    ProjectInvestmentConfig,
    project_investment,
    write_audit_csv,
    write_patch_json,
)
from gtopt_reduce_network._protected_lines import collect_protected_lines
from gtopt_reduce_network._reduce import ReduceConfig, reduce_case
from gtopt_reduce_network._user_constraints import filter_line_user_constraints

_LOG_LEVELS = ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]


def main(argv: list[str] | None = None) -> None:
    parser = argparse.ArgumentParser(
        prog="gtopt-net",
        description="Bus/line reducer and solution projector for gtopt cases.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="INFO",
        choices=_LOG_LEVELS,
        help="logging verbosity (default: INFO)",
    )

    sub = parser.add_subparsers(dest="cmd", required=True)
    _add_reduce_parser(sub.add_parser("reduce", help="reduce a gtopt JSON case"))
    _add_project_results_parser(
        sub.add_parser(
            "project-results",
            help="project dispatch parquets from reduced → nodal",
        )
    )
    _add_project_investment_parser(
        sub.add_parser(
            "project-investment",
            help="project capacity expansion outcomes from reduced → nodal",
        )
    )
    _add_cascade_run_parser(
        sub.add_parser(
            "cascade-run",
            help="reduce → run gtopt on reduced → project results",
        )
    )

    args = parser.parse_args(argv)
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    try:
        if args.cmd == "reduce":
            _cmd_reduce(args)
        elif args.cmd == "project-results":
            _cmd_project_results(args)
        elif args.cmd == "project-investment":
            _cmd_project_investment(args)
        elif args.cmd == "cascade-run":
            _cmd_cascade_run(args)
        else:
            parser.error(f"unknown subcommand {args.cmd!r}")
    except (FileNotFoundError, ValueError, KeyError) as exc:
        logging.error("%s", exc)
        sys.exit(1)
    sys.exit(0)


# ---------------------------------------------------------------------------
# reduce
# ---------------------------------------------------------------------------


def _add_reduce_parser(p: argparse.ArgumentParser) -> None:
    p.add_argument("input", type=Path, help="input gtopt JSON case")
    p.add_argument(
        "-o",
        "--output",
        type=Path,
        default=None,
        help="output JSON path (default: <input>.reduced.K{K}.json)",
    )
    p.add_argument(
        "-K",
        "--target-buses",
        type=int,
        default=None,
        help="target bus count after reduction (or use --bus-ratio)",
    )
    p.add_argument(
        "--bus-ratio",
        type=float,
        default=None,
        metavar="R",
        help=(
            "target bus count as a fraction of the original bus count "
            "(0 < R <= 1); mutually exclusive with -K/--target-buses"
        ),
    )
    p.add_argument(
        "--partition",
        choices=["hac", "louvain-mincut"],
        default="hac",
        help=(
            "partition backend: 'hac' (electrical-distance hierarchical "
            "clustering) or 'louvain-mincut' (NetworkX Louvain communities "
            "iteratively split at internal capacity min-cuts / merged by "
            "strongest coupling until exactly K clusters)"
        ),
    )
    p.add_argument(
        "--nx-weight",
        choices=["capacity", "susceptance"],
        default="capacity",
        help=(
            "edge weight for --partition=louvain-mincut: mean directional "
            "capacity (congestion-oriented, default) or 1/X"
        ),
    )
    p.add_argument(
        "--seed",
        type=int,
        default=0,
        help="random seed for the Louvain community pass (default: 0)",
    )
    p.add_argument(
        "--distance",
        choices=["reactance-shortest-path", "zbus", "ptdf"],
        default="reactance-shortest-path",
        help="distance metric for clustering (default: reactance-shortest-path)",
    )
    p.add_argument(
        "--reactance-rule",
        choices=["series-parallel"],
        default="series-parallel",
        help="equivalent-reactance rule for aggregated lines",
    )
    p.add_argument(
        "--dc-reactance-threshold",
        type=float,
        default=1e-4,
        metavar="X",
        help=(
            "lines with per-unit reactance <= X are treated as DC (no KVL, "
            "transport capacity only): near-jumper lines whose huge 1/X "
            "susceptance would wreck KVL conditioning.  Default 1e-4 = "
            "PSS/E THRSHZ closed-switch standard.  gtopt analogue: "
            "model_options.dc_line_reactance_threshold"
        ),
    )
    p.add_argument(
        "--dc-voltage-threshold",
        type=float,
        default=0.0,
        metavar="KV",
        help=(
            "lines whose voltage level min(kV(bus_a), kV(bus_b)) <= KV are "
            "treated as DC: the sub-transmission lines that should not carry "
            "the transmission KVL network.  kV is derived from the endpoint "
            "BUS NAMES (Salar110 -> 110), not line.voltage.  0 = disabled "
            "(default); <=66 recommended for the SEN"
        ),
    )
    p.add_argument(
        "--dc-power-threshold",
        type=float,
        default=0.0,
        metavar="MW",
        help=(
            "lines with capacity max(tmax_ab, tmax_ba) <= MW (schedule-"
            "flattened) are treated as DC: the small / low-capacity lines "
            "whose flow barely matters and only add KVL rows.  0 = disabled "
            "(default); ~30 MW recommended for the SEN (expert memo)"
        ),
    )
    p.add_argument(
        "--protect-constraint-lines",
        action="store_true",
        help=(
            "keep every line referenced by a full-model constraint (user "
            "constraints, uc_*.pampl, line commitment) INTACT — original "
            "uid/name/params, endpoints pinned as anchors — so its "
            "transmission-security / comparison constraint stays valid in "
            "the reduced case.  Essential when the reduced commitment is a "
            "MIP warm-start seed for the full model.  With --bus-ratio the "
            "ratio then applies to the REDUCIBLE (non-protected) buses only"
        ),
    )
    p.add_argument(
        "--anchor-uid",
        type=int,
        action="append",
        default=[],
        metavar="UID",
        help="bus uid to pin as anchor (may be passed multiple times)",
    )
    p.add_argument(
        "--min-load-mw",
        type=float,
        default=None,
        help="also pin buses with peak load above this threshold (MW)",
    )
    p.add_argument(
        "--min-gen-capacity-mw",
        type=float,
        default=None,
        help="also pin buses with gen capacity above this threshold (MW)",
    )
    p.add_argument(
        "--no-reservoir-anchors",
        action="store_true",
        help="do not pin reservoir-host buses",
    )
    p.add_argument(
        "--skip-local-simplify",
        action="store_true",
        help="skip parallel-merge + degree-2 elimination passes",
    )
    p.add_argument(
        "--drop-lines-below-mw",
        type=float,
        default=None,
        help="drop equivalent lines with capacity below this threshold (MW)",
    )
    p.add_argument(
        "--transport-only",
        action="store_true",
        help=(
            "produce a transport-only (no-KVL) reduced case by setting "
            "options.use_kirchhoff=false; line capacities still bind, "
            "but flows are not constrained by reactance"
        ),
    )
    p.add_argument(
        "--loss-mode",
        choices=["keep", "linear", "off", "uplift", "gen-lossfactor"],
        default="keep",
        help=(
            "loss formulation in the reduced case: "
            "'keep' (no change), "
            "'linear' (loss_segments=1, one PWL segment per line; keeps two "
            "directional flow variables per line), "
            "'off' (use_line_losses=false, no loss vars/rows, one signed "
            "flow variable per line), "
            "'uplift' (losses off + per-demand lossfactor = pct/100: load "
            "coefficient becomes -(1+lf)), "
            "'gen-lossfactor' (losses off + per-generator lossfactor = "
            "pct/100: injection coefficient becomes (1-lf) — the classic "
            "uninodal penalty factor; keeps demand/VoLL/reserves truthful)"
        ),
    )
    p.add_argument(
        "--loss-uplift-pct",
        type=float,
        default=3.0,
        help=(
            "percent loss factor for --loss-mode=uplift (demands) or "
            "gen-lossfactor (generators) (default: 3.0%%)"
        ),
    )
    p.add_argument(
        "--reduced-tag",
        type=str,
        default="",
        metavar="TAG",
        help=(
            "when set, aggregate per-line parquet schedules from "
            "<input_dir>/Line/<field>.parquet into sibling "
            "<input_dir>/Line/<field>_<TAG>.parquet files and rewrite each "
            "reduced line's JSON field to the new stem.  Aggregates "
            "active / tmax_ab / tmax_ba / voltage / reactance / resistance; "
            "other fields are warned + skipped."
        ),
    )
    p.add_argument(
        "--summary",
        action="store_true",
        help="print before/after bus and line counts",
    )


def _cmd_reduce(args: argparse.Namespace) -> None:
    case = load_case(args.input)
    # Count protected buses first: under --protect-constraint-lines the
    # bus-ratio is applied to the REDUCIBLE (non-protected) part only.
    n_protected_buses = 0
    if args.protect_constraint_lines:
        _, protected_buses = collect_protected_lines(
            case, input_dir=args.input.resolve().parent
        )
        n_protected_buses = len(protected_buses)
    target_buses = _resolve_target_buses(args, case, n_protected_buses)
    config = ReduceConfig(
        target_buses=target_buses,
        distance=args.distance,
        reactance_rule=args.reactance_rule,
        user_anchor_uids=tuple(args.anchor_uid),
        min_load_mw=args.min_load_mw,
        min_gen_capacity_mw=args.min_gen_capacity_mw,
        include_reservoir_hosts=not args.no_reservoir_anchors,
        skip_local_simplify=args.skip_local_simplify,
        drop_lines_below_mw=args.drop_lines_below_mw,
        dc_reactance_threshold=args.dc_reactance_threshold,
        dc_voltage_threshold=args.dc_voltage_threshold,
        dc_power_threshold=args.dc_power_threshold,
        protect_constraint_lines=args.protect_constraint_lines,
        partition=args.partition,
        nx_weight=args.nx_weight,
        seed=args.seed,
        transport_only=args.transport_only,
        loss_mode=args.loss_mode,
        loss_uplift_pct=args.loss_uplift_pct,
        reduced_tag=args.reduced_tag,
        parquet_case_dir=str(args.input.resolve().parent),
    )
    result = reduce_case(case, config)

    out_path: Path = args.output or args.input.with_suffix(
        f".reduced.K{target_buses}.json"
    )
    out_path.parent.mkdir(parents=True, exist_ok=True)
    # Lines kept intact (protected) still resolve their constraints, so
    # those constraints are NOT dropped.
    surviving_lines = frozenset(
        str(ln.get("name")) for ln in result.case.array("line_array") if ln.get("name")
    )
    n_uc_dropped = filter_line_user_constraints(
        result.case,
        input_dir=args.input.resolve().parent,
        out_dir=out_path.resolve().parent,
        tag=out_path.stem,
        surviving_lines=surviving_lines,
    )
    if n_uc_dropped:
        logging.getLogger(__name__).warning(
            "dropped %d user constraint(s) referencing original lines "
            "(per-line Tx rows are not representable after aggregation)",
            n_uc_dropped,
        )
    base = out_path.with_suffix("")
    save_case(result.case, out_path)
    save_busmap(result.busmap, base.with_name(base.name + ".busmap.csv"))
    save_linemap(result.linemap, base.with_name(base.name + ".linemap.csv"))
    save_aggregator(result.aggregator, base.with_name(base.name + ".aggregator.csv"))
    save_reducer_config(
        config.as_dict(), base.with_name(base.name + ".reducer_config.json")
    )

    if args.summary:
        n_orig_buses = len(case.array("bus_array"))
        n_orig_lines = len(case.array("line_array"))
        n_red_buses = len(result.case.array("bus_array"))
        n_red_lines = len(result.case.array("line_array"))
        print(
            f"buses: {n_orig_buses} → {n_red_buses}    "
            f"lines: {n_orig_lines} → {n_red_lines}    "
            f"anchors: {len(result.anchor_uids)}",
            file=sys.stderr,
        )
    print(str(out_path))


def _resolve_target_buses(
    args: argparse.Namespace, case: Any, n_protected_buses: int = 0
) -> int:
    """Resolve -K/--target-buses vs --bus-ratio (exactly one required).

    Under ``--protect-constraint-lines`` the ratio is applied to the
    REDUCIBLE part only: the protected buses are always kept, and the
    non-protected ones are reduced to ``ratio``.  So the target is
    ``n_protected + round(ratio · (n_total − n_protected))`` — "30% of the
    network" means 30% of what remains after discounting protected buses.
    """
    if (args.target_buses is None) == (args.bus_ratio is None):
        raise ValueError("pass exactly one of -K/--target-buses or --bus-ratio")
    if args.target_buses is not None:
        return int(args.target_buses)
    ratio = float(args.bus_ratio)
    if not 0.0 < ratio <= 1.0:
        raise ValueError(f"--bus-ratio must be in (0, 1], got {ratio}")
    n_total = len(case.array("bus_array"))
    n_reducible = max(0, n_total - n_protected_buses)
    return n_protected_buses + max(1, round(ratio * n_reducible))


# ---------------------------------------------------------------------------
# project-results
# ---------------------------------------------------------------------------


def _add_project_results_parser(p: argparse.ArgumentParser) -> None:
    p.add_argument("reduced_dir", type=Path, help="reduced run output directory")
    p.add_argument("-o", "--output-dir", type=Path, required=True)
    p.add_argument("--busmap", type=Path, required=True)
    p.add_argument("--linemap", type=Path, required=True)
    p.add_argument("--aggregator", type=Path, required=True)


def _cmd_project_results(args: argparse.Namespace) -> None:
    project_results(
        args.reduced_dir,
        busmap=args.busmap,
        linemap=args.linemap,
        aggregator=args.aggregator,
        output_dir=args.output_dir,
    )
    print(str(args.output_dir))


# ---------------------------------------------------------------------------
# project-investment
# ---------------------------------------------------------------------------


def _add_project_investment_parser(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "reduced_investment", type=Path, help="JSON of per-cluster expansion deltas"
    )
    p.add_argument("--original-case", type=Path, required=True)
    p.add_argument("--busmap", type=Path, required=True)
    p.add_argument("--linemap", type=Path, required=True)
    p.add_argument("--aggregator", type=Path, required=True)
    p.add_argument(
        "--share-mode",
        choices=["existing", "load", "resource", "nodal_lp", "manual"],
        default="existing",
    )
    p.add_argument("--share-csv", type=Path, default=None)
    p.add_argument(
        "--line-share-mode",
        choices=["capacity"],
        default="capacity",
    )
    p.add_argument("-o", "--output", type=Path, required=True)
    p.add_argument(
        "--audit-csv",
        type=Path,
        default=None,
        help="optional path for the per-bus audit CSV",
    )


def _cmd_project_investment(args: argparse.Namespace) -> None:
    with args.reduced_investment.open("r", encoding="utf-8") as f:
        reduced: dict[str, Any] = json.load(f)
    cfg = ProjectInvestmentConfig(
        share_mode=args.share_mode,
        share_csv=args.share_csv,
        line_share_mode=args.line_share_mode,
    )
    patch, audit = project_investment(
        reduced,
        original_case=args.original_case,
        busmap=args.busmap,
        aggregator=args.aggregator,
        linemap=args.linemap,
        config=cfg,
    )
    write_patch_json(patch, args.output)
    if args.audit_csv is not None:
        write_audit_csv(audit, args.audit_csv)
    print(str(args.output))


# ---------------------------------------------------------------------------
# cascade-run
# ---------------------------------------------------------------------------


def _add_cascade_run_parser(p: argparse.ArgumentParser) -> None:
    p.add_argument("input", type=Path, help="input gtopt JSON case")
    p.add_argument("-K", "--target-buses", type=int, required=True)
    p.add_argument("-o", "--output-dir", type=Path, required=True)
    p.add_argument("--gtopt-bin", type=Path, default=None)
    p.add_argument(
        "--distance",
        choices=["reactance-shortest-path", "zbus", "ptdf"],
        default="reactance-shortest-path",
    )
    p.add_argument(
        "--also-run-full",
        action="store_true",
        help="also run gtopt on the original full case for comparison",
    )
    p.add_argument(
        "extra_gtopt_args",
        nargs=argparse.REMAINDER,
        help="extra arguments forwarded to gtopt after '--'",
    )


def _cmd_cascade_run(args: argparse.Namespace) -> None:
    extras = list(args.extra_gtopt_args or [])
    if extras and extras[0] == "--":
        extras = extras[1:]
    paths = cascade_run(
        args.input,
        target_buses=args.target_buses,
        output_dir=args.output_dir,
        gtopt_bin=args.gtopt_bin,
        distance=args.distance,
        extra_gtopt_args=extras,
        skip_full=not args.also_run_full,
    )
    for k, v in paths.items():
        print(f"{k}: {v}")


if __name__ == "__main__":
    main()
