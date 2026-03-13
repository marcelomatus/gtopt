"""Integration tests for plp2gtopt using the plp_dat_ex and plp_case_2y sample cases."""

import json
import sys
from pathlib import Path
from unittest.mock import patch

import numpy as np
import pandas as pd
import pytest

from plp2gtopt.main import main
from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.plp2gtopt import convert_plp_case

# Path to the sample PLP case shipped with the repository
_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPDatEx = _CASES_DIR / "plp_dat_ex"
_PLPMin1Bus = _CASES_DIR / "plp_min_1bus"
_PLPMin2Bus = _CASES_DIR / "plp_min_2bus"
_PLPMinBess = _CASES_DIR / "plp_min_bess"
_PLPMinBattery = _CASES_DIR / "plp_min_battery"
_PLPMinEss = _CASES_DIR / "plp_min_ess"
# plp_case_2y — 2-year (51-stage) full PLP case with plpidsim/idap2/idape
_PLPCase2Y = _CASES_DIR / "plp_case_2y"
# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_opts(input_dir: Path, tmp_path: Path, case_name: str) -> dict:
    out_dir = tmp_path / case_name
    out_dir.mkdir(parents=True, exist_ok=True)
    return {
        "input_dir": input_dir,
        "output_dir": out_dir,
        "output_file": out_dir / f"{case_name}.json",
        "hydrologies": "1",
    }


# ---------------------------------------------------------------------------
# plp_dat_ex smoke tests (existing)
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_plp_parser_parses_dat_ex():
    """Integration test: PLPParser can parse all files in plp_dat_ex."""
    parser = PLPParser({"input_dir": _PLPDatEx})
    parser.parse_all()

    # All 12 parsers must have run
    expected_keys = [
        "block_parser",
        "stage_parser",
        "bus_parser",
        "line_parser",
        "central_parser",
        "demand_parser",
        "cost_parser",
        "mance_parser",
        "manli_parser",
        "aflce_parser",
        "extrac_parser",
        "manem_parser",
    ]
    for key in expected_keys:
        assert key in parser.parsed_data, f"Missing parser result: {key}"


@pytest.mark.integration
def test_plp_dat_ex_blocks_and_stages():
    """Integration test: plp_dat_ex has valid block and stage data."""
    parser = PLPParser({"input_dir": _PLPDatEx})
    parser.parse_all()

    blocks = parser.parsed_data["block_parser"].blocks
    stages = parser.parsed_data["stage_parser"].stages

    assert len(blocks) > 0, "No blocks parsed"
    assert len(stages) > 0, "No stages parsed"

    # Every block must have required fields with valid values
    for block in blocks:
        assert block["number"] > 0
        assert block["stage"] > 0
        assert block["duration"] > 0


@pytest.mark.integration
def test_plp_dat_ex_buses():
    """Integration test: plp_dat_ex has valid bus data."""
    parser = PLPParser({"input_dir": _PLPDatEx})
    parser.parse_all()

    buses = parser.parsed_data["bus_parser"]
    bus_list = buses.buses

    assert len(bus_list) > 0, "No buses parsed"
    for bus in bus_list:
        assert "name" in bus
        assert "number" in bus
        assert bus["number"] > 0


# ---------------------------------------------------------------------------
# plp_min_1bus – minimal single-bus thermal case
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_min_1bus_parse():
    """plp_min_1bus: all parsers load without error."""
    parser = PLPParser({"input_dir": _PLPMin1Bus})
    parser.parse_all()

    assert parser.parsed_data["bus_parser"].num_buses == 1
    assert parser.parsed_data["block_parser"].num_blocks == 1
    assert parser.parsed_data["stage_parser"].num_stages == 1
    assert parser.parsed_data["line_parser"].num_lines == 0

    centrals = parser.parsed_data["central_parser"]
    # Falla type is excluded from generator_array by CentralWriter; only
    # Thermal1 appears (1 termica).
    assert centrals.num_termicas == 1
    assert centrals.num_fallas == 1


