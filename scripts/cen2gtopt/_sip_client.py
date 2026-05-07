# SPDX-License-Identifier: BSD-3-Clause
"""Typed SIP-API wrappers built on top of :class:`CenApiClient`.

These return long-form pandas frames in the canonical-feed shape
(:mod:`gtopt_canonical_feed.cells`), with column normalisation
applied (NFD-fold + 31-bit hash UID where applicable).

Live confirmation status of each wrapper is tracked in
:mod:`cen2gtopt._sip_endpoints`.
"""

from __future__ import annotations

import logging
from typing import Any, Optional

import pandas as pd

from cen2gtopt._cen_client import CenApiClient, CenAuthError
from cen2gtopt._normalize import stable_uid
from cen2gtopt._sip_endpoints import (
    ALL_ENDPOINTS,
    CENTRALES,
    COSTO_COMBUSTIBLE,
    COSTO_MARGINAL_ONLINE,
    COSTO_MARGINAL_REAL,
    DEMANDA_NETA,
    GENERACION_REAL,
    INSTRUCCIONES_OPERACIONALES_CMG,
    LIMITACIONES_TRANSMISION,
    PROGRAMAS_MANTENIMIENTO_MAYOR,
    SipEndpoint,
)


_LOG = logging.getLogger("cen2gtopt.sip")


# Backwards-compat error type — older code in main.py imports this.
class SipNotImplementedError(NotImplementedError):
    """Raised when an unimplemented SIP path is requested."""


def fetch_via_sip(*_args, **_kwargs):
    """Legacy entry point — superseded by :func:`fetch_endpoint`.

    Kept so existing `main.py` imports don't break; raises a clear
    message when invoked.
    """
    raise SipNotImplementedError(
        "fetch_via_sip() is the v0.1 stub; use SipClient.fetch_endpoint(...)"
    )


# ----------------------------------------------------------------------
# Generic fetchers
# ----------------------------------------------------------------------


def fetch_endpoint(
    client: CenApiClient,
    endpoint: SipEndpoint,
    params: Optional[dict[str, Any]] = None,
) -> pd.DataFrame:
    """Fetch all pages of one SIP endpoint and return as a DataFrame.

    Auto-detects the response envelope:
      * ``{"data": [...]}``    — most v4 SIP endpoints
      * ``{"content": [...], "totalPages": M, ...}`` — v3 SIP endpoints
        (e.g. costo-combustible, programas-mantenimiento-mayor)
    """
    rows: list[dict] = []
    for body in client.paginate(
        "sip",
        endpoint.path,
        params=params,
        page_param="page",
        limit_param="limit",
    ):
        # SIP v4 → "data"; v3 → "content"; some other endpoints
        # use a top-level array (rare).
        page_rows = body.get("data") or body.get("content") or []
        if isinstance(page_rows, list):
            rows.extend(page_rows)
    if not rows:
        return pd.DataFrame()
    return pd.DataFrame(rows)


# ----------------------------------------------------------------------
# Typed wrappers — return canonical-feed long-form frames
# ----------------------------------------------------------------------


def fetch_costo_marginal_real(
    client: CenApiClient, *, start: str, end: str
) -> pd.DataFrame:
    """``GET /costo-marginal-real/v4/findByDate``.

    Returns a long-form frame with columns:
    ``date_utc, hour, bus_uid, bus_name, lmp`` (USD/MWh).
    """
    raw = fetch_endpoint(
        client,
        COSTO_MARGINAL_REAL,
        params={"startDate": start, "endDate": end, "limit": 1000},
    )
    if raw.empty:
        return raw
    return _normalize_cmg(raw, value_col="lmp")


def fetch_costo_marginal_online(
    client: CenApiClient, *, start: str, end: str
) -> pd.DataFrame:
    """``GET /costo-marginal-online/v4/findByDate``.

    Same shape as ``fetch_costo_marginal_real``; values are
    preliminary (``version: "EN LINEA"``).
    """
    raw = fetch_endpoint(
        client,
        COSTO_MARGINAL_ONLINE,
        params={"startDate": start, "endDate": end, "limit": 1000},
    )
    if raw.empty:
        return raw
    return _normalize_cmg(raw, value_col="lmp")


def fetch_centrales(client: CenApiClient) -> pd.DataFrame:
    """``GET /centrales/v4/findByDate`` — generator catalogue."""
    raw = fetch_endpoint(client, CENTRALES, params={"limit": 1000})
    if raw.empty:
        return raw
    # Expected columns from the live response: central, propietario,
    # tipo_central, id_central, instalacion, coordinado.
    out = pd.DataFrame()
    out["uid"] = raw["id_central"].astype(int)
    out["name"] = raw["central"].astype(str)
    out["technology"] = raw.get("tipo_central", pd.Series(dtype="object")).astype(str)
    out["owner"] = raw.get("propietario", pd.Series(dtype="object")).astype(str)
    out["coordinated"] = raw.get("coordinado", pd.Series(dtype="object")).astype(str)
    out["installation"] = raw.get("instalacion", pd.Series(dtype="object")).astype(str)
    return out


def fetch_generacion_real(
    client: CenApiClient,
    *,
    start: str,
    end: str,
    technology: Optional[str] = None,
) -> pd.DataFrame:
    """``GET /generacion-real/v3/findByDate`` — hourly realised
    generation per central."""
    params: dict[str, Any] = {
        "startDate": start,
        "endDate": end,
        "pageSize": 1000,
    }
    if technology is not None:
        params["tipoTecnologia"] = technology
    raw = fetch_endpoint(client, GENERACION_REAL, params=params)
    return raw


