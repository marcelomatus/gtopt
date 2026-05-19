#!/usr/bin/env python
# SPDX-License-Identifier: BSD-3-Clause
# ruff: noqa
# pylint: skip-file
# mypy: ignore-errors
#
# Prototype script — predates the polished cen2gtopt package and is kept
# only as a one-off analytical reference (24-hour real-vs-reconstructed
# CMG comparison).  Excluded from CI lint/type checks because it ships
# with prototype-grade naming and structure.  The production pipeline
# lives in ``scripts/cen2gtopt/marginal_units.py``.
"""24h comparison v2: gtopt_marginal_units vs CEN published Costo Marginal
Real, using **all available** CEN data (no synthesised dispatch).

Improvements over v1:
  * Real per-central hourly dispatch from ``/generacion-real/v3``
    (replaces the synthetic merit-order dispatch).
  * Real forced/RO flags from
    ``/instrucciones-operacionales-cmg/v4``: any central with
    ``estado in {RO, ...}`` is marked ``merit_eligible=False`` (master
    plan §4.11.1 driver #1).
  * Real ~700-unit catalogue, with declared_MC and emission_rate
    looked up by ``tipo_tecnologia`` (Hidráulica, Solar, Eólica,
    Carbón, Diésel, Gas Natural, Biomasa, Geotérmica, BESS).
  * Optional **SCVIC consolidado** Excel override
    (``--scvic-excel <path>`` or ``$SCVIC_EXCEL``): replaces the
    pmax-heuristic with the actual per-unit declared CV from the
    Coordinador's Sistema de Costos Variables. See
    ``scripts/cen2gtopt/_scvic_loader.py`` for the file format.
  * Single system-bus collapse: all centrales attach to one bus, so
    we compare the reconstructed system-mean λ_pipe against CEN's
    published per-bus λ_CEN at the same 2 reference buses (centre
    + south). The systematic offset between λ_pipe and λ_CEN now
    captures the residual congestion + loss components we cannot
    correct (factores-penalizacion 404, restricciones-operacion-real
    404 on this user_key).

Pipeline:
  1. Fetch CMG real at 15-min (192 cells = 96 quarters × 2 buses).
  2. Fetch ``generacion-real/v3`` (hourly, ~700 centrales).
  3. Fetch ``instrucciones-operacionales-cmg/v4`` (forced/RO flags).
  4. Build the canonical catalogue (centrales × declared CV from
     SCVIC if available, else technology defaults).
  5. Broadcast hourly gen → 15-min (4 quarters per hour).
  6. Build canonical feed.
  7. Run gtopt-marginal-units --mode real-reconstruct.
  8. Compare to λ_CEN at the 2 reference buses.

Skipped:
  * factores-penalizacion (404)  — loss-factor adjustment.
  * restricciones-operacion-real (404) — RIO shadow prices for
    congestion decomposition.
  * cmg-programado-pcp (502 currently) — programmed CMG comparison.

Where to download the SCVIC consolidado:
  https://www.coordinador.cl/mercados/documentos/
      costos-variables-de-generacion-y-stock-de-combustible/
      costos-variables-de-generacion/
      consolidado-mensual-de-costos-variables-y-costos-de-partida-y-detencion/

  Open the URL in a browser (Cloudflare bot-check), pick the
  month-year that brackets DATE, download the .xlsx, and pass its
  path via ``--scvic-excel`` or ``$SCVIC_EXCEL``.
"""

from __future__ import annotations

import os
import sys
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")

import pandas as pd

from cen2gtopt._cen_client import CenApiClient, CenApiConfig, CenNotFoundError
from cen2gtopt._cached_extractor import fetch_by_name
from cen2gtopt._id_bridge import (
    build_alias_index,
    resolve as bridge_resolve,
    index_stats,
)
from cen2gtopt._scvic_loader import read_scvic_consolidado
from cen2gtopt._scvic_api import cv_rpt_for_date
from gtopt_canonical_feed import (
    SCHEMA_VERSION,
    Bus,
    Cells,
    Generator,
    Manifest,
    Topology,
    write_feed,
)
from gtopt_canonical_feed.cells import (
    COL_BLOCK,
    COL_BUS_UID,
    COL_DATA_SOURCE,
    COL_DATE_UTC,
    COL_DISPATCH,
    COL_GEN_UID,
    COL_HOUR,
    COL_LMP,
    COL_LOAD,
    COL_SCENARIO,
    COL_STAGE,
)
from gtopt_marginal_units.consumer import MarginalUnitDataset
from gtopt_marginal_units.main import cli as marginal_cli


DATE = "2026-04-22"
SIP_KEY = os.environ.get("CEN_USER_KEY", "492a5028ad67fd63791e28591cd29c01")
WORK_DIR = Path("/tmp/cen_compare_24h_v2")
WORK_DIR.mkdir(parents=True, exist_ok=True)

REF_BUSES = [
    ("A.MELIP_______220", 1, "ALTO MELIPILLA 220KV (centre)"),
    ("CHARRUA_______220", 2, "CHARRÚA 220KV (south)"),
]


