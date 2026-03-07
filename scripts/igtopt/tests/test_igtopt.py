"""Tests for igtopt.py – Excel → gtopt JSON conversion."""

import argparse
import json
import logging
import pathlib
import zipfile

import pandas as pd
import pytest

from igtopt.igtopt import (
    _run as _igtopt_run,
    create_zip_output,
    df_to_str,
    log_conversion_stats,
)

# The igtopt_c0 case (xlsx) and the reference JSON live in scripts/cases/
_SCRIPTS_DIR = pathlib.Path(__file__).parent.parent.parent
_C0_XLSX = _SCRIPTS_DIR / "cases" / "igtopt_c0" / "system_c0.xlsx"
_C0_REF_JSON = _SCRIPTS_DIR / "cases" / "json_c0" / "system_c0.json"

# The igtopt_ieee57b case (xlsx) lives in scripts/cases/igtopt_ieee57b/
_IEEE57B_XLSX = _SCRIPTS_DIR / "cases" / "igtopt_ieee57b" / "ieee57b.xlsx"

# The igtopt_bat4b24 case (xlsx) lives in scripts/cases/igtopt_bat4b24/
_BAT4B24_XLSX = _SCRIPTS_DIR / "cases" / "igtopt_bat4b24" / "bat4b24.xlsx"


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _run_igtopt(xlsx: pathlib.Path, tmp_path: pathlib.Path, extra_args=None):
    """Run igtopt._run() programmatically and return the parsed JSON dict."""
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
        pretty=False,
        zip=False,
    )
    if extra_args:
        for k, v in extra_args.items():
            setattr(args, k, v)

    rc = _igtopt_run(args)
    assert rc == 0, "igtopt._run() returned non-zero"
    assert json_out.exists(), f"JSON output not created: {json_out}"
    return json.loads(json_out.read_text())


# ---------------------------------------------------------------------------
# Unit tests for df_to_str
# ---------------------------------------------------------------------------


def test_df_to_str_compact_format():
    """Compact format uses indent=0: produces newlines but no leading spaces."""
    df = pd.DataFrame([{"uid": 1, "name": "b1"}])
    result = df_to_str(df, skip_nulls=True)
    # With indent=0, json.dumps adds newlines but no space indentation.
    # Each line must have zero leading whitespace.
    lines = result.splitlines()
    assert all(not line.startswith(" ") for line in lines if line.strip())


def test_df_to_str_pretty_format():
    """Pretty format uses indented JSON."""
    from igtopt.igtopt import _PRETTY_INDENT, _PRETTY_SEPARATORS

    df = pd.DataFrame([{"uid": 1, "name": "b1"}])
    result = df_to_str(
        df, skip_nulls=True, indent=_PRETTY_INDENT, separators=_PRETTY_SEPARATORS
    )
    parsed = json.loads(result)
    assert parsed[0]["uid"] == 1
    assert parsed[0]["name"] == "b1"
    assert "\n" in result


def test_df_to_str_skip_nulls_drops_nan():
    """skip_nulls=True must drop NaN-valued keys from the output."""
    import numpy as np

    df = pd.DataFrame([{"uid": 1, "name": "g1", "pmax": np.nan}])
    result_skip = df_to_str(df.copy(), skip_nulls=True)
    parsed_skip = json.loads(result_skip)
    assert "pmax" not in parsed_skip[0]


def test_df_to_str_skip_dot_columns():
    """Columns whose name starts with '.' must be silently dropped."""
    df = pd.DataFrame([{"uid": 1, ".calc": "ignored"}])
    result = df_to_str(df, skip_nulls=True)
    parsed = json.loads(result)
    assert ".calc" not in parsed[0]
    assert parsed[0]["uid"] == 1


def test_df_to_str_independent_of_globals():
    """Two calls with different indent/separators must not interfere."""
    from igtopt.igtopt import _COMPACT_SEPARATORS, _PRETTY_INDENT, _PRETTY_SEPARATORS

    df = pd.DataFrame([{"uid": 1, "name": "x"}])
    pretty = df_to_str(
        df.copy(), skip_nulls=True, indent=_PRETTY_INDENT, separators=_PRETTY_SEPARATORS
    )
    compact = df_to_str(df.copy(), skip_nulls=True, separators=_COMPACT_SEPARATORS)
    # compact must be shorter (no whitespace padding)
    assert len(compact) < len(pretty)
    # both must parse to the same value
    assert json.loads(pretty) == json.loads(compact)


