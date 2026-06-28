"""Unit tests for the auxiliary AMM spreadsheet readers."""

from __future__ import annotations

from pathlib import Path

from psse2gtopt.aux_data import (
    ascii_sanitize,
    expand_bus_name,
    parse_ldm_merit_order,
    parse_nomenclatura,
)


def test_ascii_sanitize() -> None:
    assert ascii_sanitize("Aguacapa") == "Aguacapa"
    assert ascii_sanitize("Programación") == "Programacion"
    assert ascii_sanitize("San Antonio El Sitio") == "San_Antonio_El_Sitio"
    assert ascii_sanitize("Alborada (Escuintla 2)") == "Alborada_Escuintla_2"
    assert ascii_sanitize("") == "x"


def test_expand_bus_name() -> None:
    codes = {"AGU": "Aguacapa", "CHX": "Chixoy"}
    assert expand_bus_name("AGU-230", codes) == "Aguacapa-230"
    assert expand_bus_name("CHX-H1", codes) == "Chixoy-H1"
    # Unknown prefix -> sanitised original.
    assert expand_bus_name("ZZZ-69", codes) == "ZZZ-69"


def _write_xlsx(path: Path, rows: list[list[object]], sheet: str = "S") -> None:
    import openpyxl  # pylint: disable=import-outside-toplevel

    wb = openpyxl.Workbook()
    ws = wb.active
    ws.title = sheet
    for r in rows:
        ws.append(r)
    wb.save(path)


def test_parse_nomenclatura(tmp_path: Path) -> None:
    path = tmp_path / "nom.xlsx"
    _write_xlsx(
        path,
        [
            [None, "NOMENCLATURA", None],
            [None, "AGU", "Aguacapa"],
            [None, "CHX", "Chixoy"],
            [None, "SNT", "San Antonio El Sitio"],
        ],
    )
    codes = parse_nomenclatura(path)
    assert codes["AGU"] == "Aguacapa"
    assert codes["CHX"] == "Chixoy"
    assert "NOMENCLATURA" not in codes


def test_parse_ldm_merit_order(tmp_path: Path) -> None:
    path = tmp_path / "ldm.xlsx"
    _write_xlsx(
        path,
        [
            [None, None, None],
            ["DEMANDA MÍNIMA", None, None],
            ["Nemo", "Planta Generadora", "Potencia Disponible"],
            ["CRZ-F", "El Carrizo", 55.0],
            ["ZUN-G", "Orzunil", 15.8],
            ["AGU-H1", "Aguacapa 1", 30.0],
            [None, None, None],
            ["DEMANDA MEDIA", None, None],
            ["Nemo", "Planta Generadora", "Potencia Disponible"],
            ["CRZ-F", "El Carrizo", 55.0],
        ],
    )
    order = parse_ldm_merit_order(path)
    # Only the first block, in order, stopping at the next "DEMANDA".
    assert order == ["CRZ-F", "ZUN-G", "AGU-H1"]


def test_parse_ldm_no_header(tmp_path: Path) -> None:
    path = tmp_path / "bad.xlsx"
    _write_xlsx(path, [["foo", "bar"], ["baz", "qux"]])
    assert not parse_ldm_merit_order(path)
