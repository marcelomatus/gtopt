# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the ``--include`` dispatch in ``cen2gtopt main._run_sip``.

Patches the requests layer to serve the captured fixtures, runs the
real CLI, then verifies which raw parquet files were written and
which row counts ended up in the manifest.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any
from unittest.mock import patch

import pandas as pd

from cen2gtopt.main import EXIT_OK, cli


_FIXTURES = Path(__file__).parent / "data" / "sip_fixtures"


class _FakeResponse:
    def __init__(self, status_code: int, payload: Any) -> None:
        self.status_code = status_code
        self._payload = payload
        self.text = json.dumps(payload) if payload is not None else ""
        self.headers = {"content-type": "application/json"}

    def json(self) -> Any:
        return self._payload


# Map (path-fragment, page) → fixture name. Multi-page support is via
# this two-key dict; page=0 returns the fixture, page=1+ returns empty.
_FIXTURE_FOR_PATH = {
    "centrales": "centrales.json",
    "costo-marginal-real": "cmg_real_2025_12_01.json",
    "costo-marginal-online": "cmg_online.json",
    "generacion-real": "generacion_real.json",
    "demanda-neta": "demanda_neta.json",
    "instrucciones-operacionales-cmg": "instrucciones_cmg.json",
    "limitaciones-transmision": "limitaciones.json",
    "programas-mantenimiento-mayor": "mantenimiento.json",
}


def _route_request(url: str) -> _FakeResponse:
    """Decide which fixture to return based on the URL path."""
    # Extract page=N from query string (default 0 if absent).
    page = 0
    for q in url.split("?", 1)[-1].split("&"):
        if q.startswith("page="):
            try:
                page = int(q.split("=", 1)[1])
            except ValueError:
                pass
            break

    for fragment, fixture_name in _FIXTURE_FOR_PATH.items():
        if fragment in url:
            if page == 0:
                payload = json.loads(
                    (_FIXTURES / fixture_name).read_text(encoding="utf-8")
                )
                return _FakeResponse(200, payload)
            # Pagination stop — return empty.
            return _FakeResponse(200, {"data": []})
    # Unknown path — 404 so the test fails loudly.
    return _FakeResponse(404, {"error": f"unknown path: {url}"})


def _run_cli(tmp_path: Path, includes: list[str]) -> tuple[int, dict]:
    """Run cen2gtopt CLI with mocked HTTP and return (exit_code, manifest)."""
    out = tmp_path / "feed"
    args = [
        "--start",
        "2025-12-01",
        "--end",
        "2025-12-01",
        "--out",
        str(out),
        "--source",
        "sip",
        "--api-key",
        "test-fixture-key",
    ]
    for inc in includes:
        args.extend(["--include", inc])

    with patch(
        "requests.Session.get", side_effect=lambda url, **_: _route_request(url)
    ):
        code = cli(args)

    manifest_path = out / "manifest.json"
    manifest = (
        json.loads(manifest_path.read_text(encoding="utf-8"))
        if manifest_path.exists()
        else {}
    )
    return code, manifest


def test_dispatch_lmp_only_writes_cmg_real_and_centrales(tmp_path):
    code, manifest = _run_cli(tmp_path, includes=["lmp"])
    assert code == EXIT_OK
    counts = manifest.get("row_counts", {})
    assert counts.get("centrales", 0) > 0
    assert counts.get("costo_marginal_real", 0) > 0
    # Online not requested — must not be in row_counts.
    assert "costo_marginal_online" not in counts
    # Dispatch / demand not requested.
    assert "generacion_real" not in counts
    assert "demanda_neta" not in counts


def test_dispatch_lmp_online_writes_cmg_online(tmp_path):
    code, manifest = _run_cli(tmp_path, includes=["lmp_online"])
    assert code == EXIT_OK
    counts = manifest["row_counts"]
    assert counts["costo_marginal_online"] > 0
    assert "costo_marginal_real" not in counts


def test_dispatch_dispatch_writes_generacion_real(tmp_path):
    code, manifest = _run_cli(tmp_path, includes=["dispatch"])
    assert code == EXIT_OK
    counts = manifest["row_counts"]
    assert counts["generacion_real"] == 3  # 3 fixture rows
    # Output parquet exists.
    assert (tmp_path / "feed" / "raw" / "generacion_real.parquet").exists()


def test_dispatch_demand_writes_demanda_neta(tmp_path):
    code, manifest = _run_cli(tmp_path, includes=["demand"])
    assert code == EXIT_OK
    counts = manifest["row_counts"]
    assert counts["demanda_neta"] == 3


def test_dispatch_regimes_writes_three_regime_files(tmp_path):
    code, manifest = _run_cli(tmp_path, includes=["regimes"])
    assert code == EXIT_OK
    counts = manifest["row_counts"]
    assert counts["instrucciones_operacionales_cmg"] > 0
    assert counts["limitaciones_transmision"] > 0
    assert counts["programas_mantenimiento_mayor"] > 0
    # Files exist.
    raw = tmp_path / "feed" / "raw"
    assert (raw / "instrucciones_operacionales_cmg.parquet").exists()
    assert (raw / "limitaciones_transmision.parquet").exists()
    assert (raw / "programas_mantenimiento_mayor.parquet").exists()


def test_dispatch_default_set_is_dispatch_demand_lmp(tmp_path):
    """When --include is not passed, default = dispatch + demand + lmp."""
    code, manifest = _run_cli(tmp_path, includes=[])
    assert code == EXIT_OK
    counts = manifest["row_counts"]
    assert "costo_marginal_real" in counts
    assert "generacion_real" in counts
    assert "demanda_neta" in counts
    # Online and regimes not in default set.
    assert "costo_marginal_online" not in counts
    assert "instrucciones_operacionales_cmg" not in counts


def test_dispatch_full_set_writes_all_endpoints(tmp_path):
    code, manifest = _run_cli(
        tmp_path,
        includes=["lmp", "lmp_online", "dispatch", "demand", "regimes"],
    )
    assert code == EXIT_OK
    counts = manifest["row_counts"]
    expected = {
        "centrales",
        "costo_marginal_real",
        "costo_marginal_online",
        "generacion_real",
        "demanda_neta",
        "instrucciones_operacionales_cmg",
        "limitaciones_transmision",
        "programas_mantenimiento_mayor",
    }
    assert expected.issubset(set(counts))


def test_per_bus_recipe_round_trip_against_real_cmg(tmp_path):
    """Sanity-load the captured CMG real parquet and confirm it has
    the canonical long-form columns expected by Phase-1 Cells.lmp."""
    code, _manifest = _run_cli(tmp_path, includes=["lmp"])
    assert code == EXIT_OK
    df = pd.read_parquet(tmp_path / "feed" / "raw" / "costo_marginal_real.parquet")
    assert {"date_utc", "hour", "bus_uid", "bus_name", "lmp"}.issubset(df.columns)
    assert (df["lmp"] >= 0).all()
    assert df["hour"].between(0, 23).all()
