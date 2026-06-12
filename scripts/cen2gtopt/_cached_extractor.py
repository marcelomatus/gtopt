# SPDX-License-Identifier: BSD-3-Clause
"""Cached paginated fetcher for CEN SIP / operación REST endpoints.

Why this exists
---------------
The 3scale gateway hosting the public CEN APIs

* **caps page size at 10 rows** for several endpoints
  (``unidades-generadoras``, ``centrales``, ``generacion-real``…),
  so a full per-day pull is ~hundreds of HTTP requests.

* **rate-limits aggressively** — 429s and 502s appear after a few
  dozen calls in quick succession, with no documented quota.

* **publishes the same data deterministically per (endpoint, date)**
  — once ``2026-04-22`` has data, calling tomorrow returns
  identical rows.

Together these make it expensive *and* error-prone to re-fetch on
every run. We therefore cache:

  ``<cache_root>/<service>/<endpoint>/<param-fingerprint>.parquet``

The first call pages through the API tolerantly (sleep on 429/5xx,
up to ``max_retries`` per page), writes the result to parquet,
and returns it. Subsequent calls (until the cache is cleared)
read the parquet directly — no HTTP traffic.

Usage::

    from cen2gtopt._cached_extractor import cached_paginated_fetch

    df = cached_paginated_fetch(
        client,
        service="sip",
        endpoint="/unidades-generadoras/v4/findByDate",
        params={"startDate": "2026-04-22", "endDate": "2026-04-22"},
    )

Pre-warming the cache for a date is also exposed as a CLI:
``python -m cen2gtopt.cache fetch --date 2026-04-22``.
"""

from __future__ import annotations

import hashlib
import json
import logging
import time
from pathlib import Path
from typing import Iterable

import pandas as pd

from cen2gtopt._cen_client import CenApiClient, CenApiError, CenNotFoundError


_LOG = logging.getLogger("cen2gtopt.cache")


_DEFAULT_CACHE_ROOT = Path.home() / ".cache" / "gtopt" / "cen2gtopt"


def cache_root_default() -> Path:
    return _DEFAULT_CACHE_ROOT


def _safe_segment(s: str) -> str:
    """Make a string safe to use as a file/dir name."""
    return "".join(c if c.isalnum() else "_" for c in s).strip("_")


def _fingerprint(params: dict | None) -> str:
    canonical = json.dumps(params or {}, sort_keys=True, ensure_ascii=False)
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()[:16]


def cache_path(
    *,
    service: str,
    endpoint: str,
    params: dict | None,
    cache_root: Path | None = None,
) -> Path:
    """Compute the canonical parquet cache path for one fetch."""
    root = cache_root or _DEFAULT_CACHE_ROOT
    ep = endpoint.strip("/").replace("/", "__")
    fp = _fingerprint(params)
    # Try to put the date(s) in the filename for human inspection.
    sd = (params or {}).get("startDate", "")
    ed = (params or {}).get("endDate", "")
    date_seg = f"{sd}_to_{ed}" if sd or ed else "all"
    fname = f"{date_seg}_{fp}.parquet"
    return root / _safe_segment(service) / _safe_segment(ep) / fname


def cached_paginated_fetch(
    client: CenApiClient,
    *,
    service: str,
    endpoint: str,
    params: dict | None = None,
    page_size: int = 1000,
    max_pages: int = 500,
    max_retries: int = 3,
    sleep_on_fail_seconds: float = 5.0,
    cache_root: Path | None = None,
    bypass_cache: bool = False,
) -> pd.DataFrame:
    """Fetch a paginated SIP/operación endpoint, with disk caching.

    The endpoint must accept ``page`` (1-indexed) and ``pageSize``
    query parameters and return a JSON body with ``data`` (or
    ``content``) holding the row list.

    * On cache hit (parquet exists), reads + returns immediately.
    * On miss, pages until either an empty chunk, a sub-pageSize
      chunk, ``max_pages``, or 3 consecutive transient failures.
    * Writes the assembled frame to ``cache_path(...)`` on success.
    """
    cpath = cache_path(
        service=service,
        endpoint=endpoint,
        params=params,
        cache_root=cache_root,
    )
    if cpath.exists() and not bypass_cache:
        _LOG.info("cache hit: %s", cpath)
        return pd.read_parquet(cpath)

    cpath.parent.mkdir(parents=True, exist_ok=True)
    rows: list[dict] = []
    page = 1
    consecutive_fail = 0
    real_page_size = None
    while page <= max_pages:
        merged = dict(params or {})
        merged.setdefault("pageSize", page_size)
        merged["page"] = page
        try:
            body = client.get(service, endpoint, params=merged)
        except CenNotFoundError as exc:
            _LOG.info("404 on page %d (%s): %s", page, endpoint, exc)
            break
        except CenApiError as exc:
            consecutive_fail += 1
            if consecutive_fail >= max_retries:
                _LOG.warning(
                    "%s page %d: %d consecutive failures; stopping early",
                    endpoint,
                    page,
                    consecutive_fail,
                )
                break
            _LOG.info(
                "%s page %d transient (%s); sleeping %.1fs",
                endpoint,
                page,
                type(exc).__name__,
                sleep_on_fail_seconds,
            )
            time.sleep(sleep_on_fail_seconds)
            continue
        except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            consecutive_fail += 1
            if consecutive_fail >= max_retries:
                _LOG.warning("%s page %d: giving up: %s", endpoint, page, exc)
                break
            time.sleep(sleep_on_fail_seconds)
            continue
        consecutive_fail = 0
        chunk = body.get("data") or body.get("content") or []
        if real_page_size is None and chunk:
            real_page_size = len(chunk)
        rows.extend(chunk)
        if not chunk:
            break
        if len(chunk) < (real_page_size or page_size):
            break
        page += 1

    df = pd.DataFrame(rows) if rows else pd.DataFrame()
    if not df.empty:
        # Coerce object columns that are obviously list/dict to JSON
        # strings so parquet can serialize.
        for col in df.columns:
            if df[col].dtype == object:
                sample = df[col].dropna().head(10)
                if any(isinstance(v, (list, dict)) for v in sample):
                    df[col] = df[col].map(
                        lambda v: (
                            json.dumps(v, ensure_ascii=False)
                            if isinstance(v, (list, dict))
                            else v
                        )
                    )
        # Atomic write: write to a sibling temp file, then rename.
        # Prevents a half-written .parquet from being read on the next
        # cache lookup if the process is interrupted mid-flush.
        tmp = cpath.with_suffix(cpath.suffix + ".tmp")
        df.to_parquet(tmp, index=False)
        tmp.replace(cpath)
        _LOG.info("wrote %d rows → %s", len(df), cpath)
    return df