# ---------------------------------------------------------------------------
# Technology defaults  — declared_MC ($/MWh) and emission_rate (kg/MWh).
#
# Pure-data classification: every value below comes from a CEN
# endpoint or an IPCC-published emission factor. NO pmax-tier MC,
# NO PMGD/BESS name heuristics, NO peaker-demote rules.

# Map the ``tipo_tecnologia`` field from /generacion-real/v3 onto a
# `kind` label that the gtopt_marginal_units pipeline uses to decide
# merit-marginal candidacy. CEN-published technology names only.
TIPO_TO_KIND: dict[str, str] = {
    "Hidráulica": "hydro",
    "Solar": "profile",
    "Eólica": "profile",
    "Geotérmica": "thermal",  # geothermal sets the marginal in the north
    "Biomasa": "thermal",
    "Térmica": "thermal",
    "Cogeneración": "thermal",
    "BESS": "profile",
    "Almacenamiento": "profile",
    # Subtypes that may appear directly in /unidades-generadoras' tipo_tecnologia_unidad:
    "Termoeléctrica de turbina a vapor": "thermal",
    "Termoeléctrica de turbina a gas": "thermal",
    "Termoeléctrica de ciclo combinado - TG": "thermal",
    "Termoeléctrica de ciclo combinado - TV": "thermal",
    "Motor de combustión interna": "thermal",
    "Termosolar de concentración": "thermal",
    "Hidroeléctrica de pasada": "hydro",
    "Minihidro de pasada": "hydro",
    "Hidroeléctrica de embalse": "hydro",
    "Parque fotovoltaico": "profile",
    "Parque eólico": "profile",
}


def _kind_for(tipo: str | None) -> str:
    """Map CEN tipo_tecnologia → pipeline kind. Unknown → 'profile'."""
    return TIPO_TO_KIND.get(str(tipo or "").strip(), "profile")


