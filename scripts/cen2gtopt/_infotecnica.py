# SPDX-License-Identifier: BSD-3-Clause
"""Client for the **public** infotecnica REST API.

Base URL: ``https://api-infotecnica.coordinador.cl/v1/``

This is the unauthenticated, no-user_key REST API that powers the
public infotecnica web portal at ``infotecnica.coordinador.cl``. It
exposes the rich catalogue of CEN's regulatory technical
information: centrales, barras, subestaciones, lineas,
unidades-generadoras, plus geographic / corporate metadata.

It is **completely free** — anyone can hit these endpoints from
anywhere. The 3scale-gated counterpart at
``infotecnica.api.coordinador.cl`` requires a subscription and
appears to expose substantially the same data.

Master plan §9.4 normalisation rules (UID alignment between LMP-side
``bar_transf`` strings and the gtopt-side bus catalogue) are
unblocked by this API. Specifically, ``/v1/barras/`` provides the
~1.77 MB catalogue with both ``nemotecnico`` (the canonical
short-name CEN uses internally) and ``nombre`` (the human-readable
display name).
"""

from __future__ import annotations

import logging
from dataclasses import dataclass
from typing import Any, Optional

import pandas as pd
import requests


_LOG = logging.getLogger("cen2gtopt.infotecnica")

INFOTECNICA_BASE_URL = "https://api-infotecnica.coordinador.cl/v1"


@dataclass(slots=True)
class InfotecnicaConfig:
    base_url: str = INFOTECNICA_BASE_URL
    timeout: float = 60.0
    verify_tls: bool = True
    user_agent: str = "cen2gtopt/1.0 (+https://github.com/marcelomatus/gtopt)"


class InfotecnicaClient:
    """Read-only public-API client. No auth required."""

    def __init__(self, config: Optional[InfotecnicaConfig] = None) -> None:
        self.config = config or InfotecnicaConfig()
        self._session = requests.Session()
        self._session.headers["Accept"] = "application/json"
        self._session.headers["User-Agent"] = self.config.user_agent

    def close(self) -> None:
        self._session.close()

    def __enter__(self) -> "InfotecnicaClient":
        return self

    def __exit__(self, *_: object) -> None:
        self.close()

    def _get_list(self, path: str) -> list[dict[str, Any]]:
        """GET an endpoint that returns a JSON array directly (no
        envelope, just ``[...]`` at top level)."""
        url = f"{self.config.base_url}{path}"
        resp = self._session.get(
            url, timeout=self.config.timeout, verify=self.config.verify_tls
        )
        resp.raise_for_status()
        body = resp.json()
        if not isinstance(body, list):
            raise ValueError(
                f"expected JSON array from {path}, got {type(body).__name__}"
            )
        return body

    # ------------------------------------------------------------------
    # Typed wrappers — return DataFrames in the canonical shape used
    # by the master plan §3.3.3 Topology dataclasses.
    # ------------------------------------------------------------------

    def categories(self) -> pd.DataFrame:
        """``/v1/search/categories/`` — the endpoint catalogue itself."""
        return pd.DataFrame(self._get_list("/search/categories/"))

    def centrales(self) -> pd.DataFrame:
        """``/v1/centrales/`` — generator catalogue (~1 188 rows).

        Returns columns: ``id, nombre, nemotecnico, codigo, descripcion,
        id_propietario, propietario_nombre, grupo_nombre, id_coordinado,
        coordinado_nombre, id_central_tipo, central_tipo_nombre,
        id_centro_control, centro_control_nombre, id_comuna,
        comuna_nombre, id_region, region_nombre, id_localidad, numero``."""
        return pd.DataFrame(self._get_list("/centrales/"))

    def barras(self) -> pd.DataFrame:
        """``/v1/barras/`` — bus catalogue (~1.77 MB, full SEN bus list).

        Returns columns: ``id, nombre, nemotecnico, codigo, descripcion,
        id_subestacion, id_propietario, propietario_nombre,
        id_coordinado, coordinado_nombre, id_centro_control,
        centro_control_nombre, id_conductor_tipo,
        conductor_tipo_nombre, numero``."""
        return pd.DataFrame(self._get_list("/barras/"))

    def subestaciones(self) -> pd.DataFrame:
        """``/v1/subestaciones/`` — substation catalogue."""
        return pd.DataFrame(self._get_list("/subestaciones/"))

    def lineas(self) -> pd.DataFrame:
        """``/v1/lineas/`` — transmission-line catalogue."""
        return pd.DataFrame(self._get_list("/lineas/"))

    def unidades_generadoras(self) -> pd.DataFrame:
        """``/v1/unidades-generadoras/`` — generation-unit catalogue
        (sub-central level)."""
        return pd.DataFrame(self._get_list("/unidades-generadoras/"))

    def empresas(self) -> pd.DataFrame:
        """``/v1/empresas/`` — company catalogue."""
        return pd.DataFrame(self._get_list("/empresas/"))

    def comunas(self) -> pd.DataFrame:
        """``/v1/comunas/`` — comune catalogue."""
        return pd.DataFrame(self._get_list("/comunas/"))

    def regiones(self) -> pd.DataFrame:
        """``/v1/regiones/`` — region catalogue."""
        return pd.DataFrame(self._get_list("/regiones/"))


# ----------------------------------------------------------------------
# Convenience module-level helpers
# ----------------------------------------------------------------------


def fetch_centrales() -> pd.DataFrame:
    """One-shot fetch of the centrales catalogue."""
    with InfotecnicaClient() as c:
        return c.centrales()


def fetch_barras() -> pd.DataFrame:
    """One-shot fetch of the barras catalogue."""
    with InfotecnicaClient() as c:
        return c.barras()


def fetch_lineas() -> pd.DataFrame:
    """One-shot fetch of the lineas catalogue."""
    with InfotecnicaClient() as c:
        return c.lineas()


__all__ = [
    "INFOTECNICA_BASE_URL",
    "InfotecnicaClient",
    "InfotecnicaConfig",
    "fetch_barras",
    "fetch_centrales",
    "fetch_lineas",
]