def clear_cache(
    *,
    service: str | None = None,
    endpoint: str | None = None,
    cache_root: Path | None = None,
) -> int:
    """Delete cached parquet files matching service / endpoint.

    Returns the number of files removed.
    """
    root = cache_root or _DEFAULT_CACHE_ROOT
    if not root.exists():
        return 0
    target = root
    if service:
        target = target / _safe_segment(service)
    if endpoint:
        target = target / _safe_segment(endpoint.strip("/").replace("/", "__"))
    n = 0
    for p in target.rglob("*.parquet"):
        p.unlink()
        n += 1
    return n


# Endpoint registry — pre-wires the parameter shape and pagination
# behaviour for the SIP endpoints used by the marginal-units demo.
# CEN endpoint paths are long by nature; suppressing line-too-long
# keeps the table aligned for diff-friendly readability.
# pylint: disable=line-too-long
ENDPOINTS = {
    # --- Costo Marginal (CMG) ---
    "cmg_real": {"service": "sip", "path": "/costo-marginal-real/v4/findByDate"},
    "cmg_online": {"service": "sip", "path": "/costo-marginal-online/v4/findByDate"},
    "cmg_programado_pcp": {
        "service": "sip",
        "path": "/cmg-programado-pcp/v4/findByDate",
    },
    "cmg_programado_pid": {
        "service": "sip",
        "path": "/cmg-programado-pid/v4/findByDate",
    },
    # --- Generación ---
    "generacion_real": {"service": "sip", "path": "/generacion-real/v3/findByDate"},
    "generacion_programada_pcp": {
        "service": "sip",
        "path": "/generacion-programada-pcp/v4/findByDate",
    },
    "generacion_programada_pid": {
        "service": "sip",
        "path": "/generacion-programada-pid/v4/findByDate",
    },
    # --- Demanda ---
    "demanda_neta": {"service": "sip", "path": "/demanda-neta/v4/findByDate"},
    "demanda_programada_pcp": {
        "service": "sip",
        "path": "/demanda-programada-pcp/v4/findByDate",
    },
    "demanda_programada_pid": {
        "service": "sip",
        "path": "/demanda-programada-pid/v4/findByDate",
    },
    "demanda_real_estimada": {
        "service": "sip",
        "path": "/demanda-real-estimada/v4/findByDate",
    },
    # --- Embalses (reservoirs) ---
    "embalse_real": {"service": "sip", "path": "/embalse-real/v3/findByDate"},
    "embalse_real_findLast": {"service": "sip", "path": "/embalse-real/v3/findLast"},
    "embalse_prog_pcp": {
        "service": "sip",
        "path": "/embalse-programado-pcp/v4/findByDate",
    },
    "embalse_prog_pid": {
        "service": "sip",
        "path": "/embalse-programado-pid/v4/findByDate",
    },
    # --- Network (line flows + limits) ---
    "flujo_programado_pcp": {
        "service": "sip",
        "path": "/flujo-programado-pcp/v4/findByDate",
    },
    "flujo_programado_pid": {
        "service": "sip",
        "path": "/flujo-programado-pid/v4/findByDate",
    },
    "lineas_transmision": {
        "service": "sip",
        "path": "/lineas-transmision/v4/findByDate",
    },
    "limitaciones": {
        "service": "sip",
        "path": "/limitaciones-transmision/v4/findByDate",
    },
    # --- Plant + unit catalogues ---
    "centrales": {"service": "sip", "path": "/centrales/v4/findByDate"},
    "unidades_generadoras": {
        "service": "sip",
        "path": "/unidades-generadoras/v4/findByDate",
    },
    "capacidad_instalada": {
        "service": "sip",
        "path": "/capacidad-instalada/v4/findByDate",
    },
    "potencia_act_react": {
        "service": "sip",
        "path": "/potencia-activa-reactiva-unidad/v4/findByDate",
    },
    # --- Instructions / SSCC ---
    "instrucciones_cmg": {
        "service": "sip",
        "path": "/instrucciones-operacionales-cmg/v4/findByDate",
    },
    "instrucciones_sscc": {
        "service": "sip",
        "path": "/instrucciones-operacionales-sscc/v4/findByDate",
    },
    "sscc_prog_pcp": {
        "service": "sip",
        "path": "/servicios-complementarios-programados-pcp/v4/findByDate",
    },
    "sscc_prog_pid": {
        "service": "sip",
        "path": "/servicios-complementarios-programados-pid/v4/findByDate",
    },
    # --- Forecasts ---
    "pronostico_pasada": {
        "service": "sip",
        "path": "/pronostico-centrales-pasada/v4/findByDate",
    },
    "pronostico_erv": {"service": "sip", "path": "/pronostico-erv/v4/findByDate"},
    "pronostico_demanda_corto": {
        "service": "sip",
        "path": "/pronosticos-demanda-corto-plazo/v4/findByDate",
    },
    "pronostico_demanda_mediano": {
        "service": "sip",
        "path": "/pronosticos-demanda-mediano-plazo/v4/findByDate",
    },
    # --- Settlement ---
    "transferencia_nacional": {
        "service": "sip",
        "path": "/transferencia-economica-nacional/v4/findByDate",
    },
    "transferencia_zonal": {
        "service": "sip",
        "path": "/transferencia-economica-zonal/v4/findByDate",
    },
    # --- Mantenimiento + ENS ---
    "mantenimiento_mayor": {
        "service": "sip",
        "path": "/programas-mantenimiento-mayor/v4/findByDate",
    },
    "energia_no_suministrada": {
        "service": "sip",
        "path": "/energia-no-suministrada/v4/findByDate",
    },
    # --- Performance indices ---
    "indicador_cpf": {
        "service": "sip",
        "path": "/indicador-desempeno-cpf/v4/findByDate",
    },
    "indicador_csf": {
        "service": "sip",
        "path": "/indicador-desempeno-csf/v4/findByDate",
    },
    "indicador_ct": {"service": "sip", "path": "/indicador-desempeno-ct/v3/findByDate"},
    # --- Other ---
    "punto_control": {"service": "sip", "path": "/punto-control/v3/findByDate"},
    "solicitudes": {"service": "sip", "path": "/solicitudes-trabajo/v4/findByDate"},
}
# pylint: enable=line-too-long


