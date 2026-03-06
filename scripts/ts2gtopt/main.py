#!/usr/bin/env python3
"""ts2gtopt – Command-line interface.

Project time-series data onto a gtopt planning horizon and write the result
as gtopt-compatible schedule files (Parquet or CSV) with correct block
durations that preserve both the total period and the accumulated energy.
"""

import argparse
import calendar
import json
import logging
import sys
from pathlib import Path
from typing import Any

from ts2gtopt.ts2gtopt import (
    PRESETS,
    _sequential_hour_range,
    _sequential_month_range,
    convert_timeseries,
    energy_conservation_check,
    list_presets,
    load_horizon,
    load_timeseries,
    make_horizon,
    project_timeseries,
)

try:
    from importlib.metadata import PackageNotFoundError
    from importlib.metadata import version as _pkg_version

    try:
        __version__ = _pkg_version("gtopt-scripts")
    except PackageNotFoundError:
        __version__ = "dev"
except ImportError:
    __version__ = "dev"

_LOG_LEVEL_CHOICES = ["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"]

_DESCRIPTION = """\
Project time-series data onto a gtopt planning horizon and write the result
as gtopt-compatible schedule files (Parquet or CSV).

Given an hourly (or finer) time-series and a planning horizon specification,
ts2gtopt aggregates the raw values into representative block values that can
be used directly as gtopt input schedule files (e.g. Demand/lmax.parquet).

CONSERVATION PROPERTIES
-----------------------
When using the default mean aggregation, the output satisfies:

  1. Period conservation  – the sum of all block durations equals the total
     time span covered by the input data.

  2. Energy conservation  – the sum of (value × block_duration) over the
     projected schedule equals the sum of (value × interval_hours) over the
     original time-series.

Block durations in the generated schedule/horizon encode the number of
times each (stage, block) pattern occurs in the data multiplied by the
sampling interval (default 1 h for hourly data).

PLANNING HORIZON SPECIFICATION
-------------------------------
The planning horizon can be specified in three ways (in order of precedence):

1. --horizon FILE
     JSON file with explicit stage and block definitions.

2. --planning FILE  (requires --year)
     Full gtopt case JSON (e.g. system.json).  The stage_array and block_array
     are read from simulation.*_array; stages are mapped to calendar months
     and blocks to hours of the day sequentially.

3. --year Y  [--stages N]  [--blocks M]
     Auto-generate a standard annual horizon with conservation-correct block
     durations.  N stages distributed across 12 calendar months (default 12),
     M blocks mapped to hour-of-day windows within each day (default 24).

HORIZON JSON FORMAT
-------------------
  {
    "year": 2023,
    "interval_hours": 1.0,
    "scenarios": [{"uid": 1, "probability_factor": 1.0}],
    "stages": [
      {"uid": 1, "month": 1},
      {"uid": 2, "months": [2, 3]},
      {"uid": 3, "start_date": "2023-07-01", "end_date": "2023-09-30"}
    ],
    "blocks": [
      {"uid":  1, "hour": 0,  "duration": 31.0},
      {"uid":  2, "hours": [6, 7, 8], "duration": 93.0},
      {"uid":  3, "start_hour": 18, "end_hour": 23, "duration": 186.0}
    ]
  }
"""

_EPILOG = """
examples:
  # Monthly × hourly projection for 2023  (12 stages × 24 blocks → 288 rows)
  ts2gtopt demand.csv -y 2023 -o Demand/

  # Seasonal × hourly  (4 stages × 24 blocks → 96 rows)
  ts2gtopt demand.parquet -y 2023 --stages 4 -o Demand/

  # Seasonal preset: 4 phases × 12 monthly stages × 3 blocks (night/solar/evening)
  ts2gtopt demand.csv -y 2023 --preset seasonal-3block -o Demand/

  # Monthly × 3 blocks (night 0-6, solar 7-18, evening 19-23)
  ts2gtopt demand.csv -y 2023 --preset monthly-3block -o Demand/

  # Annual × 3 blocks (simplest preset)
  ts2gtopt demand.csv -y 2023 --preset annual-3block -o Demand/

  # List all available presets
  ts2gtopt --list-presets

  # From an explicit horizon JSON
  ts2gtopt demand.csv pmax.csv -H horizon.json -o schedules/

  # From an existing gtopt case JSON  (reads stage_array and block_array)
  ts2gtopt demand.csv -P system.json -y 2023 -o input/Demand/

  # Write updated horizon JSON with conservation-correct block durations
  ts2gtopt demand.csv -y 2023 -o Demand/ --output-horizon horizon_updated.json

  # Verify energy conservation after projection
  ts2gtopt demand.csv -y 2023 -o Demand/ --verify

  # 15-minute interval data
  ts2gtopt load_15min.csv -y 2023 -i 0.25 -o Demand/

  # Median aggregation, CSV output
  ts2gtopt energy.csv -y 2023 -a median -f csv -o output/
"""


