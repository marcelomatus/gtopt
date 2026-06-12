# SPDX-License-Identifier: BSD-3-Clause
"""Offline tests for the public infotecnica REST client.

Mocks ``requests.Session.get`` against captured fixtures from
``api-infotecnica.coordinador.cl/v1/`` (see ``tests/data/sip_fixtures/it_*.json``).
"""

from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any
from unittest.mock import patch

import pytest

from cen2gtopt._infotecnica import (
    INFOTECNICA_BASE_URL,
    InfotecnicaClient,
    InfotecnicaConfig,
)


_FIXTURES = Path(__file__).parent / "data" / "sip_fixtures"


class _FakeResponse:
    def __init__(self, status_code: int, payload: Any) -> None:
        self.status_code = status_code
        self._payload = payload
        self.headers = {"content-type": "application/json"}
        self.text = json.dumps(payload) if payload is not None else ""

    def json(self) -> Any:
        return self._payload

    def raise_for_status(self) -> None:
        if self.status_code >= 400:
            raise RuntimeError(f"HTTP {self.status_code}")


def _load(name: str) -> Any:
    return json.loads((_FIXTURES / name).read_text(encoding="utf-8"))


def test_base_url_pinned():
    assert INFOTECNICA_BASE_URL == "https://api-infotecnica.coordinador.cl/v1"


def test_centrales_returns_canonical_schema():
    fixture = _load("it_centrales.json")
    config = InfotecnicaConfig(verify_tls=False)
    with InfotecnicaClient(config) as client:
        with patch.object(
            client._session, "get", return_value=_FakeResponse(200, fixture)
        ):
            df = client.centrales()
    assert not df.empty
    # All canonical columns present.
    assert {
        "id",
        "nombre",
        "nemotecnico",
        "id_propietario",
        "propietario_nombre",
        "id_central_tipo",
        "central_tipo_nombre",
        "id_comuna",
        "comuna_nombre",
        "id_region",
        "region_nombre",
    }.issubset(df.columns)


def test_barras_carries_subestacion_link():
    fixture = _load("it_barras.json")
    config = InfotecnicaConfig(verify_tls=False)
    with InfotecnicaClient(config) as client:
        with patch.object(
            client._session, "get", return_value=_FakeResponse(200, fixture)
        ):
            df = client.barras()
    assert not df.empty
    # The bar→substation link is the column that lets us reconstruct
    # the topology graph from this catalogue.
    assert "id_subestacion" in df.columns
    assert "nemotecnico" in df.columns


def test_lineas_returns_dataframe():
    fixture = _load("it_lineas.json")
    config = InfotecnicaConfig(verify_tls=False)
    with InfotecnicaClient(config) as client:
        with patch.object(
            client._session, "get", return_value=_FakeResponse(200, fixture)
        ):
            df = client.lineas()
    assert not df.empty
    assert {"id", "nombre", "nemotecnico"}.issubset(df.columns)


def test_unwrapped_array_response_raises_on_envelope():
    """The infotecnica REST API returns top-level arrays, not
    {"data": [...]} envelopes. Guard against accidental envelope
    contamination."""
    config = InfotecnicaConfig(verify_tls=False)
    with InfotecnicaClient(config) as client:
        with patch.object(
            client._session,
            "get",
            return_value=_FakeResponse(200, {"data": [{"x": 1}]}),
        ):
            with pytest.raises(ValueError, match="expected JSON array"):
                client.centrales()


# ----------------------------------------------------------------------
# Live test (gated)
# ----------------------------------------------------------------------


@pytest.mark.skipif(
    os.environ.get("GTOPT_RUN_CEN_BACKTEST") != "1",
    reason="set GTOPT_RUN_CEN_BACKTEST=1 to run live infotecnica tests",
)
def test_live_centrales_smoke():
    """Hit the public infotecnica REST API live. No user_key needed."""
    with InfotecnicaClient(InfotecnicaConfig(verify_tls=False)) as client:
        df = client.centrales()
    assert len(df) > 100
    assert "nombre" in df.columns
    assert "nemotecnico" in df.columns