# ---------------------------------------------------------------------------
# Unit tests for log_conversion_stats
# ---------------------------------------------------------------------------


def test_log_conversion_stats_runs_without_error(caplog):
    """log_conversion_stats must not raise and must produce INFO messages."""
    counts = {
        "bus_array": 4,
        "generator_array": 3,
        "demand_array": 2,
        "line_array": 5,
        "batterie_array": 1,
        "block_array": 24,
        "stage_array": 1,
        "scenario_array": 1,
    }
    opts = {
        "use_single_bus": False,
        "scale_objective": 1000,
        "demand_fail_cost": 1000,
        "input_directory": "/tmp/x",
    }
    with caplog.at_level(logging.INFO):
        log_conversion_stats(counts, opts, elapsed=0.5)
    assert any("Buses" in r.message for r in caplog.records)
    assert any("Generators" in r.message for r in caplog.records)
    assert any("Elapsed" in r.message for r in caplog.records)


def test_log_conversion_stats_empty_counts(caplog):
    """log_conversion_stats must handle all-zero counts gracefully."""
    with caplog.at_level(logging.INFO):
        log_conversion_stats({}, {}, elapsed=0.0)
    assert any("Buses" in r.message for r in caplog.records)


# ---------------------------------------------------------------------------
# Unit tests for create_zip_output
# ---------------------------------------------------------------------------


def test_create_zip_output_bundles_json_and_data(tmp_path):
    """create_zip_output must create a ZIP with the JSON + data files."""
    json_path = tmp_path / "mycase.json"
    json_path.write_text('{"name":"test"}')

    input_dir = tmp_path / "mycase"
    (input_dir / "Demand").mkdir(parents=True)
    (input_dir / "Demand" / "lmax.parquet").write_text("fake")

    zip_path = create_zip_output(json_path, input_dir)

    assert zip_path == tmp_path / "mycase.zip"
    assert zip_path.exists()
    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()
    assert "mycase.json" in names
    assert "mycase/Demand/lmax.parquet" in names


def test_create_zip_output_no_data_dir(tmp_path):
    """create_zip_output must succeed when input_dir does not exist yet."""
    json_path = tmp_path / "empty.json"
    json_path.write_text("{}")
    input_dir = tmp_path / "nonexistent"

    zip_path = create_zip_output(json_path, input_dir)

    assert zip_path.exists()
    with zipfile.ZipFile(zip_path) as zf:
        assert "empty.json" in zf.namelist()


# ---------------------------------------------------------------------------
# Unit test: _run produces a ZIP when args.zip=True
# ---------------------------------------------------------------------------


@pytest.mark.skipif(not _C0_XLSX.exists(), reason="system_c0.xlsx not present")
def test_igtopt_c0_zip_flag_creates_archive(tmp_path):
    """Passing zip=True to _run must create a <stem>.zip archive."""
    _run_igtopt(_C0_XLSX, tmp_path, extra_args={"zip": True})
    zip_file = tmp_path / "system_c0.zip"
    assert zip_file.exists(), "ZIP archive not created when zip=True"
    with zipfile.ZipFile(zip_file) as zf:
        names = zf.namelist()
    assert "system_c0.json" in names


# ---------------------------------------------------------------------------
# Unit test: pretty formatting is independent per call
# ---------------------------------------------------------------------------


