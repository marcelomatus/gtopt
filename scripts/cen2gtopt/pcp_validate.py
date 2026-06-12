# SPDX-License-Identifier: BSD-3-Clause
"""``cen2gtopt.pcp_validate`` — back-test the marginal-emission
pipeline against CEN's PCP / PID solution.

Triangulates **three independent estimates** of the system marginal
cost (and the marginal-unit identity) per hour:

  1. ``λ_real``      — settled CMG from ``costo-marginal-real/v4``
                       (system median across reference buses).
  2. ``λ_pcp``        — CEN's PCP-cleared CMG from ``cmg-programado-pcp``
                       (system median across nodes).
  3. ``λ_inputs``    — re-derived from the PCP/PID input CSVs
                       (heat-rate × fuel-price + VOM, sorted, marginal
                       picked at observed demand).
  4. ``λ_pipeline``  — the output of ``cen2gtopt.marginal_units``
                       (our reverse-engineered pick).

For each (date, hour) cell we report:

  * λ_real, λ_pcp, λ_inputs, λ_pipeline
  * Δ_pipeline_vs_real, Δ_pipeline_vs_pcp
  * marginal unit identified by pipeline vs by PCP-merit

Output is a per-hour parquet table at
``<output>/validation_<date>.parquet`` plus a console summary.

The validation is honest about its limits:

  * **Without** the PLEXOS .accdb solution reader (deferred), the
    "ground-truth marginal-unit identity" is itself derived from
    CSV inputs (heat-rate × fuel-price ranking) — not directly from
    CEN's solver output.  This is a **proxy oracle**.
  * The proxy is still defensible because the same merit-order
    construction is what PLEXOS itself uses; the only ambiguity is
    in **which** unit straddles the cleared λ when many cluster.

Examples
--------

::

    # Validate one day against PID period 20
    python -m cen2gtopt.pcp_validate --date 2026-05-06 --period 20

    # Compare PCP vs real for a different day
    python -m cen2gtopt.pcp_validate --date 2026-04-07 --source pcp

    # Bulk validation over a date range, write parquet per day
    python -m cen2gtopt.pcp_validate \\
        --start-date 2026-04-01 --end-date 2026-04-07 \\
        --output ~/cen_validation/
"""

from __future__ import annotations

import argparse
import logging
import sys
from dataclasses import dataclass
from datetime import date, timedelta
from pathlib import Path

import pandas as pd

from cen2gtopt._cached_extractor import fetch_by_name
from cen2gtopt._cen_client import CenApiClient, CenApiConfig
from cen2gtopt.marginal_units import (
    REF_BUSES,
    SIP_KEY,
    compute_marginal_units_for_day,
)
from cen2gtopt.pcp_inputs import load_pcp_inputs

_LOG = logging.getLogger("cen2gtopt.pcp_validate")


# ---------------------------------------------------------------------------
# Per-source extractors
# ---------------------------------------------------------------------------


@dataclass
class HourLambdas:
    """Per-hour λ from the four sources."""

    hour: int
    lambda_real: float = float("nan")
    lambda_pcp: float = float("nan")
    lambda_inputs: float = float("nan")
    lambda_pipeline: float = float("nan")
    marginal_pipeline: str = ""
    marginal_inputs: str = ""


def _system_lambda_per_hour(df: pd.DataFrame) -> dict[int, float]:
    """Group by hour and median across rows."""
    if df.empty:
        return {}
    g = df.groupby("hour")["lmp"].median()
    return {int(h): float(v) for h, v in g.items()}


def fetch_real_lambda(
    client: CenApiClient,
    *,
    date_iso: str,
) -> dict[int, float]:
    """λ_real per hour — median across the v3 REF_BUSES."""
    rows: list[pd.DataFrame] = []
    for bar_transf, _, _ in REF_BUSES:
        try:
            df_raw = fetch_by_name(
                client,
                "cmg_real",
                start=date_iso,
                extra_params={"bar_transf": bar_transf},
            )
        except Exception:  # noqa: BLE001  # pylint: disable=broad-except
            continue
        if df_raw.empty:
            continue
        df = pd.DataFrame()
        df["hour"] = pd.to_numeric(df_raw["hra"], errors="coerce").astype("Int64")
        df["lmp"] = pd.to_numeric(df_raw["cmg_usd_mwh_"], errors="coerce")
        df = df.dropna(subset=["hour", "lmp"])
        rows.append(df)
    if not rows:
        return {}
    return _system_lambda_per_hour(pd.concat(rows, ignore_index=True))


