# SPDX-License-Identifier: BSD-3-Clause
"""Offline tests for the SIP-API client and CenApiClient.

Mocks `requests.Session.get` via monkeypatch so the tests run without
network access. Fixtures captured on 2026-05-06 from
``sipub.api.coordinador.cl`` live (key #3).
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any
from unittest.mock import patch

import pytest

from cen2gtopt._cen_client import (
    CenApiClient,
    CenApiConfig,
    CenAuthError,
    CenNotFoundError,
)
from cen2gtopt._sip_client import (
    fetch_centrales,
    fetch_costo_marginal_real,
)
from cen2gtopt._sip_endpoints import (
    ALL_ENDPOINTS,
    COSTO_MARGINAL_REAL,
    SIP_BASE_URL,
)


_FIXTURES = Path(__file__).parent / "data" / "sip_fixtures"


# ----------------------------------------------------------------------
# Helpers — fake HTTP responses
# ----------------------------------------------------------------------


class _FakeResponse:
    def __init__(self, status_code: int, payload: Any = None, text: str = "") -> None:
        self.status_code = status_code
        self._payload = payload
        self.text = text or json.dumps(payload) if payload is not None else ""
        self.headers = {"content-type": "application/json"}

    def json(self) -> Any:
        return self._payload


def _load_fixture(name: str) -> dict:
    return json.loads((_FIXTURES / name).read_text(encoding="utf-8"))


def _make_client(user_keys: dict[str, str] | None = None) -> CenApiClient:
    config = CenApiConfig(
        user_keys=user_keys or {"sip": "test-key", "operacion": "test-op"},
        timeout=1.0,
        max_retries=2,
        backoff_base=0.0,  # zero backoff for fast tests
        verify_tls=False,
    )
    return CenApiClient(config)


# ----------------------------------------------------------------------
# CenApiClient — auth header + URL construction
# ----------------------------------------------------------------------


def test_get_attaches_user_key_query_param():
    client = _make_client()
    captured: list[str] = []

    def fake_get(url, *_args, **_kwargs):
        captured.append(url)
        return _FakeResponse(200, {"data": []})

    with patch.object(client._session, "get", side_effect=fake_get):
        client.get("sip", "/centrales/v4/findByDate", params={"page": 0})

    assert len(captured) == 1
    url = captured[0]
    assert url.startswith(SIP_BASE_URL + "/centrales/v4/findByDate?")
    assert "user_key=test-key" in url
    assert "page=0" in url


def test_get_unknown_service_raises():
    client = _make_client()
    with pytest.raises(Exception, match="unknown service"):
        client.get("not-a-service", "/foo")


def test_get_missing_user_key_raises():
    client = _make_client(user_keys={"operacion": "x"})
    with pytest.raises(CenAuthError, match="no user_key configured"):
        client.get("sip", "/centrales/v4/findByDate")


def test_get_403_raises_auth_error():
    client = _make_client()
    with patch.object(
        client._session,
        "get",
        return_value=_FakeResponse(403, text="Authentication failed"),
    ):
        with pytest.raises(CenAuthError, match="403"):
            client.get("sip", "/x")


def test_get_404_raises_not_found():
    client = _make_client()
    with patch.object(
        client._session,
        "get",
        return_value=_FakeResponse(404, text="No Mapping Rule matched"),
    ):
        with pytest.raises(CenNotFoundError, match="404"):
            client.get("sip", "/x")


def test_get_502_retries_then_succeeds():
    client = _make_client()
    seq = [
        _FakeResponse(502, text="Bad Gateway"),
        _FakeResponse(200, {"data": [{"x": 1}]}),
    ]
    with patch.object(client._session, "get", side_effect=seq):
        body = client.get("sip", "/x")
    assert body == {"data": [{"x": 1}]}


def test_get_retries_exhausted_raises_server_error():
    from cen2gtopt._cen_client import CenServerError

    client = _make_client()
    seqs = [_FakeResponse(503, text="Service Unavailable")] * 5
    with patch.object(client._session, "get", side_effect=seqs):
        with pytest.raises(CenServerError, match="exhausted"):
            client.get("sip", "/x")


# ----------------------------------------------------------------------
# Pagination
# ----------------------------------------------------------------------


def test_paginate_stops_on_empty_data():
    client = _make_client()
    seq = [
        _FakeResponse(200, {"data": [{"a": 1}]}),
        _FakeResponse(200, {"data": [{"a": 2}]}),
        _FakeResponse(200, {"data": []}),  # stop signal
    ]
    with patch.object(client._session, "get", side_effect=seq):
        pages = list(client.paginate("sip", "/x"))
    assert len(pages) == 3
    assert pages[0]["data"] == [{"a": 1}]


def test_paginate_stops_on_total_pages():
    client = _make_client()
    seq = [
        _FakeResponse(200, {"content": [{"a": 1}], "totalPages": 2}),
        _FakeResponse(200, {"content": [{"a": 2}], "totalPages": 2}),
    ]
    with patch.object(client._session, "get", side_effect=seq):
        pages = list(client.paginate("sip", "/x"))
    assert len(pages) == 2


# ----------------------------------------------------------------------
# Typed wrappers — using captured fixtures
# ----------------------------------------------------------------------


def test_fetch_costo_marginal_real_normalises_to_canonical_long_form():
    fixture = _load_fixture("cmg_real_2025_12_01.json")
    empty = _load_fixture("cmg_real_empty_page.json")
    client = _make_client()

    seq = [
        _FakeResponse(200, fixture),  # page 0
        _FakeResponse(200, empty),  # stop signal
    ]
    with patch.object(client._session, "get", side_effect=seq):
        df = fetch_costo_marginal_real(client, start="2025-12-01", end="2025-12-01")

    assert not df.empty
    assert {"date_utc", "hour", "bus_uid", "bus_name", "lmp", "version"}.issubset(
        df.columns
    )
    # First record from the fixture: ALTO MELIPILLA, hour 0, lmp 47.39305 USD/MWh.
    assert (df["bus_name"].str.contains("ALTO MELIPILLA")).any()
    assert (df["lmp"] > 0).all()
    assert (df["version"] == "REAL-DEF").all()
    # date_utc passthrough.
    assert (df["date_utc"] == "2025-12-01").all()


def test_fetch_centrales_returns_canonical_topology_columns():
    fixture = _load_fixture("centrales.json")
    client = _make_client()

    # Two pages: first the fixture, then empty to terminate pagination.
    seq = [
        _FakeResponse(200, fixture),
        _FakeResponse(200, {"data": []}),
    ]
    with patch.object(client._session, "get", side_effect=seq):
        df = fetch_centrales(client)

    assert not df.empty
    assert {"uid", "name", "technology", "owner"}.issubset(df.columns)
    # Hand-crafted fixture has 3 known generators.
    assert len(df) == 3
    assert (df["uid"] == [2436, 47, 21]).all()
    assert "BOCAMINA II" in set(df["name"])
    # Ventanas N°1 — the special glyph survives end-to-end.
    assert "Ventanas N°1" in set(df["name"])


# ----------------------------------------------------------------------
# Endpoint catalogue sanity
# ----------------------------------------------------------------------


def test_endpoint_catalogue_has_required_marginal_cost_paths():
    """Smoke test on _sip_endpoints — the master-plan-critical paths
    must be present in the index."""
    required = {
        "costo_marginal_real",
        "costo_marginal_online",
        "centrales",
        "instrucciones_operacionales_cmg",
        "limitaciones_transmision",
        "programas_mantenimiento_mayor",
    }
    assert required.issubset(set(ALL_ENDPOINTS.keys()))


def test_costo_marginal_real_path_pinned():
    assert COSTO_MARGINAL_REAL.path == "/costo-marginal-real/v4/findByDate"
    assert "startDate" in COSTO_MARGINAL_REAL.params_required
    assert "endDate" in COSTO_MARGINAL_REAL.params_required
    assert COSTO_MARGINAL_REAL.confirmed_live


# ----------------------------------------------------------------------
# New wrappers — generacion_real / demanda_neta / cmg_online /
# instrucciones / limitaciones / mantenimiento
# ----------------------------------------------------------------------


def test_fetch_costo_marginal_online_normalises_to_long_form():
    from cen2gtopt._sip_client import fetch_costo_marginal_online

    fixture = _load_fixture("cmg_online.json")
    empty = _load_fixture("cmg_real_empty_page.json")
    client = _make_client()

    seq = [_FakeResponse(200, fixture), _FakeResponse(200, empty)]
    with patch.object(client._session, "get", side_effect=seq):
        df = fetch_costo_marginal_online(client, start="2025-12-01", end="2025-12-01")

    assert not df.empty
    assert {"date_utc", "hour", "bus_uid", "bus_name", "lmp"}.issubset(df.columns)
    # Online responses carry version="EN LINEA" rather than REAL-DEF.
    assert (df["version"] == "EN LINEA").all()


def test_fetch_generacion_real_returns_raw_frame():
    """generacion-real has no canonical normaliser — raw passthrough."""
    from cen2gtopt._sip_client import fetch_generacion_real

    fixture = _load_fixture("generacion_real.json")
    client = _make_client()

    seq = [_FakeResponse(200, fixture), _FakeResponse(200, {"data": []})]
    with patch.object(client._session, "get", side_effect=seq):
        df = fetch_generacion_real(client, start="2025-12-01", end="2025-12-01")

    assert not df.empty
    assert {"id_central", "central", "generacion_mwh"}.issubset(df.columns)
    # 3 fixture rows — one of which is the special-glyph "Ventanas N°1".
    assert "Ventanas N°1" in set(df["central"])


def test_fetch_demanda_neta_returns_system_aggregates():
    from cen2gtopt._sip_client import fetch_demanda_neta

    fixture = _load_fixture("demanda_neta.json")
    client = _make_client()

    seq = [_FakeResponse(200, fixture), _FakeResponse(200, {"data": []})]
    with patch.object(client._session, "get", side_effect=seq):
        df = fetch_demanda_neta(client, start="2025-12-01", end="2025-12-01")

    assert not df.empty
    # System-level aggregates (one row per hour, not per bus).
    assert {"demanda_neta_mwh", "fecha_hora"}.issubset(df.columns)


def test_fetch_instrucciones_operacionales_cmg_includes_estado_RO():
    """The 'estado: RO' (Recurso Obligatorio) field is the master-plan
    §4.11.1 driver-#1 source — verify it survives the parse."""
    from cen2gtopt._sip_client import fetch_instrucciones_operacionales_cmg

    fixture = _load_fixture("instrucciones_cmg.json")
    client = _make_client()

    seq = [_FakeResponse(200, fixture), _FakeResponse(200, {"data": []})]
    with patch.object(client._session, "get", side_effect=seq):
        df = fetch_instrucciones_operacionales_cmg(
            client, start="2025-12-01", end="2025-12-01"
        )

    assert not df.empty
    assert {"central", "despacho", "estado", "instruccion_cmg"}.issubset(df.columns)
    # The captured fixture has at least one RO (Recurso Obligatorio) row.
    assert "RO" in set(df["estado"])


def test_fetch_limitaciones_transmision_returns_handcrafted_fixture():
    from cen2gtopt._sip_client import fetch_limitaciones_transmision

    fixture = _load_fixture("limitaciones.json")
    client = _make_client()

    seq = [_FakeResponse(200, fixture), _FakeResponse(200, {"data": []})]
    with patch.object(client._session, "get", side_effect=seq):
        df = fetch_limitaciones_transmision(
            client, start="2025-12-01", end="2025-12-01"
        )

    assert not df.empty
    assert {"id_linea", "linea", "flujo_max_mw"}.issubset(df.columns)


def test_fetch_programas_mantenimiento_mayor_returns_handcrafted_fixture():
    from cen2gtopt._sip_client import fetch_programas_mantenimiento_mayor

    fixture = _load_fixture("mantenimiento.json")
    client = _make_client()

    seq = [_FakeResponse(200, fixture), _FakeResponse(200, {"data": []})]
    with patch.object(client._session, "get", side_effect=seq):
        df = fetch_programas_mantenimiento_mayor(
            client, start="2025-12-01", end="2025-12-01"
        )

    assert not df.empty
    assert {"id_central", "central", "tipo_mantenimiento"}.issubset(df.columns)