@pytest.mark.integration
def test_min_1bus_conversion(tmp_path):
    """plp_min_1bus: convert_plp_case produces a well-formed gtopt JSON."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "plp_min_1bus")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys = data["system"]

    assert len(sys["bus_array"]) == 1
    assert sys["bus_array"][0]["name"] == "Bus1"

    assert len(sys["generator_array"]) == 1
    g = sys["generator_array"][0]
    assert g["name"] == "Thermal1"
    assert g["gcost"] == pytest.approx(50.0)
    assert g["pmax"] == pytest.approx(100.0)
    assert g["bus"] == 1

    assert len(sys["demand_array"]) == 1
    d = sys["demand_array"][0]
    assert d["bus"] == 1

    assert len(sys["line_array"]) == 0

    sim = data["simulation"]
    assert len(sim["block_array"]) == 1
    assert len(sim["stage_array"]) == 1
    assert sim["block_array"][0]["duration"] == pytest.approx(1.0)


@pytest.mark.integration
def test_min_1bus_lmax_parquet(tmp_path):
    """plp_min_1bus: lmax.parquet is written with correct demand values."""

    opts = _make_opts(_PLPMin1Bus, tmp_path, "plp_min_1bus")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    # Row for block 1, column uid:1
    assert "block" in df.columns
    uid_col = "uid:1"
    assert uid_col in df.columns
    row = df[df["block"] == 1]
    assert len(row) == 1
    assert float(row[uid_col].iloc[0]) == pytest.approx(80.0)


# ---------------------------------------------------------------------------
# plp_min_2bus – 2-bus thermal case with transmission line
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_min_2bus_parse():
    """plp_min_2bus: all parsers load without error."""
    parser = PLPParser({"input_dir": _PLPMin2Bus})
    parser.parse_all()

    assert parser.parsed_data["bus_parser"].num_buses == 2
    assert parser.parsed_data["block_parser"].num_blocks == 2
    assert parser.parsed_data["stage_parser"].num_stages == 2
    assert parser.parsed_data["line_parser"].num_lines == 1

    centrals = parser.parsed_data["central_parser"]
    assert centrals.num_termicas == 2
    assert centrals.num_fallas == 1


@pytest.mark.integration
def test_min_2bus_conversion(tmp_path):
    """plp_min_2bus: convert_plp_case produces a well-formed gtopt JSON."""
    opts = _make_opts(_PLPMin2Bus, tmp_path, "plp_min_2bus")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys = data["system"]

    assert len(sys["bus_array"]) == 2
    assert len(sys["generator_array"]) == 2
    assert len(sys["line_array"]) == 1
    assert len(sys["demand_array"]) == 2

    # Generators on correct buses
    gens = {g["name"]: g for g in sys["generator_array"]}
    assert gens["Thermal1"]["bus"] == 1
    assert gens["Thermal2"]["bus"] == 2
    assert gens["Thermal1"]["gcost"] == pytest.approx(30.0)
    assert gens["Thermal2"]["gcost"] == pytest.approx(60.0)

    # Line properties
    line = sys["line_array"][0]
    assert line["bus_a"] == 1
    assert line["bus_b"] == 2
    assert line["tmax_ab"] == pytest.approx(150.0)
    assert line["tmax_ba"] == pytest.approx(150.0)
    assert line["reactance"] == pytest.approx(10.0)

    # Two stages
    sim = data["simulation"]
    assert len(sim["stage_array"]) == 2
    assert len(sim["block_array"]) == 2


@pytest.mark.integration
def test_min_2bus_lmax_parquet(tmp_path):
    """plp_min_2bus: lmax.parquet has correct demand for both buses."""

    opts = _make_opts(_PLPMin2Bus, tmp_path, "plp_min_2bus")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns

    # Bus1 (uid:1) = 80 MW per block
    assert float(df[df["block"] == 1]["uid:1"].iloc[0]) == pytest.approx(80.0)
    # Bus2 (uid:2) = 120 MW per block
    assert float(df[df["block"] == 1]["uid:2"].iloc[0]) == pytest.approx(120.0)


@pytest.mark.integration
def test_min_2bus_cli_creates_output_dir(tmp_path):
    """plp_min_2bus: main() auto-creates a non-existent output dir (regression test)."""
    out_dir = tmp_path / "gtopt_min_2bus"
    out_file = out_dir / "gtopt_case.json"

    # out_dir is intentionally NOT pre-created to reproduce the original bug
    test_argv = [
        "plp2gtopt",
        "-i",
        str(_PLPMin2Bus),
        "-o",
        str(out_dir),
        "-f",
        str(out_file),
    ]
    with patch.object(sys, "argv", test_argv):
        main()

    assert out_file.exists()
    data = json.loads(out_file.read_text(encoding="utf-8"))
    assert len(data["system"]["bus_array"]) == 2
    assert len(data["system"]["line_array"]) == 1


# ---------------------------------------------------------------------------
# plp_min_bess – single-bus case with one BESS
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_min_bess_parse():
    """plp_min_bess: all parsers load without error and BESS is detected."""
    parser = PLPParser({"input_dir": _PLPMinBess})
    parser.parse_all()

    assert parser.parsed_data["bus_parser"].num_buses == 1
    assert parser.parsed_data["block_parser"].num_blocks == 1
    assert parser.parsed_data["stage_parser"].num_stages == 1

    battery = parser.parsed_data.get("battery_parser")
    assert battery is not None
    assert battery.num_batteries == 1
    assert battery.batteries[0]["name"] == "BESS1"


@pytest.mark.integration
def test_min_bess_conversion(tmp_path):
    """plp_min_bess: convert_plp_case produces unified battery (no separate conv/gen/dem)."""
    opts = _make_opts(_PLPMinBess, tmp_path, "plp_min_bess")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys = data["system"]

    # 1 battery – bat central BESS1 has number=2 in plpcnfce.dat → uid=2
    assert len(sys.get("battery_array", [])) == 1
    bat = sys["battery_array"][0]
    assert bat["uid"] == 2
    assert bat["name"] == "BESS1"
    assert bat["input_efficiency"] == pytest.approx(0.95)
    assert bat["output_efficiency"] == pytest.approx(0.95)
    assert bat["capacity"] == pytest.approx(200.0)  # emax from plpcenbat.dat
    assert "eini" not in bat  # eini not read from PLP files
    # Unified fields – expand_batteries() will auto-generate gen/dem/conv
    assert bat["bus"] == 1
    assert bat["pmax_discharge"] == pytest.approx(50.0)
    assert bat["pmax_charge"] == pytest.approx(50.0)
    assert bat["gcost"] == pytest.approx(0.0)

    # No converter – auto-generated at LP construction time
    assert "converter_array" not in sys

    # 1 generator: only thermal (battery gen auto-generated)
    gens = sys.get("generator_array", [])
    assert len(gens) == 1
    assert gens[0]["name"] == "Thermal1"

    # 1 demand: only thermal (battery dem auto-generated)
    dems = sys.get("demand_array", [])
    assert len(dems) == 1


@pytest.mark.integration
def test_min_bess_lmax_parquet(tmp_path):
    """plp_min_bess: lmax.parquet contains the thermal demand column."""

    opts = _make_opts(_PLPMinBess, tmp_path, "plp_min_bess")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns

    # Thermal demand column (bus uid = 1)
    assert "uid:1" in df.columns
    assert float(df[df["block"] == 1]["uid:1"].iloc[0]) == pytest.approx(80.0)


# ---------------------------------------------------------------------------
# plp_min_battery – single-bus case with one battery (plpcenbat.dat path)
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_min_battery_parse():
    """plp_min_battery: all parsers load without error and battery is detected."""
    parser = PLPParser({"input_dir": _PLPMinBattery})
    parser.parse_all()

    assert parser.parsed_data["bus_parser"].num_buses == 1
    assert parser.parsed_data["block_parser"].num_blocks == 1
    assert parser.parsed_data["stage_parser"].num_stages == 1

    battery = parser.parsed_data.get("battery_parser")
    assert battery is not None
    assert battery.num_batteries == 1
    assert battery.batteries[0]["name"] == "BESS1"

    # ESS file is absent → ess_parser must not be created
    assert "ess_parser" not in parser.parsed_data


@pytest.mark.integration
def test_min_battery_conversion(tmp_path):
    """plp_min_battery: convert_plp_case produces unified battery (no separate conv/gen/dem)."""
    opts = _make_opts(_PLPMinBattery, tmp_path, "plp_min_battery")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys = data["system"]

    # 1 battery – bat central BESS1 has number=2 in plpcnfce.dat → uid=2
    assert len(sys.get("battery_array", [])) == 1
    bat = sys["battery_array"][0]
    assert bat["uid"] == 2
    assert bat["name"] == "BESS1"
    assert bat["input_efficiency"] == pytest.approx(0.95)
    assert bat["output_efficiency"] == pytest.approx(0.95)
    assert bat["capacity"] == pytest.approx(200.0)  # emax from plpcenbat.dat
    assert "eini" not in bat  # eini not read from PLP files
    # Unified fields – expand_batteries() will auto-generate gen/dem/conv
    assert bat["bus"] == 1
    assert bat["pmax_discharge"] == pytest.approx(50.0)
    assert bat["pmax_charge"] == pytest.approx(50.0)
    assert bat["gcost"] == pytest.approx(0.0)

    # No converter – auto-generated at LP construction time
    assert "converter_array" not in sys

    # 1 generator: only thermal (battery gen auto-generated)
    gens = sys.get("generator_array", [])
    assert len(gens) == 1
    assert gens[0]["name"] == "Thermal1"

    # 1 demand: only thermal (battery dem auto-generated)
    dems = sys.get("demand_array", [])
    assert len(dems) == 1


@pytest.mark.integration
def test_min_battery_lmax_parquet(tmp_path):
    """plp_min_battery: lmax.parquet contains the thermal demand column."""
    opts = _make_opts(_PLPMinBattery, tmp_path, "plp_min_battery")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns

    # Thermal demand column (bus uid = 1)
    assert "uid:1" in df.columns
    assert float(df[df["block"] == 1]["uid:1"].iloc[0]) == pytest.approx(80.0)


# ---------------------------------------------------------------------------
# plp_min_ess – single-bus case with one ESS (plpess.dat path)
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_min_ess_parse():
    """plp_min_ess: all parsers load without error and ESS is detected."""
    parser = PLPParser({"input_dir": _PLPMinEss})
    parser.parse_all()

    assert parser.parsed_data["bus_parser"].num_buses == 1
    assert parser.parsed_data["block_parser"].num_blocks == 1

    ess = parser.parsed_data.get("ess_parser")
    assert ess is not None
    assert ess.num_esses == 1
    assert ess.esses[0]["name"] == "BESS1"

    # plpcenbat.dat is absent → battery_parser must not be created
    assert "battery_parser" not in parser.parsed_data


@pytest.mark.integration
def test_min_ess_conversion(tmp_path):
    """plp_min_ess: convert_plp_case produces unified battery (no separate conv/gen/dem)."""
    opts = _make_opts(_PLPMinEss, tmp_path, "plp_min_ess")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys = data["system"]

    # 1 battery – bat central BESS1 has number=2 in plpcnfce.dat → uid=2
    assert len(sys.get("battery_array", [])) == 1
    bat = sys["battery_array"][0]
    assert bat["uid"] == 2
    assert bat["name"] == "BESS1"
    assert bat["input_efficiency"] == pytest.approx(0.95)
    assert bat["output_efficiency"] == pytest.approx(0.95)
    # ESS capacity = emax from plpess.dat = 200.0 MWh
    assert bat["capacity"] == pytest.approx(200.0)
    assert "eini" not in bat  # eini not read from PLP files
    # ESS has no active restriction
    assert "active" not in bat
    # Unified fields – expand_batteries() will auto-generate gen/dem/conv
    assert bat["bus"] == 1
    assert bat["pmax_discharge"] == pytest.approx(50.0)  # dcmax from plpess.dat
    assert bat["pmax_charge"] == pytest.approx(50.0)
    assert bat["gcost"] == pytest.approx(0.0)

    # No converter – auto-generated at LP construction time
    assert "converter_array" not in sys

    # 1 generator: only thermal (ESS gen auto-generated)
    gens = sys.get("generator_array", [])
    assert len(gens) == 1
    assert gens[0]["name"] == "Thermal1"

    # 1 demand: only thermal (ESS dem auto-generated)
    dems = sys.get("demand_array", [])
    assert len(dems) == 1


@pytest.mark.integration
def test_min_ess_lmax_parquet(tmp_path):
    """plp_min_ess: lmax.parquet contains the thermal demand column."""
    opts = _make_opts(_PLPMinEss, tmp_path, "plp_min_ess")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns

    # Thermal demand column (bus uid = 1)
    assert "uid:1" in df.columns
    assert float(df[df["block"] == 1]["uid:1"].iloc[0]) == pytest.approx(80.0)


# ---------------------------------------------------------------------------
# plp_min_hydro – hydro pasada pura with 2-hydrology stochastic flow
# (dat files grounded in Fortran leeaflcen.f / leecnfce.f formats from
# the plp_storage reference repository)
# ---------------------------------------------------------------------------

_PLPMinHydro = _CASES_DIR / "plp_min_hydro"


@pytest.mark.integration
def test_min_hydro_parse():
    """plp_min_hydro: all parsers load, HydroGen detected as pasada pura."""
    parser = PLPParser({"input_dir": _PLPMinHydro})
    parser.parse_all()

    assert parser.parsed_data["bus_parser"].num_buses == 1
    assert parser.parsed_data["block_parser"].num_blocks == 2
    assert parser.parsed_data["stage_parser"].num_stages == 1
    assert parser.parsed_data["line_parser"].num_lines == 0

    cp = parser.parsed_data["central_parser"]
    assert cp.num_pasadas == 1
    assert cp.num_fallas == 1
    assert cp.num_termicas == 0

    hydro = cp.get_item_by_name("HydroGen")
    assert hydro is not None
    assert hydro["type"] == "pasada"
    assert hydro["pmax"] == pytest.approx(200.0)
    assert hydro["gcost"] == pytest.approx(0.0)

    aflce = parser.parsed_data["aflce_parser"]
    assert aflce.num_flows == 1
    assert aflce.flows[0]["name"] == "HydroGen"
    # 2 hydrologies encoded in the flow arrays
    assert len(aflce.flows[0]["flow"]) == 2


@pytest.mark.integration
def test_min_hydro_conversion_two_scenarios(tmp_path):
    """plp_min_hydro: convert with 2 hydrologies produces 2 balanced scenarios."""
    opts = _make_opts(_PLPMinHydro, tmp_path, "plp_min_hydro")
    opts["hydrologies"] = "1,2"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]
    scenarios = sim["scenario_array"]

    assert len(scenarios) == 2
    # Equal probability when only hydrologies given (no explicit probability_factors)
    assert scenarios[0]["probability_factor"] == pytest.approx(0.5)
    assert scenarios[1]["probability_factor"] == pytest.approx(0.5)
    assert scenarios[0]["hydrology"] == 0
    assert scenarios[1]["hydrology"] == 1


@pytest.mark.integration
def test_min_hydro_json_structure(tmp_path):
    """plp_min_hydro: JSON output has correct generator and profile arrays."""
    opts = _make_opts(_PLPMinHydro, tmp_path, "plp_min_hydro")
    opts["hydrologies"] = "1,2"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys = data["system"]

    # Hydro generator (no falla exported)
    assert len(sys["generator_array"]) == 1
    g = sys["generator_array"][0]
    assert g["name"] == "HydroGen"
    assert g["type"] == "pasada"
    assert g["gcost"] == pytest.approx(0.0)
    assert g["pmax"] == pytest.approx(200.0)
    assert g["bus"] == 1

    # generator_profile_array points to the afluent parquet
    profiles = sys.get("generator_profile_array", [])
    assert len(profiles) == 1
    assert profiles[0]["name"] == "HydroGen"
    assert profiles[0]["generator"] == 1
    assert profiles[0]["profile"] == "Afluent@afluent"


@pytest.mark.integration
def test_min_hydro_afluent_parquet(tmp_path):
    """plp_min_hydro: Afluent/afluent.parquet contains normalised per-scenario flows."""

    opts = _make_opts(_PLPMinHydro, tmp_path, "plp_min_hydro")
    opts["hydrologies"] = "1,2"
    convert_plp_case(opts)

    afluent_path = Path(opts["output_dir"]) / "Afluent" / "afluent.parquet"
    assert afluent_path.exists(), "Afluent/afluent.parquet not written"

    df = pd.read_parquet(afluent_path)
    assert "scenario" in df.columns
    assert "block" in df.columns
    assert "uid:1" in df.columns  # HydroGen uid=1

    # 2 scenarios × 2 blocks = 4 rows
    assert len(df) == 4

    # Values are normalised capacity factors: flow / pmax (pmax=200)
    # Hydrology 0: block1→50/200=0.25, block2→55/200=0.275
    # Hydrology 1: block1→80/200=0.40, block2→85/200=0.425
    # Check all four expected normalised values are present.
    values = sorted(df["uid:1"].tolist())
    assert values[0] == pytest.approx(50.0 / 200.0)  # 0.25 (hyd0 b1)
    assert values[1] == pytest.approx(55.0 / 200.0)  # 0.275 (hyd0 b2)
    assert values[2] == pytest.approx(80.0 / 200.0)  # 0.40 (hyd1 b1)
    assert values[3] == pytest.approx(85.0 / 200.0)  # 0.425 (hyd1 b2)


# ---------------------------------------------------------------------------
# plp_min_mance – 1-bus thermal with generator maintenance
# (dat files grounded in Fortran leemance.f / leecnfce.f formats from
# the plp_storage reference repository)
# ---------------------------------------------------------------------------

_PLPMinMance = _CASES_DIR / "plp_min_mance"


@pytest.mark.integration
def test_min_mance_parse():
    """plp_min_mance: ManceParser finds ThermalMant with 2 maintenance blocks."""
    parser = PLPParser({"input_dir": _PLPMinMance})
    parser.parse_all()

    mance = parser.parsed_data["mance_parser"]
    assert mance.num_mances == 1

    m = mance.mances[0]
    assert m["name"] == "ThermalMant"
    assert len(m["block"]) == 2

    np.testing.assert_array_almost_equal(m["pmin"], [10.0, 20.0])
    np.testing.assert_array_almost_equal(m["pmax"], [90.0, 70.0])


@pytest.mark.integration
def test_min_mance_conversion(tmp_path):
    """plp_min_mance: ThermalMant JSON generator references pmin/pmax parquet."""
    opts = _make_opts(_PLPMinMance, tmp_path, "plp_min_mance")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys = data["system"]

    assert len(sys["generator_array"]) == 1
    g = sys["generator_array"][0]
    assert g["name"] == "ThermalMant"
    # Generator with active maintenance references parquet column names
    assert g["pmin"] == "pmin"
    assert g["pmax"] == "pmax"
    assert g["gcost"] == pytest.approx(40.0)


@pytest.mark.integration
def test_min_mance_pmin_pmax_parquet(tmp_path):
    """plp_min_mance: Generator/pmin.parquet and pmax.parquet have block-level values."""

    opts = _make_opts(_PLPMinMance, tmp_path, "plp_min_mance")
    convert_plp_case(opts)

    pmin_path = Path(opts["output_dir"]) / "Generator" / "pmin.parquet"
    pmax_path = Path(opts["output_dir"]) / "Generator" / "pmax.parquet"
    assert pmin_path.exists(), "Generator/pmin.parquet not written"
    assert pmax_path.exists(), "Generator/pmax.parquet not written"

    df_pmin = pd.read_parquet(pmin_path)
    df_pmax = pd.read_parquet(pmax_path)

    # ThermalMant uid=1
    assert "uid:1" in df_pmin.columns
    assert "uid:1" in df_pmax.columns
    assert len(df_pmin) == 2  # 2 blocks
    assert len(df_pmax) == 2

    # block 1: pmin=10, pmax=90
    row1_pmin = df_pmin[df_pmin["block"] == 1]
    row1_pmax = df_pmax[df_pmax["block"] == 1]
    assert float(row1_pmin["uid:1"].iloc[0]) == pytest.approx(10.0)
    assert float(row1_pmax["uid:1"].iloc[0]) == pytest.approx(90.0)

    # block 2: pmin=20, pmax=70
    row2_pmin = df_pmin[df_pmin["block"] == 2]
    row2_pmax = df_pmax[df_pmax["block"] == 2]
    assert float(row2_pmin["uid:1"].iloc[0]) == pytest.approx(20.0)
    assert float(row2_pmax["uid:1"].iloc[0]) == pytest.approx(70.0)


# ---------------------------------------------------------------------------
# plp_min_manli – 2-bus case with line maintenance
# (dat files grounded in Fortran leemanli.f / leecnfli.f formats from
# the plp_storage reference repository)
# ---------------------------------------------------------------------------

_PLPMinManli = _CASES_DIR / "plp_min_manli"


@pytest.mark.integration
def test_min_manli_parse():
    """plp_min_manli: ManliParser finds Bus1->Bus2 with 2 maintenance blocks."""
    parser = PLPParser({"input_dir": _PLPMinManli})
    parser.parse_all()

    manli = parser.parsed_data["manli_parser"]
    assert manli.num_manlis == 1

    m = manli.manlis[0]
    assert m["name"] == "Bus1->Bus2"
    assert len(m["block"]) == 2

    np.testing.assert_array_almost_equal(m["tmax_ab"], [50.0, 150.0])
    np.testing.assert_array_almost_equal(m["tmax_ba"], [50.0, 150.0])


@pytest.mark.integration
def test_min_manli_conversion(tmp_path):
    """plp_min_manli: line Bus1->Bus2 JSON references tmax_ab/tmax_ba parquet."""
    opts = _make_opts(_PLPMinManli, tmp_path, "plp_min_manli")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys = data["system"]

    assert len(sys["line_array"]) == 1
    line = sys["line_array"][0]
    assert line["name"] == "Bus1->Bus2"
    # Line with active maintenance references parquet column names
    assert line["tmax_ab"] == "tmax_ab"
    assert line["tmax_ba"] == "tmax_ba"
    assert line["bus_a"] == 1
    assert line["bus_b"] == 2
    assert line["reactance"] == pytest.approx(10.0)


@pytest.mark.integration
def test_min_manli_tmax_parquet(tmp_path):
    """plp_min_manli: Line/tmax_ab.parquet has block-level capacity values."""

    opts = _make_opts(_PLPMinManli, tmp_path, "plp_min_manli")
    convert_plp_case(opts)

    tmax_ab = Path(opts["output_dir"]) / "Line" / "tmax_ab.parquet"
    tmax_ba = Path(opts["output_dir"]) / "Line" / "tmax_ba.parquet"
    active_p = Path(opts["output_dir"]) / "Line" / "active.parquet"
    assert tmax_ab.exists(), "Line/tmax_ab.parquet not written"
    assert tmax_ba.exists(), "Line/tmax_ba.parquet not written"
    assert active_p.exists(), "Line/active.parquet not written"

    df = pd.read_parquet(tmax_ab)
    # Line Bus1->Bus2 uid=1
    assert "uid:1" in df.columns
    assert len(df) == 2  # 2 blocks

    # block 1: tmax reduced to 50 MW
    row1 = df[df["block"] == 1]
    assert float(row1["uid:1"].iloc[0]) == pytest.approx(50.0)

    # block 2: tmax restored to 150 MW
    row2 = df[df["block"] == 2]
    assert float(row2["uid:1"].iloc[0]) == pytest.approx(150.0)


@pytest.mark.integration
def test_min_manli_generators_unaffected(tmp_path):
    """plp_min_manli: generators (no maintenance) keep numeric pmin/pmax."""
    opts = _make_opts(_PLPMinManli, tmp_path, "plp_min_manli")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    gens = {g["name"]: g for g in data["system"]["generator_array"]}

    assert gens["Thermal1"]["pmax"] == pytest.approx(200.0)
    assert gens["Thermal2"]["pmax"] == pytest.approx(100.0)
    assert isinstance(gens["Thermal1"]["pmax"], float)


@pytest.mark.integration
def test_min_manli_lmax_parquet(tmp_path):
    """plp_min_manli: lmax.parquet has correct demand for both buses and stages."""

    opts = _make_opts(_PLPMinManli, tmp_path, "plp_min_manli")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "Demand/lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns
    assert "uid:1" in df.columns
    assert "uid:2" in df.columns

    assert float(df[df["block"] == 1]["uid:1"].iloc[0]) == pytest.approx(80.0)
    assert float(df[df["block"] == 1]["uid:2"].iloc[0]) == pytest.approx(120.0)


# ---------------------------------------------------------------------------
# plp_min_reservoir – single-bus hydro reservoir (embalse) with turbine and
# flow.  Validates the full hydro system: junction, waterway, reservoir,
# turbine, and flow arrays.
# ---------------------------------------------------------------------------

_PLPMinReservoir = _CASES_DIR / "plp_min_reservoir"


@pytest.mark.integration
def test_min_reservoir_parse():
    """plp_min_reservoir: parser loads embalse + serie + falla centrals."""
    parser = PLPParser({"input_dir": _PLPMinReservoir})
    parser.parse_all()

    cp = parser.parsed_data["central_parser"]
    assert cp.num_embalses == 1
    assert cp.num_series == 1
    assert cp.num_fallas == 1

    embalse = cp.get_item_by_name("Reservoir1")
    assert embalse is not None
    assert embalse["type"] == "embalse"
    assert embalse["pmax"] == pytest.approx(100.0)
    assert embalse["emin"] == pytest.approx(100.0)
    assert embalse["emax"] == pytest.approx(1000.0)
    assert embalse["vol_ini"] == pytest.approx(500.0)
    assert embalse["vol_fin"] == pytest.approx(400.0)


@pytest.mark.integration
def test_min_reservoir_conversion(tmp_path):
    """plp_min_reservoir: JSON output has reservoir, junction, waterway, turbine, flow."""
    opts = _make_opts(_PLPMinReservoir, tmp_path, "plp_min_reservoir")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys_data = data["system"]

    # Reservoir
    reservoirs = sys_data.get("reservoir_array", [])
    assert len(reservoirs) == 1
    rsv = reservoirs[0]
    assert rsv["name"] == "Reservoir1"
    assert rsv["junction"] == 1
    assert rsv["emin"] == pytest.approx(100.0)
    assert rsv["emax"] == pytest.approx(1000.0)
    assert rsv["capacity"] == pytest.approx(1000.0)
    assert rsv["eini"] == pytest.approx(500.0)
    assert rsv["efin"] == pytest.approx(400.0)
    assert rsv["flow_conversion_rate"] == pytest.approx(3.6 / 1000.0)

    # Junctions (embalse + serie)
    junctions = sys_data.get("junction_array", [])
    assert len(junctions) == 2
    j_names = {j["name"] for j in junctions}
    assert "Reservoir1" in j_names
    assert "TurbineGen" in j_names
    # Embalse junction is not a drain
    rsv_j = next(j for j in junctions if j["name"] == "Reservoir1")
    assert rsv_j["drain"] is False
    # Serie without downstream is a drain
    turb_j = next(j for j in junctions if j["name"] == "TurbineGen")
    assert turb_j["drain"] is True

    # Waterway connects embalse → serie
    waterways = sys_data.get("waterway_array", [])
    assert len(waterways) == 1
    ww = waterways[0]
    assert ww["junction_a"] == 1  # Reservoir1
    assert ww["junction_b"] == 2  # TurbineGen

    # Turbine links waterway to generator
    turbines = sys_data.get("turbine_array", [])
    assert len(turbines) == 1
    assert turbines[0]["generator"] == 1  # Reservoir1 uid
    assert turbines[0]["waterway"] == ww["uid"]

    # Flow (afluent) is a parquet reference
    flows = sys_data.get("flow_array", [])
    assert len(flows) == 1
    assert flows[0]["junction"] == 1
    assert flows[0]["discharge"] == "Afluent@afluent"

    # Generators: embalse + serie (no falla)
    gens = sys_data.get("generator_array", [])
    assert len(gens) == 2
    gen_names = {g["name"] for g in gens}
    assert "Reservoir1" in gen_names
    assert "TurbineGen" in gen_names
    assert "Falla1" not in gen_names

    # Generator types preserved
    rsv_g = next(g for g in gens if g["name"] == "Reservoir1")
    assert rsv_g["type"] == "embalse"
    turb_g = next(g for g in gens if g["name"] == "TurbineGen")
    assert turb_g["type"] == "serie"


@pytest.mark.integration
def test_min_reservoir_afluent_parquet(tmp_path):
    """plp_min_reservoir: Afluent/afluent.parquet contains reservoir inflows."""
    opts = _make_opts(_PLPMinReservoir, tmp_path, "plp_min_reservoir")
    convert_plp_case(opts)

    afluent_path = Path(opts["output_dir"]) / "Afluent" / "afluent.parquet"
    assert afluent_path.exists(), "Afluent/afluent.parquet not written"

    df = pd.read_parquet(afluent_path)
    assert "block" in df.columns
    assert "uid:1" in df.columns  # Reservoir1 uid=1

    # 1 scenario × 2 blocks = 2 rows
    assert len(df) == 2

    # Embalse afluent values are raw water inflows (not normalised)
    values = sorted(df["uid:1"].tolist())
    assert values[0] == pytest.approx(20.0)
    assert values[1] == pytest.approx(25.0)


# ---------------------------------------------------------------------------
# plp_bat_4b_24 – 4-bus 24-block case with battery and solar profile
# ---------------------------------------------------------------------------

_PLPBat4b24 = _CASES_DIR / "plp_bat_4b_24"


@pytest.mark.integration
def test_bat_4b_24_parse():
    """plp_bat_4b_24: all parsers load without error, 4 buses, 24 blocks, 5 lines."""
    parser = PLPParser({"input_dir": _PLPBat4b24})
    parser.parse_all()

    assert parser.parsed_data["bus_parser"].num_buses == 4
    assert parser.parsed_data["block_parser"].num_blocks == 24
    assert parser.parsed_data["stage_parser"].num_stages == 1
    assert parser.parsed_data["line_parser"].num_lines == 5

    central = parser.parsed_data["central_parser"]
    assert central.num_termicas == 3  # g1, g2, g_solar (all thermal in PLP)
    assert central.num_fallas == 1
    assert central.num_baterias == 1  # BESS1

    mance = parser.parsed_data["mance_parser"]
    assert mance.num_mances == 1
    assert mance.mances[0]["name"] == "g_solar"
    assert len(mance.mances[0]["block"]) == 24


@pytest.mark.integration
def test_bat_4b_24_conversion(tmp_path):
    """plp_bat_4b_24: convert_plp_case produces 4 buses, 5 lines, BESS1 battery."""
    opts = _make_opts(_PLPBat4b24, tmp_path, "plp_bat_4b_24")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys = data["system"]

    # 4 buses
    assert len(sys["bus_array"]) == 4
    bus_names = {b["name"] for b in sys["bus_array"]}
    assert "b1" in bus_names
    assert "b4" in bus_names

    # 5 lines with expected reactances
    assert len(sys["line_array"]) == 5
    line_by_name = {ln["name"]: ln for ln in sys["line_array"]}
    assert line_by_name["l1_2"]["reactance"] == pytest.approx(0.02)
    assert line_by_name["l2_3"]["reactance"] == pytest.approx(0.03)
    assert line_by_name["l3_4"]["tmax_ab"] == pytest.approx(150.0)

    # 3 generators (battery discharge gen is auto-generated by gtopt LP assembly, not here)
    gens = sys["generator_array"]
    assert len(gens) == 3
    gen_names = {g["name"] for g in gens}
    assert "g1" in gen_names
    assert "g2" in gen_names
    assert "g_solar" in gen_names

    # 1 battery (unified: bus, pmax_charge, pmax_discharge)
    bats = sys.get("battery_array", [])
    assert len(bats) == 1
    bat = bats[0]
    assert bat["name"] == "BESS1"
    assert bat["bus"] == 3
    assert bat["pmax_discharge"] == pytest.approx(60.0)
    assert bat["pmax_charge"] == pytest.approx(60.0)
    assert bat["input_efficiency"] == pytest.approx(0.95)
    assert bat["output_efficiency"] == pytest.approx(0.95)
    assert bat["emax"] == pytest.approx(200.0)

    # 2 demands (d3 at b3, d4 at b4) — lmax stored in parquet
    dems = sys.get("demand_array", [])
    assert len(dems) == 2

    # Simulation: 24 blocks, 1 stage, 1 scenario
    sim = data["simulation"]
    assert len(sim["block_array"]) == 24
    assert len(sim["stage_array"]) == 1
    assert len(sim["scenario_array"]) == 1


@pytest.mark.integration
def test_bat_4b_24_pmax_parquet(tmp_path):
    """plp_bat_4b_24: Generator/pmax.parquet contains the solar profile."""
    opts = _make_opts(_PLPBat4b24, tmp_path, "plp_bat_4b_24")
    convert_plp_case(opts)

    pmax_path = Path(opts["output_dir"]) / "Generator" / "pmax.parquet"
    assert pmax_path.exists(), "Generator/pmax.parquet not written"

    df = pd.read_parquet(pmax_path)
    assert "block" in df.columns

    # g_solar is uid=3 (3rd central in plpcnfce.dat)
    assert "uid:3" in df.columns

    # Solar peaks at block 13 (profile factor 1.0 × 90 MW)
    peak_row = df[df["block"] == 13]
    assert float(peak_row["uid:3"].iloc[0]) == pytest.approx(90.0)

    # No generation at night (blocks 1–6, 21–24)
    night_rows = df[df["block"].isin([1, 2, 3, 4, 5, 6])]
    assert all(v == pytest.approx(0.0) for v in night_rows["uid:3"].tolist())


@pytest.mark.integration
def test_bat_4b_24_lmax_parquet(tmp_path):
    """plp_bat_4b_24: Demand/lmax.parquet contains per-block demand for b3 and b4."""
    opts = _make_opts(_PLPBat4b24, tmp_path, "plp_bat_4b_24")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "Demand/lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns

    # d3 (bus b3) and d4 (bus b4) – check peak values
    # d3 peak = 110 MW at block 20, d4 peak = 75 MW at block 20
    dem_cols = [c for c in df.columns if c.startswith("uid:")]
    assert len(dem_cols) == 2

    # Peak demand block 20
    peak_row = df[df["block"] == 20]
    peak_vals = sorted(float(peak_row[c].iloc[0]) for c in dem_cols)
    assert peak_vals[0] == pytest.approx(75.0)
    assert peak_vals[1] == pytest.approx(110.0)


# ---------------------------------------------------------------------------
# plp_min_hydro_ms – multi-stage hydro pasada pura with 2-hydrology stochastic
# flow. Tests both SDDP mode (one scene per scenario, one phase per stage) and
# monolithic mode (one scene for all scenarios, one phase for all stages).
# ---------------------------------------------------------------------------

_PLPMinHydroMs = _CASES_DIR / "plp_min_hydro_ms"


@pytest.mark.integration
def test_min_hydro_ms_parse():
    """plp_min_hydro_ms: all parsers load, 3 stages, 1 block each, 2 hydrologies."""
    parser = PLPParser({"input_dir": _PLPMinHydroMs})
    parser.parse_all()

    assert parser.parsed_data["bus_parser"].num_buses == 1
    assert parser.parsed_data["block_parser"].num_blocks == 3
    assert parser.parsed_data["stage_parser"].num_stages == 3
    assert parser.parsed_data["line_parser"].num_lines == 0

    cp = parser.parsed_data["central_parser"]
    assert cp.num_pasadas == 1
    assert cp.num_fallas == 1

    aflce = parser.parsed_data["aflce_parser"]
    assert aflce.num_flows == 1
    assert aflce.flows[0]["name"] == "HydroGen"
    # flow shape: (num_blocks, num_hydrologies) = (3, 2)
    assert aflce.flows[0]["flow"].shape == (3, 2)


@pytest.mark.integration
def test_min_hydro_ms_scene_phase_structure(tmp_path):
    """plp_min_hydro_ms: one scene per PLP scenario, one phase per PLP stage."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms")
    opts["hydrologies"] = "1,2"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]

    # 2 PLP scenarios → 2 gtopt scenarios with unique UIDs
    scenarios = sim["scenario_array"]
    assert len(scenarios) == 2
    assert scenarios[0]["uid"] == 1
    assert scenarios[1]["uid"] == 2
    assert scenarios[0]["hydrology"] == 0
    assert scenarios[1]["hydrology"] == 1

    # 2 PLP scenarios → 2 gtopt scenes (one per scenario)
    scenes = sim["scene_array"]
    assert len(scenes) == 2
    assert scenes[0]["uid"] == 1
    assert scenes[0]["first_scenario"] == 0
    assert scenes[0]["count_scenario"] == 1
    assert scenes[1]["uid"] == 2
    assert scenes[1]["first_scenario"] == 1
    assert scenes[1]["count_scenario"] == 1

    # 3 PLP stages → 3 gtopt stages
    stages = sim["stage_array"]
    assert len(stages) == 3

    # 3 PLP stages → 3 gtopt phases (one per stage)
    phases = sim["phase_array"]
    assert len(phases) == 3
    for i, phase in enumerate(phases):
        assert phase["first_stage"] == i
        assert phase["count_stage"] == 1