def fetch_pcp_lambda(
    client: CenApiClient,
    *,
    date_iso: str,
) -> dict[int, float]:
    """λ_pcp per hour — system median from cmg-programado-pcp."""
    try:
        df_raw = fetch_by_name(client, "cmg_programado_pcp", start=date_iso)
    except Exception:  # noqa: BLE001  # pylint: disable=broad-except
        return {}
    if df_raw.empty:
        return {}
    df = pd.DataFrame()
    df["fecha_hora"] = pd.to_datetime(df_raw["fecha_hora"], errors="coerce")
    df["hour"] = df["fecha_hora"].dt.hour.astype("Int64")
    df["lmp"] = pd.to_numeric(df_raw["cmg_usd_mwh"], errors="coerce")
    df = df.dropna(subset=["hour", "lmp"])
    return _system_lambda_per_hour(df)


def derive_inputs_lambda(
    inputs,
    *,
    hour: int,
) -> tuple[float, str]:
    """λ derived from PCP/PID input CSVs at one period.

    Builds the merit order (per-unit MC = HeatRate × FuelPrice + VOM
    + transport), sorts ascending, and returns the **median** MC of
    the top quartile of dispatched-eligible units (a coarse proxy
    for the cleared-thermal CV when we lack the actual dispatch).

    Returns ``(lambda_inputs, marginal_unit_name)``.  Returns
    ``(NaN, "")`` when the input CSVs lack fuel-name resolution
    (typical without PLEXOS XML parsing).
    """
    period = hour + 1  # PCP periods are 1-indexed hours
    mc = inputs.marginal_cost_per_unit(period=period)
    if mc.empty:
        return float("nan"), ""
    mc = mc.dropna(subset=["mc_usd_mwh"])
    mc = mc[mc["mc_usd_mwh"] > 0]
    if mc.empty:
        return float("nan"), ""
    mc = mc.sort_values("mc_usd_mwh")
    # Marginal proxy: top quartile of MCs (where the marginal unit
    # typically lives — the cheap units are inframarginal, the most
    # expensive are out of merit).
    top_q = mc.tail(max(1, len(mc) // 4))
    median_mc = float(top_q["mc_usd_mwh"].median())
    # Marginal unit = the one whose MC is closest to that median
    top_q = top_q.copy()
    top_q["abs_dev"] = (top_q["mc_usd_mwh"] - median_mc).abs()
    chosen = top_q.nsmallest(1, "abs_dev").iloc[0]
    return median_mc, str(chosen["unit_name"])


# ---------------------------------------------------------------------------
# Per-day driver
# ---------------------------------------------------------------------------


#: Hand-curated CEN bus_label → PLEXOS bus_name mapping.  Used to join
#: our pipeline's output (keyed by bus_label) with PLEXOS's solution
#: DB output (keyed by node name).  Extend as more REF_BUSES are
#: added.
PLEXOS_BUS_MAP: dict[str, str] = {
    "ALTO MELIPILLA 220KV (Santiago)": "AMelipilla220",
    "CRUCERO 220KV (north)": "Crucero220",
    "GUACOLDA 220KV (north-centre)": "Guacolda220",
    "ESPERANZA 220KV (north)": "Esperanza220",
    "CHARRÚA 220KV (south-centre)": "Charrua220",
    "CONCEPCION 220KV (south-centre)": "Concepcion066",
    "EL SALTO 220KV (centre)": "ElSalto110",
    "EL MAITEN 66KV (centre-north)": "ElMaiten066",
    "LOS ALMENDROS 220KV (centre)": "Almendros110",
    "DIEGO DE ALMAGRO 110KV (north-centre)": "DAlmagro110",
}


def _fetch_plexos_lmp_per_bus(date_iso: str) -> pd.DataFrame:
    """Per-(plexos_bus, hour) mean LMP from the PCP solution .accdb.
    Returns empty DataFrame on any error."""
    # Local import to avoid pulling mdbtools-dependent code on every
    # validation run.
    # pylint: disable=import-outside-toplevel
    try:
        from cen2gtopt.pcp_solution import (
            extract_property,
            resolve_solution,
        )
    except ImportError:
        return pd.DataFrame()
    try:
        sol = resolve_solution(date_iso, source="pcp")
        df = extract_property(
            sol.accdb_path,
            property_name="Price",
            collection_id=281,  # System.Nodes
            period_table=0,
        )
    except (FileNotFoundError, ValueError, RuntimeError) as exc:
        _LOG.warning("[%s] PLEXOS LMP unavailable: %s", date_iso, exc)
        return pd.DataFrame()
    if df.empty:
        return df
    return (
        df.groupby(["object_name", "hour"])["value"]
        .mean()
        .reset_index()
        .rename(columns={"object_name": "plexos_bus_name", "value": "lambda_plexos"})
    )


def validate_one_day(
    *,
    date_iso: str,
    period: int | None = None,
    source: str = "auto",
    output_dir: Path | None = None,
    write_parquet: bool = True,
    include_plexos: bool = True,
) -> pd.DataFrame:
    """Per-(bus, hour) triangulation on one date.

    Returns a long-form DataFrame with one row per (bus_label, hour),
    carrying:

      * ``lambda_real``     — published cmg-real (15-min averaged to h)
      * ``lambda_pipeline`` — our pipeline's reconstruction
      * ``lambda_inputs``   — PID/PCP merit-derived MC (proxy)
      * ``lambda_plexos``   — PLEXOS PCP solver dual (when available)
      * Δ_* columns versus each oracle
      * marginal-unit identity from each source

    The **per-bus per-hour** granularity is critical: prior versions
    medianed across buses, manufacturing aggregation noise where the
    pipeline was actually accurate.  See the 2026-04-22 analysis in
    docs/scripts/cen_marginal_emissions_review.md.
    """
    sip_cfg = CenApiConfig(user_keys={"sip": SIP_KEY}, verify_tls=False)
    with CenApiClient(sip_cfg) as client:
        _LOG.info("[%s] running pipeline …", date_iso)
        pipe = compute_marginal_units_for_day(
            client,
            date_iso=date_iso,
            ref_buses=REF_BUSES,
            cmg_source="best_real",
        )

    if pipe.empty:
        _LOG.warning("[%s] empty pipeline output — skipping", date_iso)
        return pd.DataFrame()

    # Per-bus per-hour aggregation of the 15-min pipeline output
    pipe_h = (
        pipe.groupby(["bus_label", "hour"])
        .agg(
            lambda_cen=("lambda_cen", "mean"),
            lambda_pipeline=("lambda_pipe", "mean"),
            lambda_pipeline_pid=("lambda_constructed_pid", "mean"),
            marginal_pipeline=(
                "marginal_central",
                lambda s: s.mode().iloc[0] if not s.mode().empty else "",
            ),
            epsilon=("epsilon", "mean"),
            n_quarters=("quarter", "count"),
        )
        .reset_index()
        .rename(columns={"lambda_cen": "lambda_real"})
    )

    _LOG.info("[%s] loading PCP/PID inputs …", date_iso)
    try:
        inputs = load_pcp_inputs(date_iso, source=source, period=period)
        is_pid = inputs.is_pid
        eff_period = inputs.period or 0
    except FileNotFoundError as exc:
        _LOG.warning("[%s] PCP/PID inputs unavailable: %s", date_iso, exc)
        inputs = None
        is_pid = False
        eff_period = 0

    # Inputs-derived merit (system-level, per hour)
    if inputs is not None:
        inputs_lambda: dict[int, float] = {}
        inputs_marginal: dict[int, str] = {}
        for h in range(24):
            li, mu = derive_inputs_lambda(inputs, hour=h)
            inputs_lambda[h] = li
            inputs_marginal[h] = mu
        pipe_h["lambda_inputs"] = pipe_h["hour"].map(inputs_lambda)
        pipe_h["marginal_inputs"] = pipe_h["hour"].map(inputs_marginal)
    else:
        pipe_h["lambda_inputs"] = float("nan")
        pipe_h["marginal_inputs"] = ""

    # PLEXOS per-bus per-hour LMP (the ground-truth oracle)
    if include_plexos:
        _LOG.info("[%s] extracting PLEXOS LMPs …", date_iso)
        plexos = _fetch_plexos_lmp_per_bus(date_iso)
        if not plexos.empty:
            pipe_h["plexos_bus_name"] = pipe_h["bus_label"].map(PLEXOS_BUS_MAP)
            pipe_h = pipe_h.merge(
                plexos,
                on=["plexos_bus_name", "hour"],
                how="left",
            )
            pipe_h.drop(columns=["plexos_bus_name"], inplace=True)
        else:
            pipe_h["lambda_plexos"] = float("nan")
    else:
        pipe_h["lambda_plexos"] = float("nan")

    # Per-cell deltas (the headline metrics)
    pipe_h["date_iso"] = date_iso
    pipe_h["source"] = "PID" if is_pid else "PCP"
    pipe_h["period"] = eff_period
    pipe_h["delta_pipeline_vs_real"] = pipe_h["lambda_pipeline"] - pipe_h["lambda_real"]
    pipe_h["delta_pipeline_pid_vs_real"] = (
        pipe_h["lambda_pipeline_pid"] - pipe_h["lambda_real"]
    )
    pipe_h["delta_pipeline_vs_plexos"] = (
        pipe_h["lambda_pipeline"] - pipe_h["lambda_plexos"]
    )
    pipe_h["delta_plexos_vs_real"] = pipe_h["lambda_plexos"] - pipe_h["lambda_real"]

    if write_parquet and output_dir:
        output_dir.mkdir(parents=True, exist_ok=True)
        out_path = output_dir / f"validation_{date_iso}.parquet"
        pipe_h.to_parquet(out_path, index=False)
        _LOG.info("  wrote %s", out_path)

    return pipe_h


def _print_table(df: pd.DataFrame) -> None:
    pd.set_option("display.width", 220)
    pd.set_option("display.float_format", lambda x: f"{x:.2f}")
    # Per-bus mean accuracy
    print("--- per-bus mean |Δ| (USD/MWh) ---")
    by_bus = (
        df.groupby("bus_label")[
            [
                "delta_pipeline_vs_real",
                "delta_pipeline_pid_vs_real",
                "delta_pipeline_vs_plexos",
                "delta_plexos_vs_real",
            ]
        ]
        .apply(lambda d: d.abs().mean())
        .round(3)
    )
    by_bus["n_hours"] = df.groupby("bus_label")["hour"].count()
    print(by_bus.to_string())
    print()

    # System aggregate across all bus-hours
    print("--- system aggregate across all (bus × hour) cells ---")
    for col in (
        "delta_pipeline_vs_real",
        "delta_pipeline_pid_vs_real",
        "delta_pipeline_vs_plexos",
        "delta_plexos_vs_real",
    ):
        s = df[col].dropna()
        if s.empty:
            continue
        print(
            f"  {col:30s} mean=${s.mean():+.2f}  "
            f"|mean|=${s.abs().mean():.2f}  P90=${s.abs().quantile(0.9):.2f}  "
            f"max=${s.abs().max():.2f}  n={len(s)}"
        )


def _date_range(start: date, end: date) -> list[str]:
    out: list[str] = []
    cur = start
    while cur <= end:
        out.append(cur.isoformat())
        cur += timedelta(days=1)
    return out


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _cmd_run(args: argparse.Namespace) -> int:
    if args.start_date and args.end_date:
        dates = _date_range(
            date.fromisoformat(args.start_date),
            date.fromisoformat(args.end_date),
        )
    else:
        dates = [args.date]

    summaries: list[pd.DataFrame] = []
    for d in dates:
        try:
            df = validate_one_day(
                date_iso=d,
                period=args.period,
                source=args.source,
                output_dir=args.output,
                write_parquet=bool(args.output),
            )
        except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-except
            _LOG.error("[%s] failed: %s", d, exc)
            continue
        if len(dates) == 1:
            print(
                f"=== validation for {d} "
                f"({df['source'].iloc[0]}"
                + (
                    f" period={int(df['period'].iloc[0]):02d}"
                    if df["period"].iloc[0]
                    else ""
                )
                + ") ==="
            )
            _print_table(df)
        summaries.append(df)

    if len(dates) > 1 and summaries:
        all_df = pd.concat(summaries, ignore_index=True)
        print("=" * 80)
        print(f"PERIOD SUMMARY — {len(dates)} days")
        print("=" * 80)
        for col in (
            "delta_pipeline_vs_real",
            "delta_pipeline_vs_pcp",
            "delta_pcp_vs_real",
        ):
            s = all_df[col].dropna()
            if s.empty:
                continue
            print(
                f"  {col:30s} mean=${s.mean():+.2f}  "
                f"|mean|=${s.abs().mean():.2f}  P90=${s.abs().quantile(0.9):.2f}"
            )
        if args.output:
            agg_path = args.output / "validation_aggregate.parquet"
            all_df.to_parquet(agg_path, index=False)
            print(f"\n  aggregate parquet: {agg_path}")
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="cen2gtopt.pcp_validate",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    g = p.add_argument_group("date selection (pick one)")
    g.add_argument("--date", help="YYYY-MM-DD")
    g.add_argument("--start-date", help="YYYY-MM-DD (inclusive, with --end-date)")
    g.add_argument("--end-date", help="YYYY-MM-DD (inclusive)")

    p.add_argument(
        "--source",
        default="auto",
        choices=("auto", "pcp", "pid"),
        help="LP-input source (default: auto = prefer PID)",
    )
    p.add_argument(
        "--period", type=int, default=None, help="PID re-solve period (1..24)"
    )
    p.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Write per-day validation_<date>.parquet under "
        "this dir (skipped if not given)",
    )

    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("-q", "--quiet", action="store_true")
    p.add_argument(
        "-V", "--version", action="version", version="cen2gtopt.pcp_validate 0.1.0"
    )
    p.set_defaults(func=_cmd_run)
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    if not (args.date or (args.start_date and args.end_date)):
        print("error: must specify --date OR --start-date+--end-date")
        return 2
    if args.quiet:
        log_level = logging.WARNING
    elif args.verbose:
        log_level = logging.DEBUG
    else:
        log_level = logging.INFO
    logging.basicConfig(
        level=log_level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