def _build_stage_blocks(
    block_array: list[dict[str, Any]],
    n_days: int,
    n_bl: int,
    interval_hours: float,
    block_uid_start: int,
) -> tuple[list[dict[str, Any]], int]:
    """Build per-stage blocks with conservation-correct durations.

    Returns (blocks, next_uid).
    """
    blocks: list[dict[str, Any]] = []
    uid = block_uid_start
    for j, bl in enumerate(block_array):
        hours = _sequential_hour_range(j, n_bl)
        duration = float(n_days * len(hours) * interval_hours)
        blocks.append({**bl, "uid": uid, "hours": hours, "duration": duration})
        uid += 1
    return blocks, uid


def _build_horizon_from_planning(
    planning_path: Path, year: int | None, interval_hours: float
) -> dict[str, Any]:
    """Build a ts2gtopt horizon dict from an existing gtopt case JSON.

    Stages are mapped sequentially to calendar months (using calendar day
    counts for conservation-correct block durations) and blocks to hours of
    the day.
    """
    with open(planning_path, encoding="utf-8") as f:
        case: dict[str, Any] = json.load(f)

    sim = case.get("simulation", {})
    stage_array: list[dict[str, Any]] = sim.get("stage_array", [])
    block_array: list[dict[str, Any]] = sim.get("block_array", [])
    scenario_array: list[dict[str, Any]] = sim.get(
        "scenario_array", [{"uid": 1, "probability_factor": 1.0}]
    )

    n_st = max(len(stage_array), 1)
    n_bl = max(len(block_array), 1)

    blocks: list[dict[str, Any]] = []
    stages: list[dict[str, Any]] = []
    block_uid_counter = 1

    for i, st in enumerate(stage_array):
        months = _sequential_month_range(i, n_st)
        n_days = (
            sum(calendar.monthrange(year, m)[1] for m in months)
            if year is not None
            else 30
        )
        stage_first_block = block_uid_counter - 1
        stage_blocks, block_uid_counter = _build_stage_blocks(
            block_array, n_days, n_bl, interval_hours, block_uid_counter
        )
        blocks.extend(stage_blocks)
        stages.append(
            {
                **st,
                "months": months,
                "first_block": stage_first_block,
                "count_block": len(block_array),
            }
        )

    h: dict[str, Any] = {
        "scenarios": scenario_array,
        "stages": stages,
        "blocks": blocks,
        "interval_hours": interval_hours,
    }
    if year is not None:
        h["year"] = year
    return h