@pytest.mark.integration
def test_min_hydro_ms_afluent_parquet(tmp_path):
    """plp_min_hydro_ms: afluent parquet has unique scenario UIDs across 3 stages."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms")
    opts["hydrologies"] = "1,2"
    convert_plp_case(opts)

    afluent_path = Path(opts["output_dir"]) / "Afluent" / "afluent.parquet"
    assert afluent_path.exists(), "Afluent/afluent.parquet not written"

    df = pd.read_parquet(afluent_path)
    assert "scenario" in df.columns
    assert "stage" in df.columns
    assert "block" in df.columns
    assert "uid:1" in df.columns  # HydroGen uid=1

    # 2 scenarios × 3 blocks = 6 rows
    assert len(df) == 6

    # Scenario UIDs must be unique: 1 and 2 (not all 1)
    assert set(df["scenario"].unique()) == {1, 2}

    # Hydrology 0 (scenario 1): blocks 1,2,3 → 50/200, 55/200, 60/200
    s1 = df[df["scenario"] == 1].sort_values("block")
    assert s1["uid:1"].iloc[0] == pytest.approx(50.0 / 200.0)
    assert s1["uid:1"].iloc[1] == pytest.approx(55.0 / 200.0)
    assert s1["uid:1"].iloc[2] == pytest.approx(60.0 / 200.0)

    # Hydrology 1 (scenario 2): blocks 1,2,3 → 80/200, 85/200, 90/200
    s2 = df[df["scenario"] == 2].sort_values("block")
    assert s2["uid:1"].iloc[0] == pytest.approx(80.0 / 200.0)
    assert s2["uid:1"].iloc[1] == pytest.approx(85.0 / 200.0)
    assert s2["uid:1"].iloc[2] == pytest.approx(90.0 / 200.0)


@pytest.mark.integration
def test_min_hydro_ms_monolithic_structure(tmp_path):
    """plp_min_hydro_ms + --solver mono: one scene (all scenarios), one phase (all stages)."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms_mono")
    opts["hydrologies"] = "1,2"
    opts["solver_type"] = "mono"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]

    # sddp_solver_type in options must be "monolithic" (nested in sddp_options)
    assert data["options"]["sddp_options"]["sddp_solver_type"] == "monolithic"

    # 2 scenarios with equal probability
    scenarios = sim["scenario_array"]
    assert len(scenarios) == 2
    for s in scenarios:
        assert s["probability_factor"] == pytest.approx(0.5)

    # Monolithic: exactly one scene covering all scenarios
    scenes = sim["scene_array"]
    assert len(scenes) == 1
    assert scenes[0]["uid"] == 1
    assert scenes[0]["first_scenario"] == 0
    assert scenes[0]["count_scenario"] == 2

    # 3 PLP stages → 3 gtopt stages
    stages = sim["stage_array"]
    assert len(stages) == 3

    # Monolithic: exactly one phase covering all stages
    phases = sim["phase_array"]
    assert len(phases) == 1
    assert phases[0]["uid"] == 1
    assert phases[0]["first_stage"] == 0
    assert phases[0]["count_stage"] == 3


