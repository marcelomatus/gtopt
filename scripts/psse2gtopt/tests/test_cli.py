"""CLI / façade tests for ``psse2gtopt``."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from psse2gtopt.main import main
from psse2gtopt.psse2gtopt import (
    convert_psse_case,
    resolve_raw_file,
    validate_psse_case,
)


def test_resolve_raw_file_direct(synthetic_raw: Path) -> None:
    assert resolve_raw_file(synthetic_raw) == synthetic_raw


def test_resolve_raw_file_directory(data_dir: Path) -> None:
    # The data dir has several .raw files; --raw selects one.
    chosen = resolve_raw_file(data_dir, "synthetic")
    assert chosen.name == "synthetic.raw"


def test_resolve_raw_file_ambiguous(data_dir: Path) -> None:
    with pytest.raises(ValueError, match="ambiguous|matched no"):
        resolve_raw_file(data_dir, "ieee")  # matches ieee14 + ieee39


def test_resolve_raw_file_no_match(data_dir: Path) -> None:
    with pytest.raises(ValueError, match="matched no"):
        resolve_raw_file(data_dir, "nonexistent_pattern")


def test_resolve_raw_file_missing(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        resolve_raw_file(tmp_path / "nope.raw")


def test_resolve_raw_file_empty_dir(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError, match="no .raw files"):
        resolve_raw_file(tmp_path)


def test_validate_ok(synthetic_raw: Path) -> None:
    assert validate_psse_case({"input": synthetic_raw}) is True


def test_validate_missing_input() -> None:
    assert validate_psse_case({}) is False


def test_validate_bad_file(tmp_path: Path) -> None:
    bad = tmp_path / "bad.raw"
    bad.write_text("nonsense\n", encoding="latin-1")
    assert validate_psse_case({"input": bad}) is False


def test_convert_default_output(synthetic_raw: Path, tmp_path: Path) -> None:
    out = tmp_path / "out"
    rc = convert_psse_case({"input": synthetic_raw, "output_dir": out})
    assert rc == 0
    planning_file = out / "out.json"
    assert planning_file.is_file()
    data = json.loads(planning_file.read_text(encoding="utf-8"))
    assert data["system"]["name"] == "synthetic"


def test_convert_missing_input_raises() -> None:
    with pytest.raises(ValueError, match="'input' option is required"):
        convert_psse_case({})


def test_cli_info(synthetic_raw: Path, capsys: pytest.CaptureFixture[str]) -> None:
    main(["--info", str(synthetic_raw)])
    out = capsys.readouterr().out
    assert "PSS/E RAW case" in out
    assert "buses" in out


def test_cli_validate_exit_zero(synthetic_raw: Path) -> None:
    with pytest.raises(SystemExit) as exc:
        main(["--validate", str(synthetic_raw)])
    assert exc.value.code == 0


def test_cli_validate_exit_one(tmp_path: Path) -> None:
    bad = tmp_path / "bad.raw"
    bad.write_text("nope\n", encoding="latin-1")
    with pytest.raises(SystemExit) as exc:
        main(["--validate", str(bad)])
    assert exc.value.code == 1


def test_cli_convert(synthetic_raw: Path, tmp_path: Path) -> None:
    out = tmp_path / "cli_out"
    main(["-i", str(synthetic_raw), "-o", str(out), "-l", "ERROR"])
    assert (out / "cli_out.json").is_file()


def test_cli_convert_single_bus(synthetic_raw: Path, tmp_path: Path) -> None:
    out = tmp_path / "cp_out"
    main(["-i", str(synthetic_raw), "-o", str(out), "--single-bus", "-l", "ERROR"])
    data = json.loads((out / "cp_out.json").read_text(encoding="utf-8"))
    assert data["options"]["model_options"]["use_single_bus"] is True
    assert "line_array" not in data["system"]


def test_cli_rating_set(synthetic_raw: Path, tmp_path: Path) -> None:
    out = tmp_path / "rs_out"
    main(["-i", str(synthetic_raw), "-o", str(out), "--rating-set", "C", "-l", "ERROR"])
    data = json.loads((out / "rs_out.json").read_text(encoding="utf-8"))
    line = next(ln for ln in data["system"]["line_array"] if ln["name"] == "l1_2_1")
    assert line["tmax_ab"] == 300.0


def test_cli_nomenclatura_and_ldm(synthetic_raw: Path, tmp_path: Path) -> None:
    import openpyxl  # pylint: disable=import-outside-toplevel

    nom = tmp_path / "nom.xlsx"
    wb = openpyxl.Workbook()
    wb.active.append(["GEN", "Generador"])
    wb.active.append(["SLACK", "Referencia"])
    wb.save(nom)

    ldm = tmp_path / "ldm.xlsx"
    wb2 = openpyxl.Workbook()
    wb2.active.append(["Nemo", "Planta Generadora", "Potencia Disponible"])
    wb2.active.append(["GEN-PV", "Gen PV", 100.0])
    wb2.save(ldm)

    out = tmp_path / "enh_out"
    main(
        [
            "-i",
            str(synthetic_raw),
            "-o",
            str(out),
            "--nomenclatura",
            str(nom),
            "--ldm",
            str(ldm),
            "-l",
            "ERROR",
        ]
    )
    data = json.loads((out / "enh_out.json").read_text(encoding="utf-8"))
    names = {b["name"] for b in data["system"]["bus_array"]}
    assert "Generador-PV" in names
    gens = {g["name"]: g for g in data["system"]["generator_array"]}
    assert gens["g2_1"]["gcost"] == 10.0  # matched rank 0


def test_cli_no_input_errors() -> None:
    with pytest.raises(SystemExit) as exc:
        main([])
    assert exc.value.code != 0
