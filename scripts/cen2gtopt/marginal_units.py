#!/usr/bin/env python
# SPDX-License-Identifier: BSD-3-Clause
#
# pylint: disable=non-ascii-name
#   Greek symbols (λ, ε, Δ) are intentional in dict-construction kwargs
#   that become output column names — they match the published-paper
#   notation (LMP λ, marginal emission factor ε, residual Δ).
#
# pylint: disable=broad-exception-caught
#   Network/IO catches downgrade any upstream exception to a warning
#   + skip rather than a crash; pinning to specific types would couple
#   the pipeline to one of many library error classes.
"""Reverse-engineered marginal-unit + emission-factor identification.

Strategy
--------
Goal: emission factor ε per (bus, quarter), not LMP reconstruction.

Inputs (all from the cached CEN/SCVIC extractor):
  1. λ_CEN  — 15-min settlement price per bus (`/costo-marginal-real/v4`).
  2. Real per-central hourly dispatch (`/generacion-real/v3`).
  3. SCVIC declared CV per unit per day (`/cvrpt/show_cv_rpt`).
  4. Forced/RO instructions (`/instrucciones-operacionales-cmg/v4`).
  5. Per-unit declared pmin (`/unidades-generadoras/v4`).
  6. id_central alias index bridging all of the above
     (`cen2gtopt._id_bridge`).

Algorithm — for each (bus, hour):
  a. Set ``λ_target = λ_CEN[bus, hour]``.
  b. Build the candidate set: centrales with ``gen_real_mw > 0`` at
     that hour AND **interior dispatch** (``pmin < dispatch < pmax``)
     AND NOT in the forced/RO set.
  c. For each candidate, look up its SCVIC declared CV.
  d. Pick the candidate whose ``|CV − λ_target|`` is smallest as
     **the marginal unit**. (When the gap > tol, fall back: the
     dispatched unit with CV ≤ λ_target and highest CV.)
  e. ``ε[bus, hour] = emission_factor(marginal.fuel_type)`` from the
     IPCC 2006 GL Vol. 2 Tab. 1.4 derived table.
  f. Special cases:
     * λ_CEN = 0 → renewable curtailment regime → ε = 0.
     * λ_CEN > demand_fail_cost  → unattributed.

This is **reverse-engineered**: we don't try to reproduce the LP
solve; we use the cleared price as the anchor and ask "which unit's
declared CV best matches it among those actually running interior".
The output is ε, not λ — λ is the input.

Outputs
-------
  * per-(bus, quarter) parquet: ``date_utc, hour, minute, bus_uid,
    bus_name, lambda_cen, marginal_central, marginal_unit,
    marginal_cv, marginal_fuel, epsilon_kg_per_mwh, regime``.
  * Pretty hourly + daily-summary tables.
"""

from __future__ import annotations

import os
import sys
import warnings
from pathlib import Path

import numpy as np
import pandas as pd

from cen2gtopt._cached_extractor import fetch_by_name
from cen2gtopt._cen_client import CenApiClient, CenApiConfig
from cen2gtopt._id_bridge import (
    build_alias_index,
    index_stats,
    resolve as bridge_resolve,
)
from cen2gtopt._scvic_api import cv_rpt_for_date

# Silence numeric-conversion noise from pandas/pyarrow during heavy
# DataFrame construction.  Scoped so the global filter is NOT
# applied at module import (was hiding warnings from any other
# code in the same process); the legacy ``main()`` re-applies it
# locally after entry for back-compat.

DATE = "2026-04-22"
#: Public CEN SIP browse-key embedded in CEN's own Angular SPA;
#: this is the documented anonymous-read key, NOT a per-user
#: credential.  Override with $CEN_USER_KEY when CEN rotates it.
SIP_KEY = os.environ.get("CEN_USER_KEY", "492a5028ad67fd63791e28591cd29c01")
#: Default workdir for the legacy ``main()`` CLI.  Created lazily
#: inside ``main()`` — never at import time.
WORK_DIR = Path("/tmp/cen_emission_factor_v3")

#: CEN consigna codes that flag a unit as price-taker (forced).
#: Six SSCC + take-or-pay reasons combined:
#:   CI  — Cumplimiento Inflexible (TOP / take-or-pay GNL)
#:   MT  — Mínimo Técnico (held at pmin for stability)
#:   PMT — Pre-Mínimo Técnico (ramping up to MT)
#:   PP  — Punto Programado (CTF activation carrier)
#:   PS  — Punto Seguimiento (held for ramp-reserve)
#:   PC  — Pico Carga (peak / ramp pre-stage)
FORCED_CONSIGNAS = frozenset({"CI", "MT", "PMT", "PP", "PS", "PC"})

# CMG source policy:
#   "real"      → settlement only (cmg-real). Has 02:30→23:45 gap.
#   "online"    → real-time only (cmg-online). Full 24h, pre-validation.
#   "best_real" → cmg-real where available, fall back to cmg-online for
#                 the missing first 10 quarters of the day.  (default)
CMG_SOURCE = os.environ.get("CMG_SOURCE", "best_real")

# Reference buses (10 confirmed published per the 190-bus dump).
# Mix of 220 kV transmission backbone + 110/154 kV regional.
REF_BUSES = [
    ("CRUCERO_______220", 10, "CRUCERO 220KV (north)"),
    ("D.ALMAGRO_____110", 12, "DIEGO DE ALMAGRO 110KV (north-centre)"),
    ("ELMAITEN______066", 13, "EL MAITEN 66KV (centre-north)"),
    ("GUACOLDA______220", 16, "GUACOLDA 220KV (north-centre)"),
    ("ESPERANZA_____220", 17, "ESPERANZA 220KV (north)"),
    ("L.ALMENDROS___220", 18, "LOS ALMENDROS 220KV (centre)"),
    ("EL_SALTO______220", 19, "EL SALTO 220KV (centre)"),
    ("A.MELIP_______220", 1, "ALTO MELIPILLA 220KV (Santiago)"),
    ("CONCEPCION____220", 20, "CONCEPCION 220KV (south-centre)"),
    ("CHARRUA_______220", 2, "CHARRÚA 220KV (south-centre)"),
]
# CMG-clustering tolerance — buses with λ within this band are in the
# same island. Master plan §4.7 R3 uses the same heuristic.
ISLAND_LMP_TOL = 0.5  # USD/MWh


# ---------------------------------------------------------------------------
# Fuel → emission factor (kg CO₂ / MWh).
# IPCC 2006 GL Vol. 2 Tab. 1.4 default × typical SEN heat-rate.
# ---------------------------------------------------------------------------
FUEL_EF: dict[str, float] = {
    # SCVIC fuel name → kg CO₂ / MWh
    "Carbón": 1062.0,
    "Carbón + Petcoke": 1062.0,
    "Petcoke": 1062.0,
    "Diesel": 847.0,
    "Diésel": 847.0,
    "Petróleo Diésel": 847.0,
    "Fuel Oil": 780.0,
    "Fuel Oil Nro. 6": 780.0,
    "Gas Natural": 380.0,
    "GNL": 380.0,
    "GLP": 700.0,
    "Gas Propano": 700.0,
    "Biogas": 0.0,
    "Biomasa": 0.0,  # IPCC biogenic-zero
    "Cogeneración": 400.0,  # mixed
    "Geotérmica": 0.0,
    "Hidráulica": 0.0,
    "Solar": 0.0,
    "Eólica": 0.0,
    "": 0.0,
}


def _to_float_es(s: object) -> float:
    if s is None or (isinstance(s, float) and pd.isna(s)):
        return float("nan")
    if isinstance(s, (int, float)):
        return float(s)
    txt = str(s).strip()
    if not txt:
        return float("nan")
    if "," in txt and "." in txt:
        txt = txt.replace(".", "").replace(",", ".")
    elif "," in txt:
        txt = txt.replace(",", ".")
    try:
        return float(txt)
    except ValueError:
        return float("nan")