@pytest.mark.integration
def test_min_hydro_ms_sddp_options_key(tmp_path):
    """plp_min_hydro_ms + --solver sddp: options must have sddp_solver_type='sddp'."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms_sddp")
    opts["hydrologies"] = "1,2"
    opts["solver_type"] = "sddp"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    assert data["options"]["sddp_options"]["sddp_solver_type"] == "sddp"

    sim = data["simulation"]
    # SDDP: one scene per scenario
    assert len(sim["scene_array"]) == 2
    # SDDP: one phase per stage
    assert len(sim["phase_array"]) == 3


@pytest.mark.integration
def test_min_hydro_ms_num_apertures(tmp_path):
    """plp_min_hydro_ms + --num-apertures: sddp_num_apertures exported to sddp_options."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms_apertures")
    opts["hydrologies"] = "1,2"
    opts["solver_type"] = "sddp"
    opts["num_apertures"] = "2"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sddp = data["options"]["sddp_options"]
    assert sddp["sddp_solver_type"] == "sddp"
    assert sddp["sddp_num_apertures"] == 2


@pytest.mark.integration
def test_min_hydro_ms_num_apertures_all(tmp_path):
    """plp_min_hydro_ms + --num-apertures all/-1: auto-detect (no explicit count set)."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms_apertures_all")
    opts["hydrologies"] = "1,2"
    opts["solver_type"] = "sddp"
    # "all" (or legacy -1) means auto-detect from aperture files.
    # plp_min_hydro_ms has no plpidap2.dat, so no sddp_num_apertures is set.
    opts["num_apertures"] = "-1"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sddp = data["options"].get("sddp_options", {})
    assert "sddp_num_apertures" not in sddp


@pytest.mark.integration
def test_min_hydro_ms_no_apertures_by_default(tmp_path):
    """plp_min_hydro_ms without --num-apertures: sddp_num_apertures absent in output."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms_no_apertures")
    opts["hydrologies"] = "1,2"
    opts["solver_type"] = "sddp"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sddp = data["options"].get("sddp_options", {})
    assert "sddp_num_apertures" not in sddp


