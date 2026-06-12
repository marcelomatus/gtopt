# SPDX-License-Identifier: BSD-3-Clause
"""Tests for gtopt_results_summary."""

from __future__ import annotations

import json

import pytest

from gtopt_results_summary import summarize_output_dict, summarize_results
from gtopt_results_summary.main import main as cli_main


def _sample_results() -> dict:
    """Build a minimal gtopt-style results dict."""
    return {
        "solution": {"obj_value": "5.0", "status": "0"},
        "outputs": {
            "Generator/generation_sol": {
                "columns": ["scenario", "stage", "block", "uid:1", "uid:2"],
                "data": [
                    [1, 1, 1, 100.0, 50.0],
                    [1, 1, 2, 120.0, 30.0],
                ],
            },
            "Generator/generation_cost": {
                "columns": ["scenario", "stage", "block", "uid:1", "uid:2"],
                "data": [
                    [1, 1, 1, 2000.0, 1750.0],
                    [1, 1, 2, 2400.0, 1050.0],
                ],
            },
            "Demand/load_sol": {
                "columns": ["scenario", "stage", "block", "uid:3"],
                "data": [
                    [1, 1, 1, 150.0],
                    [1, 1, 2, 150.0],
                ],
            },
            "Demand/fail_sol": {
                "columns": ["scenario", "stage", "block", "uid:3"],
                "data": [
                    [1, 1, 1, 0.0],
                    [1, 1, 2, 1.5],
                ],
            },
            "Bus/balance_dual": {
                "columns": ["scenario", "stage", "block", "uid:1", "uid:2"],
                "data": [
                    [1, 1, 1, 20.0, 22.0],
                    [1, 1, 2, 25.0, 30.0],
                ],
            },
            "Line/flowp_sol": {
                "columns": ["scenario", "stage", "block", "uid:1", "uid:2", "uid:3"],
                "data": [
                    [1, 1, 1, 10.0, 5.0, 3.0],
                    [1, 1, 2, 12.0, 7.0, 0.0],
                ],
            },
        },
    }


def test_summarize_basic():
    s = summarize_output_dict(_sample_results(), scale_objective=1000.0)
    assert s["status"] == "0"
    assert s["obj_value_raw"] == pytest.approx(5.0)
    assert s["obj_value"] == pytest.approx(5000.0)
    assert s["total_generation"] == pytest.approx(300.0)
    assert s["total_load"] == pytest.approx(300.0)
    assert s["total_unserved"] == pytest.approx(1.5)
    assert s["peak_unserved"] == pytest.approx(1.5)
    assert s["n_generators"] == 2
    assert s["n_buses"] == 2
    assert s["n_lines"] == 3
    assert s["n_blocks"] == 2
    assert s["lmp_min"] == pytest.approx(20.0)
    assert s["lmp_max"] == pytest.approx(30.0)
    assert s["lmp_mean"] == pytest.approx(24.25)


def test_summarize_with_tech_map():
    tech = {"1": "hydro", "2": "thermal"}
    s = summarize_output_dict(_sample_results(), tech_map=tech)
    assert s["generation_by_tech"]["hydro"] == pytest.approx(220.0)
    assert s["generation_by_tech"]["thermal"] == pytest.approx(80.0)


def test_summarize_empty():
    s = summarize_output_dict({"solution": {}, "outputs": {}})
    assert s["total_generation"] == 0.0
    assert s["obj_value"] is None
    assert s["lmp_min"] is None
    assert s["n_generators"] == 0


def test_summarize_results_dir_missing():
    with pytest.raises(FileNotFoundError):
        summarize_results("/nonexistent/path/xyz")


def test_summarize_results_wrong_type(tmp_path):
    fake = tmp_path / "data.txt"
    fake.write_text("hello")
    with pytest.raises(ValueError):
        summarize_results(fake)


def test_summarize_results_dir(tmp_path):
    outdir = tmp_path / "output"
    outdir.mkdir()
    # Write a trivial solution.csv and a single parquet/csv file
    (outdir / "solution.csv").write_text("obj_value,3.0\nstatus,0\n")
    gen_dir = outdir / "Generator"
    gen_dir.mkdir()
    (gen_dir / "generation_sol.csv").write_text(
        "scenario,stage,block,uid:1\n1,1,1,100.0\n1,1,2,200.0\n"
    )
    s = summarize_results(outdir, scale_objective=1.0)
    assert s["total_generation"] == pytest.approx(300.0)
    assert s["obj_value"] == pytest.approx(3.0)


