# SPDX-License-Identifier: BSD-3-Clause
"""SCVIC public REST API client (cv-rpt endpoint).

The Coordinador's SCVIC platform (``costosvariables.coordinador.cl``)
exposes one **public** report endpoint that does NOT require NetIQ
SSO:

    GET https://costosvariables.coordinador.cl:8080/v1/cvrpt/show_cv_rpt
        ?yyyy=YYYY&mm=MM     (mm zero-padded "01"…"12")

The response is a JSON envelope ``{"payload": "<json-string>"}`` whose
inner JSON has shape::

    {
      "header":   [{"field":"coordinatedName","header":"Empresa"}, …,
                   {"field":"2026-04-01","header":"01-04-2026"}, …],
      "headerCC": [...],
      "data":     [{"coordinatedName":..., "powerStationName":...,
                    "powerPlantName":..., "configurationName":...,
                    "fuelsTypesName":..., "2026-04-01":"50,343", …}, …]
    }

This is the **per-unit per-day declared Costo Variable** that PLEXOS
uses as input for `cmg-programado-pcp` and `cmg-programado-pid`.
Replaces the deprecated ``/costo-combustible/v3/findAll`` SIP endpoint
with FAR richer data (per-day instead of per-event, with fuel type
and configuration metadata).

Discovered 2026-05-06 by reading ``costosvariables.coordinador.cl``'s
Angular bundle ``main.<hash>.js`` for the URL constructed under
``app-cv-rpt`` route.
"""

from __future__ import annotations

import json
import logging
from pathlib import Path

import pandas as pd
import requests

_LOG = logging.getLogger("cen2gtopt.scvic_api")

CV_RPT_BASE = "https://costosvariables.coordinador.cl:8080/v1"


def _to_float_es(s: object) -> float:
    """Spanish-decimal coerce; NaN on failure."""
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


def fetch_cv_rpt(
    year: int,
    month: int,
    *,
    cache_root: Path | None = None,
    bypass_cache: bool = False,
    timeout: float = 60.0,
) -> pd.DataFrame:
    """Fetch and unpivot the SCVIC monthly CV report into long form.

    Args:
        year: e.g. 2026.
        month: 1-12.
        cache_root: where to cache the parquet (default
            ``~/.cache/gtopt/cen2gtopt/scvic``).
        bypass_cache: if True, re-fetch and overwrite.
        timeout: HTTP request timeout (seconds).

    Returns:
        Long-form DataFrame with columns:
          ``date_utc, empresa, central_name, unidad_name,
            configuracion, tipo_combustible, costo_variable_total``

        ``costo_variable_total`` is in USD/MWh (per CEN convention).
        ``date_utc`` is ISO YYYY-MM-DD per row.
    """
    cache_root = cache_root or Path.home() / ".cache" / "gtopt" / "cen2gtopt" / "scvic"
    cache_root.mkdir(parents=True, exist_ok=True)
    mm = f"{int(month):02d}"
    cpath = cache_root / f"cv_rpt_{year:04d}_{mm}.parquet"
    if cpath.exists() and not bypass_cache:
        _LOG.info("cv-rpt cache hit: %s", cpath)
        return pd.read_parquet(cpath)

    url = f"{CV_RPT_BASE}/cvrpt/show_cv_rpt"
    _LOG.info("fetching cv-rpt %d-%s from %s", year, mm, url)
    params: dict[str, str | int] = {"yyyy": year, "mm": mm}
    resp = requests.get(
        url,
        params=params,
        verify=False,
        headers={"Accept": "application/json", "User-Agent": "cen2gtopt/0.1"},
        timeout=timeout,
    )
    resp.raise_for_status()
    envelope = resp.json()
    payload = envelope.get("payload")
    if isinstance(payload, str):
        body = json.loads(payload)
    else:
        body = payload or {}
    rows = body.get("data") or []
    headers = body.get("header") or []
    date_cols = [h["field"] for h in headers if "-" in h.get("field", "")]
    if not rows:
        _LOG.warning("cv-rpt %d-%s returned 0 rows", year, mm)
        return pd.DataFrame()

    # Unpivot wide → long
    long_rows = []
    for r in rows:
        empresa = r.get("coordinatedName", "")
        central = r.get("powerStationName", "")
        unidad = r.get("powerPlantName", "")
        config = r.get("configurationName", "")
        fuel = r.get("fuelsTypesName", "")
        for date_col in date_cols:
            cv = _to_float_es(r.get(date_col))
            if pd.isna(cv):
                continue
            long_rows.append(
                {
                    "date_utc": date_col,
                    "empresa": empresa,
                    "central_name": central,
                    "unidad_name": unidad,
                    "configuracion": config,
                    "tipo_combustible": fuel,
                    "costo_variable_total": cv,
                }
            )
    out = pd.DataFrame(long_rows)
    if not out.empty:
        out.to_parquet(cpath, index=False)
        _LOG.info("wrote %d long-form rows → %s", len(out), cpath)
    return out


def cv_rpt_for_date(date_iso: str, **kwargs) -> pd.DataFrame:
    """Convenience: fetch the month covering ``date_iso`` (YYYY-MM-DD)
    and filter to that exact day."""
    y, m, _d = date_iso.split("-")
    full = fetch_cv_rpt(int(y), int(m), **kwargs)
    if full.empty:
        return full
    return full[full["date_utc"] == date_iso].reset_index(drop=True)


__all__ = ["CV_RPT_BASE", "fetch_cv_rpt", "cv_rpt_for_date"]
