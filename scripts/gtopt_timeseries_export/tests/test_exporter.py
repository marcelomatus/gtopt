# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_timeseries_export."""

from __future__ import annotations


import openpyxl
import pytest

from gtopt_timeseries_export import export_from_dict, export_to_excel
from gtopt_timeseries_export.main import main as cli_main


def _sample_results() -> dict:
    return {
        "solution": {"obj_value": "2.5", "status": "0"},
        "outputs": {
            "Generator/generation_sol": {
                "columns": ["scenario", "stage", "block", "uid:1"],
                "data": [
                    [1, 1, 1, 10.0],
                    [1, 1, 2, 20.0],
                ],
            },
            "Demand/fail_sol": {
                "columns": ["scenario", "stage", "block", "uid:2"],
                "data": [
                    [1, 1, 1, 0.0],
                    [1, 1, 2, 1.0],
                ],
            },
        },
    }


def test_export_from_dict(tmp_path):
    out = tmp_path / "result.xlsx"
    path = export_from_dict(_sample_results(), out, scale_objective=1000.0)
    assert path == out
    assert out.exists()

    wb = openpyxl.load_workbook(out)
    assert "Overview" in wb.sheetnames
    # The Overview sheet must list at least Status and Objective metrics
    overview_cells = [
        (row[0].value, row[1].value if len(row) > 1 else None)
        for row in wb["Overview"].iter_rows(min_row=2)
    ]
    metrics = {k: v for (k, v) in overview_cells if k}
    assert "Status" in metrics
    assert "Objective (scaled)" in metrics
    assert "Total generation (MWh)" in metrics
    assert metrics["Objective (scaled)"] == pytest.approx(2500.0)

    # Per-output sheets are present
    sheet_names = [s.lower() for s in wb.sheetnames]
    assert any("generator" in s for s in sheet_names)
    assert any("demand" in s for s in sheet_names)


def test_export_to_excel_dir(tmp_path):
    # Create a minimal output directory
    outdir = tmp_path / "out"
    outdir.mkdir()
    (outdir / "solution.csv").write_text("obj_value,1.5\nstatus,0\n")
    gen_dir = outdir / "Generator"
    gen_dir.mkdir()
    (gen_dir / "generation_sol.csv").write_text(
        "scenario,stage,block,uid:1\n1,1,1,5.0\n"
    )
    dest = tmp_path / "wb.xlsx"
    result = export_to_excel(outdir, dest)
    assert result == dest
    assert dest.exists()


def test_export_nonexistent_source(tmp_path):
    with pytest.raises(FileNotFoundError):
        export_to_excel(tmp_path / "missing", tmp_path / "out.xlsx")


def test_export_wrong_suffix(tmp_path):
    fake = tmp_path / "data.txt"
    fake.write_text("hi")
    with pytest.raises(ValueError):
        export_to_excel(fake, tmp_path / "out.xlsx")


def test_cli(tmp_path, capsys):
    outdir = tmp_path / "out"
    outdir.mkdir()
    (outdir / "solution.csv").write_text("obj_value,1.0\nstatus,0\n")
    dest = tmp_path / "cli.xlsx"
    code = cli_main([str(outdir), "-o", str(dest)])
    assert code == 0
    assert dest.exists()
    captured = capsys.readouterr()
    assert str(dest) in captured.out


def test_sheet_name_sanitization(tmp_path):
    results = {
        "solution": {},
        "outputs": {
            "a/b:c?d*e[f]g\\h/i": {
                "columns": ["col"],
                "data": [[1]],
            },
        },
    }
    dest = tmp_path / "safe.xlsx"
    export_from_dict(results, dest)
    wb = openpyxl.load_workbook(dest)
    # None of the sheet names may contain Excel-invalid characters
    for name in wb.sheetnames:
        for bad in r":\/?*[]":
            assert bad not in name
