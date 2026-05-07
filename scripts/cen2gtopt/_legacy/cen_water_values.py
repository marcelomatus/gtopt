#!/usr/bin/env python
# SPDX-License-Identifier: BSD-3-Clause
# ruff: noqa
# pylint: skip-file
# mypy: ignore-errors
#
# Prototype script for inferring per-plant water values from λ_CEN and
# dispatch state.  Predates the polished cen2gtopt package and is kept
# only as an analytical reference (the merit-margin / capped-pmax /
# at-pmin theoretical framework).  Excluded from CI lint/type checks.
"""Infer per-plant water values for Chilean SEN hydros.

Theory (LP merit-order clearing)
--------------------------------
For a hydro unit dispatched at hour ``h`` with output ``mw_h``:

  * **Interior dispatch** (``pmin < mw_h < pmax``):
      water_value(h) ≈ λ_CEN(h)
    (The unit is on the merit margin: if WV < λ it would ramp to
    pmax; if WV > λ it would shut down. Therefore WV equals the
    cleared system price at that moment.)

  * **Capped at pmax** (``mw_h ≈ pmax``):
      water_value(h) ≤ λ_CEN(h)
    (The unit is inframarginal — its WV is at most the cleared price.)

  * **Off / at pmin** (``mw_h ≈ 0`` or ``≈ pmin``):
      water_value(h) ≥ λ_CEN(h)
    (The operator is saving water; WV must exceed the cleared price.)

Daily water value estimate per plant
-----------------------------------
We compute three estimates per hydro:

  WV_interior  = median(λ_CEN[h])  for h in interior hours
  WV_upper     = max(λ_CEN[h])     for h in interior + capped hours
  WV_lower     = min(λ_CEN[h])     for h in interior + off hours

The interior estimate is the most reliable; upper/lower are bounds
useful when a plant has no interior hours that day.

Inputs (from cached extractor)
------------------------------
  /generacion-real/v3       — hourly mw per central
  /unidades-generadoras/v4  — pot_min_tecnica + pot_neta_efectiva
  /costo-marginal-real/v4   — 15-min λ_CEN per ref bus
  id_central alias bridge   — to join the above
"""

from __future__ import annotations

import os
import sys
import warnings
from pathlib import Path

warnings.filterwarnings("ignore")

import pandas as pd

from cen2gtopt._cen_client import CenApiClient, CenApiConfig
from cen2gtopt._cached_extractor import fetch_by_name
from cen2gtopt._id_bridge import build_alias_index, resolve as bridge_resolve


DATE = "2026-04-22"
SIP_KEY = os.environ.get("CEN_USER_KEY", "492a5028ad67fd63791e28591cd29c01")

REF_BUSES = [
    ("CRUCERO_______220", 10),
    ("CHARRUA_______220", 2),
    ("A.MELIP_______220", 1),
]

# Boundary tolerance: dispatch within `BOUNDARY_RATIO × pmax` of pmin
# or pmax counts as the boundary.
BOUNDARY_RATIO = 0.05

# Hydro tipo strings in /unidades-generadoras
HYDRO_TIPOS = (
    "Hidroeléctrica de pasada",
    "Minihidro de pasada",
    "Hidroeléctrica de embalse",
    "Hidráulica",
)


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
    out["lmp"] = pd.to_numeric(raw["cmg_usd_mwh_"], errors="coerce").astype(float)
    return out.dropna(subset=["lmp"]).reset_index(drop=True)