# ---------------------------------------------------------------------------
# plp_case_2y — full 2-year (51-stage) reference case
# ---------------------------------------------------------------------------
# plp_case_2y uses plpidsim.dat (16 simulations → hydros 51-66) and
# plpidap2.dat (16 apertures per stage, with hydros 1 and 2 appearing in
# the last 3 stages, wrapping around the 66-hydrology plpaflce.dat).
# This is the reference case for testing idsim-based scenario mapping,
# stage-indexed aperture handling, and the aperture Afluent writer.
# ---------------------------------------------------------------------------


def _make_opts_2y(tmp_path: Path, case_name: str = "gtopt_case_2y") -> dict:
    """Build conversion options for the plp_case_2y reference case."""
    out_dir = tmp_path / case_name
    out_dir.mkdir(parents=True, exist_ok=True)
    return {
        "input_dir": _PLPCase2Y,
        "output_dir": out_dir,
        "output_file": out_dir.parent / f"{case_name}.json",
        "hydrologies": "all",
        "num_apertures": "all",
        "last_stage": -1,
        "last_time": -1,
        "compression": "gzip",
        "probability_factors": None,
        "discount_rate": 0.0,
        "management_factor": 0.0,
    }


@pytest.mark.integration
def test_plp_case_2y_parse():
    """plp_case_2y: parsers including idsim, idap2, and idape load correctly."""
    parser = PLPParser({"input_dir": _PLPCase2Y})
    parser.parse_all()

    # Simulation scenario mapping
    idsim = parser.parsed_data.get("idsim_parser")
    assert idsim is not None, "plpidsim.dat should be parsed"
    assert idsim.num_simulations == 16
    assert idsim.num_stages == 51
    # Simulation 1 (0-based 0) at stage 1 → hydrology 51
    assert idsim.get_index(0, 1) == 51
    # Simulation 16 (0-based 15) at stage 1 → hydrology 66
    assert idsim.get_index(15, 1) == 66

    # Aperture definitions (stage-indexed, simulation-independent)
    idap2 = parser.parsed_data.get("idap2_parser")
    assert idap2 is not None, "plpidap2.dat should be parsed"
    assert idap2.num_stages == 51
    # Stage 1: 16 apertures in range 51-66
    stage1_aps = idap2.get_apertures(1)
    assert len(stage1_aps) == 16
    assert set(stage1_aps) == set(range(51, 67))
    # Stages 49-51: apertures wrap around to include hydros 1 and 2
    stage51_aps = idap2.get_apertures(51)
    assert 1 in stage51_aps or 2 in stage51_aps, (
        "Late stages should reference hydros 1 and 2"
    )

    # Per-simulation apertures
    idape = parser.parsed_data.get("idape_parser")
    assert idape is not None, "plpidape.dat should be parsed"
    assert idape.num_simulations == 16

    # Affluent data
    aflce = parser.parsed_data.get("aflce_parser")
    assert aflce is not None
    assert aflce.items[0]["num_hydrologies"] == 66
    assert len(aflce.items) == 167


