# SPDX-License-Identifier: BSD-3-Clause
"""Typed wrappers for the operación API (master plan §11 P2.B).

Live-confirmed endpoints used by the marginal-units pipeline:

* ``/topologia/v3/fuelcons/{infotecnica_id}`` — heat rate per generation
  unit (``consumo_especifico`` in fuel-units / MWh).
* ``/operativos/v1/estados`` — operating-state codes (LF, LP, RO, …)
  used as the lookup table for master plan §4.11.1 driver #1.
* ``/reportes/v3/generation`` — daily aggregate generation by
  technology (Hidráulica, Térmica, Eólica, Solar, BESS).
* ``/mantenimiento-mayor/v1`` — major-maintenance program
  (master plan §4.11.1 driver #5; previously confirmed live).

Auth: ``?user_key=<key>`` against
``https://operacion.api.coordinador.cl``. Pagination envelope is
``{page, pageSize, totalElements, totalPages, content}``.
"""

from __future__ import annotations

import logging
from typing import Optional

import pandas as pd

from cen2gtopt._cen_client import CenApiClient, CenNotFoundError


_LOG = logging.getLogger("cen2gtopt.operacion")


def fetch_operativos_estados(client: CenApiClient) -> pd.DataFrame:
    """``GET /operativos/v1/estados`` — the operating-state code
    catalogue.

    Returns columns:
      ``id, name, active, modules, created, modified``

    Each row is a code like ``LF (Unidad con limitación forzada)``,
    ``LP (Unidad con limitación de programada)``, ``RO`` (Recurso
    Obligatorio), used as a join target for master plan §4.11.1
    driver #1 forced/must-run classification.
    """
    rows: list[dict] = []
    for body in client.paginate(
        "operacion",
        "/operativos/v1/estados",
        params={"limit": 100},
        page_param="page",
        limit_param="limit",
    ):
        rows.extend(body.get("content", []))
    return pd.DataFrame(rows)


def fetch_topologia_fuelcons(
    client: CenApiClient,
    infotecnica_id: int,
) -> pd.DataFrame:
    """``GET /topologia/v3/fuelcons/{infotecnica_id}`` — heat rate
    per generation unit.

    Returns a DataFrame with columns:
      ``unit_id, plant_name, fuel, heat_rate, fuel_unit, own_consumption,
      start_day, end_day``

    Raises ``CenNotFoundError`` (which the caller can catch) for
    plants that don't report fuel consumption (renewables, hydro).

    Heat rate semantics: ``consumo_especifico`` is fuel-units per MWh
    (e.g. 0.2672 ton-of-diesel per MWh). Multiply by the
    matching ``costo-combustible.costoCombustible`` (USD per
    fuel-unit) to compose declared_MC in USD/MWh.
    """
    body = client.get(
        "operacion",
        f"/topologia/v3/fuelcons/{infotecnica_id}",
    )
    rows: list[dict] = []
    for entry in body.get("content", []):
        params = entry.get("params") or {}
        rows.append(
            {
                "unit_id": entry.get("id"),
                "plant_name": entry.get("name"),
                "fuel": params.get("combustible"),
                "heat_rate": params.get("consumo_especifico"),
                "fuel_unit": params.get("unidad_de_combustible"),
                "own_consumption": params.get("consumo_propio"),
                "start_day": entry.get("startDayId"),
                "end_day": entry.get("endDayId"),
            }
        )
    out = pd.DataFrame(rows)
    if not out.empty:
        # Defensive de-dup — operación occasionally returns repeated
        # entries for the same unit_id.
        out = out.drop_duplicates(subset=["unit_id"]).reset_index(drop=True)
    return out


def fetch_topologia_fuelcons_safe(
    client: CenApiClient,
    infotecnica_id: int,
) -> Optional[pd.DataFrame]:
    """Same as ``fetch_topologia_fuelcons`` but returns ``None``
    instead of raising when a plant has no fuel-consumption record."""
    try:
        return fetch_topologia_fuelcons(client, infotecnica_id)
    except CenNotFoundError:
        return None


def fetch_reportes_generation(
    client: CenApiClient,
    *,
    date: str,
) -> pd.DataFrame:
    """``GET /reportes/v3/generation`` — daily aggregate generation
    by technology (Hidráulica, Térmica, Eólica, Solar, BESS,
    Geotérmica).

    Returns columns:
      ``technology, daily_current, monthly_current_todate,
      annual_current_todate, date``
    """
    body = client.get(
        "operacion",
        "/reportes/v3/generation",
        params={"date": date},
    )
    rows: list[dict] = []
    for entry in body.get("content", []):
        rows.append(
            {
                "technology": entry.get("description"),
                "daily_current": entry.get("dailyCurrent"),
                "monthly_current_todate": entry.get("monthlyCurrentTodate"),
                "annual_current_todate": entry.get("annualCurrentTodate"),
                "date": entry.get("date"),
            }
        )
    return pd.DataFrame(rows)


def fetch_mantenimiento_mayor(
    client: CenApiClient,
    *,
    page_size: int = 1000,
    max_pages: int = 50,
) -> pd.DataFrame:
    """``GET /mantenimiento-mayor/v1`` — major-maintenance program.

    Master plan §4.11.1 driver #5 (planned maintenance). Note the
    different path from the SIP variant
    ``/programas-mantenimiento-mayor/v4/findByDate``; the operación
    namespace is the historical authoritative one.
    """
    rows: list[dict] = []
    for body in client.paginate(
        "operacion",
        "/mantenimiento-mayor/v1",
        params={"limit": page_size},
        page_param="page",
        limit_param="limit",
        max_pages=max_pages,
    ):
        rows.extend(body.get("content", []))
    return pd.DataFrame(rows)


__all__ = [
    "fetch_mantenimiento_mayor",
    "fetch_operativos_estados",
    "fetch_reportes_generation",
    "fetch_topologia_fuelcons",
    "fetch_topologia_fuelcons_safe",
]
