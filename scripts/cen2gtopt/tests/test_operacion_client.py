# SPDX-License-Identifier: BSD-3-Clause
"""Offline tests for the operación API typed wrappers."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any
from unittest.mock import patch

import pytest

from cen2gtopt._cen_client import CenApiClient, CenApiConfig, CenNotFoundError
from cen2gtopt._operacion_client import (
    fetch_operativos_estados,
    fetch_reportes_generation,
    fetch_topologia_fuelcons,
    fetch_topologia_fuelcons_safe,
)


_FIXTURES = Path(__file__).parent / "data" / "sip_fixtures"


class _FakeResponse:
    def __init__(self, status_code: int, payload: Any = None, text: str = "") -> None:
        self.status_code = status_code
        self._payload = payload
        self.text = text or (json.dumps(payload) if payload is not None else "")
        self.headers = {"content-type": "application/json"}

    def json(self) -> Any:
        return self._payload


def _load(name: str) -> dict:
    return json.loads((_FIXTURES / name).read_text(encoding="utf-8"))


def _client() -> CenApiClient:
    return CenApiClient(
        CenApiConfig(
            user_keys={"operacion": "k"},
            backoff_base=0.0,
            verify_tls=False,
        )
    )


# ----------------------------------------------------------------------
# fetch_topologia_fuelcons
# ----------------------------------------------------------------------


def test_fuelcons_returns_heat_rate_and_fuel_unit():
    fixture = _load("op_fuelcons.json")
    client = _client()
    with patch.object(client._session, "get", return_value=_FakeResponse(200, fixture)):
        df = fetch_topologia_fuelcons(client, infotecnica_id=47)
    # The fixture has a single PMGD TER TIRUA entry (the operación
    # API returned it duplicated; the wrapper de-dups).
    assert len(df) == 1
    row = df.iloc[0]
    assert row["plant_name"] == "PMGD TER TIRUA"
    assert row["fuel"] == "Diésel"
    assert row["heat_rate"] == pytest.approx(0.2672)
    assert row["fuel_unit"] == "ton"


def test_fuelcons_safe_returns_none_on_404():
    client = _client()
    with patch.object(
        client._session,
        "get",
        return_value=_FakeResponse(
            404, {"message": "NotFoundException", "detail": "no records found"}
        ),
    ):
        df = fetch_topologia_fuelcons_safe(client, infotecnica_id=999)
    assert df is None


def test_fuelcons_404_raises_when_not_using_safe_variant():
    client = _client()
    with patch.object(
        client._session,
        "get",
        return_value=_FakeResponse(404, {"message": "NotFoundException"}),
    ):
        with pytest.raises(CenNotFoundError):
            fetch_topologia_fuelcons(client, infotecnica_id=999)


# ----------------------------------------------------------------------
# fetch_operativos_estados
# ----------------------------------------------------------------------


def test_operativos_estados_includes_LF_and_LP():
    fixture = _load("op_estados.json")
    client = _client()
    seq = [
        _FakeResponse(200, fixture),
        _FakeResponse(200, {"content": []}),  # stop pagination
    ]
    with patch.object(client._session, "get", side_effect=seq):
        df = fetch_operativos_estados(client)
    assert not df.empty
    names = " ".join(df["name"].astype(str))
    assert "LF" in names
    assert "LP" in names


# ----------------------------------------------------------------------
# fetch_reportes_generation
# ----------------------------------------------------------------------


def test_reportes_generation_returns_technology_breakdown():
    fixture = _load("op_reportes_gen.json")
    client = _client()
    with patch.object(client._session, "get", return_value=_FakeResponse(200, fixture)):
        df = fetch_reportes_generation(client, date="2025-12-01")
    assert not df.empty
    assert {"technology", "daily_current", "date"}.issubset(df.columns)
    techs = set(df["technology"])
    # Five categories in the fixture; at least Hidráulica and Solar.
    assert "Hidráulica" in techs
    assert "Solar" in techs