@pytest.mark.integration
def test_plp_case_2y_single_stage_all_scenarios(tmp_path):
    """plp_case_2y: -y all -a all -s 1 produces 16 scenarios using hydros 51-66.

    This is the canonical reference test:
      plp2gtopt --num-apertures all -y all -s 1 -i plp_case_2y -o gtopt_case_2y

    With plpidsim.dat present, '-y all' expands to all 16 simulations and
    maps each to its actual hydrology column (51-66).  '-s 1' limits to
    stage 1 only.  For stage 1, all aperture hydros (51-66) are already in
    the forward set, so no extra Afluent files are needed.
    """
    opts = _make_opts_2y(tmp_path)
    opts["last_stage"] = 1
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]

    # 16 forward scenarios (one per simulation from plpidsim.dat)
    assert len(sim["scenario_array"]) == 16

    # Hydrology indices are 0-based: simulations map to hydros 51-66
    # (Fortran 1-based 51-66 → Python 0-based 50-65)
    hydro_indices = {s["hydrology"] for s in sim["scenario_array"]}
    assert hydro_indices == set(range(50, 66)), (
        "Expected 0-based hydrology indices 50-65 (1-based 51-66 from idsim)"
    )

    # UIDs are sequential 1-based
    uids = sorted(s["uid"] for s in sim["scenario_array"])
    assert uids == list(range(1, 17))

    # Equal probability weights
    for s in sim["scenario_array"]:
        assert s["probability_factor"] == pytest.approx(1.0 / 16)

    # Only one stage
    assert len(sim["stage_array"]) == 1
    assert sim["stage_array"][0]["uid"] == 1

    # SDDP mode: one phase per stage, one scene per scenario
    assert len(sim["phase_array"]) == 1
    assert len(sim["scene_array"]) == 16

    # Aperture array: stage 1 apertures are all in the forward set
    # → 16 apertures referencing existing scenario UIDs
    assert "aperture_array" in sim
    aps = sim["aperture_array"]
    assert len(aps) == 16
    source_uids = {a["source_scenario"] for a in aps}
    # All apertures reference forward scenario UIDs 1-16
    assert source_uids.issubset(set(range(1, 17))), (
        "Stage-1 apertures all map to forward scenarios, no extra UIDs expected"
    )

    # No aperture Afluent directory (all hydros already in forward set)
    aperture_afluent = Path(opts["output_dir"]) / "apertures" / "Afluent"
    assert not aperture_afluent.exists(), (
        "No extra Afluent files needed when all aperture hydros are in forward set"
    )

    # Main Afluent directory has parquet files for the 16 forward scenarios
    afluent_dir = Path(opts["output_dir"]) / "Afluent"
    parquet_files = list(afluent_dir.glob("*.parquet"))
    assert len(parquet_files) > 0, "Main Afluent/*.parquet files should be written"
    df = pd.read_parquet(parquet_files[0])
    scenario_cols = [c for c in df.columns if c.startswith("uid:")]
    assert len(scenario_cols) == 16, "Each parquet file should have 16 scenario columns"

    # sddp_num_apertures auto-set to 16 (one per aperture in aperture_array)
    sddp = data["options"]["sddp_options"]
    assert sddp.get("sddp_num_apertures") == 16