def fetch_demanda_neta(client: CenApiClient, *, start: str, end: str) -> pd.DataFrame:
    """``GET /demanda-neta/v4/findByDate`` — hourly net demand per bus."""
    raw = fetch_endpoint(
        client,
        DEMANDA_NETA,
        params={"startDate": start, "endDate": end, "limit": 1000},
    )
    return raw


def fetch_instrucciones_operacionales_cmg(
    client: CenApiClient, *, start: str, end: str
) -> pd.DataFrame:
    """``GET /instrucciones-operacionales-cmg/v4/findByDate`` — CDC
    operational instructions, including ``estado`` ('RO' = Recurso
    Obligatorio / forced pmin). Master plan §4.11.1 driver #1+#7."""
    raw = fetch_endpoint(
        client,
        INSTRUCCIONES_OPERACIONALES_CMG,
        params={"startDate": start, "endDate": end, "limit": 1000},
    )
    return raw


def fetch_limitaciones_transmision(
    client: CenApiClient, *, start: str, end: str
) -> pd.DataFrame:
    """``GET /limitaciones-transmision/v4/findByDate`` — CDC
    transmission-system limitations. Master plan §4.7 R1 priority-1
    saturation source."""
    raw = fetch_endpoint(
        client,
        LIMITACIONES_TRANSMISION,
        params={"startDate": start, "endDate": end, "limit": 1000},
    )
    return raw


def fetch_programas_mantenimiento_mayor(
    client: CenApiClient, *, start: str, end: str
) -> pd.DataFrame:
    """``GET /programas-mantenimiento-mayor/v4/findByDate`` — major-
    maintenance program. Master plan §4.11.1 driver #5."""
    raw = fetch_endpoint(
        client,
        PROGRAMAS_MANTENIMIENTO_MAYOR,
        params={"startDate": start, "endDate": end, "limit": 1000},
    )
    return raw


def fetch_costo_combustible(
    client: CenApiClient, *, start: str, end: str
) -> pd.DataFrame:
    """``GET /costo-combustible/v3/findAll`` — declared fuel cost
    per central per day. Master plan §4.11.1 driver #2 (gas
    inflexible / fuel cost).

    Returns a DataFrame with columns:
      ``date_utc, central_name, configuracion, empresa, tipo_combustible, costo_combustible``

    NOTE: CEN appears to have stopped publishing through this endpoint
    after a 2025 cutoff. Pass historical dates (e.g. 2024-11-01) to
    see data; recent dates return ``404 NotFoundException``.

    The ``configuracion`` field is the canonical join key against
    ``instrucciones-operacionales-cmg.configuracion``.
    """
    raw = fetch_endpoint(
        client,
        COSTO_COMBUSTIBLE,
        params={"startDate": start, "endDate": end},
    )
    if raw.empty:
        return raw
    # Normalise the Spanish field names — collapse the ``ó`` accent.
    out = pd.DataFrame()
    out["date_utc"] = (
        raw.get("fecha", pd.Series(dtype="object")).astype(str).str.slice(0, 10)
    )
    out["central_name"] = raw.get("nombreCentral", pd.Series(dtype="object")).astype(
        str
    )
    # The accent on `configuración` is preserved — guard against either form.
    config_col = "configuración" if "configuración" in raw.columns else "configuracion"
    out["configuracion"] = raw.get(config_col, pd.Series(dtype="object")).astype(str)
    out["empresa"] = raw.get("empresa", pd.Series(dtype="object")).astype(str)
    out["tipo_combustible"] = raw.get(
        "tipoCombustible", pd.Series(dtype="object")
    ).astype(str)
    out["costo_combustible"] = pd.to_numeric(
        raw.get("costoCombustible", pd.Series(dtype="float")), errors="coerce"
    ).astype(float)
    return out.reset_index(drop=True)


# ----------------------------------------------------------------------
# Helpers
# ----------------------------------------------------------------------


def _normalize_cmg(raw: pd.DataFrame, *, value_col: str) -> pd.DataFrame:
    """Normalize the SIP CMG response to canonical long-form.

    Input columns (per the live probe):
      ``id_info, barra_info, barra_transf, fecha, hra, min,
      cmg_clp_kwh_, cmg_usd_mwh_, version, fecha_hora, fecha_minuto``

    Output columns:
      ``date_utc, hour, bus_uid, bus_name, lmp, lmp_clp_per_kwh, version``
    """
    out = pd.DataFrame()
    out["date_utc"] = raw["fecha"].astype(str).str.slice(0, 10)
    out["hour"] = pd.to_numeric(raw["hra"], errors="coerce").astype("Int64")
    # Prefer the canonical normalised name (barra_transf) for stable UIDs.
    canonical = raw["barra_transf"].astype(str)
    out["bus_uid"] = canonical.map(stable_uid).astype(int)
    out["bus_name"] = raw["barra_info"].astype(str)
    out[value_col] = pd.to_numeric(raw["cmg_usd_mwh_"], errors="coerce").astype(float)
    if "cmg_clp_kwh_" in raw.columns:
        out["lmp_clp_per_kwh"] = pd.to_numeric(
            raw["cmg_clp_kwh_"], errors="coerce"
        ).astype(float)
    if "version" in raw.columns:
        out["version"] = raw["version"].astype(str)
    return out.dropna(subset=["hour", value_col]).reset_index(drop=True)


__all__ = [
    "ALL_ENDPOINTS",
    "CenAuthError",
    "SipNotImplementedError",
    "fetch_centrales",
    "fetch_costo_marginal_online",
    "fetch_costo_marginal_real",
    "fetch_demanda_neta",
    "fetch_endpoint",
    "fetch_generacion_real",
    "fetch_instrucciones_operacionales_cmg",
    "fetch_limitaciones_transmision",
    "fetch_programas_mantenimiento_mayor",
    "fetch_via_sip",
]