@pytest.mark.skipif(not _C0_XLSX.exists(), reason="system_c0.xlsx not present")
def test_igtopt_pretty_compact_independent(tmp_path):
    """Two _run() calls with different pretty settings must not interfere."""
    pretty_dir = tmp_path / "pretty"
    compact_dir = tmp_path / "compact"
    pretty_dir.mkdir()
    compact_dir.mkdir()

    json_pretty = pretty_dir / "system_c0.json"
    json_compact = compact_dir / "system_c0.json"

    for json_out, input_dir, is_pretty in [
        (json_pretty, pretty_dir / "system_c0", True),
        (json_compact, compact_dir / "system_c0", False),
    ]:
        args = argparse.Namespace(
            filenames=[str(_C0_XLSX)],
            json_file=json_out,
            input_directory=input_dir,
            input_format="parquet",
            name="system_c0",
            compression="gzip",
            skip_nulls=True,
            parse_unexpected_sheets=False,
            pretty=is_pretty,
            zip=False,
        )
        rc = _igtopt_run(args)
        assert rc == 0

    # Pretty JSON should be longer (indented)
    assert json_pretty.stat().st_size > json_compact.stat().st_size
    # Both must produce valid JSON with identical bus/generator/demand arrays
    data_pretty = json.loads(json_pretty.read_text())
    data_compact = json.loads(json_compact.read_text())
    for key in ("bus_array", "generator_array", "demand_array", "block_array"):
        assert data_pretty.get(key) == data_compact.get(key), (
            f"Mismatch in '{key}' between pretty and compact output"
        )


# ---------------------------------------------------------------------------
# Basic smoke tests – igtopt_c0
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

    _run_igtopt(_C0_XLSX, tmp_path)
    input_dir = tmp_path / "system_c0"
    # At least one .parquet file should have been created somewhere in input_dir
    parquet_files = list(input_dir.rglob("*.parquet"))
    assert len(parquet_files) > 0, "No parquet files written by igtopt"


# ---------------------------------------------------------------------------
# Integration tests – IEEE 57-bus
# ---------------------------------------------------------------------------


@pytest.mark.integration
@pytest.mark.skipif(not _IEEE57B_XLSX.exists(), reason="ieee57b.xlsx not present")
def test_igtopt_ieee57b_produces_valid_json(tmp_path):
    """igtopt must convert the IEEE 57-bus workbook to valid JSON."""
    data = _run_igtopt(_IEEE57B_XLSX, tmp_path)
    assert isinstance(data, dict)
    assert "bus_array" in data
    assert "generator_array" in data
    assert "demand_array" in data
    assert "line_array" in data
    assert "options" in data


@pytest.mark.integration
@pytest.mark.skipif(not _IEEE57B_XLSX.exists(), reason="ieee57b.xlsx not present")
def test_igtopt_ieee57b_element_counts(tmp_path):
    """IEEE 57-bus case must have exactly 57 buses, 7 generators, 42 demands, 80 lines."""
    data = _run_igtopt(_IEEE57B_XLSX, tmp_path)
    assert len(data["bus_array"]) == 57
    assert len(data["generator_array"]) == 7
    assert len(data["demand_array"]) == 42
    assert len(data["line_array"]) == 80


@pytest.mark.integration
@pytest.mark.skipif(not _IEEE57B_XLSX.exists(), reason="ieee57b.xlsx not present")
def test_igtopt_ieee57b_simulation_structure(tmp_path):
    """IEEE 57-bus case must have 1 block, 1 stage, 1 scenario (single-snapshot OPF)."""
    data = _run_igtopt(_IEEE57B_XLSX, tmp_path)
    sim = data.get("simulation", data)
    assert len(sim["block_array"]) == 1
    assert len(sim["stage_array"]) == 1
    assert len(sim["scenario_array"]) == 1


@pytest.mark.integration
@pytest.mark.skipif(not _IEEE57B_XLSX.exists(), reason="ieee57b.xlsx not present")
def test_igtopt_ieee57b_options(tmp_path):
    """IEEE 57-bus options must include use_kirchhoff and scale_objective."""
    data = _run_igtopt(_IEEE57B_XLSX, tmp_path)
    opts = data["options"]
    assert opts.get("use_kirchhoff") is True
    assert opts.get("scale_objective") == 1000
    assert opts.get("demand_fail_cost") == 1000


@pytest.mark.integration
@pytest.mark.skipif(not _IEEE57B_XLSX.exists(), reason="ieee57b.xlsx not present")
def test_igtopt_ieee57b_generator_fields(tmp_path):
    """All generators must have uid, name, bus, gcost, pmax, capacity fields."""
    data = _run_igtopt(_IEEE57B_XLSX, tmp_path)
    for gen in data["generator_array"]:
        for field in ("uid", "name", "bus", "gcost", "pmax", "capacity"):
            assert field in gen, f"Generator {gen.get('name', '?')} missing '{field}'"