# ---------------------------------------------------------------------------
# Step 1 — CMG real at 15-min granularity.
# ---------------------------------------------------------------------------
def _normalize_cmg_15min(raw: pd.DataFrame) -> pd.DataFrame:
    out = pd.DataFrame()
    out["date_utc"] = raw["fecha"].astype(str).str.slice(0, 10)
    out["hour"] = pd.to_numeric(raw["hra"], errors="coerce").astype("Int64")
    out["minute"] = pd.to_numeric(raw["min"], errors="coerce").astype("Int64")
    out["quarter"] = out["hour"].astype(int) * 4 + (out["minute"].astype(int) // 15)
    out["bus_name"] = raw["barra_info"].astype(str)
    out["lmp"] = pd.to_numeric(raw["cmg_usd_mwh_"], errors="coerce").astype(float)
    return out.dropna(subset=["quarter", "lmp"]).reset_index(drop=True)


def fetch_cmg_real(client: CenApiClient) -> pd.DataFrame:
    """Fetch 15-min CMG real per reference bus, via the cached
    extractor (one parquet per (bar_transf, date)).
    """
    rows = []
    for bar_transf, bus_uid, name in REF_BUSES:
        try:
            df_raw = fetch_by_name(
                client,
                "cmg_real",
                start=DATE,
                extra_params={"bar_transf": bar_transf},
            )
        except CenNotFoundError as exc:
            print(f"  skip {name}: {exc}")
            continue
        if df_raw.empty:
            print(f"  no data for {name}")
            continue
        df = _normalize_cmg_15min(df_raw)
        df["bus_uid"] = bus_uid
        rows.append(df)
        print(f"  {name}: {len(df)} 15-min rows")
    return pd.concat(rows, ignore_index=True) if rows else pd.DataFrame()


# ---------------------------------------------------------------------------
# Step 2 — real per-central hourly dispatch.
# ---------------------------------------------------------------------------
def fetch_generacion_real(client: CenApiClient) -> pd.DataFrame:
    """Hourly generation per central via the cached extractor."""
    df = fetch_by_name(client, "generacion_real", start=DATE)
    if df.empty:
        return df
    df["hour"] = pd.to_numeric(df["hora"], errors="coerce").astype("Int64") - 1
    df["mw"] = pd.to_numeric(df["gen_real_mw"], errors="coerce").astype(float)
    df["pmax"] = pd.to_numeric(df["potencia_maxima"], errors="coerce").astype(float)
    return df.dropna(subset=["hour", "mw", "pmax"]).reset_index(drop=True)


# ---------------------------------------------------------------------------
# Step 3 — forced / RO instructions.
# ---------------------------------------------------------------------------
def fetch_instrucciones(client: CenApiClient) -> pd.DataFrame:
    """Operating instructions / RO flags via the cached extractor."""
    df = fetch_by_name(client, "instrucciones_cmg", start=DATE)
    if df.empty:
        return df
    df["hour"] = df["hora"].astype(str).str.split(":").str[0].astype(int)
    return df


def fetch_unidades_generadoras(client: CenApiClient) -> pd.DataFrame:
    """Per-unit catalogue (incl. ``pot_min_tecnica``) via the cached
    extractor. The endpoint caps at 10 rows/page, so first call is
    slow (~150 pages); subsequent calls hit the parquet cache.
    """
    return fetch_by_name(client, "unidades_generadoras", start=DATE)


def _to_float_es(s: object) -> float:
    """Convert Spanish-decimal value to float, NaN on failure."""
    if s is None:
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


def aggregate_units_to_centrales(units_df: pd.DataFrame) -> pd.DataFrame:
    """Aggregate per-unit pmin/pmax to per-central level.

    pmax_central = sum(pot_neta_efectiva) across all units
    pmin_central = sum(pot_min_tecnica) across all units

    Each row's ``central`` is the canonical name from the unit row;
    we use ``nombre_central`` first, falling back to ``central``.
    """
    if units_df.empty:
        return pd.DataFrame(
            columns=["central", "pmax_central", "pmin_central", "n_units"]
        )
    df = units_df.copy()
    # Use ``central`` directly — the ``nombre_central`` field is
    # often empty or contains description blobs / file references,
    # whereas ``central`` is the canonical plant name that matches
    # ``/generacion-real``.
    df["central"] = df["central"].astype(str).str.strip()
    df["_pmax"] = df.get("pot_neta_efectiva", df.get("pot_max_bruta", 0)).map(
        _to_float_es
    )
    df["_pmin"] = df.get("pot_min_tecnica", 0).map(_to_float_es)
    grouped = (
        df.groupby("central")
        .agg(
            pmax_central=("_pmax", "sum"),
            pmin_central=("_pmin", "sum"),
            n_units=("central", "size"),
        )
        .reset_index()
    )
    return grouped


# ---------------------------------------------------------------------------
# Step 4 — build catalogue from the union of all dispatched centrales.
# ---------------------------------------------------------------------------
def _norm_name(s: object) -> str:
    """NFD-fold + lower + alnum-only; matches SCVIC names against
    ``/generacion-real`` names robustly."""
    import unicodedata as _u

    if s is None or (isinstance(s, float) and pd.isna(s)):
        return ""
    nfd = _u.normalize("NFD", str(s))
    no_marks = "".join(c for c in nfd if not _u.combining(c))
    ascii_only = no_marks.encode("ascii", errors="ignore").decode()
    return "".join(ch for ch in ascii_only.lower() if ch.isalnum())


def _name_aliases(name: str) -> list[str]:
    """Generate normalised aliases for a central/unit name.

    Bridges naming differences across CEN APIs:
    - "TERMOELÉCTRICA HORNITOS" → ["termoelectricahornitos", "terhornitos", "hornitos"]
    - "TER NEHUENCO 9B"         → ["ternehuenco9b", "nehuenco9b"]
    - "ANTILHUE_U2"             → ["antilhueu2", "antilhue"]
    """
    aliases: set[str] = set()
    base = _norm_name(name)
    if not base:
        return []
    aliases.add(base)
    # Strip TERMOELECTRICA / TERMICA / TER prefix
    for pfx in ("termoelectrica", "termica", "ter"):
        if base.startswith(pfx) and len(base) > len(pfx):
            aliases.add(base[len(pfx) :])
    # Strip CENTRAL / CT / CC prefix
    for pfx in ("central", "ct", "cc"):
        if base.startswith(pfx) and len(base) > len(pfx):
            aliases.add(base[len(pfx) :])
    # Strip trailing unit suffixes "u1","u2","tg","tv","9b" etc.
    for suf in ("u1", "u2", "u3", "u4", "tg", "tv", "9b", "1", "2", "3", "4"):
        for a in list(aliases):
            if a.endswith(suf) and len(a) > len(suf):
                aliases.add(a[: -len(suf)])
    # Add TER+name variant for reverse-direction matching
    for a in list(aliases):
        if not a.startswith("ter"):
            aliases.add("ter" + a)
    return [a for a in aliases if a and len(a) >= 3]


def _scvic_lookup_table(scvic_df: pd.DataFrame) -> dict[str, dict]:
    """Build ``{normalised_alias → {MC, fuel, configuracion}}``.

    Each SCVIC central contributes several aliases (see
    :func:`_name_aliases`) so naming differences with /generacion-real
    are bridged. When two SCVIC entries map to the same alias, the
    one with the **lower** CV wins (cheapest configuration).
    """
    if scvic_df.empty:
        return {}
    df = scvic_df.copy()
    if "costo_variable_total" in df.columns:
        df["_cv"] = pd.to_numeric(df["costo_variable_total"], errors="coerce")
    else:
        df["_cv"] = float("nan")
    df = df.dropna(subset=["_cv"])

    # Build per-central aliases.
    out: dict[str, dict] = {}
    for _, row in df.iterrows():
        central = str(row.get("central_name", ""))
        unidad = str(row.get("unidad_name", "") or "")
        # Try aliases from BOTH the central and the unit name.
        aliases = set(_name_aliases(central)) | set(_name_aliases(unidad))
        cv = float(row["_cv"])
        fuel = str(row.get("tipo_combustible", "")).strip()
        config = str(row.get("configuracion", ""))
        for a in aliases:
            existing = out.get(a)
            if existing is None or cv < existing["MC"]:
                out[a] = {
                    "MC": cv,
                    "fuel": fuel,
                    "configuracion": config,
                    "source_central": central,
                }
    return out


# CO2 emission factor lookup by fuel name (kg/MWh) — derived as
# heat-rate × IPCC factor at typical SEN heat rates. Falls back to
# 0 when the fuel is renewable / hydro / biogenic.
_FUEL_EF_KG_PER_MWH: dict[str, float] = {
    "carbon": 1062.0,
    "diesel": 847.0,
    "gasnatural": 380.0,
    "fueloil": 780.0,
    "fueloilnro6": 780.0,
    "biomasa": 0.0,
    "glp": 700.0,
    "kerosene": 800.0,
    "petcoke": 1062.0,
}


def _ef_for_fuel(fuel: str) -> float:
    return _FUEL_EF_KG_PER_MWH.get(_norm_name(fuel), 0.0)


def build_topology(
    gen_df: pd.DataFrame,
    *,
    scvic_by_id: dict[int, dict] | None = None,
    forced_id_set: set[int] | None = None,
    pmin_by_id: dict[int, float] | None = None,
    id_by_name: dict[str, int | None] | None = None,
) -> Topology:
    """Single-bus topology, **pure-data classification** — no
    pmax-tier MCs, no PMGD/BESS/peaker name heuristics, no
    pmin-from-pmax fallback.

    Each /generacion-real central is bridged to ``id_central`` via the
    pre-built ``id_by_name`` map (from
    :func:`cen2gtopt._id_bridge.build_alias_index`). All downstream
    lookups (SCVIC CV, forced/RO, pmin) happen on ``id_central``.

    Classification rules (data-only):
      * ``kind`` = :func:`_kind_for` of the unit's
        ``tipo_tecnologia`` / ``tipo_tecnologia_unidad``.
      * If ``id_central`` is in ``forced_id_set`` and the unit is
        thermal, demote to ``profile``.
      * ``declared_MC`` = SCVIC CV when bridged; otherwise 0.
      * ``emission_rate`` derived from SCVIC fuel type via the
        IPCC-derived lookup table.
      * ``pmin`` = declared ``pot_min_tecnica`` aggregated per
        ``id_central``; ``0`` if not declared (no 30% × pmax fallback).
    """
    scvic_by_id = scvic_by_id or {}
    forced_id_set = forced_id_set or set()
    pmin_by_id = pmin_by_id or {}
    id_by_name = id_by_name or {}

    buses = [Bus(uid=1, name="SEN")]
    centrales = (
        gen_df.groupby("central")
        .agg(
            tipo=("tipo_tecnologia", "first"),
            subtipo=("subtipo_tecnologia", "first"),
            pmax=("pmax", "max"),
        )
        .reset_index()
    )
    gens = []
    n_scvic = n_forced = n_id_unmatched = 0
    for i, r in enumerate(centrales.itertuples(index=False)):
        central_name = str(r.central)
        ic = id_by_name.get(central_name)
        if ic is None:
            n_id_unmatched += 1

        # Kind from CEN tipo_tecnologia (no name-prefix heuristics).
        kind = _kind_for(r.tipo)
        # Subtipo override only for cases where the parent maps to
        # something more specific (e.g. parent "Térmica" subtipo
        # "Pasada" stays thermal). Use _kind_for which falls back to
        # profile on unknown.
        if r.subtipo:
            sub_kind = _kind_for(r.subtipo)
            # Hydro subtypes upgrade Térmica/profile to hydro.
            if sub_kind == "hydro":
                kind = "hydro"

        # MC + EF from SCVIC declared CV (if bridged).
        sm = scvic_by_id.get(ic) if ic is not None else None
        if sm is not None:
            mc = float(sm["MC"])
            ef = _ef_for_fuel(sm["fuel"])
            n_scvic += 1
        else:
            mc = 0.0
            ef = 0.0

        # Forced-RO demote (id-based, not name-based).
        if ic is not None and ic in forced_id_set and kind == "thermal":
            kind = "profile"
            n_forced += 1

        pmax = float(r.pmax) if pd.notna(r.pmax) else 0.0
        # pmin from declared technical-min only (no 30%·pmax fallback).
        declared_pmin = pmin_by_id.get(ic) if ic is not None else None
        pmin = (
            float(declared_pmin)
            if declared_pmin and not pd.isna(declared_pmin)
            else 0.0
        )
        if pmin > pmax:
            pmin = pmax

        gens.append(
            Generator(
                uid=10_000 + i,
                name=central_name[:64],
                bus_uid=1,
                pmin=pmin,
                pmax=pmax,
                declared_MC=mc,
                kind=kind,
                emission_rate=ef,
            )
        )
    print(f"  catalogue: {len(gens)} centrales (data-only — no heuristics)")
    print(f"    id_central matched:        {len(gens) - n_id_unmatched}/{len(gens)}")
    print(f"    SCVIC CV applied:          {n_scvic}/{len(gens)}")
    print(f"    forced-RO mask demotes:    {n_forced}")
    print(
        f"    declared pmin > 0 applied: "
        f"{sum(1 for g in gens if g.pmin > 0)}/{len(gens)}"
    )
    print(
        f"    MC tiers: ${min(g.declared_MC for g in gens):.0f}-"
        f"${max(g.declared_MC for g in gens):.0f}/MWh"
    )
    return Topology(buses=buses, generators=gens, lines=[])


def central_to_uid_map(topology: Topology) -> dict[str, int]:
    return {g.name: g.uid for g in topology.generators}


# ---------------------------------------------------------------------------
# Step 5 — broadcast hourly dispatch to 15-min quarters.
# ---------------------------------------------------------------------------
def hourly_to_quarters(
    gen_df: pd.DataFrame, name_to_uid: dict[str, int]
) -> pd.DataFrame:
    out_rows = []
    for _, r in gen_df.iterrows():
        uid = name_to_uid.get(str(r["central"])[:64])
        if uid is None or pd.isna(r["hour"]):
            continue
        h = int(r["hour"])
        if h < 0 or h > 23:
            continue
        for q in (h * 4, h * 4 + 1, h * 4 + 2, h * 4 + 3):
            out_rows.append(
                {
                    "quarter": q,
                    "gen_uid": uid,
                    "dispatch": float(r["mw"]),
                }
            )
    return pd.DataFrame(out_rows)


# ---------------------------------------------------------------------------
# Step 6 — assemble feed (single-bus, system-level).
# ---------------------------------------------------------------------------
def build_feed(
    out: Path, topology: Topology, cmg_df: pd.DataFrame, disp_df: pd.DataFrame
) -> Path:
    # Use the median CMG across the 2 reference buses as the system λ.
    sys_lmp = cmg_df.groupby(["date_utc", "quarter"], as_index=False)["lmp"].median()
    lmp_rows = [
        {
            COL_SCENARIO: pd.NA,
            COL_STAGE: pd.NA,
            COL_BLOCK: pd.NA,
            COL_DATE_UTC: str(r["date_utc"]),
            COL_HOUR: int(r["quarter"]),
            COL_DATA_SOURCE: "real",
            COL_BUS_UID: 1,
            COL_LMP: float(r["lmp"]),
        }
        for _, r in sys_lmp.iterrows()
    ]

    disp_rows = [
        {
            COL_SCENARIO: pd.NA,
            COL_STAGE: pd.NA,
            COL_BLOCK: pd.NA,
            COL_DATE_UTC: DATE,
            COL_HOUR: int(r["quarter"]),
            COL_DATA_SOURCE: "real",
            COL_GEN_UID: int(r["gen_uid"]),
            COL_DISPATCH: float(r["dispatch"]),
        }
        for _, r in disp_df.iterrows()
    ]

    # load = sum of dispatch per quarter so the bus balance closes.
    bus_load = (
        disp_df.groupby("quarter", as_index=False)["dispatch"]
        .sum()
        .rename(columns={"dispatch": "load_mw"})
    )
    load_rows = [
        {
            COL_SCENARIO: pd.NA,
            COL_STAGE: pd.NA,
            COL_BLOCK: pd.NA,
            COL_DATE_UTC: DATE,
            COL_HOUR: int(r["quarter"]),
            COL_DATA_SOURCE: "real",
            COL_BUS_UID: 1,
            COL_LOAD: float(r["load_mw"]),
        }
        for _, r in bus_load.iterrows()
    ]

    cells = Cells(
        dispatch=pd.DataFrame(disp_rows),
        lmp=pd.DataFrame(lmp_rows),
        load=pd.DataFrame(load_rows),
    )
    feed_path = out / "feed.parquet"
    manifest = Manifest.make(
        producer="cen_compare_24h_v2",
        producer_version="0.2.0",
        schema_version=SCHEMA_VERSION,
        extras={"date": DATE},
    )
    write_feed(feed_path, topology, cells, manifest)
    return feed_path


# ---------------------------------------------------------------------------
# Step 7 — render comparison tables.
# ---------------------------------------------------------------------------
def render_tables(
    out: Path,
    cmg_df: pd.DataFrame,
    topology: Topology,
    forced_count_per_quarter: pd.Series,
) -> None:
    ds = MarginalUnitDataset.open(out)
    pb = ds.per_bus()
    pb = pb.rename(columns={"hour": "quarter"})

    # Pick the marginal unit per (quarter) as the highest-MC dispatched
    # unit that is *merit-eligible* (i.e. kind=='thermal' in our
    # canonical-feed topology). Profile-demoted units (forced peakers,
    # PMGDs, BESS) are still in dispatch but cannot set the marginal
    # price — the pipeline's §4.7 R3 already excludes them, so we
    # match that here for display consistency.
    eligible_uids = {g.uid for g in topology.generators if g.kind == "thermal"}
    dispatched = pb[(pb["dispatch"] > 0) & pb["gen_uid"].isin(eligible_uids)].copy()
    if dispatched.empty:
        print("  WARN — pipeline produced no merit-eligible dispatched cells")
        return

    marg_idx = dispatched.groupby("quarter")["marginal_cost"].idxmax()
    marg = dispatched.loc[
        marg_idx,
        [
            "quarter",
            "gen_uid",
            "gen_name",
            "marginal_cost",
        ],
    ].rename(columns={"gen_name": "marginal_unit", "marginal_cost": "MC_star"})

    ef_by_uid = {g.uid: g.emission_rate for g in topology.generators}
    marg["epsilon_pipe"] = marg["gen_uid"].map(ef_by_uid).fillna(0.0)

    sys_lmp_pipe = (
        pb.groupby("quarter", as_index=False)["zone_lmp"]
        .first()
        .rename(columns={"zone_lmp": "lmp_pipe"})
    )
    # When all thermals are forced_pmin (no interior) the pipeline
    # falls back to demand_fail_cost (1000). In real Chilean dispatch
    # the marginal in such hours is renewable curtailment at $0 —
    # remap to make the comparison physically sensible.
    sys_lmp_pipe.loc[sys_lmp_pipe["lmp_pipe"] >= 999.0, "lmp_pipe"] = 0.0
    cmp = sys_lmp_pipe.merge(marg, on="quarter", how="left")
    # Same: when λ_pipe = 0, the marginal is curtailed renewable, ε = 0.
    cmp.loc[cmp["lmp_pipe"] == 0.0, "epsilon_pipe"] = 0.0
    cmp.loc[cmp["lmp_pipe"] == 0.0, "marginal_unit"] = "renewable_curtailment"
    cmp["hour"] = cmp["quarter"] // 4
    cmp["minute"] = (cmp["quarter"] % 4) * 15
    cmp["time"] = cmp.apply(
        lambda r: f"{int(r['hour']):02d}:{int(r['minute']):02d}", axis=1
    )

    # Compare to each reference bus's published λ.
    for bar_transf, bus_uid, label in REF_BUSES:
        bus_cmg = cmg_df[cmg_df["bus_uid"] == bus_uid][["quarter", "lmp"]].rename(
            columns={"lmp": f"lmp_cen_{bus_uid}"}
        )
        cmp = cmp.merge(bus_cmg, on="quarter", how="left")
        cmp[f"delta_{bus_uid}"] = cmp["lmp_pipe"] - cmp[f"lmp_cen_{bus_uid}"]

    pd.set_option("display.max_colwidth", 36)
    pd.set_option("display.width", 220)
    pd.set_option("display.float_format", lambda x: f"{x:.2f}")

    # ---- TABLE 1: hourly view ----
    print()
    print("=" * 130)
    print(
        f"  TABLE — System-level λ_pipe vs CEN λ at 2 reference buses ({DATE}) — hourly view"
    )
    print("=" * 130)
    cmp["forced_in_quarter"] = (
        cmp["quarter"].map(forced_count_per_quarter.to_dict()).fillna(0).astype(int)
    )
    agg = (
        cmp.groupby("hour")
        .agg(
            n=("quarter", "count"),
            λ_pipe=("lmp_pipe", "mean"),
            λ_CEN_centre=("lmp_cen_1", "mean"),
            λ_CEN_south=("lmp_cen_2", "mean"),
            Δ_centre=("delta_1", lambda s: s.abs().mean()),
            Δ_south=("delta_2", lambda s: s.abs().mean()),
            ε_pipe=("epsilon_pipe", "mean"),
            forced=("forced_in_quarter", "max"),
            marginal=(
                "marginal_unit",
                lambda s: (s.value_counts().index[0] if not s.empty else "")[:30],
            ),
        )
        .reset_index()
        .round(2)
    )
    print(agg.to_string(index=False))

    # ---- TABLE 2: morning peak detail at 15-min ----
    print()
    print("=" * 130)
    print(f"  TABLE — Morning peak detail at 15-min granularity ({DATE} 06:00-08:00)")
    print("=" * 130)
    peak = cmp[(cmp["hour"] >= 6) & (cmp["hour"] <= 7)].sort_values("quarter")
    print(
        peak[
            [
                "time",
                "lmp_pipe",
                "lmp_cen_1",
                "delta_1",
                "lmp_cen_2",
                "delta_2",
                "epsilon_pipe",
                "marginal_unit",
            ]
        ].to_string(index=False)
    )

    # ---- TABLE 3: Δλ distribution ----
    print()
    print("=" * 130)
    print("  TABLE — Δλ distribution at 15-min granularity (96 cells)")
    print("=" * 130)
    for bar_transf, bus_uid, label in REF_BUSES:
        d = cmp[f"delta_{bus_uid}"].abs().dropna()
        if d.empty:
            continue
        print(f"\n  vs Bus {bus_uid} ({label}):")
        print(f"    cells:        {len(d)}")
        print(f"    mean |Δλ|:    {d.mean():.4f} USD/MWh")
        print(f"    median |Δλ|:  {d.median():.4f} USD/MWh")
        print(f"    P90  |Δλ|:    {d.quantile(0.90):.4f} USD/MWh")
        print(f"    max  |Δλ|:    {d.max():.4f} USD/MWh")
        print(f"    within $0.5:  {(d <= 0.5).sum()} / {len(d)}")
        print(f"    within $2.0:  {(d <= 2.0).sum()} / {len(d)}")
        print(f"    within $5.0:  {(d <= 5.0).sum()} / {len(d)}")

    cmp.to_parquet(out / "comparison.parquet", index=False)
    print()
    print(f"  Comparison frame: {out / 'comparison.parquet'}")


# ---------------------------------------------------------------------------
# Main.
# ---------------------------------------------------------------------------
def _parse_args() -> tuple[Path | None, str]:
    """Parse CLI flags. Returns (scvic_excel_path, date)."""
    import argparse

    p = argparse.ArgumentParser(
        description="24h CEN vs gtopt-marginal-units comparison (full-data v2)"
    )
    p.add_argument(
        "--scvic-excel",
        type=Path,
        default=None,
        help="Path to a SCVIC consolidado-mensual Excel file. "
        "Overrides the pmax-heuristic with declared CV. "
        "Falls back to env $SCVIC_EXCEL if unset.",
    )
    p.add_argument("--date", default=DATE, help=f"Date to query (default: {DATE}).")
    args = p.parse_args()
    excel = args.scvic_excel or (
        Path(os.environ["SCVIC_EXCEL"]) if "SCVIC_EXCEL" in os.environ else None
    )
    return excel, args.date


def main() -> int:
    scvic_excel, date_arg = _parse_args()
    global DATE  # noqa: PLW0603 — single-process CLI tool
    DATE = date_arg

    print("=" * 80)
    print(f"  CEN comparison v2 (full-data) — date={DATE}")
    if scvic_excel is not None:
        print(f"  SCVIC override: {scvic_excel}")
    print("=" * 80)

    # Load SCVIC declared CVs.  Priority order:
    #   1. Explicit --scvic-excel (offline, manual download).
    #   2. SCVIC public cv-rpt API (per-day, ~6k unit-rows, no auth).
    #   3. None → pmax-heuristic only.
    scvic_df = None
    if scvic_excel is not None:
        if not scvic_excel.exists():
            print(f"  ERROR: SCVIC excel not found at {scvic_excel}")
            return 2
        ym = "-".join(DATE.split("-")[:2])
        try:
            scvic_df = read_scvic_consolidado(scvic_excel, default_year_month=ym)
            print(f"  SCVIC excel loaded: {len(scvic_df)} rows from {scvic_excel.name}")
        except Exception as exc:  # noqa: BLE001
            print(
                f"  WARN: SCVIC parse failed ({type(exc).__name__}: {exc}) — "
                "trying SCVIC API"
            )
            scvic_df = None
    if scvic_df is None:
        try:
            api_df = cv_rpt_for_date(DATE)
            if not api_df.empty:
                # SCVIC declares MULTIPLE configurations per unit per
                # day — start-up CVs (~$0.5), tech-min CVs (~$5),
                # full-load CVs ($40-$200), and PLEXOS sentinels
                # (>$1000) signalling "out of economic order".
                # Filter to the realistic full-load range $5-$1000
                # and take the **median** per (unit, fuel) — that's
                # the value PLEXOS uses on the merit margin most of
                # the time.
                clean = api_df[
                    (api_df["costo_variable_total"] >= 5.0)
                    & (api_df["costo_variable_total"] < 1000.0)
                ]
                # Per (central, unidad, fuel) median CV.
                grouped = clean.groupby(
                    ["central_name", "unidad_name", "tipo_combustible"],
                    as_index=False,
                )["costo_variable_total"].median()
                # Then per central, take the unit with the LOWEST
                # full-load CV (cheapest available unit at the central).
                cheapest = grouped.loc[
                    grouped.groupby("central_name")["costo_variable_total"].idxmin()
                ].copy()
                # Ensure consumer-expected columns exist.
                cheapest["empresa"] = ""
                cheapest["configuracion"] = cheapest["unidad_name"]
                cheapest["date_utc"] = DATE
                for c in (
                    "costo_combustible",
                    "fuel_unit",
                    "costo_variable_no_combustible",
                    "costo_partida",
                    "costo_detencion",
                ):
                    if c not in cheapest.columns:
                        cheapest[c] = pd.NA
                scvic_df = cheapest
                print(
                    f"  SCVIC API loaded: {len(scvic_df)} centrales (median "
                    f"of full-load configs $5-$1000), CV range "
                    f"${scvic_df['costo_variable_total'].min():.1f}–"
                    f"${scvic_df['costo_variable_total'].max():.1f}/MWh"
                )
        except Exception as exc:  # noqa: BLE001
            print(
                f"  WARN: SCVIC API fetch failed "
                f"({type(exc).__name__}: {exc}) — pmax-heuristic only"
            )
            scvic_df = None

    sip_cfg = CenApiConfig(user_keys={"sip": SIP_KEY}, verify_tls=False)
    with CenApiClient(sip_cfg) as client:
        print("\n[step 1]  CMG real at 15-min for 2 reference buses")
        cmg_df = fetch_cmg_real(client)
        if cmg_df.empty:
            print("  no CMG — aborting")
            return 1

        print("\n[step 2]  generacion-real (hourly per central)")
        gen_df = fetch_generacion_real(client)
        print(f"  {len(gen_df)} rows, {gen_df['central'].nunique()} centrales")

        print("\n[step 3]  instrucciones-operacionales-cmg (forced/RO flags)")
        instr_df = fetch_instrucciones(client)
        print(
            f"  {len(instr_df)} instructions; "
            f"estado breakdown: {dict(instr_df['estado'].value_counts().head(5)) if not instr_df.empty else {}}"
        )

        print("\n[step 3b]  unidades-generadoras (declared technical-min)")
        units_df = fetch_unidades_generadoras(client)
        print(f"  {len(units_df)} units fetched")

    # ---- Build the cross-API id_central bridge from cached units. ----
    print("\n[bridge]  id_central alias index from /unidades-generadoras")
    alias_index = build_alias_index(units_df)
    stats = index_stats(alias_index)
    print(
        f"  {stats['alias_keys']} alias keys → "
        f"{stats['distinct_id_centrales']} distinct id_central"
    )

    # Bridge: gen_real central name → id_central
    gen_centrales = gen_df["central"].astype(str).unique()
    id_by_name: dict[str, int | None] = {
        c: bridge_resolve(c, alias_index) for c in gen_centrales
    }
    n_bridged = sum(1 for v in id_by_name.values() if v is not None)
    print(f"  gen_real centrales bridged: {n_bridged}/{len(gen_centrales)}")

    # Bridge: forced/RO central names → id_central
    forced_id_set: set[int] = set()
    if not instr_df.empty:
        forced_rows = instr_df[instr_df["estado"].astype(str).str.upper() != "N"]
        for raw in forced_rows["central"].astype(str).unique():
            ic = bridge_resolve(raw, alias_index)
            if ic is not None:
                forced_id_set.add(ic)
    print(f"  forced id_central set: {len(forced_id_set)} unique")

    # Bridge: per-unit pmin aggregated to id_central
    pmin_by_id: dict[int, float] = {}
    if not units_df.empty:
        df = units_df.copy()
        df["_pmax"] = df.get("pot_neta_efectiva", df.get("pot_max_bruta", 0)).map(
            _to_float_es
        )
        df["_pmin"] = df.get("pot_min_tecnica", 0).map(_to_float_es)
        df["_id"] = pd.to_numeric(df["id_central"], errors="coerce")
        df = df.dropna(subset=["_id"])
        df["_id"] = df["_id"].astype(int)
        for ic, grp in df.groupby("_id"):
            pmin_by_id[int(ic)] = float(grp["_pmin"].sum())
    print(
        f"  pmin map (id-keyed): {sum(1 for v in pmin_by_id.values() if v > 0)} "
        f"of {len(pmin_by_id)} ids have pmin > 0"
    )

    # Bridge: SCVIC declared CV → id_central
    scvic_by_id: dict[int, dict] = {}
    if scvic_df is not None and not scvic_df.empty:
        for _, r in scvic_df.iterrows():
            cn = str(r.get("central_name", ""))
            un = str(r.get("unidad_name", "") or "")
            cf = str(r.get("configuracion", "") or "")
            ic = bridge_resolve(cn, alias_index, un, cf)
            if ic is None:
                continue
            cv = float(r.get("costo_variable_total", 0.0) or 0.0)
            existing = scvic_by_id.get(ic)
            if existing is None or cv < existing["MC"]:
                scvic_by_id[ic] = {
                    "MC": cv,
                    "fuel": str(r.get("tipo_combustible", "")).strip(),
                    "configuracion": cf,
                }
    print(f"  SCVIC map (id-keyed): {len(scvic_by_id)} centrales")

    print("\n[step 4]  build single-bus catalogue from generacion-real")
    topology = build_topology(
        gen_df,
        scvic_by_id=scvic_by_id,
        forced_id_set=forced_id_set,
        pmin_by_id=pmin_by_id,
        id_by_name=id_by_name,
    )

    name_to_uid = central_to_uid_map(topology)
    print("\n[step 5]  broadcast hourly dispatch → 15-min quarters")
    disp_df = hourly_to_quarters(gen_df, name_to_uid)
    print(f"  {len(disp_df)} dispatch rows (centrales × quarters)")

    print("\n[step 6]  assemble canonical feed")
    feed = build_feed(WORK_DIR, topology, cmg_df, disp_df)
    print(f"  → {feed}")

    print("\n[step 7]  run gtopt-marginal-units --mode real-reconstruct")
    out = WORK_DIR / "marginals.parquet"
    code = marginal_cli(
        [
            "--input-kind",
            "feed-parquet",
            "--mode",
            "real-reconstruct",
            "--feed",
            str(feed),
            "--out",
            str(out),
        ]
    )
    print(f"  → exit {code}")

    print("\n[step 8]  render comparison tables")
    # Forced count per quarter for the diagnostic column.
    forced_per_q = pd.Series(0, index=range(96), dtype=int)
    if not instr_df.empty:
        forced_q = instr_df[instr_df["estado"].astype(str).str.upper() != "N"].assign(
            quarter=lambda d: d["hour"].astype(int) * 4
        )
        forced_per_q = (
            forced_q.groupby("quarter")["central"]
            .nunique()
            .reindex(range(96), fill_value=0)
            .astype(int)
        )
    render_tables(out, cmg_df, topology, forced_per_q)

    return 0


if __name__ == "__main__":
    sys.exit(main())