def main() -> int:
    print("=" * 80)
    print(f"  Hydro water-value inference — {DATE}")
    print("=" * 80)

    sip_cfg = CenApiConfig(user_keys={"sip": SIP_KEY}, verify_tls=False)
    with CenApiClient(sip_cfg) as client:
        print("\n[1] CMG real (system median across reference buses)")
        cmg_rows = []
        for bar_transf, _bus in REF_BUSES:
            try:
                df_raw = fetch_by_name(
                    client,
                    "cmg_real",
                    start=DATE,
                    extra_params={"bar_transf": bar_transf},
                )
                if df_raw.empty:
                    continue
                cmg_rows.append(_normalize_cmg_15min(df_raw))
            except Exception:
                pass
        cmg = pd.concat(cmg_rows, ignore_index=True)
        # System λ per hour = median across buses + quarters in that hour
        sys_lambda = cmg.groupby("hour")["lmp"].median().to_dict()
        print(f"  hours covered: {len(sys_lambda)}")

        print("\n[2] generacion-real (hourly per central)")
        gen = fetch_by_name(client, "generacion_real", start=DATE)
        gen["hour"] = pd.to_numeric(gen["hora"], errors="coerce").astype("Int64") - 1
        gen["mw"] = pd.to_numeric(gen["gen_real_mw"], errors="coerce").astype(float)
        print(f"  {len(gen)} rows / {gen['central'].nunique()} centrales")

        print("\n[3] unidades-generadoras (pmin, pmax, tipo)")
        units = fetch_by_name(client, "unidades_generadoras", start=DATE)

    # --- Build hydro central catalog with aggregated pmin/pmax ---
    hydro_units = units[
        units["tipo_tecnologia_unidad"].astype(str).isin(HYDRO_TIPOS)
    ].copy()
    hydro_units["pmin"] = hydro_units["pot_min_tecnica"].map(_to_float_es)
    hydro_units["pmax"] = hydro_units.get(
        "pot_neta_efectiva", hydro_units["pot_max_bruta"]
    ).map(_to_float_es)
    hydro_units["id_central"] = pd.to_numeric(
        hydro_units["id_central"], errors="coerce"
    ).astype("Int64")
    hydro_units = hydro_units.dropna(subset=["id_central"])
    # Aggregate per central
    hydro_cat = (
        hydro_units.groupby("central")
        .agg(
            id_central=("id_central", "first"),
            pmin=("pmin", "sum"),
            pmax=("pmax", "sum"),
            n_units=("central", "size"),
            tipo=("tipo_tecnologia_unidad", "first"),
        )
        .reset_index()
    )
    print(f"\n  {len(hydro_cat)} hydro centrales in catalogue")

    # --- Per (hydro central, hour): mw + status ---
    hydro_names = set(hydro_cat["central"])
    hydro_disp = gen[gen["central"].isin(hydro_names)].copy()
    hydro_disp = hydro_disp.merge(
        hydro_cat[["central", "pmin", "pmax"]], on="central", how="left"
    )
    hydro_disp["lambda"] = hydro_disp["hour"].map(sys_lambda)

    # Status per row
    pmin_pad = hydro_disp["pmin"] + BOUNDARY_RATIO * (
        hydro_disp["pmax"] - hydro_disp["pmin"]
    )
    pmax_pad = hydro_disp["pmax"] - BOUNDARY_RATIO * (
        hydro_disp["pmax"] - hydro_disp["pmin"]
    )
    hydro_disp["status"] = "off"
    mask_pmin = (hydro_disp["mw"] > 0.5) & (hydro_disp["mw"] <= pmin_pad)
    mask_pmax = hydro_disp["mw"] >= pmax_pad
    mask_interior = (hydro_disp["mw"] > pmin_pad) & (hydro_disp["mw"] < pmax_pad)
    hydro_disp.loc[mask_pmin, "status"] = "near_pmin"
    hydro_disp.loc[mask_pmax, "status"] = "capped_pmax"
    hydro_disp.loc[mask_interior, "status"] = "interior"

    # --- Aggregate to per-hydro daily water-value estimate ---
    rows = []
    for central, grp in hydro_disp.groupby("central"):
        interior = grp[grp["status"] == "interior"]["lambda"].dropna()
        capped = grp[grp["status"] == "capped_pmax"]["lambda"].dropna()
        off = grp[grp["status"] == "off"]["lambda"].dropna()
        rows.append(
            {
                "central": central,
                "tipo": hydro_cat[hydro_cat["central"] == central]["tipo"].iloc[0],
                "pmax": float(
                    hydro_cat[hydro_cat["central"] == central]["pmax"].iloc[0]
                ),
                "pmin": float(
                    hydro_cat[hydro_cat["central"] == central]["pmin"].iloc[0]
                ),
                "n_interior_h": len(interior),
                "n_capped_h": len(capped),
                "n_off_h": len(off),
                "WV_interior_median": interior.median()
                if not interior.empty
                else float("nan"),
                "WV_interior_p25": interior.quantile(0.25)
                if not interior.empty
                else float("nan"),
                "WV_interior_p75": interior.quantile(0.75)
                if not interior.empty
                else float("nan"),
                "WV_upper_capped_max": capped.max()
                if not capped.empty
                else float("nan"),
                "WV_lower_off_min": off.min() if not off.empty else float("nan"),
                "lambda_when_capped_mean": capped.mean()
                if not capped.empty
                else float("nan"),
                "lambda_when_off_mean": off.mean() if not off.empty else float("nan"),
            }
        )
    wv = pd.DataFrame(rows)

    pd.set_option("display.float_format", lambda x: f"{x:.2f}")
    pd.set_option("display.max_colwidth", 30)
    pd.set_option("display.width", 240)

    # ---- Display the user's specific hydros first ----
    target_names = [
        "RAPEL",
        "PEHUENCHE",
        "COLBUN",
        "PANGUE",
        "ANGOSTURA",
        "EL TORO",
        "ANTUCO",
    ]
    print()
    print("=" * 130)
    print(f"  WATER VALUES — user-requested hydros — {DATE}")
    print("=" * 130)
    cols = [
        "central",
        "tipo",
        "pmax",
        "pmin",
        "n_interior_h",
        "WV_interior_median",
        "WV_interior_p25",
        "WV_interior_p75",
        "WV_upper_capped_max",
        "WV_lower_off_min",
        "n_capped_h",
        "n_off_h",
    ]
    target = wv[
        wv["central"].str.contains("|".join(target_names), case=False, na=False)
    ].sort_values("pmax", ascending=False)
    print(target[cols].to_string(index=False))

    # ---- Show top 25 hydros by capacity ----
    print()
    print("=" * 130)
    print("  WATER VALUES — top 25 hydros by pmax")
    print("=" * 130)
    top25 = wv.sort_values("pmax", ascending=False).head(25)
    print(top25[cols].to_string(index=False))

    # ---- System-level summary ----
    print()
    print("=" * 130)
    print("  SYSTEM HYDRO SUMMARY")
    print("=" * 130)
    has_wv = wv.dropna(subset=["WV_interior_median"])
    print(f"  hydro centrales total:                   {len(wv)}")
    print(f"  hydros with interior-dispatch hours:     {len(has_wv)}")
    print(
        f"  median WV across hydros (interior):      "
        f"${has_wv['WV_interior_median'].median():.2f}/MWh"
    )
    print(
        f"  P25 / P75 of WV medians:                 "
        f"${has_wv['WV_interior_median'].quantile(0.25):.2f} / "
        f"${has_wv['WV_interior_median'].quantile(0.75):.2f}/MWh"
    )
    print(
        f"  WV-weighted by pmax:                     "
        f"${(has_wv['WV_interior_median'] * has_wv['pmax']).sum() / has_wv['pmax'].sum():.2f}/MWh"
    )

    out_path = Path("/tmp/cen_water_values_v1.parquet")
    wv.to_parquet(out_path, index=False)
    print()
    print(f"  Full water-value dataset: {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