@pytest.mark.integration
@pytest.mark.skipif(not _IEEE57B_XLSX.exists(), reason="ieee57b.xlsx not present")
def test_igtopt_ieee57b_line_fields(tmp_path):
    """All lines must have uid, name, bus_a, bus_b, reactance fields."""
    data = _run_igtopt(_IEEE57B_XLSX, tmp_path)
    for line in data["line_array"]:
        for field in ("uid", "name", "bus_a", "bus_b", "reactance"):
            assert field in line, f"Line {line.get('name', '?')} missing '{field}'"


# ---------------------------------------------------------------------------
# Integration tests – bat_4b_24 (4-bus, 24-block with time-series profiles)
# ---------------------------------------------------------------------------


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_produces_valid_json(tmp_path):
    """igtopt must convert the bat_4b_24 workbook to valid JSON."""
    data = _run_igtopt(_BAT4B24_XLSX, tmp_path)
    assert isinstance(data, dict)
    assert "bus_array" in data
    assert "generator_array" in data
    assert "batterie_array" in data
    assert "generator_profile_array" in data
    assert "options" in data


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_element_counts(tmp_path):
    """bat_4b_24 case must have 4 buses, 3 generators, 2 demands, 5 lines, 1 battery."""
    data = _run_igtopt(_BAT4B24_XLSX, tmp_path)
    assert len(data["bus_array"]) == 4
    assert len(data["generator_array"]) == 3
    assert len(data["demand_array"]) == 2
    assert len(data["line_array"]) == 5
    assert (
        len(data["batterie_array"]) == 1
    )  # igtopt uses 'batterie_array' (legacy spelling)
    assert len(data["generator_profile_array"]) == 1


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_24_block_simulation(tmp_path):
    """bat_4b_24 simulation must have 24 blocks in 1 stage."""
    data = _run_igtopt(_BAT4B24_XLSX, tmp_path)
    sim = data.get("simulation", data)
    assert len(sim["block_array"]) == 24
    assert len(sim["stage_array"]) == 1
    assert len(sim["scenario_array"]) == 1


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_battery_unified_definition(tmp_path):
    """The battery must use the unified definition (bus, pmax_charge, pmax_discharge)."""
    data = _run_igtopt(_BAT4B24_XLSX, tmp_path)
    bat = data["batterie_array"][0]
    assert bat["name"] == "bat1"
    assert bat["bus"] == "b3"
    assert bat["pmax_charge"] == pytest.approx(60)
    assert bat["pmax_discharge"] == pytest.approx(60)
    assert bat["emax"] == pytest.approx(200)
    assert bat["input_efficiency"] == pytest.approx(0.95)
    assert bat["output_efficiency"] == pytest.approx(0.95)


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_demand_lmax_parquet(tmp_path):
    """Demand@lmax sheet must create Demand/lmax.parquet with 24 rows."""
    import pyarrow.parquet as pq  # pylint: disable=import-outside-toplevel

    _run_igtopt(_BAT4B24_XLSX, tmp_path)
    lmax_path = tmp_path / "bat4b24" / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "Demand/lmax.parquet not created"

    table = pq.read_table(str(lmax_path))
    assert table.num_rows == 24, f"Expected 24 rows, got {table.num_rows}"
    # Must have scenario, stage, block index columns + one column per demand
    assert "scenario" in table.column_names
    assert "stage" in table.column_names
    assert "block" in table.column_names
    assert "d3" in table.column_names
    assert "d4" in table.column_names


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_demand_lmax_profile_values(tmp_path):
    """Demand lmax parquet must contain the correct 24-hour demand profiles."""
    import pyarrow.parquet as pq  # pylint: disable=import-outside-toplevel

    _run_igtopt(_BAT4B24_XLSX, tmp_path)
    lmax_path = tmp_path / "bat4b24" / "Demand" / "lmax.parquet"
    table = pq.read_table(str(lmax_path))
    d3_vals = table.column("d3").to_pylist()
    d4_vals = table.column("d4").to_pylist()

    # Reference values from bat_4b_24.json
    _D3_LMAX = [
        30,
        28,
        27,
        27,
        28,
        32,
        40,
        55,
        70,
        80,
        85,
        88,
        90,
        88,
        84,
        80,
        82,
        88,
        100,
        110,
        105,
        95,
        75,
        50,
    ]
    _D4_LMAX = [
        20,
        18,
        17,
        17,
        18,
        22,
        28,
        38,
        48,
        55,
        58,
        60,
        62,
        60,
        57,
        55,
        56,
        60,
        68,
        75,
        72,
        65,
        50,
        32,
    ]

    assert d3_vals == pytest.approx(_D3_LMAX)
    assert d4_vals == pytest.approx(_D4_LMAX)


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_generator_profile_parquet(tmp_path):
    """GeneratorProfile@profile sheet must create GeneratorProfile/profile.parquet."""
    import pyarrow.parquet as pq  # pylint: disable=import-outside-toplevel

    _run_igtopt(_BAT4B24_XLSX, tmp_path)
    profile_path = tmp_path / "bat4b24" / "GeneratorProfile" / "profile.parquet"
    assert profile_path.exists(), "GeneratorProfile/profile.parquet not created"

    table = pq.read_table(str(profile_path))
    assert table.num_rows == 24, f"Expected 24 rows, got {table.num_rows}"
    assert "block" in table.column_names
    assert "gp_solar" in table.column_names


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_solar_profile_values(tmp_path):
    """Solar profile parquet must match the reference 24-hour profile."""
    import pyarrow.parquet as pq  # pylint: disable=import-outside-toplevel

    _run_igtopt(_BAT4B24_XLSX, tmp_path)
    profile_path = tmp_path / "bat4b24" / "GeneratorProfile" / "profile.parquet"
    table = pq.read_table(str(profile_path))
    vals = table.column("gp_solar").to_pylist()

    # Reference profile from bat_4b_24.json
    _SOLAR_PROFILE = [
        0.00,
        0.00,
        0.00,
        0.00,
        0.00,
        0.00,
        0.05,
        0.15,
        0.35,
        0.55,
        0.75,
        0.90,
        1.00,
        0.95,
        0.85,
        0.70,
        0.50,
        0.30,
        0.10,
        0.02,
        0.00,
        0.00,
        0.00,
        0.00,
    ]
    assert vals == pytest.approx(_SOLAR_PROFILE, abs=1e-6)


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_demand_references_parquet_file(tmp_path):
    """Demand lmax field in the JSON must be a string (file reference), not inline values."""
    data = _run_igtopt(_BAT4B24_XLSX, tmp_path)
    for dem in data["demand_array"]:
        assert isinstance(dem["lmax"], str), (
            f"Demand '{dem['name']}' lmax should be a file reference string, "
            f"got {type(dem['lmax'])}"
        )


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_generator_profile_references_parquet_file(tmp_path):
    """GeneratorProfile profile field must be a string (file reference)."""
    data = _run_igtopt(_BAT4B24_XLSX, tmp_path)
    for gp in data["generator_profile_array"]:
        assert isinstance(gp["profile"], str), (
            f"GeneratorProfile '{gp['name']}' profile should be a file reference, "
            f"got {type(gp['profile'])}"
        )


@pytest.mark.integration
@pytest.mark.skipif(not _BAT4B24_XLSX.exists(), reason="bat4b24.xlsx not present")
def test_igtopt_bat4b24_zip_bundles_parquet_files(tmp_path):
    """ZIP output must contain the JSON and all Parquet data files."""
    _run_igtopt(_BAT4B24_XLSX, tmp_path, extra_args={"zip": True})
    zip_path = tmp_path / "bat4b24.zip"
    assert zip_path.exists(), "ZIP archive not created"

    with zipfile.ZipFile(zip_path) as zf:
        names = zf.namelist()
    assert "bat4b24.json" in names
    assert any("Demand/lmax.parquet" in n for n in names)
    assert any("GeneratorProfile/profile.parquet" in n for n in names)