def fetch_by_name(
    client: CenApiClient,
    name: str,
    *,
    start: str,
    end: str | None = None,
    cache_root: Path | None = None,
    bypass_cache: bool = False,
    extra_params: dict | None = None,
) -> pd.DataFrame:
    """Convenience wrapper: fetch a known endpoint by its short name.

    ``name`` is one of :data:`ENDPOINTS`. ``start``/``end`` populate
    ``startDate`` and ``endDate`` (defaulting ``end`` to ``start``).
    """
    spec = ENDPOINTS[name]
    params = {"startDate": start, "endDate": end or start}
    if extra_params:
        params.update(extra_params)
    return cached_paginated_fetch(
        client,
        service=spec["service"],
        endpoint=spec["path"],
        params=params,
        cache_root=cache_root,
        bypass_cache=bypass_cache,
    )


def prefetch_date(
    client: CenApiClient,
    *,
    date: str,
    names: Iterable[str] | None = None,
    cache_root: Path | None = None,
    bypass_cache: bool = False,
) -> dict[str, int]:
    """Fetch every registered endpoint for one date.

    Returns ``{name → row_count}``. Failures are logged but do not
    abort the loop — the caller can still read the partial cache.
    """
    targets = list(names) if names else list(ENDPOINTS.keys())
    out: dict[str, int] = {}
    for n in targets:
        try:
            df = fetch_by_name(
                client,
                n,
                start=date,
                cache_root=cache_root,
                bypass_cache=bypass_cache,
            )
            out[n] = len(df)
            _LOG.info("[%s] %d rows", n, len(df))
        except Exception as exc:  # noqa: BLE001  # pylint: disable=broad-exception-caught
            _LOG.warning("[%s] failed: %s", n, exc)
            out[n] = -1
    return out


__all__ = [
    "ENDPOINTS",
    "cache_root_default",
    "cache_path",
    "cached_paginated_fetch",
    "clear_cache",
    "fetch_by_name",
    "prefetch_date",
]
