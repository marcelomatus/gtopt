"""Tests for igtopt.py – Excel → gtopt JSON conversion."""

import json
import pathlib

import pytest

# The igtopt_c0 case (xlsx) and the reference JSON live in scripts/cases/
_SCRIPTS_DIR = pathlib.Path(__file__).parent.parent.parent
_C0_XLSX = _SCRIPTS_DIR / "cases" / "igtopt_c0" / "system_c0.xlsx"
_C0_REF_JSON = _SCRIPTS_DIR / "cases" / "json_c0" / "system_c0.json"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _run_igtopt(xlsx: pathlib.Path, tmp_path: pathlib.Path, extra_args=None):
    """Run igtopt._run() programmatically and return the parsed JSON dict."""
    import argparse
    from igtopt.igtopt import _run

    json_out = tmp_path / (xlsx.stem + ".json")
    input_dir = tmp_path / xlsx.stem

    args = argparse.Namespace(
        filenames=[str(xlsx)],
        json_file=json_out,
        input_directory=input_dir,
        input_format="parquet",
        name=xlsx.stem,
        compression="gzip",
        skip_nulls=True,
        parse_unexpected_sheets=False,
    )
    if extra_args:
        for k, v in extra_args.items():
            setattr(args, k, v)

    rc = _run(args)
    assert rc == 0, "igtopt._run() returned non-zero"
    assert json_out.exists(), f"JSON output not created: {json_out}"
    return json.loads(json_out.read_text())


# ---------------------------------------------------------------------------
# Basic smoke test
# ---------------------------------------------------------------------------


@pytest.mark.skipif(not _C0_XLSX.exists(), reason="system_c0.xlsx not present")
def test_igtopt_c0_produces_valid_json(tmp_path):
    data = _run_igtopt(_C0_XLSX, tmp_path)
    assert isinstance(data, dict)


@pytest.mark.skipif(not _C0_XLSX.exists(), reason="system_c0.xlsx not present")
def test_igtopt_c0_has_options(tmp_path):
    data = _run_igtopt(_C0_XLSX, tmp_path)
    assert "options" in data
    opts = data["options"]
    assert "input_directory" in opts
    assert "input_format" in opts


# ---------------------------------------------------------------------------
# Structure matches reference json_c0
# ---------------------------------------------------------------------------


@pytest.mark.skipif(
    not _C0_XLSX.exists() or not _C0_REF_JSON.exists(),
    reason="xlsx or reference JSON not present",
)
def test_igtopt_c0_simulation_structure(tmp_path):
    """Block and stage arrays must match the reference json_c0."""
    data = _run_igtopt(_C0_XLSX, tmp_path)
    ref = json.loads(_C0_REF_JSON.read_text())

    # simulation section
    sim = data.get("simulation", data)
    ref_sim = ref.get("simulation", ref)

    for key in ("block_array", "stage_array"):
        if key in ref_sim:
            assert key in sim, f"Missing key '{key}' in igtopt output"
            assert len(sim[key]) == len(ref_sim[key]), (
                f"Length mismatch for '{key}': got {len(sim[key])}, "
                f"expected {len(ref_sim[key])}"
            )


@pytest.mark.skipif(
    not _C0_XLSX.exists() or not _C0_REF_JSON.exists(),
    reason="xlsx or reference JSON not present",
)
def test_igtopt_c0_system_structure(tmp_path):
    """Bus, generator and demand arrays must match the reference."""
    data = _run_igtopt(_C0_XLSX, tmp_path)
    ref = json.loads(_C0_REF_JSON.read_text())

    sys = data.get("system", data)
    ref_sys = ref.get("system", ref)

    for key in ("bus_array", "generator_array", "demand_array"):
        if key in ref_sys:
            assert key in sys, f"Missing key '{key}' in igtopt output"
            assert len(sys[key]) == len(ref_sys[key]), (
                f"Length mismatch for '{key}': got {len(sys[key])}, "
                f"expected {len(ref_sys[key])}"
            )


@pytest.mark.skipif(
    not _C0_XLSX.exists() or not _C0_REF_JSON.exists(),
    reason="xlsx or reference JSON not present",
)
def test_igtopt_c0_parquet_files_written(tmp_path):
    """Sheets containing '@' must produce parquet files in input_directory."""
    ref = json.loads(_C0_REF_JSON.read_text())
    # Only run if reference has an input_directory with known parquet files
    opts = ref.get("options", {})
    if "input_directory" not in opts:
        pytest.skip("no input_directory in reference options")

    data = _run_igtopt(_C0_XLSX, tmp_path)
    input_dir = tmp_path / "system_c0"
    # At least one .parquet file should have been created somewhere in input_dir
    parquet_files = list(input_dir.rglob("*.parquet"))
    assert len(parquet_files) > 0, "No parquet files written by igtopt"
