# SPDX-License-Identifier: BSD-3-Clause
"""Tests for the SCVIC consolidado-mensual Excel loader."""

from __future__ import annotations

from pathlib import Path

import pandas as pd
import pytest

from cen2gtopt._scvic_loader import (
    CANONICAL_COLUMNS,
    read_scvic_consolidado,
    to_costo_combustible_frame,
)


def _write_synthetic_consolidado(
    path: Path,
    *,
    leading_metadata_rows: int = 4,
    use_spanish_decimals: bool = True,
) -> None:
    """Synthesise a SCVIC-shaped Excel with leading metadata rows.

    The structure mimics what the Coordinador's ``Consolidado Mensual``
    Excels look like: a logo / title / period block, blank rows, the
    table header, then the data rows.
    """
    headers = [
        "Empresa Coordinada",
        "Central",
        "Configuración",
        "Combustible",
        "Costo Combustible (USD/ton)",
        "Costo Variable Total (USD/MWh)",
        "Costo Variable No Combustible",
        "Costo de Partida (USD)",
        "Costo de Detención (USD)",
        "Período",
    ]
    rows = [
        ["", "", "", "", "", "", "", "", "", ""],
        ["Coordinador Eléctrico Nacional", "", "", "", "", "", "", "", "", ""],
        ["Consolidado Mensual de Costos Variables", "", "", "", "", "", "", "", "", ""],
        ["", "", "", "", "", "", "", "", "", ""],
        headers,
    ]
    spanish_decimals = [
        (
            "ENEL GENERACION CHILE",
            "BOCAMINA II",
            "BOCAMINA_2",
            "Carbón",
            "150,19",
            "63,08",
            "5,30",
            "47.500,00",
            "12.300,00",
            "Abril 2026",
        ),
        (
            "ENGIE ENERGIA CHILE",
            "PMGD TER TIRUA",
            "TIRUA_DIESEL",
            "Diésel",
            "1.345,35",
            "359,40",
            "8,90",
            "15.000,00",
            "5.000,00",
            "Abril 2026",
        ),
        (
            "COMASA",
            "LAUTARO 2 BL1",
            "LAUTARO_2_BL1",
            "Biomasa",
            "20,04",
            "29,06",
            "3,40",
            "8.500,00",
            "2.500,00",
            "Abril 2026",
        ),
        (
            "AES GENER",
            "ANGAMOS 1",
            "ANGAMOS_1",
            "Carbón",
            "152,30",
            "65,00",
            "5,00",
            "50.000,00",
            "12.500,00",
            "Abril 2026",
        ),
    ]
    plain_decimals = [
        (
            "ENEL GENERACION CHILE",
            "BOCAMINA II",
            "BOCAMINA_2",
            "Carbón",
            150.19,
            63.08,
            5.30,
            47500.00,
            12300.00,
            "2026-04-01",
        ),
        (
            "ENGIE ENERGIA CHILE",
            "PMGD TER TIRUA",
            "TIRUA_DIESEL",
            "Diésel",
            1345.35,
            359.40,
            8.90,
            15000.00,
            5000.00,
            "2026-04-01",
        ),
        (
            "COMASA",
            "LAUTARO 2 BL1",
            "LAUTARO_2_BL1",
            "Biomasa",
            20.04,
            29.06,
            3.40,
            8500.00,
            2500.00,
            "2026-04-01",
        ),
        (
            "AES GENER",
            "ANGAMOS 1",
            "ANGAMOS_1",
            "Carbón",
            152.30,
            65.00,
            5.00,
            50000.00,
            12500.00,
            "2026-04-01",
        ),
    ]
    rows.extend(spanish_decimals if use_spanish_decimals else plain_decimals)
    rows.append(["", "", "", "", "", "", "", "", "", ""])
    df = pd.DataFrame(rows[:leading_metadata_rows] + rows[leading_metadata_rows:])
    df.to_excel(path, index=False, header=False, engine="openpyxl")


def test_round_trip_basic_consolidado(tmp_path: Path) -> None:
    p = tmp_path / "consolidado.xlsx"
    _write_synthetic_consolidado(p)

    out = read_scvic_consolidado(p, default_year_month="2026-04")
    assert set(CANONICAL_COLUMNS).issubset(out.columns)
    assert len(out) == 4
    by_central = out.set_index("central_name")["costo_combustible"]
    assert by_central["BOCAMINA II"] == pytest.approx(150.19)
    assert by_central["PMGD TER TIRUA"] == pytest.approx(1345.35)
    assert by_central["LAUTARO 2 BL1"] == pytest.approx(20.04)


def test_spanish_decimals_are_coerced(tmp_path: Path) -> None:
    p = tmp_path / "consolidado.xlsx"
    _write_synthetic_consolidado(p, use_spanish_decimals=True)
    out = read_scvic_consolidado(p, default_year_month="2026-04")
    # "47.500,00" → 47500.00 (dot is thousands, comma is decimal)
    bocamina = out[out["central_name"] == "BOCAMINA II"].iloc[0]
    assert bocamina["costo_partida"] == pytest.approx(47500.00)
    assert bocamina["costo_detencion"] == pytest.approx(12300.00)