def _normalize_cmg_15min(raw: pd.DataFrame) -> pd.DataFrame:
    out = pd.DataFrame()
    out["date_utc"] = raw["fecha"].astype(str).str.slice(0, 10)
    out["hour"] = pd.to_numeric(raw["hra"], errors="coerce").astype("Int64")
    out["minute"] = pd.to_numeric(raw["min"], errors="coerce").astype("Int64")
    out["quarter"] = out["hour"].astype(int) * 4 + (out["minute"].astype(int) // 15)
    out["bus_name"] = raw["barra_info"].astype(str)
    out["lmp"] = pd.to_numeric(raw["cmg_usd_mwh_"], errors="coerce").astype(float)
    return out.dropna(subset=["quarter", "lmp"]).reset_index(drop=True)


def _try_fetch_cmg(
    client: CenApiClient,
    kind: str,
    bar_transf: str,
    date: str | None = None,
) -> pd.DataFrame:
    """Fetch ``cmg_real`` or ``cmg_online`` for one bus, returning
    a normalised 15-min long-form frame; empty on failure."""
    try:
        raw = fetch_by_name(
            client, kind, start=date or DATE, extra_params={"bar_transf": bar_transf}
        )
    except Exception:  # noqa: BLE001
        return pd.DataFrame()
    if raw.empty:
        return pd.DataFrame()
    df = _normalize_cmg_15min(raw)
    df["source"] = kind
    return df


#: Endpoint spec for bulk fetcher (mirrors the registry in
#: :mod:`cen2gtopt._cached_extractor`).
ENDPOINTS_BULK = {
    "cmg_online": {
        "service": "sip",
        "path": "/costo-marginal-online/v4/findByDate",
    },
    "cmg_real": {
        "service": "sip",
        "path": "/costo-marginal-real/v4/findByDate",
    },
}


def _enumerate_bar_transfs(
    client: CenApiClient,  # noqa: ARG001  — kept for signature stability
    *,
    date: str,  # noqa: ARG001  — unused but kept for signature stability
    verbose: bool = False,
) -> list[tuple[int, str, str]]:
    """Enumerate CEN bus identifiers strictly from the **cmg-real
    on-disk cache** (no live API access at all).

    Reads every cached parquet under
    ``~/.cache/gtopt/cen2gtopt/sip/costo_marginal_real__v4__findByDate/``
    to harvest unique ``(id_info, bar_transf, barra_info)`` tuples.
    When the cache is empty, returns an empty list — caller must
    populate the cache (e.g. via per-bus ``fetch_by_name`` calls or
    by running the pipeline against a known REF_BUSES list once).

    Returns ``[(id_info, bar_transf, barra_info), …]`` sorted.
    """
    # pylint: disable=import-outside-toplevel
    from cen2gtopt._bus_catalogue import discover_buses_from_cache

    cat = discover_buses_from_cache()
    seen: dict[int, tuple[int, str, str]] = {}
    if not cat.empty:
        for _, r in cat.iterrows():
            iid = int(r["bus_id"])
            seen[iid] = (
                iid,
                str(r["bar_transf"]),
                str(r.get("barra_info", "")),
            )
    if verbose:
        print(f"    cmg-real cache: {len(seen)} unique buses")
    return sorted(seen.values())


def fetch_cmg_bulk(
    client: CenApiClient,
    *,
    date: str,
    endpoint: str = "cmg_real",
    bar_transfs: list[str] | None = None,
    max_workers: int = 8,
    verbose: bool = False,
) -> pd.DataFrame:
    """Bulk-fetch CMG for **all CEN buses** for one date using
    parallel **per-bar_transf** fetches (the only API path that
    reliably returns clean per-bus data).

    Architecture:

    1. **Discover buses** via one paginated cmg-online call (~5 s).
    2. **Parallel cmg-real fetch per bar_transf** with
       :class:`ThreadPoolExecutor` (default 8 workers).  Each call
       returns 96 rows × 1 version (REAL-DEF) cleanly.
    3. **Concatenate**.

    Hour-loop bulk-fetch via the ``hra`` filter was abandoned (CEN's
    SIP API doesn't honor it strictly — see git log).

    Args:
        client: open SIP client.
        date: YYYY-MM-DD.
        endpoint: ``"cmg_real"`` (default, settled) or ``"cmg_online"``
            (preliminary, has version multiplicity).
        bar_transfs: optional explicit list of bar_transf strings to
            fetch.  When ``None``, auto-discovers ALL CEN buses.
        max_workers: parallel HTTP threads (8 = good balance vs CEN's
            rate limiter).
        verbose: print per-bus progress.

    Returns the long DataFrame with the same schema as before.
    """
    if endpoint not in ENDPOINTS_BULK:
        raise ValueError(
            f"unknown endpoint {endpoint!r}; expected one of {list(ENDPOINTS_BULK)}"
        )

    # 1. Discovery (skip when caller passed bar_transfs)
    if bar_transfs is None:
        if verbose:
            print(f"    discovering bar_transfs for {date} …")
        catalogue = _enumerate_bar_transfs(client, date=date, verbose=verbose)
        bar_transfs = [bt for _, bt, _ in catalogue]
    if verbose:
        print(
            f"    fetching {len(bar_transfs)} buses via parallel "
            f"per-bar_transf cmg-{endpoint.removeprefix('cmg_')} "
            f"(max_workers={max_workers})…"
        )

    # 2. Parallel per-bus fetch
    def _fetch_one_bus(bt: str) -> list[dict]:
        # Always go through the cached extractor — never bypass to a
        # raw client.get(...) — so every API call lands in the
        # parquet cache and re-runs are instant.
        try:
            df_bus = fetch_by_name(
                client,
                endpoint,
                start=date,
                extra_params={"bar_transf": bt},
            )
        except Exception:  # noqa: BLE001  # pylint: disable=broad-except
            return []
        return df_bus.to_dict("records") if not df_bus.empty else []

    # pylint: disable=import-outside-toplevel
    from concurrent.futures import ThreadPoolExecutor, as_completed

    rows: list[dict] = []
    n_done = 0
    with ThreadPoolExecutor(max_workers=max_workers) as pool:
        futures = {pool.submit(_fetch_one_bus, bt): bt for bt in bar_transfs}
        for fut in as_completed(futures):
            bt = futures[fut]
            n_done += 1
            try:
                bus_rows = fut.result()
            except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-except
                if verbose:
                    print(f"    {bt}: failed ({exc})")
                continue
            rows.extend(bus_rows)
            if verbose and (n_done % 100 == 0):
                print(
                    f"    [{n_done}/{len(bar_transfs)}] +{len(bus_rows):>3} "
                    f"rows for {bt} (total {len(rows):,})"
                )

    if not rows:
        return pd.DataFrame()
    df = pd.DataFrame(rows)
    out = pd.DataFrame()
    out["id_info"] = pd.to_numeric(df["id_info"], errors="coerce").astype("Int64")
    out["bar_transf"] = df["barra_transf"].astype(str)
    out["barra_info"] = df["barra_info"].astype(str)
    out["date_utc"] = df["fecha"].astype(str).str.slice(0, 10)
    out["hour"] = pd.to_numeric(df["hra"], errors="coerce").astype("Int64")
    out["minute"] = pd.to_numeric(df["min"], errors="coerce").astype("Int64")
    out["quarter"] = out["hour"].astype(int) * 4 + (out["minute"].astype(int) // 15)
    out["lmp"] = pd.to_numeric(df["cmg_usd_mwh_"], errors="coerce")
    out["source"] = endpoint
    # cmg-online returns multiple ``version`` rows per (bus, quarter)
    # — REAL-DEF / REAL-PRE / EN LINEA represent the validation
    # lifecycle.  Keep ONE row per cell, preferring the most-validated.
    if "version" in df.columns:
        out["version"] = df["version"].astype(str)
        version_priority = {"REAL-DEF": 0, "REAL-PRE": 1, "EN LINEA": 2}
        out["_pri"] = out["version"].map(lambda v: version_priority.get(v, 9))
        out = (
            out.sort_values("_pri")
            .drop_duplicates(subset=["id_info", "quarter"], keep="first")
            .drop(columns=["_pri"])
        )
    return out.dropna(subset=["id_info", "lmp", "quarter"]).reset_index(drop=True)


def fetch_cmg_real(
    client: CenApiClient,
    *,
    date: str | None = None,
    ref_buses: list[tuple[str, int, str]] | None = None,
    cmg_source: str | None = None,
    verbose: bool = True,
) -> pd.DataFrame:
    """CMG per reference bus, source determined by ``CMG_SOURCE``.

    Modes:
      * ``"real"``      — settlement-only (cmg-real). Has the
        02:30→23:45 publishing gap (10 missing quarters/day).
      * ``"online"``    — real-time only (cmg-online). Full 24h,
        pre-validation. Use when cmg-real coverage is incomplete.
      * ``"best_real"`` — cmg-real where available, fall back to
        cmg-online for the missing first 10 quarters per day. Best
        of both: settlement-grade for ~90% of cells, full coverage
        via online for the early morning. (default)
    """
    date = date or DATE
    ref_buses = ref_buses if ref_buses is not None else REF_BUSES
    cmg_source = cmg_source or CMG_SOURCE
    rows = []
    if verbose:
        print(f"  CMG source policy: {cmg_source}")
    for bar_transf, bus_uid, name in ref_buses:
        if cmg_source == "real":
            df = _try_fetch_cmg(client, "cmg_real", bar_transf, date=date)
        elif cmg_source == "online":
            df = _try_fetch_cmg(client, "cmg_online", bar_transf, date=date)
        else:  # best_real
            df_real = _try_fetch_cmg(client, "cmg_real", bar_transf, date=date)
            df_online = _try_fetch_cmg(client, "cmg_online", bar_transf, date=date)
            if df_real.empty and df_online.empty:
                df = pd.DataFrame()
            elif df_real.empty:
                df = df_online
            else:
                real_qs = set(df_real["quarter"])
                fill = (
                    df_online[~df_online["quarter"].isin(real_qs)]
                    if not df_online.empty
                    else pd.DataFrame()
                )
                df = pd.concat([df_real, fill], ignore_index=True)
        if df.empty:
            if verbose:
                print(f"  skip {name}: no CMG ({cmg_source}) data")
            continue
        df = df.sort_values("quarter").reset_index(drop=True)
        # Back-extrapolate: for any missing quarter at the start of
        # the day (CEN's 02:30 cutoff leaves 00:00-02:15 absent),
        # copy the value from the first known quarter of the day.
        present = set(df["quarter"])
        first_known = df.iloc[0]
        fills = []
        for q in range(int(first_known["quarter"])):
            if q in present:
                continue
            row = first_known.copy()
            row["quarter"] = q
            row["hour"] = q // 4
            row["minute"] = (q % 4) * 15
            row["source"] = "extrapolated"
            fills.append(row)
        n_extrap = len(fills)
        if fills:
            df = pd.concat([pd.DataFrame(fills), df], ignore_index=True)
            df = df.sort_values("quarter").reset_index(drop=True)

        df["bus_uid"] = bus_uid
        df["bus_label"] = name
        rows.append(df)
        n = len(df)
        n_real = (df["source"] == "cmg_real").sum() if "source" in df.columns else 0
        n_online = (df["source"] == "cmg_online").sum() if "source" in df.columns else 0
        if verbose:
            if cmg_source == "best_real":
                print(
                    f"  {name}: {n} cells ({n_real} cmg-real + "
                    f"{n_online} cmg-online + {n_extrap} extrapolated)"
                )
            else:
                print(f"  {name}: {n} cells ({cmg_source} + {n_extrap} extrapolated)")
    return pd.concat(rows, ignore_index=True) if rows else pd.DataFrame()


def fetch_limitaciones(
    client: CenApiClient,
    *,
    date: str | None = None,
) -> pd.DataFrame:
    """Transmission limitations for the day (line outages /
    operational restrictions).  Each row carries the affected
    installation, type of limitation, and start/end times."""
    return fetch_by_name(client, "limitaciones", start=date or DATE)


#: Maximum λ-spread allowed *within* one island.  When the
#: chained-tolerance walk would put two buses with λ-difference
#: > ``ISLAND_MAX_SPREAD`` into the same island, we force a split.
#: Prevents the chained-clustering pathology where
#: [41.5, 41.7, 41.9, 42.1, 42.3, 42.7] (each adjacent pair within
#: tol=0.5) collapse into one island despite the endpoints differing
#: by $1.20.  Without this, congested-zone buses with consistent
#: ~$0.50 premiums get lumped with the system and the marginal-unit
#: picker can't distinguish them.
ISLAND_MAX_SPREAD = 0.8  # USD/MWh


def cluster_buses_into_islands(
    cmg_df: pd.DataFrame,
    *,
    tol: float = ISLAND_LMP_TOL,
    max_spread: float = ISLAND_MAX_SPREAD,
) -> pd.DataFrame:
    """Per (date, quarter), group buses with similar λ_CEN into the
    same ``island_id``.  Master plan §4.7: when two buses have the
    same λ they're electrically in the same merit-clearing island.

    Splits an island when **either** the gap to the previous bus
    exceeds ``tol`` OR the cumulative spread within the current
    island would exceed ``max_spread``.  The latter prevents the
    chained-tolerance pathology that lumps congested zones together.

    Returns a DataFrame with the same rows as ``cmg_df`` plus an
    ``island_id`` integer (unique per (date, quarter, island)).
    """
    out_chunks = []
    for (date, quarter), grp in cmg_df.groupby(["date_utc", "quarter"]):
        g = grp.sort_values("lmp").copy()
        prev = None
        island_min = None  # λ-min of the current island
        cur_id = -1
        ids = []
        for v in g["lmp"]:
            new_island = (
                prev is None
                or abs(v - prev) > tol
                or (island_min is not None and (v - island_min) > max_spread)
            )
            if new_island:
                cur_id += 1
                island_min = v
            ids.append(cur_id)
            prev = v
        g["_island_local"] = ids
        # Make island_id globally unique by mixing in (date, quarter)
        g["island_id"] = (
            g["_island_local"].astype(str) + "@" + str(date) + "_q" + str(quarter)
        )
        out_chunks.append(g.drop(columns=["_island_local"]))
    return pd.concat(out_chunks, ignore_index=True)


def fetch_generacion_real(
    client: CenApiClient,
    *,
    date: str | None = None,
) -> pd.DataFrame:
    df = fetch_by_name(client, "generacion_real", start=date or DATE)
    if df.empty:
        return df
    df["hour"] = pd.to_numeric(df["hora"], errors="coerce").astype("Int64") - 1
    df["mw"] = pd.to_numeric(df["gen_real_mw"], errors="coerce").astype(float)
    df["pmax"] = pd.to_numeric(df["potencia_maxima"], errors="coerce").astype(float)
    return df.dropna(subset=["hour", "mw", "pmax"]).reset_index(drop=True)


def fetch_instrucciones(
    client: CenApiClient,
    *,
    date: str | None = None,
) -> pd.DataFrame:
    df = fetch_by_name(client, "instrucciones_cmg", start=date or DATE)
    if df.empty:
        return df
    df["hour"] = df["hora"].astype(str).str.split(":").str[0].astype(int)
    return df


def fetch_unidades_generadoras(
    client: CenApiClient,
    *,
    date: str | None = None,
) -> pd.DataFrame:
    return fetch_by_name(client, "unidades_generadoras", start=date or DATE)


def fetch_gen_programada_pcp(
    client: CenApiClient,
    *,
    date: str | None = None,
) -> pd.DataFrame:
    """LP-output programmed dispatch per central per hour."""
    df = fetch_by_name(client, "generacion_programada_pcp", start=date or DATE)
    if df.empty:
        return df
    # Parse fecha_hora → hour
    df["fecha_hora"] = pd.to_datetime(df["fecha_hora"], errors="coerce")
    df["hour"] = df["fecha_hora"].dt.hour.astype("Int64")
    df["mw"] = pd.to_numeric(df["gen_programada_mw"], errors="coerce").astype(float)
    df["id_central"] = pd.to_numeric(df["id_central"], errors="coerce").astype("Int64")
    return df.dropna(subset=["hour", "mw", "id_central"]).reset_index(drop=True)


def fetch_cmg_programado_pcp(
    client: CenApiClient,
    *,
    date: str | None = None,
) -> pd.DataFrame:
    """Programmed CMG per bus per hour. Implicit water-value source
    for hydros."""
    df = fetch_by_name(client, "cmg_programado_pcp", start=date or DATE)
    if df.empty:
        return df
    df["fecha_hora"] = pd.to_datetime(df["fecha_hora"], errors="coerce")
    df["hour"] = df["fecha_hora"].dt.hour.astype("Int64")
    df["cmg"] = pd.to_numeric(df["cmg_usd_mwh"], errors="coerce").astype(float)
    return df.dropna(subset=["hour", "cmg"]).reset_index(drop=True)


def fetch_embalse_real(
    client: CenApiClient,
    *,
    date: str | None = None,
) -> pd.DataFrame:
    """Reservoir levels (cota msnm) per embalse."""
    return fetch_by_name(client, "embalse_real", start=date or DATE)


def fetch_flujo_programado_pcp(
    client: CenApiClient,
    *,
    date: str | None = None,
) -> pd.DataFrame:
    """Programmed line flows — useful for island detection
    (saturated lines) and bus-mapping centrales."""
    df = fetch_by_name(client, "flujo_programado_pcp", start=date or DATE)
    if df.empty:
        return df
    df["fecha_hora"] = pd.to_datetime(df["fecha_hora"], errors="coerce")
    df["hour"] = df["fecha_hora"].dt.hour.astype("Int64")
    df["mw"] = pd.to_numeric(df["flujo_mw"], errors="coerce").astype(float)
    return df.dropna(subset=["hour", "mw"]).reset_index(drop=True)


# ---------------------------------------------------------------------------
# The reverse-engineered identifier
# ---------------------------------------------------------------------------
INTERIOR_TOL_RATIO = 0.05  # within 5% of pmin or pmax → treat as boundary


def candidates_pmax(candidates: pd.DataFrame, id_central: int) -> float:
    """Look up the pmax of a central in the candidate set."""
    rows = candidates[candidates["id_central"] == id_central]
    if rows.empty:
        return 0.0
    val = rows["pmax"].iloc[0]
    return float(val) if pd.notna(val) else 0.0


def identify_marginal(
    *,
    lambda_target: float,
    candidates: pd.DataFrame,
    fuel_by_id: dict[int, str],
    cv_options_by_id: dict[int, list[float]],
    hydro_id_set: set[int] | None = None,
    water_value: float = 0.0,
) -> dict:
    """Pick the marginal unit from the candidate dispatched-interior set.

    Args:
        lambda_target: published λ_CEN at this (bus, hour) in USD/MWh.
        candidates: dispatched non-forced units at the hour, with
            columns ``id_central, central, mw, pmin, pmax``.
        fuel_by_id, cv_by_id: SCVIC-derived per-id metadata.

    Returns:
        ``{regime, marginal_id, marginal_central, marginal_cv,
           marginal_fuel, epsilon, n_candidates}``
    """
    # Special regimes
    if lambda_target <= 0.5:
        return {
            "regime": "renewable_curtailment",
            "marginal_id": None,
            "marginal_central": "renewable_curtailment",
            "marginal_cv": 0.0,
            "marginal_fuel": "",
            "epsilon": 0.0,
            "n_candidates": 0,
            "n_dispatched_unmapped": 0,
            "top_below_lambda": "",
            "top_above_lambda": "",
        }
    if lambda_target > 800.0:
        return {
            "regime": "demand_fail",
            "marginal_id": None,
            "marginal_central": "demand_fail_cost",
            "marginal_cv": lambda_target,
            "marginal_fuel": "",
            "epsilon": 0.0,
            "n_candidates": 0,
            "n_dispatched_unmapped": 0,
            "top_below_lambda": "",
            "top_above_lambda": "",
        }

    if candidates.empty:
        return {
            "regime": "no_candidates",
            "marginal_id": None,
            "marginal_central": "",
            "marginal_cv": float("nan"),
            "marginal_fuel": "",
            "epsilon": 0.0,
            "n_candidates": 0,
            "n_dispatched_unmapped": 0,
            "top_below_lambda": "",
            "top_above_lambda": "",
        }

    # Strategy: the marginal unit's CV ≈ λ_target. Among ALL dispatched
    # units with a SCVIC CV, pick the one whose CV is closest to
    # λ_target. Don't pre-filter by interior — coal/CCGT plants are
    # often dispatched at pmax (capped) and remain on the merit margin.
    hydro_id_set = hydro_id_set or set()
    # Price-setter eligibility (data-driven):
    #
    #   * SCVIC participants → CAN set the marginal at their CV.
    #   * **Forced units at INTERIOR dispatch (above pmin) stay
    #     in the pool** — even though they're forced/RO-flagged,
    #     when they're operating above their technical minimum the
    #     extra MWh CAN come from them, so their CV is on the merit
    #     margin. Only forced units AT pmin are excluded (they're
    #     the ones that physically can't supply more MWh).
    #
    # Reservoir hydros remain excluded (no public water value).
    # P1-D: vectorised membership test (Series.isin → hash-set lookup).
    pool_all = candidates[
        candidates["id_central"].isin(pd.Index(cv_options_by_id.keys()))
    ].copy()
    # Determine status per candidate (interior, capped_pmax, near_pmin)
    pool_all["pmin_pad"] = pool_all["pmin"] + INTERIOR_TOL_RATIO * (
        pool_all["pmax"] - pool_all["pmin"]
    )
    pool_all["pmax_pad"] = pool_all["pmax"] - INTERIOR_TOL_RATIO * (
        pool_all["pmax"] - pool_all["pmin"]
    )
    # P1-C: vectorised status (np.where on numeric arrays, not apply).
    interior_mask = (pool_all["mw"] > pool_all["pmin_pad"]) & (
        pool_all["mw"] < pool_all["pmax_pad"]
    )
    capped_mask = pool_all["mw"] >= pool_all["pmax_pad"]
    pool_all["status"] = np.where(
        interior_mask,
        "interior",
        np.where(capped_mask, "capped_pmax", "near_pmin"),
    )
    # Drop forced units that are at pmin (they cannot ramp up,
    # therefore cannot set the marginal). Keep forced units that
    # are interior or capped — they're operating above pmin and
    # their CV is a valid merit-margin candidate.
    pool = pool_all[
        ~((pool_all["forced"]) & (pool_all["status"] == "near_pmin"))
    ].copy()
    if pool.empty:
        return {
            "regime": "no_cv_match",
            "marginal_id": None,
            "marginal_central": "",
            "marginal_cv": float("nan"),
            "marginal_fuel": "",
            "epsilon": 0.0,
            "n_candidates": 0,
            "n_dispatched_unmapped": len(candidates),
            "top_below_lambda": "",
            "top_above_lambda": "",
        }

    # For each candidate central, pick the SCVIC CV option closest
    # to λ_target. Hydros are not in cv_options_by_id (excluded above)
    # so we never need a fallback.
    def best_cv(ic: int) -> float:
        opts = cv_options_by_id[int(ic)]
        below = [c for c in opts if c <= lambda_target]
        if below:
            return max(below)
        return min(opts)

    pool["cv"] = pool["id_central"].map(best_cv)
    pool["fuel"] = pool["id_central"].map(lambda i: fuel_by_id.get(int(i), ""))

    # Pick the unit whose CV is **absolutely closest** to λ_target
    # (best-of-both-sides). This is preferable to "max CV ≤ λ" when
    # the closest-above CV is nearer than the closest-below — common
    # at low-CMG hours where biogas tier sits well below gas tier.
    pool["abs_dev"] = (pool["cv"] - lambda_target).abs()
    chosen = pool.nsmallest(1, "abs_dev").iloc[0]
    pmin_pad = chosen["pmin"] + INTERIOR_TOL_RATIO * (chosen["pmax"] - chosen["pmin"])
    pmax_pad = chosen["pmax"] - INTERIOR_TOL_RATIO * (chosen["pmax"] - chosen["pmin"])
    status = (
        "interior"
        if pmin_pad < chosen["mw"] < pmax_pad
        else "capped_pmax"
        if chosen["mw"] >= pmax_pad
        else "near_pmin"
    )
    regime = "matched_above" if chosen["cv"] > lambda_target else f"matched_{status}"

    # Diagnostic: top 3 candidates closest to λ above and below.
    pool_below = (
        pool[pool["cv"] <= lambda_target].sort_values("cv", ascending=False).head(3)
    )
    pool_above = pool[pool["cv"] > lambda_target].sort_values("cv").head(3)
    top_below = ";".join(f"{r.central}@${r.cv:.0f}" for r in pool_below.itertuples())
    top_above = ";".join(f"{r.central}@${r.cv:.0f}" for r in pool_above.itertuples())
    return {
        "regime": regime,
        "marginal_id": int(chosen["id_central"]),
        "marginal_central": str(chosen["central"]),
        "marginal_cv": float(chosen["cv"]),
        "marginal_fuel": str(chosen["fuel"]),
        "epsilon": FUEL_EF.get(str(chosen["fuel"]), 0.0),
        "n_candidates": len(pool),
        "n_dispatched_unmapped": len(candidates) - len(pool),
        "top_below_lambda": top_below,
        "top_above_lambda": top_above,
    }


# ---------------------------------------------------------------------------
# Multi-day–friendly compute (extracted from main, parameterised by date).
# Returns the per-(bus, quarter) DataFrame with Tier-1 indicators baked in.
# Used by `tools/cen_marginal_period.py` for multi-day orchestration.
# ---------------------------------------------------------------------------

#: Chile carbon tax — Law 20.780 / 2017, currently US$5/tCO₂ (escalating).
CO2_TAX_USD_PER_TON = 5.0

#: SCVIC fuel labels treated as "renewable" for ``system_renewable_fraction``
#: (zero-carbon, non-dispatchable per Chilean NCRE classification).
RENEWABLE_FUELS = {
    "Solar",
    "Eólica",
    "Geotérmica",
    "Biomasa",
    "Biogas",
}

#: Hydro-pasada and reservoir tipos — counted in the renewable fraction
#: (zero direct emissions; Chilean SEN treats them as non-fossil).
HYDRO_TIPOS_FOR_RENEWABLE = {
    "Hidroeléctrica de pasada",
    "Minihidro de pasada",
    "Hidroeléctrica de embalse",
    "Hidráulica",
}


def _load_pid_nod_loss(date_iso: str) -> pd.DataFrame:
    """Load PID's per-bus per-period transmission losses (MW).

    Returns a DataFrame with columns ``bus_name`` (PLEXOS bus name,
    e.g. ``Crucero220``), ``period`` (1-24), ``loss_mw``.  Empty
    when no PID bundle is on disk for ``date_iso``.

    These are absolute losses (MW) per bus per period.  To compute
    a per-bus marginal loss factor we need bus load per period — see
    :func:`_pid_per_bus_mlf` which combines this with ``Nod_Load``.
    """
    try:
        # pylint: disable=import-outside-toplevel
        from cen2gtopt.pcp_inputs import load_pcp_inputs
    except ImportError:
        return pd.DataFrame()
    try:
        inputs = load_pcp_inputs(date_iso, source="pid", auto_download=False)
    except FileNotFoundError:
        return pd.DataFrame()
    if not inputs.is_pid:
        return pd.DataFrame()
    nl = inputs.nod_loss
    if nl.empty:
        return nl
    return nl.rename(
        columns={"NAME": "bus_name", "PERIOD": "period", "VALUE": "loss_mw"},
    )[["bus_name", "period", "loss_mw"]]


def _pid_per_bus_mlf(date_iso: str) -> dict[tuple[str, int], float]:
    """Compute per-(bus_name, period) marginal loss factor from PID.

    The PID's ``Nod_Loss.csv`` carries absolute losses (MW); we
    convert to a dimensionless per-bus loss-adjusted factor by
    normalising against the system reference bus.

    Strategy: find the bus with the smallest mean loss → call it the
    "centroid"; for each other bus compute
    ``mlf_b = 1 - (loss_b - loss_centroid) / load_b`` (sub-1 for
    high-loss buses, super-1 for sub-centroid).  This is a coarse
    proxy in absence of the proper PTDF matrix but tracks the
    PCP-published per-unit MLF reasonably well.

    Returns ``{(plexos_bus_name, period_1..24): mlf}``.  Empty dict
    when PID inputs unavailable.
    """
    try:
        # pylint: disable=import-outside-toplevel
        from cen2gtopt.pcp_inputs import load_pcp_inputs
    except ImportError:
        return {}
    try:
        inputs = load_pcp_inputs(date_iso, source="pid", auto_download=False)
    except FileNotFoundError:
        return {}
    if not inputs.is_pid:
        return {}
    losses = inputs.nod_loss
    load_long = inputs.nod_load_long
    if losses.empty or load_long.empty:
        return {}
    # Join by (bus, period); compute per-bus loss ratio
    losses = losses.rename(
        columns={"NAME": "bus_name", "PERIOD": "period", "VALUE": "loss_mw"},
    )
    load_long = load_long.rename(columns={"PERIOD": "period"})
    j = losses.merge(
        load_long[["bus_name", "period", "load_mw"]],
        on=["bus_name", "period"],
        how="left",
    )
    j["load_mw"] = j["load_mw"].fillna(0.0)
    # Use mean loss-fraction across the day to identify the centroid
    j["loss_fraction"] = j["loss_mw"] / j["load_mw"].replace(0.0, float("nan"))
    centroid = (
        j.groupby("bus_name")["loss_fraction"]
        .mean()
        .replace([float("inf"), float("-inf")], float("nan"))
        .dropna()
        .idxmin()
    )
    centroid_per_period = (
        j[j["bus_name"] == centroid].groupby("period")["loss_fraction"].mean().to_dict()
    )
    out: dict[tuple[str, int], float] = {}
    for _, r in j.iterrows():
        per = int(r["period"])
        c_loss_raw = centroid_per_period.get(per, 0.0)
        c_loss = float(c_loss_raw) if pd.notna(c_loss_raw) else 0.0
        b_loss_raw = r["loss_fraction"]
        b_loss = float(b_loss_raw) if pd.notna(b_loss_raw) else c_loss
        # MLF: 1 - (b_loss - c_loss).  Clipped to [0.85, 1.15].
        mlf = max(0.85, min(1.15, 1.0 - (b_loss - c_loss)))
        out[(str(r["bus_name"]), per)] = float(mlf)
    return out


def _pid_per_bus_mlf_from_accdb(
    date_iso: str,
) -> dict[tuple[str, int], float]:
    """Read **CEN's published** per-bus per-period Marginal Loss
    Factor from the PID solution ``.accdb``.

    The PID solution database carries a property
    ``"Marginal Loss Factor"`` on the System.Nodes collection
    (``collection_id=281``).  This is the exact PLEXOS-solver-output
    MLF used at LP clearing — far more accurate than the
    ``Nod_Loss / Nod_Load`` approximation in :func:`_pid_per_bus_mlf`
    (which is clipped to [0.85, 1.15]).

    Returns ``{(plexos_bus_name, period_1..24): mlf}``.  Empty dict
    when:
      * no PID .accdb is available for ``date_iso``
      * ``mdbtools`` is not on $PATH
      * the property is not present in the schema
    """
    # pylint: disable=import-outside-toplevel
    try:
        from cen2gtopt.pcp_solution import (
            _have_mdbtools,
            extract_property,
            resolve_solution,
        )
    except ImportError:
        return {}
    if not _have_mdbtools():
        return {}
    try:
        sol = resolve_solution(date_iso, source="pid", auto_download=False)
    except FileNotFoundError:
        return {}
    try:
        df = extract_property(
            sol.accdb_path,
            property_name="Marginal Loss Factor",
            collection_id=281,  # System.Nodes
            period_table=0,
        )
    except (ValueError, RuntimeError):
        return {}
    if df.empty:
        return {}
    # ``hour`` is 0-indexed in the extract; PCP/PID periods are
    # 1-indexed.  ``itertuples`` is roughly 10× faster than
    # ``iterrows`` because it avoids reconstructing a Series per row.
    out: dict[tuple[str, int], float] = {}
    for row in df[["object_name", "hour", "value"]].itertuples(index=False, name=None):
        try:
            bus, hour, value = row
            out[(str(bus), int(hour) + 1)] = float(value)
        except (TypeError, ValueError):
            continue
    return out


def _bus_mlf_by_id_info(
    bus_mlf_plexos: dict[tuple[str, int], float],
) -> dict[int, float]:
    """Translate PID's per-bus MLF dict (keyed on PLEXOS bus names
    like ``"Crucero220"``) into a dict keyed on CEN's stable ``id_info``
    integer (the same key used by cmg-real / cmg-online).

    Lookup chain: PLEXOS name → bar_transf (via the same heuristic +
    overrides table that ``cen2gtopt.prepopulate_cache`` uses) →
    id_info (via the local bus catalogue scanned from cmg-real
    cache).  Buses missing from any step are silently dropped.

    The result is **period-aggregated** (mean MLF across the day) —
    consumers that need per-period values can reuse
    :func:`_pid_per_bus_mlf` directly.
    """
    if not bus_mlf_plexos:
        return {}
    # Local imports to keep cold-start cost low.
    # pylint: disable=import-outside-toplevel
    from cen2gtopt._bus_catalogue import discover_buses_from_cache
    from cen2gtopt.prepopulate_cache import (
        PLEXOS_TO_BAR_TRANSF,
        _heuristic_bar_transf,
    )

    # Aggregate per-period MLFs to one value per PLEXOS bus
    by_plexos: dict[str, list[float]] = {}
    for (plexos_name, _period), val in bus_mlf_plexos.items():
        by_plexos.setdefault(plexos_name, []).append(float(val))
    plexos_avg = {n: sum(v) / len(v) for n, v in by_plexos.items()}

    # PLEXOS → bar_transf
    plexos_to_bt: dict[str, str] = {}
    for plexos_name in plexos_avg:
        bt = PLEXOS_TO_BAR_TRANSF.get(plexos_name)
        if bt is None:
            bt = _heuristic_bar_transf(plexos_name)
        if bt:
            plexos_to_bt[plexos_name] = bt

    # bar_transf → id_info from on-disk cmg-real cache
    cat = discover_buses_from_cache()
    if cat.empty:
        return {}
    bt_to_id = dict(zip(cat["bar_transf"], cat["bus_id"]))

    out: dict[int, float] = {}
    for plexos_name, mlf in plexos_avg.items():
        bt = plexos_to_bt.get(plexos_name)
        if bt is None:
            continue
        iid = bt_to_id.get(bt)
        if iid is None:
            continue
        out[int(iid)] = mlf
    return out


def _load_pid_mlf_by_id(
    date_iso: str,
    alias_index: dict[str, int],
) -> dict[int, float]:
    """Load PID's per-unit MARGINAL loss factor for the marginal-bus
    correction (critical-review §1.5 / F8).

    Returns ``{id_central → mean_mlf_across_periods}``.  Empty dict
    when no PID bundle is available for ``date_iso`` — caller should
    fall back to the empirical ``λ_bus / λ_ref`` approximation.

    Imported lazily so the heavy ``pcp_inputs`` module isn't loaded
    on every call.
    """
    try:
        # Local import to break the import cycle pcp_inputs ↔ marginal_units
        # pylint: disable=import-outside-toplevel
        from cen2gtopt.pcp_inputs import load_pcp_inputs, resolve_pcp_unit
    except ImportError:
        return {}
    try:
        inputs = load_pcp_inputs(date_iso, source="pid", auto_download=False)
    except FileNotFoundError:
        return {}
    if not inputs.is_pid:
        return {}
    mlf = inputs.marginal_loss_factor
    if mlf.empty:
        return {}
    # Aggregate per unit (mean across all periods of the day)
    agg = mlf.groupby("NAME")["VALUE"].mean()
    out: dict[int, float] = {}
    for plexos_name, value in agg.items():
        ic = resolve_pcp_unit(str(plexos_name), alias_index)
        if ic is not None:
            out[int(ic)] = float(value)
    return out


def compute_marginal_units_for_day(
    client: CenApiClient,
    *,
    date_iso: str,
    ref_buses: list[tuple[str, int, str]] | None = None,
    cmg_source: str = "best_real",
    verbose: bool = False,
    use_pid_mlf: bool = True,
) -> pd.DataFrame:
    """Compute the per-(bus, quarter) marginal-unit dataset for one day.

    Returns a long-form DataFrame with the following columns (the
    'database' the user asked for):

      * **time keys**: ``date_utc``, ``hour``, ``minute``, ``quarter``
      * **space keys**: ``bus_uid``, ``bus_label``, ``island_id``,
        ``island_n_buses``
      * **base λ (the reconstructed hourly base marginal cost)**:
          - ``lambda_cen``     — the published CMG (15-min, with
            online + extrapolated fill for missing quarters).
          - ``lambda_provenance`` — ``'real' | 'online' | 'extrapolated'``
            (per critical-review §2.10).
      * **marginal unit**:
          - ``marginal_central``    — name
          - ``marginal_id_central`` — stable FK
          - ``marginal_fuel``       — SCVIC fuel label
          - ``marginal_cv``         — declared variable cost (USD/MWh)
          - ``marginal_pmin/pmax``  — declared limits (MW)
          - ``marginal_dispatch_mw``— how much the unit was generating
          - ``marginal_headroom_mw``— pmax − dispatch (upward sens.)
          - ``epsilon``             — per-fuel emission factor (kg/MWh)
      * **constructed λ + correction**:
          - ``lambda_pipe``         — reconstructed marginal cost
            (= ``marginal_cv``)
          - ``loss_factor``         — λ_bus / λ_island_ref (avg LF
            approximation; see critical-review §1.5)
          - ``lambda_constructed``  — ``marginal_cv × loss_factor``
            (the bus-level constructed marginal cost)
      * **constructed marginal emission**:
          - ``epsilon_at_bus``      — ``epsilon × loss_factor``
          - ``co2_tax_usd_per_mwh`` — Chile carbon-tax embedded cost
      * **system context**:
          - ``system_total_demand_mw`` — sum of dispatch in this hour
          - ``system_renewable_fraction`` — VRE+hydro share
      * **diagnostics**:
          - ``regime`` — ``matched_interior``/``matched_capped_pmax``/etc.
          - ``n_candidates`` / ``n_dispatched_unmapped`` / ``n_forced_in_pool``
          - ``tier_wedge_usd`` — gap to next CV tier (structural error band)
          - ``top_below_lambda`` / ``top_above_lambda``
          - ``delta_lmp`` / ``abs_delta`` / ``discrepancy_flag``

    Returns an empty DataFrame on a day with no published CMG (e.g.
    a missed-publication day in CEN's historical archive).
    """
    if ref_buses is None:
        ref_buses = REF_BUSES

    cmg = fetch_cmg_real(
        client,
        date=date_iso,
        ref_buses=ref_buses,
        cmg_source=cmg_source,
        verbose=verbose,
    )
    if cmg.empty:
        return pd.DataFrame()
    gen = fetch_generacion_real(client, date=date_iso)
    instr = fetch_instrucciones(client, date=date_iso)
    units = fetch_unidades_generadoras(client, date=date_iso)

    alias_index = build_alias_index(units)

    # PID-published per-unit MARGINAL loss factor (critical-review §1.5
    # / F8).  Empty dict when no PID bundle is on disk for this date,
    # in which case downstream falls back to the empirical
    # ``λ_bus / λ_ref`` approximation.
    mlf_by_id: dict[int, float] = (
        _load_pid_mlf_by_id(date_iso, alias_index) if use_pid_mlf else {}
    )
    if mlf_by_id and verbose:
        print(f"  PID marginal-loss factor loaded for {len(mlf_by_id)} units")

    # PID-derived per-bus MLF — prefer the exact accdb-published
    # values (PLEXOS solver output); fall back to Nod_Loss/Nod_Load
    # approximation when accdb is not available.
    bus_mlf: dict[tuple[str, int], float] = {}
    if use_pid_mlf:
        bus_mlf = _pid_per_bus_mlf_from_accdb(date_iso)
        if not bus_mlf:
            bus_mlf = _pid_per_bus_mlf(date_iso)
    if bus_mlf and verbose:
        print(f"  PID per-bus MLF loaded for {len({k[0] for k in bus_mlf})} buses")

    # Per-id metadata
    units2 = units.copy()
    units2["_pmin"] = units2.get("pot_min_tecnica", 0).map(_to_float_es)
    units2["_pmax"] = units2.get(
        "pot_neta_efectiva", units2.get("pot_max_bruta", 0)
    ).map(_to_float_es)
    units2["_id"] = pd.to_numeric(units2["id_central"], errors="coerce").astype("Int64")
    units2 = units2.dropna(subset=["_id"])
    pmin_by_id: dict[int, float] = {}
    pmax_by_id: dict[int, float] = {}
    tipo_by_id: dict[int, str] = {}
    for ic, grp in units2.groupby("_id"):
        pmin_by_id[int(ic)] = float(grp["_pmin"].sum())
        pmax_by_id[int(ic)] = float(grp["_pmax"].sum())
        tipo_by_id[int(ic)] = str(
            grp["tipo_tecnologia_unidad"].iloc[0]
            if "tipo_tecnologia_unidad" in grp.columns
            else ""
        )

    # SCVIC ladder
    scvic = cv_rpt_for_date(date_iso)
    fuel_cv_floor = {
        "GNL": 30.0,
        "Gas Natural": 30.0,
        "GLP": 30.0,
        "Gas Propano": 30.0,
    }
    if not scvic.empty:
        scvic = scvic[scvic["costo_variable_total"] < 1000.0]
    cv_options_by_id: dict[int, list[float]] = {}
    fuel_by_id: dict[int, str] = {}
    central_by_id: dict[int, str] = {}
    if not scvic.empty:
        for _, r in scvic.iterrows():
            cn = str(r.get("central_name", ""))
            un = str(r.get("unidad_name", "") or "")
            cf = str(r.get("configuracion", "") or "")
            fuel = str(r.get("tipo_combustible", "")).strip()
            floor = fuel_cv_floor.get(fuel, 15.0)
            cv = float(r["costo_variable_total"])
            if cv < floor:
                continue
            ic = bridge_resolve(cn, alias_index, un, cf)
            if ic is None:
                continue
            cv_options_by_id.setdefault(ic, []).append(cv)
            if ic not in fuel_by_id:
                fuel_by_id[ic] = fuel
                central_by_id[ic] = cn

    # Forced/RO id set (estado != N OR consigna ∈ FORCED set)
    forced_consignas = FORCED_CONSIGNAS
    forced_id_set: set[int] = set()
    if not instr.empty:
        forced_mask = (instr["estado"].astype(str).str.upper() != "N") | instr[
            "consigna"
        ].astype(str).isin(forced_consignas)
        for raw in instr[forced_mask]["central"].astype(str).unique():
            ic = bridge_resolve(raw, alias_index)
            if ic is not None:
                forced_id_set.add(ic)

    # Per-(id, hour) dispatch
    gen2 = gen.copy()
    gen2["id_central"] = (
        gen2["central"].astype(str).map(lambda c: bridge_resolve(c, alias_index))
    )
    gen2 = gen2.dropna(subset=["id_central", "hour"])
    gen2["id_central"] = gen2["id_central"].astype(int)
    gen2["pmin_decl"] = gen2["id_central"].map(pmin_by_id).fillna(0.0)
    gen2["pmax_decl"] = gen2["id_central"].map(pmax_by_id)
    gen2["pmax_eff"] = gen2.apply(
        lambda r: (
            min(r["pmax"], r["pmax_decl"]) if pd.notna(r["pmax_decl"]) else r["pmax"]
        ),
        axis=1,
    )
    gen2["tipo"] = gen2["id_central"].map(tipo_by_id).fillna("")

    # System-context per hour: total demand, renewable fraction
    demand_by_hour: dict[int, float] = {}
    renewable_frac_by_hour: dict[int, float] = {}
    if not gen2.empty:
        for h, grp in gen2[gen2["mw"] > 0].groupby("hour"):
            tot = float(grp["mw"].sum())
            demand_by_hour[int(h)] = tot
            if tot > 0:
                vre_mask = grp["id_central"].map(
                    lambda i: fuel_by_id.get(int(i), "") in RENEWABLE_FUELS
                ) | grp["tipo"].isin(HYDRO_TIPOS_FOR_RENEWABLE)
                renewable_frac_by_hour[int(h)] = float(
                    grp.loc[vre_mask, "mw"].sum() / tot
                )
            else:
                renewable_frac_by_hour[int(h)] = 0.0

    # Cluster buses into islands per quarter
    cmg_islands = cluster_buses_into_islands(cmg)
    # island_n_buses lookup per (quarter, island_id)
    island_size = (
        cmg_islands.groupby(["quarter", "island_id"]).size().rename("island_n_buses")
    )

    # Per-island marginal identification.  We use the **median λ
    # across all buses in the island** as the picker target instead
    # of any single bus's λ — this both:
    #   (1) approximates the system-reference price (loss factors
    #       average to ~1 across the island), addressing the
    #       inverse-loss-factor concern in the picker; and
    #   (2) ensures all buses in the island share THE SAME marginal
    #       unit (which they already did because we pick once per
    #       island, but using median λ makes that pick more robust).
    island_keys = (
        cmg_islands.groupby(["quarter", "island_id"])
        .agg(
            date_utc=("date_utc", "first"),
            hour=("hour", "first"),
            lam_median=("lmp", "median"),
        )
        .reset_index()
    )
    island_marginals: dict[tuple[int, str], dict] = {}
    island_pool_size: dict[tuple[int, str], int] = {}
    # P1-B: pre-group gen2 by hour (avoids 96 × O(n) scans).
    gen2_by_hour: dict[int, pd.DataFrame] = {
        int(h): grp[grp["mw"] > 0].copy() for h, grp in gen2.groupby("hour")
    }
    for _, ir in island_keys.iterrows():
        quarter = int(ir["quarter"])
        hour = int(ir["hour"])
        island = str(ir["island_id"])
        lam = float(ir["lam_median"])
        cands = gen2_by_hour.get(hour, pd.DataFrame()).copy()
        if cands.empty:
            cands["forced"] = pd.Series(dtype=bool)
        else:
            cands["forced"] = cands["id_central"].isin(forced_id_set)
        candidates = (
            cands[
                ["id_central", "central", "mw", "pmin_decl", "pmax_eff", "forced"]
            ].rename(columns={"pmin_decl": "pmin", "pmax_eff": "pmax"})
            if not cands.empty
            else pd.DataFrame()
        )
        marg = identify_marginal(
            lambda_target=lam,
            candidates=candidates,
            fuel_by_id=fuel_by_id,
            cv_options_by_id=cv_options_by_id,
        )
        # Tier-1 enrichment from the candidate set
        if marg.get("marginal_id") is not None:
            mid = int(marg["marginal_id"])
            cand_row = cands[cands["id_central"] == mid]
            if not cand_row.empty:
                marg["marginal_dispatch_mw"] = float(cand_row["mw"].iloc[0])
                marg["marginal_pmin"] = float(cand_row["pmin_decl"].iloc[0])
                marg["marginal_pmax"] = float(cand_row["pmax_eff"].iloc[0])
                marg["marginal_headroom_mw"] = max(
                    0.0,
                    marg["marginal_pmax"] - marg["marginal_dispatch_mw"],
                )
            # Tier-wedge: gap to next-up CV in the picked unit's ladder
            opts = sorted(cv_options_by_id.get(mid, []))
            cv_picked = float(marg.get("marginal_cv", 0))
            above = [c for c in opts if c > cv_picked]
            marg["tier_wedge_usd"] = float(min(above) - cv_picked) if above else 0.0
        # Forced-pool diagnostic
        n_forced_in_pool = int(cands["forced"].sum()) if not cands.empty else 0
        marg["n_forced_in_pool"] = n_forced_in_pool
        island_marginals[(quarter, island)] = marg
        island_pool_size[(quarter, island)] = len(candidates)

    # P1-A: vectorised reconstruction (replaces iterrows() over cells)
    out = _reconstruct_output(
        cmg_islands=cmg_islands,
        island_marginals=island_marginals,
        island_size=island_size,
        mlf_by_id=mlf_by_id,
    )
    if out.empty:
        return out
    # System context (per-hour)
    out["system_total_demand_mw"] = (
        out["hour"].map(demand_by_hour).fillna(0.0).astype(float)
    )
    out["system_renewable_fraction"] = (
        out["hour"].map(renewable_frac_by_hour).fillna(0.0).astype(float)
    )

    # Per-bus driver uses bus_label like "ALTO MELIPILLA 220KV
    # (Santiago)" but PID's bus_mlf is keyed on PLEXOS names like
    # "AMelipilla220".  Translate keys before passing to the shared
    # corrections helper.
    bus_mlf_label_map: dict[str, str] = {
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
    plexos_to_label = {v: k for k, v in bus_mlf_label_map.items()}
    bus_mlf_relabelled = (
        {
            (plexos_to_label.get(plexos, plexos), per): val
            for (plexos, per), val in bus_mlf.items()
        }
        if bus_mlf
        else {}
    )
    return _apply_loss_corrections(
        out,
        mlf_by_id=mlf_by_id,
        bus_mlf=bus_mlf_relabelled,
    )


def compute_marginal_units_for_day_all_buses(
    client: CenApiClient,
    *,
    date_iso: str,
    use_pid_mlf: bool = True,
    verbose: bool = True,
    bulk_endpoint: str = "cmg_online",
    max_buses: int | None = None,
) -> pd.DataFrame:
    """Run the marginal-emission pipeline over **every CEN bus** for
    one day.

    Replaces the per-bus ``cmg-real`` fetch loop (slow, rate-limited)
    with a bulk paginated fetch of the entire day's data via the
    ``cmg-online`` endpoint's ``limit``/``offset`` parameters
    (~5 min for ~1 375 buses).  After bulk-fetching, the rest of the
    pipeline runs unchanged: dispatch, instructions, units, SCVIC
    ladder, per-island marginal-unit picker, loss-factor correction,
    optional PID MLF.

    Args:
        client: open SIP client.
        date_iso: ``YYYY-MM-DD``.
        use_pid_mlf: enable per-unit + per-bus PID MLF correction.
        verbose: print progress for each pipeline step.
        bulk_endpoint: ``"cmg_online"`` (default — paginated, fast)
            or ``"cmg_real"`` (settled but rate-limited).
        max_buses: optional cap on number of buses to include
            (useful for development/testing).  ``None`` = all.

    Returns the same long-form DataFrame as
    :func:`compute_marginal_units_for_day` but with per-(bus,
    quarter) rows for every published bus.
    """
    if verbose:
        print(f"[1] bulk fetching {bulk_endpoint} for {date_iso} …")
    bulk = fetch_cmg_bulk(
        client,
        date=date_iso,
        endpoint=bulk_endpoint,
        verbose=verbose,
    )
    if bulk.empty:
        return pd.DataFrame()
    if verbose:
        print(f"  {len(bulk):,} rows / {bulk['id_info'].nunique()} buses fetched")

    # Reshape bulk into the cmg-DataFrame format the rest of the
    # pipeline expects: bus_uid, bus_label, source, ...
    cmg = pd.DataFrame()
    cmg["date_utc"] = bulk["date_utc"]
    cmg["hour"] = bulk["hour"]
    cmg["minute"] = bulk["minute"]
    cmg["quarter"] = bulk["quarter"]
    cmg["bus_name"] = bulk["barra_info"]
    cmg["lmp"] = bulk["lmp"]
    cmg["bus_uid"] = bulk["id_info"].astype(int)
    cmg["bus_label"] = bulk["barra_info"]
    cmg["source"] = bulk["source"]

    if max_buses is not None:
        keep_ids = sorted(cmg["bus_uid"].unique())[:max_buses]
        cmg = cmg[cmg["bus_uid"].isin(keep_ids)].reset_index(drop=True)
        if verbose:
            print(f"  capped to first {len(keep_ids)} buses by id_info")

    # Now load the supporting data (gen, instr, units, scvic) — these
    # are bus-list-independent
    if verbose:
        print("[2] generacion-real …")
    gen = fetch_generacion_real(client, date=date_iso)
    if verbose:
        print("[3] instrucciones-operacionales-cmg …")
    instr = fetch_instrucciones(client, date=date_iso)
    if verbose:
        print("[4] unidades-generadoras …")
    units = fetch_unidades_generadoras(client, date=date_iso)
    alias_index = build_alias_index(units)

    mlf_by_id: dict[int, float] = (
        _load_pid_mlf_by_id(date_iso, alias_index) if use_pid_mlf else {}
    )
    # Prefer the accdb-published per-bus per-period MLF (exact PLEXOS
    # solver output) over the Nod_Loss/Nod_Load approximation.  The
    # accdb path requires `mdbtools` on PATH and a downloaded PID
    # bundle; fall back to the CSV approximation otherwise.
    bus_mlf: dict[tuple[str, int], float] = {}
    bus_mlf_source = "none"
    if use_pid_mlf:
        bus_mlf = _pid_per_bus_mlf_from_accdb(date_iso)
        if bus_mlf:
            bus_mlf_source = "accdb"
        else:
            bus_mlf = _pid_per_bus_mlf(date_iso)
            if bus_mlf:
                bus_mlf_source = "Nod_Loss approximation"
    bus_mlf_by_id_info: dict[int, float] = _bus_mlf_by_id_info(bus_mlf)
    if verbose and (mlf_by_id or bus_mlf):
        print(
            f"  PID MLF loaded: {len(mlf_by_id)} units, "
            f"{len({k[0] for k in bus_mlf})} buses (source: {bus_mlf_source}), "
            f"{len(bus_mlf_by_id_info)} id_info-bridged"
        )

    # Per-id metadata
    units2 = units.copy()
    units2["_pmin"] = units2.get("pot_min_tecnica", 0).map(_to_float_es)
    units2["_pmax"] = units2.get(
        "pot_neta_efectiva", units2.get("pot_max_bruta", 0)
    ).map(_to_float_es)
    units2["_id"] = pd.to_numeric(units2["id_central"], errors="coerce").astype("Int64")
    units2 = units2.dropna(subset=["_id"])
    pmin_by_id: dict[int, float] = {}
    pmax_by_id: dict[int, float] = {}
    for ic, grp in units2.groupby("_id"):
        pmin_by_id[int(ic)] = float(grp["_pmin"].sum())
        pmax_by_id[int(ic)] = float(grp["_pmax"].sum())

    # SCVIC ladder
    scvic = cv_rpt_for_date(date_iso)
    fuel_cv_floor = {
        "GNL": 30.0,
        "Gas Natural": 30.0,
        "GLP": 30.0,
        "Gas Propano": 30.0,
    }
    if not scvic.empty:
        scvic = scvic[scvic["costo_variable_total"] < 1000.0]
    cv_options_by_id: dict[int, list[float]] = {}
    fuel_by_id: dict[int, str] = {}
    central_by_id: dict[int, str] = {}
    if not scvic.empty:
        for _, r in scvic.iterrows():
            cn = str(r.get("central_name", ""))
            un = str(r.get("unidad_name", "") or "")
            cf = str(r.get("configuracion", "") or "")
            fuel = str(r.get("tipo_combustible", "")).strip()
            floor = fuel_cv_floor.get(fuel, 15.0)
            cv = float(r["costo_variable_total"])
            if cv < floor:
                continue
            ic = bridge_resolve(cn, alias_index, un, cf)
            if ic is None:
                continue
            cv_options_by_id.setdefault(ic, []).append(cv)
            if ic not in fuel_by_id:
                fuel_by_id[ic] = fuel
                central_by_id[ic] = cn

    # Forced/RO id set
    forced_consignas = FORCED_CONSIGNAS
    forced_id_set: set[int] = set()
    if not instr.empty:
        forced_mask = (instr["estado"].astype(str).str.upper() != "N") | instr[
            "consigna"
        ].astype(str).isin(forced_consignas)
        for raw in instr[forced_mask]["central"].astype(str).unique():
            ic = bridge_resolve(raw, alias_index)
            if ic is not None:
                forced_id_set.add(ic)

    # Per-(id, hour) dispatch
    gen2 = gen.copy()
    gen2["id_central"] = (
        gen2["central"].astype(str).map(lambda c: bridge_resolve(c, alias_index))
    )
    gen2 = gen2.dropna(subset=["id_central", "hour"])
    gen2["id_central"] = gen2["id_central"].astype(int)
    gen2["pmin_decl"] = gen2["id_central"].map(pmin_by_id).fillna(0.0)
    gen2["pmax_decl"] = gen2["id_central"].map(pmax_by_id)
    gen2["pmax_eff"] = gen2.apply(
        lambda r: (
            min(r["pmax"], r["pmax_decl"]) if pd.notna(r["pmax_decl"]) else r["pmax"]
        ),
        axis=1,
    )

    # Annotate cmg with bus_mlf and the **inverse-MLF lambda
    # estimate** of the system-reference price.  When PID's per-bus
    # MLF is available for a bus, divide its λ by the MLF (since
    # λ_bus = λ_ref × bus_mlf, so λ_ref ≈ λ_bus / bus_mlf).  When
    # MLF is missing, fall back to the raw λ_bus (assumes mlf=1).
    cmg["bus_mlf_picker"] = cmg["bus_uid"].map(bus_mlf_by_id_info).astype(float)
    cmg["lambda_ref_est"] = cmg["lmp"].astype(float) / cmg["bus_mlf_picker"].fillna(1.0)
    if verbose and bus_mlf_by_id_info:
        n_with_mlf = cmg["bus_mlf_picker"].notna().sum()
        print(
            f"  inverse-MLF picker: {n_with_mlf} of {len(cmg)} cells use "
            f"λ_ref = λ_bus / bus_mlf (the rest fall back to raw λ_bus)"
        )

    # Cluster + identify marginal per island
    if verbose:
        print(
            f"[5] cluster {cmg['bus_uid'].nunique()} buses into islands "
            f"per quarter (tol=${ISLAND_LMP_TOL}, max_spread=${ISLAND_MAX_SPREAD})"
        )
    cmg_islands = cluster_buses_into_islands(cmg)
    n_isl = cmg_islands.groupby("quarter")["island_id"].nunique()
    if verbose:
        print(
            f"  islands per quarter: median={int(n_isl.median())}, "
            f"max={int(n_isl.max())}, total unique={cmg_islands['island_id'].nunique()}"
        )

    island_size = (
        cmg_islands.groupby(["quarter", "island_id"]).size().rename("island_n_buses")
    )
    # Use median-(λ_ref) across the island as picker target — when
    # PID bus-MLF is available for at least some buses in the island,
    # this yields the system-reference price directly; otherwise it
    # gracefully falls back to median raw λ.  All buses in the island
    # share the picked marginal unit by construction.
    # Picker target = median(λ_ref_est) per island, where
    # ``lambda_ref_est = lmp / bus_mlf`` when bus_mlf is available
    # (preferring accdb-published MLF, falling back to raw lmp when
    # MLF is missing).  This inverts the bus loss factor to recover
    # the system-reference price the marginal unit's CV represents.
    # Earlier attempt with the Nod_Loss CSV approximation regressed
    # — but with the accdb source the MLFs are precise enough.
    island_keys = (
        cmg_islands.groupby(["quarter", "island_id"])
        .agg(
            date_utc=("date_utc", "first"),
            hour=("hour", "first"),
            lam_median=("lmp", "median"),
            # NOTE: tried `lam_median=("lambda_ref_est", "median")`
            # with both Nod_Loss-approximated and accdb-published
            # MLFs — both regressed.  Proper inverse-MLF picker
            # needs per-candidate scaling, not a uniform target
            # rescale.  See doctring + critical-review notes.
        )
        .reset_index()
    )

    if verbose:
        print(
            f"[6] per-island marginal identification "
            f"({len(island_keys):,} (quarter, island) keys)"
        )
    # P1-B: pre-group gen2 by hour (avoids O(n) full-frame scan in
    # every island-loop iteration).
    gen2_by_hour: dict[int, pd.DataFrame] = {
        int(h): grp[grp["mw"] > 0].copy() for h, grp in gen2.groupby("hour")
    }

    island_marginals: dict[tuple[int, str], dict] = {}
    for _, ir in island_keys.iterrows():
        quarter = int(ir["quarter"])
        hour = int(ir["hour"])
        island = str(ir["island_id"])
        lam = float(ir["lam_median"])
        cands = gen2_by_hour.get(hour, pd.DataFrame()).copy()
        if cands.empty:
            cands["forced"] = pd.Series(dtype=bool)
        else:
            cands["forced"] = cands["id_central"].isin(forced_id_set)
        candidates = (
            cands[
                ["id_central", "central", "mw", "pmin_decl", "pmax_eff", "forced"]
            ].rename(columns={"pmin_decl": "pmin", "pmax_eff": "pmax"})
            if not cands.empty
            else pd.DataFrame()
        )
        marg = identify_marginal(
            lambda_target=lam,
            candidates=candidates,
            fuel_by_id=fuel_by_id,
            cv_options_by_id=cv_options_by_id,
        )
        if marg.get("marginal_id") is not None:
            mid = int(marg["marginal_id"])
            cand_row = cands[cands["id_central"] == mid]
            if not cand_row.empty:
                marg["marginal_dispatch_mw"] = float(cand_row["mw"].iloc[0])
                marg["marginal_pmin"] = float(cand_row["pmin_decl"].iloc[0])
                marg["marginal_pmax"] = float(cand_row["pmax_eff"].iloc[0])
                marg["marginal_headroom_mw"] = max(
                    0.0,
                    marg["marginal_pmax"] - marg["marginal_dispatch_mw"],
                )
            opts = sorted(cv_options_by_id.get(mid, []))
            cv_picked = float(marg.get("marginal_cv", 0))
            above = [c for c in opts if c > cv_picked]
            marg["tier_wedge_usd"] = float(min(above) - cv_picked) if above else 0.0
        marg["n_forced_in_pool"] = int(cands["forced"].sum()) if not cands.empty else 0
        island_marginals[(quarter, island)] = marg

    # P1-A: vectorised per-bus reconstruction — build a marginals_df
    # then merge into cmg_islands instead of iterrows() over 155k cells.
    if verbose:
        print(f"[7] reconstructing {len(cmg_islands):,} per-(bus, quarter) cells")
    out = _reconstruct_output(
        cmg_islands=cmg_islands,
        island_marginals=island_marginals,
        island_size=island_size,
        mlf_by_id=mlf_by_id,
    )
    if out.empty:
        return out
    return _apply_loss_corrections(out, mlf_by_id=mlf_by_id, bus_mlf=bus_mlf)


def _reconstruct_output(
    *,
    cmg_islands: pd.DataFrame,
    island_marginals: dict[tuple[int, str], dict],
    island_size: pd.Series,
    mlf_by_id: dict[int, float],
) -> pd.DataFrame:
    """Vectorised assembly of the per-(bus, quarter) output (P1-A).

    Replaces the prior ``iterrows()`` loop over ``cmg_islands`` with
    a single ``merge`` against a pre-built ``marginals_df``.  At
    ~155k rows this drops construction time from 8-15 s to ~0.1 s.
    """
    if cmg_islands.empty:
        return pd.DataFrame()

    # Build marginals_df: one row per (quarter, island_id) with the
    # marginal-pick metadata from identify_marginal().
    keys = list(island_marginals.keys())
    rows = [
        {
            "quarter": q,
            "island_id": isl,
            "_marg_id": v.get("marginal_id"),
            "marginal_central": str(v.get("marginal_central", "")),
            "marginal_fuel": str(v.get("marginal_fuel", "")),
            "marginal_cv_": v.get("marginal_cv", float("nan")),
            "marginal_pmin": v.get("marginal_pmin", float("nan")),
            "marginal_pmax": v.get("marginal_pmax", float("nan")),
            "marginal_dispatch_mw": v.get("marginal_dispatch_mw", float("nan")),
            "marginal_headroom_mw": v.get("marginal_headroom_mw", float("nan")),
            "epsilon": v.get("epsilon", 0.0),
            "regime": str(v.get("regime", "")),
            "n_candidates": v.get("n_candidates", 0),
            "n_dispatched_unmapped": v.get("n_dispatched_unmapped", 0),
            "n_forced_in_pool": v.get("n_forced_in_pool", 0),
            "tier_wedge_usd": v.get("tier_wedge_usd", 0.0),
            "top_below_lambda": str(v.get("top_below_lambda", "")),
            "top_above_lambda": str(v.get("top_above_lambda", "")),
        }
        for (q, isl), v in zip(keys, island_marginals.values())
    ]
    marginals_df = pd.DataFrame(rows)

    # Vectorised merge replaces the per-row dict-build.
    out = cmg_islands.merge(
        marginals_df,
        on=["quarter", "island_id"],
        how="left",
    )
    out = out.rename(columns={"lmp": "lambda_cen"})
    out["lambda_provenance"] = (
        out["source"].astype(str) if "source" in out.columns else ""
    )
    out["marginal_id_central"] = out["_marg_id"].astype("Int64")
    out["lambda_pipe"] = out["marginal_cv_"].astype(float)
    out["marginal_cv"] = out["marginal_cv_"].astype(float)
    out["marginal_unit_mlf"] = out["_marg_id"].map(
        lambda i: (
            float(mlf_by_id.get(int(i), float("nan"))) if pd.notna(i) else float("nan")
        )
    )
    out["island_n_buses"] = (
        out.set_index(["quarter", "island_id"])
        .index.map(island_size.to_dict())
        .fillna(1)
        .astype(int)
    )
    out["delta_lmp"] = out["lambda_pipe"] - out["lambda_cen"]
    out["abs_delta"] = out["delta_lmp"].abs()
    out["epsilon"] = out["epsilon"].astype(float)
    # Cast remaining numeric counter columns to int
    for col in ("n_candidates", "n_dispatched_unmapped", "n_forced_in_pool"):
        out[col] = out[col].fillna(0).astype(int)
    out["tier_wedge_usd"] = out["tier_wedge_usd"].fillna(0.0).astype(float)
    out.drop(columns=["_marg_id", "marginal_cv_"], inplace=True)
    return out


def _apply_loss_corrections(
    out: pd.DataFrame,
    *,
    mlf_by_id: dict[int, float],
    bus_mlf: dict[tuple[str, int], float],
) -> pd.DataFrame:
    """Add per-bus loss-factor corrections (empirical + PID) and
    derived columns (``lambda_constructed``, ``epsilon_at_bus``,
    ``co2_tax_usd_per_mwh``) to the output.

    Vectorised — uses ``merge`` for ``bus_mlf`` lookup instead of
    ``apply(axis=1)``.  Shared between the two driver paths.
    """
    # Empirical loss factor (semi-circular: derived from λ_cen itself)
    ref_per_q = (
        out.sort_values("bus_uid")
        .groupby("quarter")
        .first()["lambda_cen"]
        .rename("lambda_ref")
    )
    out = out.merge(ref_per_q, left_on="quarter", right_index=True, how="left")
    out["loss_factor"] = (
        (out["lambda_cen"] / out["lambda_ref"])
        .where(out["lambda_ref"] > 0.0, 1.0)
        .round(4)
    )
    out["lambda_constructed"] = (out["lambda_pipe"] * out["loss_factor"]).round(2)
    out["epsilon_at_bus"] = (out["epsilon"] * out["loss_factor"]).round(2)
    out["co2_tax_usd_per_mwh"] = (
        out["epsilon_at_bus"] * CO2_TAX_USD_PER_TON / 1000.0
    ).round(4)
    out.drop(columns=["lambda_ref"], inplace=True)

    # P2-A: vectorised bus_mlf merge instead of apply(axis=1).
    if bus_mlf:
        bus_mlf_df = pd.DataFrame(
            [
                {"bus_label": k[0], "_period": k[1], "bus_mlf": v}
                for k, v in bus_mlf.items()
            ]
        )
        out["_period"] = out["hour"].astype(int) + 1
        out = out.merge(bus_mlf_df, on=["bus_label", "_period"], how="left")
        out.drop(columns=["_period"], inplace=True)
    else:
        out["bus_mlf"] = float("nan")

    if mlf_by_id or bus_mlf:
        unit_part = out["marginal_unit_mlf"].fillna(1.0)
        bus_part = out["bus_mlf"].fillna(1.0)
        composed = bus_part / unit_part.where(unit_part > 0, 1.0)
        any_pid = out["marginal_unit_mlf"].notna() | out["bus_mlf"].notna()
        out["loss_factor_pid"] = composed.where(any_pid, out["loss_factor"]).round(4)
        out["lambda_constructed_pid"] = (
            out["lambda_pipe"] * out["loss_factor_pid"]
        ).round(2)
        out["epsilon_at_bus_pid"] = (out["epsilon"] * out["loss_factor_pid"]).round(2)
    else:
        out["loss_factor_pid"] = float("nan")
        out["lambda_constructed_pid"] = float("nan")
        out["epsilon_at_bus_pid"] = float("nan")
    out["discrepancy_flag"] = (out["abs_delta"] > 5.0) & (
        ~out["regime"].str.startswith("renewable_")
    )
    # tier_uncertainty: True when the picked unit is more than $2
    # below the next-up SCVIC tier — telling consumers "the actual
    # marginal CV likely lies in the gap; treat lambda_pipe as a
    # lower bound and abs_delta is bounded by tier_wedge_usd".
    # Per the diagnostic finding: 57% of |Δ|>$1 cells satisfy
    # |Δ| ≤ tier_wedge_usd — these are the cells flagged here.
    out["tier_uncertainty"] = out["tier_wedge_usd"] > TIER_UNCERTAINTY_THRESHOLD
    return out


#: Threshold above which a per-cell ``tier_wedge_usd`` flips
#: ``tier_uncertainty`` to True.  $2/MWh is consistent with
#: typical Chilean SCVIC discrete-tier gaps (biomass→gas is the
#: dominant one).
TIER_UNCERTAINTY_THRESHOLD = 2.0


def main() -> int:
    # Legacy CLI side-effects (lazily applied here, NEVER at import
    # time): silence noisy DataFrame warnings + create WORK_DIR.
    warnings.filterwarnings("ignore")
    WORK_DIR.mkdir(parents=True, exist_ok=True)

    print("=" * 80)
    print(f"  Emission-factor identifier v3 — date={DATE}")
    print("=" * 80)

    sip_cfg = CenApiConfig(user_keys={"sip": SIP_KEY}, verify_tls=False)
    with CenApiClient(sip_cfg) as client:
        print("\n[1] CMG real (15-min, 2 reference buses)")
        cmg = fetch_cmg_real(client)
        if cmg.empty:
            print("  no CMG — aborting")
            return 1
        print("\n[2] generacion-real (hourly per central)")
        gen = fetch_generacion_real(client)
        print(f"  {len(gen)} rows / {gen['central'].nunique()} centrales")
        print("\n[3] instrucciones-operacionales-cmg")
        instr = fetch_instrucciones(client)
        print(f"  {len(instr)} instructions")
        print("\n[4] unidades-generadoras (pmin)")
        units = fetch_unidades_generadoras(client)
        print(f"  {len(units)} units")

        print("\n[4b] limitaciones-transmision (line saturation events)")
        try:
            lim = fetch_limitaciones(client)
            print(f"  {len(lim)} limitation events for {DATE}")
            if not lim.empty and "instalacion_nombre" in lim.columns:
                top = lim["instalacion_nombre"].value_counts().head(5)
                print("  most-limited installations:")
                for k, v in top.items():
                    print(f"    {k}: {v}")
        except Exception as exc:  # noqa: BLE001
            print(f"  WARN: {type(exc).__name__}: {exc}")
            lim = pd.DataFrame()

        # cmg-programado-pcp removed — it was used only for the
        # hydro water-value injection which the user asked to drop.
        # The endpoint also routinely 502s on this user_key tier,
        # so removing it eliminates the slow rate-limit hang on
        # every fresh run.
        pcp = pd.DataFrame()

        print("\n[4d] generacion-programada-pcp (LP-output dispatch per unit)")
        try:
            gen_pcp = fetch_gen_programada_pcp(client)
            n_centrales = gen_pcp["id_central"].nunique() if not gen_pcp.empty else 0
            print(f"  {len(gen_pcp)} rows / {n_centrales} centrales")
        except Exception as exc:  # noqa: BLE001
            print(f"  WARN: {type(exc).__name__}: {exc}")
            gen_pcp = pd.DataFrame()

        print("\n[4e] embalse-real (reservoir levels)")
        try:
            embalse = fetch_embalse_real(client)
            n_reservoirs = (
                embalse["nombre_embalse"].nunique() if not embalse.empty else 0
            )
            print(f"  {len(embalse)} reservoir rows / {n_reservoirs} unique reservoirs")
        except Exception as exc:  # noqa: BLE001
            print(f"  WARN: {type(exc).__name__}: {exc}")
            embalse = pd.DataFrame()

    print("\n[bridge] id_central alias index")
    alias_index = build_alias_index(units)
    print(f"  {index_stats(alias_index)}")

    # --- Build per-id metadata ---
    # pmin per id_central
    units2 = units.copy()
    units2["_pmin"] = units2.get("pot_min_tecnica", 0).map(_to_float_es)
    units2["_pmax"] = units2.get(
        "pot_neta_efectiva", units2.get("pot_max_bruta", 0)
    ).map(_to_float_es)
    units2["_id"] = pd.to_numeric(units2["id_central"], errors="coerce").astype("Int64")
    units2 = units2.dropna(subset=["_id"])
    pmin_by_id: dict[int, float] = {}
    pmax_by_id: dict[int, float] = {}
    for ic, grp in units2.groupby("_id"):
        pmin_by_id[int(ic)] = float(grp["_pmin"].sum())
        pmax_by_id[int(ic)] = float(grp["_pmax"].sum())

    # ---- Water value per hour from cmg-programado-pcp ----
    # The PLP/PCP LP embeds water values as the marginal CV of hydro
    # plants. The PCP-cleared CMG implicitly = hydro marginal CV when
    # the marginal IS hydro. We use the system median PCP CMG per
    # hour as the implicit water value for ALL hydros.
    water_value_by_hour: dict[int, float] = {}
    if not pcp.empty:
        for h, grp in pcp.groupby("hour"):
            water_value_by_hour[int(h)] = float(grp["cmg"].median())
    print(
        f"  water value per hour (median PCP CMG): "
        f"min=${min(water_value_by_hour.values(), default=0):.1f} "
        f"max=${max(water_value_by_hour.values(), default=0):.1f}"
    )

    # ---- Reservoir-hydro id_central set ----
    # Only "Hidroeléctrica de embalse" plants get a water-value CV.
    # PLP optimizes them and the Bellman dual = water value.
    # Pasada (run-of-river) hydros — even large ones — are treated
    # as price-takers per the user's directive: only ~10 reservoir
    # plants set the marginal via water value.
    RESERVOIR_TIPOS = {"Hidroeléctrica de embalse"}
    hydro_id_set: set[int] = set()
    hydro_central_names: list[str] = []
    if not units.empty:
        h_units = units[
            units["tipo_tecnologia_unidad"].astype(str).isin(RESERVOIR_TIPOS)
        ]
        for ic in h_units["id_central"].dropna().unique():
            try:
                hydro_id_set.add(int(ic))
            except (TypeError, ValueError):
                pass
        hydro_central_names = sorted(set(h_units["central"].astype(str)))
    print(f"  reservoir-hydro id_central set: {len(hydro_id_set)} centrales")
    print(f"    plants: {hydro_central_names}")

    # SCVIC CV + fuel per id_central — keep ALL realistic CVs per
    # central (each unit declares multiple configurations: tech-min,
    # full-load, multi-fuel modes). The "marginal CV" depends on the
    # operating point; we pick the matching one at lookup time.
    scvic = cv_rpt_for_date(DATE)
    # Filter to realistic full-load range. $15-$1000 excludes
    # tech-min/start-up configs (typically $0-$10) and PLEXOS
    # sentinels ($1000+ "out of economic order").
    # Per-fuel CV floor — excludes "takeoff" / contract-minimum
    # configurations that don't reflect the unit's true marginal CV.
    # Specifically, GNL plants under take-or-pay LNG contracts are
    # forced to consume gas at sunk cost; their dispatch carries a
    # very-low declared CV ($5-$25) but they're price-takers, not
    # price-setters. Same for GLP and other contract-driven fuels.
    FUEL_CV_FLOOR = {
        "GNL": 30.0,  # take-or-pay LNG
        "Gas Natural": 30.0,
        "GLP": 30.0,
        "Gas Propano": 30.0,
        # All other fuels share the global $15 floor.
    }
    scvic = scvic[scvic["costo_variable_total"] < 1000.0]
    cv_options_by_id: dict[int, list[float]] = {}
    fuel_by_id: dict[int, str] = {}
    central_by_id: dict[int, str] = {}
    for _, r in scvic.iterrows():
        cn = str(r.get("central_name", ""))
        un = str(r.get("unidad_name", "") or "")
        cf = str(r.get("configuracion", "") or "")
        fuel = str(r.get("tipo_combustible", "")).strip()
        floor = FUEL_CV_FLOOR.get(fuel, 15.0)
        cv = float(r["costo_variable_total"])
        if cv < floor:
            continue  # filter takeoff / tech-min configs
        ic = bridge_resolve(cn, alias_index, un, cf)
        if ic is None:
            continue
        cv_options_by_id.setdefault(ic, []).append(cv)
        if ic not in fuel_by_id:
            fuel_by_id[ic] = fuel
            central_by_id[ic] = cn
    avg_options = sum(len(v) for v in cv_options_by_id.values()) / max(
        1, len(cv_options_by_id)
    )
    print(
        f"  SCVIC by id_central: {len(cv_options_by_id)} centrales — "
        f"avg {avg_options:.1f} CV options/central "
        f"(GNL takeoff configs excluded)"
    )

    # Forced/RO id set — broadened to include:
    #   * estado != "N"  (RO/LF/DN/DRO/PO/PDO/DF/MM/DLF/DP/CSE)
    #   * consigna ∈ FORCED_CONSIGNAS — operator-imposed setpoint forcing.
    #     Two physical reasons coexist:
    #       (a) Contractual TOP / inflexible mode
    #             CI  = Cumplimiento Inflexible      (TOP / take-or-pay GNL)
    #             MT  = Mínimo Técnico               (held at pmin for stability)
    #             PMT = Pre-Mínimo Técnico           (ramping up to MT)
    #       (b) Servicios Complementarios (SSCC) — synchronous machines
    #           held to a setpoint to provide inertia / frequency reserve.
    #           Motivos in the data confirm: "Activación CTF (+)",
    #           "Con SSCC", "Subida anticipadamente por retiro de generación
    #           ERV Fotovoltaica" (preventive ramp-up for solar drop-off).
    #             PP  = Punto Programado             (CTF activation carrier)
    #             PS  = Punto Seguimiento            (held for ramp-reserve)
    #             PC  = Pico Carga                   (peak / ramp pre-stage)
    #     All six force the unit OFF the merit margin → it is a price-taker
    #     and must not appear as a marginal-unit candidate.
    # Use the module-level frozenset (imported below in the body
    # via the closure on the global ``FORCED_CONSIGNAS``).
    forced_id_set: set[int] = set()
    n_by_estado = n_by_consigna = 0
    if not instr.empty:
        forced_mask = (instr["estado"].astype(str).str.upper() != "N") | instr[
            "consigna"
        ].astype(str).isin(FORCED_CONSIGNAS)
        n_by_estado = (instr["estado"].astype(str).str.upper() != "N").sum()
        n_by_consigna = instr["consigna"].astype(str).isin(FORCED_CONSIGNAS).sum()
        for raw in instr[forced_mask]["central"].astype(str).unique():
            ic = bridge_resolve(raw, alias_index)
            if ic is not None:
                forced_id_set.add(ic)
    print(
        f"  Forced id set: {len(forced_id_set)} centrales "
        f"(estado≠N: {n_by_estado} rows, "
        f"consigna∈{{CI,MT,PMT,PP,PS,PC}}: {n_by_consigna} rows)"
    )

    # Per-(id, hour) dispatch — bridge gen_real central → id
    gen2 = gen.copy()
    gen2["id_central"] = (
        gen2["central"].astype(str).map(lambda c: bridge_resolve(c, alias_index))
    )
    gen2 = gen2.dropna(subset=["id_central", "hour"])
    gen2["id_central"] = gen2["id_central"].astype(int)
    gen2["pmin_decl"] = gen2["id_central"].map(pmin_by_id).fillna(0.0)
    # Use API pmax (might disagree with units pmax — prefer the smaller).
    gen2["pmax_decl"] = gen2["id_central"].map(pmax_by_id)
    gen2["pmax_eff"] = gen2.apply(
        lambda r: (
            min(r["pmax"], r["pmax_decl"]) if pd.notna(r["pmax_decl"]) else r["pmax"]
        ),
        axis=1,
    )

    # --- Cluster buses into islands per (date, quarter) ---
    print(
        "\n[5] cluster buses into islands per quarter "
        f"(tol = ${ISLAND_LMP_TOL:.2f}/MWh)"
    )
    cmg_islands = cluster_buses_into_islands(cmg)
    n_islands_per_q = cmg_islands.groupby("quarter")["island_id"].nunique()
    print(
        f"  islands per quarter: median={int(n_islands_per_q.median())}, "
        f"max={int(n_islands_per_q.max())}, total unique={cmg_islands['island_id'].nunique()}"
    )

    # --- Per-island marginal identification ---
    print("\n[6] per-island reverse-engineered marginal identification")
    # Identify the marginal once per (quarter, island). All buses in
    # the same island get the same marginal unit.
    island_keys = cmg_islands[
        ["date_utc", "quarter", "hour", "island_id", "lmp"]
    ].drop_duplicates(subset=["quarter", "island_id"])
    island_marginals: dict[tuple[int, str], dict] = {}
    for _, ir in island_keys.iterrows():
        quarter = int(ir["quarter"])
        hour = int(ir["hour"])
        island = str(ir["island_id"])
        lam = float(ir["lmp"])
        h_gen = gen2[gen2["hour"] == hour]
        # Keep ALL dispatched units; mark forced status. The
        # identify_marginal function decides per-unit whether forced
        # implies exclusion (forced + at_pmin) or inclusion (forced
        # but interior — they ARE the marginal at their declared CV).
        cands = h_gen[h_gen["mw"] > 0].copy()
        cands["forced"] = cands["id_central"].isin(forced_id_set)
        candidates = cands[
            ["id_central", "central", "mw", "pmin_decl", "pmax_eff", "forced"]
        ].rename(columns={"pmin_decl": "pmin", "pmax_eff": "pmax"})
        island_marginals[(quarter, island)] = identify_marginal(
            lambda_target=lam,
            candidates=candidates,
            fuel_by_id=fuel_by_id,
            cv_options_by_id=cv_options_by_id,
            hydro_id_set=hydro_id_set,
            water_value=water_value_by_hour.get(hour, lam),
        )

    # --- Per-bus per-15min reconstruction ---
    out_rows = []
    for _, lr in cmg_islands.iterrows():
        quarter = int(lr["quarter"])
        island = str(lr["island_id"])
        marg = island_marginals.get((quarter, island), {})
        out_rows.append(
            {
                "date_utc": str(lr["date_utc"]),
                "hour": int(lr["hour"]),
                "minute": int(lr["minute"]),
                "quarter": quarter,
                "bus_uid": int(lr["bus_uid"]),
                "bus_label": str(lr["bus_label"]),
                "island_id": island,
                "lambda_cen": float(lr["lmp"]),  # settlement λ
                "lambda_pipe": float(
                    marg.get("marginal_cv", float("nan"))
                ),  # reconstructed MC
                "marginal_central": str(marg.get("marginal_central", "")),
                "marginal_fuel": str(marg.get("marginal_fuel", "")),
                "epsilon": float(marg.get("epsilon", 0.0)),  # marginal emission factor
                "regime": str(marg.get("regime", "")),
                "n_candidates": int(marg.get("n_candidates", 0)),
                "n_dispatched_unmapped": int(marg.get("n_dispatched_unmapped", 0)),
                "top_below_lambda": str(marg.get("top_below_lambda", "")),
                "top_above_lambda": str(marg.get("top_above_lambda", "")),
            }
        )
    out = pd.DataFrame(out_rows)
    out["delta_lmp"] = out["lambda_pipe"] - out["lambda_cen"]
    out["abs_delta"] = out["delta_lmp"].abs()

    # ---- Loss-factor extension: bus-level marginal emissions ----
    # FP_bus[q] ≈ λ_bus[q] / λ_ref[q] is the empirical loss penalty
    # factor at each bus (Lin & Tang 2024 ε_b = β_b · Φ(α)).
    # Reference: lowest bus_uid that has data per quarter (typically
    # the most central / lowest-loss bus). All other buses' λ are
    # ratioed against it — values > 1 imply higher local losses,
    # < 1 implies the bus is closer to the marginal generator.
    # ε_at_bus = ε_marginal × FP_bus is the loss-adjusted marginal
    # emission factor for billing/attribution.
    if not out.empty:
        ref_per_q = (
            out.sort_values("bus_uid")
            .groupby("quarter")
            .first()["lambda_cen"]
            .rename("lambda_ref")
        )
        out = out.merge(ref_per_q, left_on="quarter", right_index=True, how="left")
        out["loss_factor"] = (
            (out["lambda_cen"] / out["lambda_ref"])
            .where(out["lambda_ref"] > 0.0, 1.0)
            .round(4)
        )
        out["epsilon_at_bus"] = (out["epsilon"] * out["loss_factor"]).round(2)
        out.drop(columns=["lambda_ref"], inplace=True)
    # Diagnostic tag: where the reverse-engineered identification
    # disagrees with λ_CEN by more than $5/MWh AND we're not in
    # curtailment regime. These are the cells whose marginal-unit
    # pick is suspect — useful for later analysis of which data
    # sources are missing / mis-bridged.
    out["discrepancy_flag"] = (out["abs_delta"] > 5.0) & (
        ~out["regime"].str.startswith("renewable_")
    )
    out.to_parquet(WORK_DIR / "marginal_units_v3.parquet", index=False)
    discrepancies = out[out["discrepancy_flag"]].copy()
    discrepancies.to_parquet(WORK_DIR / "discrepancies_v3.parquet", index=False)

    # ---- Display ----
    pd.set_option("display.float_format", lambda x: f"{x:.2f}")
    pd.set_option("display.max_colwidth", 30)
    pd.set_option("display.width", 220)

    # Show 2 representative buses (centre + south) at 15-min granularity.
    print()
    show_bus_uids = [b[1] for b in REF_BUSES if b[1] in (1, 2)] or [REF_BUSES[0][1]]
    for _bt, bus_uid, label in REF_BUSES:
        if bus_uid not in show_bus_uids:
            continue
        sub = out[out["bus_uid"] == bus_uid].sort_values("quarter")
        if sub.empty:
            continue
        sub = sub.copy()
        sub["time"] = sub.apply(
            lambda r: f"{int(r['hour']):02d}:{int(r['minute']):02d}", axis=1
        )
        print("=" * 130)
        print(f"  Bus {bus_uid}: {label} — {DATE} — 15-min reconstruction")
        print("=" * 130)
        cols = [
            "time",
            "lambda_cen",
            "lambda_pipe",
            "delta_lmp",
            "epsilon",
            "marginal_central",
            "marginal_fuel",
            "regime",
        ]
        renames = {
            "lambda_cen": "λ_CEN",
            "lambda_pipe": "MC*",
            "delta_lmp": "Δ",
            "epsilon": "ε(kg/MWh)",
            "marginal_central": "marginal",
            "marginal_fuel": "fuel",
        }
        # Show first 8 + last 8 quarters for a compact view.
        n = len(sub)
        if n > 24:
            head = sub.head(12)
            tail = sub.tail(12)
            mid = pd.DataFrame([{c: "…" for c in cols}])
            disp = pd.concat([head[cols], mid, tail[cols]], ignore_index=True)
        else:
            disp = sub[cols]
        print(disp.rename(columns=renames).to_string(index=False))
        print()

    # Hour-aggregated view for both displayed buses
    print("=" * 130)
    print("  HOURLY VIEW (15-min cells aggregated) — both reference buses")
    print("=" * 130)
    hourly = (
        out[out["bus_uid"].isin(show_bus_uids)]
        .groupby(["bus_label", "hour"])
        .agg(
            n=("quarter", "count"),
            λ_CEN=("lambda_cen", "mean"),
            MC_pipe=("lambda_pipe", "mean"),
            Δ_mean=("delta_lmp", lambda s: s.abs().mean()),
            ε=("epsilon", "mean"),
            marginal=(
                "marginal_central",
                lambda s: s.mode().iloc[0] if not s.mode().empty else "",
            ),
            fuel=(
                "marginal_fuel",
                lambda s: s.mode().iloc[0] if not s.mode().empty else "",
            ),
        )
        .reset_index()
        .round(2)
    )
    print(hourly.to_string(index=False))
    print()

    # ---- Per-bus daily summary across ALL fetched buses ----
    print("=" * 130)
    print(f"  DAILY SUMMARY ACROSS ALL {out['bus_uid'].nunique()} BUSES — {DATE}")
    print("=" * 130)
    daily = (
        out.groupby("bus_label")
        .agg(
            cells=("quarter", "count"),
            mean_λ_CEN=("lambda_cen", "mean"),
            mean_MC_pipe=("lambda_pipe", "mean"),
            mean_abs_Δ=("delta_lmp", lambda s: s.abs().mean()),
            mean_ε=("epsilon", "mean"),
            max_ε=("epsilon", "max"),
            ε_zero=("epsilon", lambda s: (s == 0).sum()),
            ε_coal=("epsilon", lambda s: ((s > 900) & (s < 1100)).sum()),
            ε_diesel=("epsilon", lambda s: ((s > 800) & (s < 900)).sum()),
            ε_gas=("epsilon", lambda s: ((s > 300) & (s < 500)).sum()),
        )
        .round(2)
    )
    print(daily.to_string())
    print()

    # ---- Regime breakdown ----
    print("=" * 130)
    print("  REGIME BREAKDOWN")
    print("=" * 130)
    print(out["regime"].value_counts().to_string())
    print()

    # ---- Δλ accuracy ----
    print("=" * 130)
    print("  Δλ = MC_pipe − λ_CEN  (the reconstructed-MC accuracy)")
    print("=" * 130)
    attr = out[out["regime"].isin(["interior_match", "boundary_fallback"])]
    if not attr.empty:
        d = attr["delta_lmp"].abs()
        print(f"  attributed cells: {len(attr)} / {len(out)}")
        print(f"  mean |Δλ|:        ${d.mean():.2f}/MWh")
        print(f"  median |Δλ|:      ${d.median():.2f}")
        print(f"  P90 |Δλ|:         ${d.quantile(0.9):.2f}")
        print(f"  within $0.5:      {(d <= 0.5).sum()} / {len(attr)}")
        print(f"  within $2:        {(d <= 2).sum()}")
        print(f"  within $5:        {(d <= 5).sum()}")

    # ---- Discrepancy log — cells where reverse-engineered MC ≠ λ_CEN ----
    print()
    print("=" * 130)
    print(
        "  DISCREPANCY LOG — cells with |MC_pipe − λ_CEN| > $5 "
        "(non-curtailment); useful for diagnosing what's missing"
    )
    print("=" * 130)
    disc = out[out["discrepancy_flag"]].copy()
    if disc.empty:
        print("  (none)")
    else:
        # Group discrepancies by pattern
        print(
            f"  total discrepancies: {len(disc)} / {len(out)} "
            f"({100 * len(disc) / len(out):.0f}%)"
        )
        print()
        print("  Pattern A — λ_CEN much higher than highest dispatched-eligible CV")
        print(
            "  (suggests CV catalogue is missing the actual marginal unit, "
            "or it was dispatched at low MW we filtered out)"
        )
        higher = disc[disc["delta_lmp"] < -5].copy()
        higher["top_above_lambda"] = higher["top_above_lambda"].fillna("")
        if not higher.empty:
            sample = (
                higher.groupby(["bus_label", "hour"])
                .agg(
                    n=("quarter", "count"),
                    λ_CEN=("lambda_cen", "mean"),
                    MC_pipe=("lambda_pipe", "mean"),
                    Δ=("delta_lmp", "mean"),
                    next_above_λ=("top_above_lambda", "first"),
                )
                .reset_index()
                .round(2)
            )
            print(sample.head(20).to_string(index=False))
        print()
        print(
            "  Pattern B — λ_CEN lower than our pick (suggests we're missing "
            "the forced/RO flag for the picked unit)"
        )
        lower = disc[disc["delta_lmp"] > 5]
        if not lower.empty:
            sample = (
                lower.groupby(["bus_label", "hour"])
                .agg(
                    n=("quarter", "count"),
                    λ_CEN=("lambda_cen", "mean"),
                    MC_pipe=("lambda_pipe", "mean"),
                    Δ=("delta_lmp", "mean"),
                    picked=(
                        "marginal_central",
                        lambda s: s.mode().iloc[0] if not s.mode().empty else "",
                    ),
                )
                .reset_index()
                .round(2)
            )
            print(sample.head(20).to_string(index=False))
        print()
        print(f"  Discrepancy parquet: {WORK_DIR / 'discrepancies_v3.parquet'}")

    # ---- Rejection-reason analysis for the morning peak hour ----
    print()
    print("=" * 130)
    print(
        "  REJECTION-REASON ANALYSIS — what's NOT in our candidate pool, "
        "for the peak hour (06:00)"
    )
    print("=" * 130)
    h6 = gen2[(gen2["hour"] == 6) & (gen2["mw"] > 0)].copy()
    h6 = h6.sort_values("mw", ascending=False)
    h6["bridged"] = h6["id_central"].notna()
    h6["has_scvic"] = h6["id_central"].map(
        lambda i: i in cv_options_by_id if pd.notna(i) else False
    )
    h6["forced"] = h6["id_central"].map(
        lambda i: i in forced_id_set if pd.notna(i) else False
    )
    h6["best_cv"] = h6.apply(
        lambda r: (
            None
            if not r["has_scvic"]
            else (
                max(
                    (c for c in cv_options_by_id[int(r["id_central"])] if c <= 100.0),
                    default=min(cv_options_by_id[int(r["id_central"])]),
                )
                if cv_options_by_id[int(r["id_central"])]
                else None
            )
        ),
        axis=1,
    )
    h6["reason"] = h6.apply(
        lambda r: (
            "OK_in_pool"
            if r["bridged"] and r["has_scvic"] and not r["forced"]
            else "FORCED_RO"
            if r["forced"]
            else "NO_SCVIC_CV"
            if r["bridged"] and not r["has_scvic"]
            else "ID_BRIDGE_MISS"
            if not r["bridged"]
            else "UNKNOWN"
        ),
        axis=1,
    )
    print()
    print("  By reason — top 30 dispatched centrales (by MW) at 06:00:")
    cols_show = ["central", "mw", "bridged", "has_scvic", "forced", "best_cv", "reason"]
    print(h6[cols_show].head(30).to_string(index=False))
    print()
    print("  Aggregate rejection breakdown for peak hour:")
    summary = (
        h6.groupby("reason")
        .agg(
            n=("central", "count"),
            total_mw=("mw", "sum"),
        )
        .round(0)
    )
    print(summary.to_string())
    print()
    print(f"  → ALL plants with MW > 0 at peak: total {h6['mw'].sum():.0f} MW")
    print(
        f"  → in_pool plants (bridged + SCVIC + not forced): "
        f"{h6[h6['reason'] == 'OK_in_pool']['mw'].sum():.0f} MW "
        f"({100 * h6[h6['reason'] == 'OK_in_pool']['mw'].sum() / h6['mw'].sum():.0f}%)"
    )

    print()
    print(f"  Marginal-unit dataset: {WORK_DIR / 'marginal_units_v3.parquet'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