def main() -> None:
    """CLI entry point for ts2gtopt."""
    parser = argparse.ArgumentParser(
        prog="ts2gtopt",
        description=_DESCRIPTION,
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=_EPILOG,
    )

    parser.add_argument(
        "input",
        nargs="*",
        metavar="FILE",
        help="input time-series file(s) (CSV or Parquet)",
    )
    parser.add_argument(
        "-o",
        "--output",
        metavar="DIR",
        default=".",
        help="output directory for schedule files (default: current directory)",
    )

    # --- Horizon source (mutually exclusive) ----------------------------------
    hgroup = parser.add_mutually_exclusive_group()
    hgroup.add_argument(
        "-H",
        "--horizon",
        metavar="FILE",
        help="JSON file defining the planning horizon (stages, blocks, scenarios)",
    )
    hgroup.add_argument(
        "-P",
        "--planning",
        metavar="FILE",
        help=(
            "gtopt case JSON; reads simulation.stage_array and block_array "
            "(requires --year)"
        ),
    )

    parser.add_argument(
        "-y",
        "--year",
        type=int,
        metavar="YEAR",
        help=(
            "calendar year of the time-series: used to filter the input and "
            "to build the auto-horizon (conservation-correct block durations "
            "require the year to compute exact days per month)"
        ),
    )
    parser.add_argument(
        "-s",
        "--stages",
        type=int,
        default=12,
        metavar="N",
        help="number of planning stages for auto-horizon (default: 12)",
    )
    parser.add_argument(
        "-b",
        "--blocks",
        type=int,
        default=24,
        metavar="M",
        help="number of blocks per stage for auto-horizon (default: 24)",
    )
    parser.add_argument(
        "--preset",
        metavar="NAME",
        default=None,
        help=(
            "use a built-in horizon preset (overrides --stages/--blocks). "
            "Available: " + ", ".join(sorted(PRESETS))
        ),
    )
    parser.add_argument(
        "--list-presets",
        action="store_true",
        default=False,
        help="list available horizon presets and exit",
    )
    parser.add_argument(
        "-i",
        "--interval-hours",
        type=float,
        default=None,
        metavar="H",
        help=(
            "sampling interval of the input time-series in hours "
            "(default: auto-detected; 1.0 for hourly, 0.25 for 15-min, …)"
        ),
    )
    parser.add_argument(
        "-t",
        "--time-column",
        default="datetime",
        metavar="COL",
        help="name of the datetime column in the input files (default: 'datetime')",
    )
    parser.add_argument(
        "-a",
        "--agg",
        default="mean",
        choices=["mean", "median", "min", "max", "sum"],
        metavar="FUNC",
        help="aggregation function: mean (default), median, min, max, sum",
    )
    parser.add_argument(
        "-f",
        "--format",
        dest="output_format",
        default="parquet",
        choices=["parquet", "csv"],
        help="output file format (default: parquet)",
    )
    parser.add_argument(
        "-c",
        "--compression",
        default="gzip",
        metavar="ALG",
        help=(
            "Parquet compression algorithm (default: gzip); "
            "pass '' to disable compression"
        ),
    )
    parser.add_argument(
        "--output-horizon",
        metavar="FILE",
        default=None,
        help=(
            "write the duration-updated horizon JSON to this file "
            "(block durations derived from the first input file)"
        ),
    )
    parser.add_argument(
        "--verify",
        action="store_true",
        default=False,
        help=(
            "after projection, print energy conservation ratios for each "
            "element column (ratio ≈ 1.0 means perfect conservation)"
        ),
    )
    parser.add_argument(
        "-l",
        "--log-level",
        default="INFO",
        choices=_LOG_LEVEL_CHOICES,
        metavar="LEVEL",
        help="logging verbosity (default: INFO)",
    )
    parser.add_argument(
        "-V",
        "--version",
        action="version",
        version=f"%(prog)s {__version__}",
    )

    args = parser.parse_args()

    # --- Handle --list-presets early ------------------------------------------
    if args.list_presets:
        print("Available horizon presets:\n")
        for pname, pdesc in list_presets().items():
            print(f"  {pname:24s} {pdesc}")
        sys.exit(0)

    if not args.input:
        parser.error("the following arguments are required: FILE")

    logging.basicConfig(
        level=getattr(logging, args.log_level),
        format="%(asctime)s %(levelname)s %(message)s",
    )

    interval_hours: float | None = args.interval_hours

    # --- Resolve horizon ------------------------------------------------------
    horizon: dict[str, Any]
    if args.horizon:
        horizon = load_horizon(Path(args.horizon))
        logging.info("Loaded horizon from %s", args.horizon)
    elif args.planning:
        if args.year is None:
            parser.error("--planning requires --year to map stages to calendar months")
        ih = interval_hours if interval_hours is not None else 1.0
        horizon = _build_horizon_from_planning(Path(args.planning), args.year, ih)
        logging.info("Built horizon from planning file %s", args.planning)
    else:
        if args.year is None:
            parser.error(
                "Provide --year when using the auto-horizon "
                "(neither --horizon nor --planning was given)"
            )
        ih = interval_hours if interval_hours is not None else 1.0
        horizon = make_horizon(
            args.year,
            n_stages=args.stages,
            n_blocks=args.blocks,
            interval_hours=ih,
            preset=args.preset,
        )
        preset_msg = f" preset={args.preset}" if args.preset else ""
        logging.info(
            "Auto-generated horizon: year=%d stages=%d blocks=%d "
            "interval_hours=%.4g%s",
            args.year,
            args.stages,
            len(horizon.get("blocks", [])) // max(len(horizon.get("stages", [1])), 1),
            ih,
            preset_msg,
        )

    # Year from horizon if not overridden on CLI
    effective_year: int | None = args.year
    if effective_year is None and "year" in horizon:
        effective_year = int(horizon["year"])

    input_paths = [Path(p) for p in args.input]
    output_dir = Path(args.output)
    output_horizon_path = Path(args.output_horizon) if args.output_horizon else None

    try:
        results = convert_timeseries(
            input_paths=input_paths,
            output_dir=output_dir,
            horizon=horizon,
            time_column=args.time_column,
            agg_func=args.agg,
            year=effective_year,
            interval_hours=interval_hours,
            output_format=args.output_format,
            compression=args.compression,
            output_horizon_path=output_horizon_path,
        )
        for stem, out_path in results.items():
            print(f"{stem} → {out_path}")

        # --- Conservation verification -----------------------------------
        if args.verify and input_paths:
            ts_df, _ = load_timeseries(input_paths[0], args.time_column)
            ih_verify = interval_hours if interval_hours is not None else 1.0
            sched_df = project_timeseries(
                ts_df,
                horizon=horizon,
                time_column=args.time_column,
                agg_func=args.agg,
                year=effective_year,
                interval_hours=ih_verify,
            )
            ratios = energy_conservation_check(
                ts_df, sched_df, interval_hours=ih_verify
            )
            print("\nEnergy conservation check (ratio ≈ 1.0 = perfect):")
            for col, ratio in ratios.items():
                flag = "  ✓" if abs(ratio - 1.0) < 1e-6 else "  !"
                print(f"  {col}: {ratio:.8f}{flag}")

    except (ValueError, IOError, FileNotFoundError) as exc:
        logging.error("%s", exc)
        sys.exit(1)

    sys.exit(0)


if __name__ == "__main__":
    main()