def test_plain_decimals_pass_through(tmp_path: Path) -> None:
    p = tmp_path / "consolidado.xlsx"
    _write_synthetic_consolidado(p, use_spanish_decimals=False)
    out = read_scvic_consolidado(p, default_year_month="2026-04")
    bocamina = out[out["central_name"] == "BOCAMINA II"].iloc[0]
    assert bocamina["costo_combustible"] == pytest.approx(150.19)
    assert bocamina["costo_variable_total"] == pytest.approx(63.08)


def test_spanish_month_name_parsed_to_date(tmp_path: Path) -> None:
    p = tmp_path / "consolidado.xlsx"
    _write_synthetic_consolidado(p, use_spanish_decimals=True)
    out = read_scvic_consolidado(p, default_year_month="2026-04")
    # "Abril 2026" → "2026-04-01"
    assert (out["date_utc"] == "2026-04-01").all()


def test_default_year_month_fallback(tmp_path: Path) -> None:
    """If date cells are unparseable the loader falls back to the
    user-supplied default_year_month."""
    p = tmp_path / "consolidado.xlsx"
    headers = [
        "Central",
        "Configuración",
        "Combustible",
        "Costo Combustible",
        "Período",
    ]
    rows = [
        ["", "", "", "", ""],
        ["Consolidado Mensual", "", "", "", ""],
        ["", "", "", "", ""],
        headers,
        ["BOCAMINA II", "BOCAMINA_2", "Carbón", "150,19", "—"],
        ["LAUTARO 2 BL1", "LAUTARO_2_BL1", "Biomasa", "20,04", "—"],
    ]
    pd.DataFrame(rows).to_excel(p, index=False, header=False, engine="openpyxl")
    out = read_scvic_consolidado(p, default_year_month="2026-04")
    assert (out["date_utc"] == "2026-04-01").all()


def test_to_costo_combustible_frame_projects_legacy_schema(tmp_path: Path) -> None:
    p = tmp_path / "consolidado.xlsx"
    _write_synthetic_consolidado(p)
    full = read_scvic_consolidado(p, default_year_month="2026-04")
    legacy = to_costo_combustible_frame(full)
    # Same six fields the deprecated SIP endpoint returned.
    assert list(legacy.columns) == [
        "date_utc",
        "central_name",
        "configuracion",
        "empresa",
        "tipo_combustible",
        "costo_combustible",
    ]
    assert len(legacy) == 4


def test_missing_file_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        read_scvic_consolidado(tmp_path / "nope.xlsx")


def test_no_recognisable_header_raises(tmp_path: Path) -> None:
    p = tmp_path / "garbage.xlsx"
    pd.DataFrame([["a", "b", "c"], ["1", "2", "3"]]).to_excel(
        p, index=False, header=False, engine="openpyxl"
    )
    with pytest.raises(ValueError, match="Could not locate"):
        read_scvic_consolidado(p)


def test_compose_with_scvic_data(tmp_path: Path) -> None:
    """End-to-end: SCVIC excel → costo-combustible frame →
    compose_declared_mc joins against fuelcons heat rates."""
    from cen2gtopt._mc_composition import compose_declared_mc

    p = tmp_path / "consolidado.xlsx"
    _write_synthetic_consolidado(p)
    scvic = read_scvic_consolidado(p, default_year_month="2026-04")
    costos = to_costo_combustible_frame(scvic)

    fuelcons = pd.DataFrame(
        [
            {
                "unit_id": 47,
                "plant_name": "BOCAMINA II",
                "fuel": "Carbón",
                "heat_rate": 0.420,
                "fuel_unit": "ton",
                "own_consumption": 0.05,
                "start_day": "2010-01-01",
                "end_day": None,
            },
            {
                "unit_id": 6765,
                "plant_name": "PMGD TER TIRUA",
                "fuel": "Diésel",
                "heat_rate": 0.2672,
                "fuel_unit": "ton",
                "own_consumption": 0.0,
                "start_day": "2016-01-01",
                "end_day": None,
            },
            {
                "unit_id": 81,
                "plant_name": "LAUTARO 2 BL1",
                "fuel": "Biomasa",
                "heat_rate": 1.450,
                "fuel_unit": "ton",
                "own_consumption": 0.0,
                "start_day": "2015-01-01",
                "end_day": None,
            },
        ]
    )
    result = compose_declared_mc(costos, fuelcons)

    by_plant = result.declared_mc.set_index("plant_name")["declared_MC"]
    assert by_plant["BOCAMINA II"] == pytest.approx(0.420 * 150.19)
    assert by_plant["PMGD TER TIRUA"] == pytest.approx(0.2672 * 1345.35)
    assert by_plant["LAUTARO 2 BL1"] == pytest.approx(1.450 * 20.04)