@pytest.mark.integration
def test_plp_case_2y_all_stages_extra_hydros(tmp_path):
    """plp_case_2y: all stages — late-stage apertures include hydros 1+2 → extra files.

    plpidap2.dat stages 49-51 reference hydrology indices 1 and 2 in addition
    to 53-66.  When hydros 1 and 2 are NOT in the forward set (forward set =
    {50,...,65}, i.e. hydros 51-66), the aperture writer must create parquet
    files for those extra hydros in the apertures/Afluent/ directory.
    """
    opts = _make_opts_2y(tmp_path, "gtopt_case_2y_all")
    # All 51 stages
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]

    # 51 stages
    assert len(sim["stage_array"]) == 51

    # Aperture array covers the union of all 51 stages:
    # stages 1-48: {51,...,66}; stages 49-51: {53,...,66,1,2}
    # union = {1,2,51,52,...,66} = 18 unique hydros
    aps = sim.get("aperture_array", [])
    assert len(aps) == 18, (
        f"Expected 18 apertures (union across all 51 stages), got {len(aps)}"
    )

    # Extra hydros (0-based 0 and 1, i.e. Fortran hydros 1 and 2)
    # should have parquet files in apertures/Afluent/
    aperture_afluent = Path(opts["output_dir"]) / "apertures" / "Afluent"
    assert aperture_afluent.exists(), "apertures/Afluent/ should be created"

    pfiles = list(aperture_afluent.glob("*.parquet"))
    assert len(pfiles) > 0, "Extra hydro Afluent parquet files should be written"

    # Each file must have columns uid:1 and uid:2 (for extra hydros 1 and 2)
    df = pd.read_parquet(pfiles[0])
    assert "uid:1" in df.columns, "Column uid:1 (hydro 1) missing from aperture parquet"
    assert "uid:2" in df.columns, "Column uid:2 (hydro 2) missing from aperture parquet"
    assert "stage" in df.columns
    assert "block" in df.columns
    # Values must be finite floats
    assert np.isfinite(df["uid:1"].values).all()
    assert np.isfinite(df["uid:2"].values).all()

    # Forward afluent files for the 16 scenarios (hydros 51-66)
    afluent_dir = Path(opts["output_dir"]) / "Afluent"
    assert afluent_dir.exists()
    fwd_files = list(afluent_dir.glob("*.parquet"))
    assert len(fwd_files) > 0
