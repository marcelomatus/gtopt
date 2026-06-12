# SPDX-License-Identifier: BSD-3-Clause
"""CEN public-website CSV endpoint catalogue.

The canonical landing page for marginal-cost data products is
https://www.coordinador.cl/costos-marginales/ which routes to:

  * Costo Marginal Online — preliminary hourly value calculated from
    operating instructions; updated near-real-time.
  * Costo Marginal Real (Nuevo) — final post-July-15-2024 series after
    operator observations are resolved; the energy-transfer reference.
  * Costo Marginal Real (legacy / pre-2024-07-14) — historical series
    served from cmgreal.coordinador.cl.
  * Bulk download endpoints under
    /mercados/graficos/descarga-datos-costos-marginales/ — provide CSV
    and XLS exports with date/frequency/bar filters.
  * Costo Marginal Programado — scheduled-operation reference.
  * Costo Marginal Proyectado — long-term planning reference.
  * Desviación de los Costos Marginales Programados — scheduled-vs-actual.

Each endpoint URL below is marked ``TODO(verify-at-integration)``
because the export-button URLs are not officially documented and CEN
restructures pages occasionally. The Phase-2 production deployment
captures the live URLs in a one-shot integration session and pins
them here. v1 ships these as stubs so the offline-fixture tests
exercise the parser layer.
"""

from __future__ import annotations

from dataclasses import dataclass


# Canonical landing page (human-readable hub — not directly fetched).
COSTOS_MARGINALES_HUB = "https://www.coordinador.cl/costos-marginales/"


@dataclass(slots=True, frozen=True)
class CenCsvEndpoint:
    name: str  # canonical short name
    page_url: str  # human-readable landing page (the "hub" for that product)
    url_template: str  # python str.format-able with {start}/{end}
    sample_columns: tuple[str, ...]  # Spanish column names we expect


# Costo Marginal Real (Nuevo) — final, post-July-15-2024.
COSTO_MARGINAL_REAL = CenCsvEndpoint(
    name="costo_marginal_real",
    page_url=(
        "https://www.coordinador.cl/mercados/graficos/costos-marginales/"
        "costo-marginal-real-nuevo/"
    ),
    url_template=(
        # TODO(verify-at-integration): export-button URL captured from
        # the Real (Nuevo) page. The bulk-download endpoint at
        # /mercados/graficos/descarga-datos-costos-marginales/
        # costo-marginal-preliminar-real/ may be a better source for
        # large date ranges.
        "https://www.coordinador.cl/mercados/graficos/costos-marginales/"
        "costo-marginal-real-nuevo/export.csv?from={start}&to={end}"
    ),
    sample_columns=("Fecha", "Hora", "Barra", "Costo Marginal (USD/MWh)"),
)

COSTO_MARGINAL_ONLINE = CenCsvEndpoint(
    name="costo_marginal_online",
    page_url=(
        "https://www.coordinador.cl/mercados/graficos/costos-marginales/"
        "costo-marginal-online/"
    ),
    url_template=(
        # TODO(verify-at-integration): export-button URL captured from
        # the Online page; an alternative source is the bulk-download
        # at /mercados/graficos/descarga-datos-costos-marginales/
        # costo-marginal-en-linea-descarga-datos-costo-marginal/ which
        # offers CSV + XLS with advanced filters.
        "https://www.coordinador.cl/mercados/graficos/costos-marginales/"
        "costo-marginal-online/export.csv?from={start}&to={end}"
    ),
    sample_columns=("Fecha", "Hora", "Barra", "Costo Marginal Online (USD/MWh)"),
)

GENERACION_REAL = CenCsvEndpoint(
    name="generacion_real",
    page_url=(
        "https://www.coordinador.cl/operacion/graficos/operacion-real/generacion-real/"
    ),
    url_template=(
        # TODO(verify-at-integration)
        "https://www.coordinador.cl/operacion/graficos/operacion-real/"
        "generacion-real/export.csv?from={start}&to={end}"
    ),
    sample_columns=("Fecha", "Hora", "Central", "Generacion (MWh)"),
)

DEMANDA_REAL = CenCsvEndpoint(
    name="demanda_real",
    page_url=(
        "https://www.coordinador.cl/operacion/graficos/operacion-real/demanda-real/"
    ),
    url_template=(
        # TODO(verify-at-integration)
        "https://www.coordinador.cl/operacion/graficos/operacion-real/"
        "demanda-real/export.csv?from={start}&to={end}"
    ),
    sample_columns=("Fecha", "Hora", "Barra", "Demanda (MWh)"),
)


ALL_ENDPOINTS = {
    e.name: e
    for e in (COSTO_MARGINAL_REAL, COSTO_MARGINAL_ONLINE, GENERACION_REAL, DEMANDA_REAL)
}