def test_cli_pretty(tmp_path, capsys):
    outdir = tmp_path / "out"
    outdir.mkdir()
    (outdir / "solution.csv").write_text("obj_value,1.0\nstatus,0\n")
    code = cli_main([str(outdir), "--pretty"])
    assert code == 0
    captured = capsys.readouterr()
    summary = json.loads(captured.out)
    assert summary["obj_value_raw"] == pytest.approx(1.0)


def test_cli_with_tech_map(tmp_path, capsys):
    outdir = tmp_path / "out"
    outdir.mkdir()
    (outdir / "solution.csv").write_text("obj_value,1.0\nstatus,0\n")
    gen_dir = outdir / "Generator"
    gen_dir.mkdir()
    (gen_dir / "generation_sol.csv").write_text(
        "scenario,stage,block,uid:1,uid:2\n1,1,1,10.0,20.0\n"
    )
    tech_file = tmp_path / "tech.json"
    tech_file.write_text(json.dumps({"1": "hydro", "2": "thermal"}))
    code = cli_main([str(outdir), "--tech-map", str(tech_file)])
    assert code == 0
    captured = capsys.readouterr()
    summary = json.loads(captured.out)
    assert summary["generation_by_tech"]["hydro"] == pytest.approx(10.0)
    assert summary["generation_by_tech"]["thermal"] == pytest.approx(20.0)


# ---------------------------------------------------------------------------
# Additional coverage: ZIP file input
# ---------------------------------------------------------------------------


def test_summarize_results_zip(tmp_path):
    """summarize_results should accept a .zip file path."""
    import zipfile as zf

    zip_path = tmp_path / "output.zip"
    with zf.ZipFile(zip_path, "w") as z:
        z.writestr("solution.csv", "obj_value,2.0\nstatus,0\n")
        z.writestr(
            "Generator/generation_sol.csv",
            "scenario,stage,block,uid:1\n1,1,1,80.0\n1,1,2,40.0\n",
        )
    s = summarize_results(zip_path, scale_objective=1.0)
    assert s["total_generation"] == pytest.approx(120.0)
    assert s["obj_value"] == pytest.approx(2.0)


def test_summarize_results_zip_with_parquet(tmp_path):
    """ZIP file can also contain Parquet tables (skip if pyarrow missing)."""
    import io
    import zipfile as zf

    pytest.importorskip("pyarrow")
    import pyarrow as pa
    import pyarrow.parquet as pq

    # Write a parquet table of generation_sol into a ZIP
    tbl = pa.table(
        {
            "scenario": pa.array([1], type=pa.int32()),
            "stage": pa.array([1], type=pa.int32()),
            "block": pa.array([1], type=pa.int32()),
            "uid:5": pa.array([999.0], type=pa.float64()),
        }
    )
    buf = io.BytesIO()
    pq.write_table(tbl, buf)
    buf.seek(0)

    zip_path = tmp_path / "output.zip"
    with zf.ZipFile(zip_path, "w") as z:
        z.writestr("solution.csv", "obj_value,1.0\nstatus,0\n")
        z.writestr("Generator/generation_sol.parquet", buf.read())

    s = summarize_results(zip_path)
    assert s["total_generation"] == pytest.approx(999.0)


# ---------------------------------------------------------------------------
# Additional coverage: scale_objective edge cases
# ---------------------------------------------------------------------------


def test_scale_objective_zero_returns_none():
    """scale_objective=0 should not produce a division-by-zero."""
    r = {
        "solution": {"obj_value": "5.0", "status": "0"},
        "outputs": {},
    }
    s = summarize_output_dict(r, scale_objective=0)
    # obj_value_raw is always set regardless of scale_objective
    assert s["obj_value_raw"] == pytest.approx(5.0)


def test_scale_objective_negative():
    r = {"solution": {"obj_value": "-3.0", "status": "0"}, "outputs": {}}
    s = summarize_output_dict(r, scale_objective=-1.0)
    assert s["obj_value_raw"] == pytest.approx(-3.0)


# ---------------------------------------------------------------------------
# LMP statistics (balance_dual)
# ---------------------------------------------------------------------------


def test_lmp_stats_single_bus():
    r = {
        "solution": {},
        "outputs": {
            "Bus/balance_dual": {
                "columns": ["scenario", "stage", "block", "uid:1"],
                "data": [[1, 1, 1, 15.0], [1, 1, 2, 25.0]],
            }
        },
    }
    s = summarize_output_dict(r)
    assert s["lmp_min"] == pytest.approx(15.0)
    assert s["lmp_max"] == pytest.approx(25.0)
    assert s["lmp_mean"] == pytest.approx(20.0)


def test_lmp_stats_multi_bus():
    """With multiple buses, lmp_mean should average across all uid columns."""
    r = {
        "solution": {},
        "outputs": {
            "Bus/balance_dual": {
                "columns": ["scenario", "stage", "block", "uid:1", "uid:2"],
                "data": [[1, 1, 1, 10.0, 30.0]],
            }
        },
    }
    s = summarize_output_dict(r)
    assert s["lmp_min"] == pytest.approx(10.0)
    assert s["lmp_max"] == pytest.approx(30.0)
    assert s["lmp_mean"] == pytest.approx(20.0)


# ---------------------------------------------------------------------------
# Unserved energy stats
# ---------------------------------------------------------------------------


def test_unserved_energy_zero():
    r = {
        "solution": {},
        "outputs": {
            "Demand/fail_sol": {
                "columns": ["scenario", "stage", "block", "uid:1"],
                "data": [[1, 1, 1, 0.0], [1, 1, 2, 0.0]],
            }
        },
    }
    s = summarize_output_dict(r)
    assert s["total_unserved"] == pytest.approx(0.0)
    assert s["peak_unserved"] == pytest.approx(0.0)


def test_unserved_energy_nonzero():
    r = {
        "solution": {},
        "outputs": {
            "Demand/fail_sol": {
                "columns": ["scenario", "stage", "block", "uid:1", "uid:2"],
                "data": [[1, 1, 1, 5.0, 3.0], [1, 1, 2, 0.0, 0.0]],
            }
        },
    }
    s = summarize_output_dict(r)
    # total = sum of all cells across both uid columns
    assert s["total_unserved"] == pytest.approx(8.0)
    # peak = max single cell per uid column: uid:1 max=5.0, uid:2 max=3.0 → peak=5.0
    assert s["peak_unserved"] == pytest.approx(5.0)


# ---------------------------------------------------------------------------
# CLI: output via stdout (no -o flag in this CLI)
# ---------------------------------------------------------------------------


def test_cli_output_stdout(tmp_path, capsys):
    """CLI writes JSON summary to stdout; --pretty makes it human-readable."""
    outdir = tmp_path / "out"
    outdir.mkdir()
    (outdir / "solution.csv").write_text("obj_value,2.5\nstatus,0\n")
    code = cli_main([str(outdir), "--pretty"])
    assert code == 0
    captured = capsys.readouterr()
    d = json.loads(captured.out)
    assert d["obj_value_raw"] == pytest.approx(2.5)


def test_cli_raises_on_missing_path():
    """CLI propagates FileNotFoundError for a non-existent source."""
    with pytest.raises((FileNotFoundError, SystemExit)):
        cli_main(["/totally/nonexistent/path/xyz"])


# ---------------------------------------------------------------------------
# n_generators / n_buses / n_lines detection
# ---------------------------------------------------------------------------


def test_counts_from_generation_sol():
    r = {
        "solution": {},
        "outputs": {
            "Generator/generation_sol": {
                "columns": ["scenario", "stage", "block", "uid:1", "uid:2", "uid:3"],
                "data": [[1, 1, 1, 10.0, 20.0, 30.0]],
            },
            "Bus/balance_dual": {
                "columns": ["scenario", "stage", "block", "uid:10", "uid:11"],
                "data": [[1, 1, 1, 5.0, 7.0]],
            },
            "Line/flowp_sol": {
                "columns": ["scenario", "stage", "block", "uid:100"],
                "data": [[1, 1, 1, 0.5]],
            },
        },
    }
    s = summarize_output_dict(r)
    assert s["n_generators"] == 3
    assert s["n_buses"] == 2
    assert s["n_lines"] == 1
