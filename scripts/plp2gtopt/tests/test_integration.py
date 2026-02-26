"""Integration tests for plp2gtopt using the plp_dat_ex sample case."""

import json
import sys
from pathlib import Path
from unittest.mock import patch

import numpy as np
import pandas as pd
import pytest

from plp2gtopt.battery_writer import BATTERY_UID_OFFSET
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
        "hydrologies": "0",
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
    """plp_min_bess: convert_plp_case produces battery, converter, gen and demand arrays."""
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
    assert bat["vini"] == pytest.approx(0.50)

    # 1 converter
    assert len(sys.get("converter_array", [])) == 1
    conv = sys["converter_array"][0]
    assert conv["battery"] == 2
    assert conv["capacity"] == pytest.approx(50.0)

    # 2 generators: 1 thermal + 1 BESS discharge
    gens = sys.get("generator_array", [])
    assert len(gens) == 2
    gen_names = {g["name"] for g in gens}
    assert "Thermal1" in gen_names
    assert "BESS1_disch" in gen_names

    bess_gen = next(g for g in gens if g["name"] == "BESS1_disch")
    assert bess_gen["pmax"] == pytest.approx(50.0)
    assert bess_gen["gcost"] == pytest.approx(0.0)
    assert bess_gen["bus"] == 1

    # 2 demands: 1 thermal + 1 BESS charge
    dems = sys.get("demand_array", [])
    assert len(dems) == 2
    dem_names = {d["name"] for d in dems}
    assert "BESS1_chrg" in dem_names

    bess_dem = next(d for d in dems if d["name"] == "BESS1_chrg")
    assert bess_dem["bus"] == 1
    assert bess_dem["lmax"] == "lmax"


@pytest.mark.integration
def test_min_bess_lmax_parquet(tmp_path):
    """plp_min_bess: lmax.parquet contains both thermal and BESS charge columns."""

    opts = _make_opts(_PLPMinBess, tmp_path, "plp_min_bess")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns

    # Thermal demand column (bus uid = 1)
    assert "uid:1" in df.columns
    assert float(df[df["block"] == 1]["uid:1"].iloc[0]) == pytest.approx(80.0)

    # BESS charge column – bat central BESS1 has number=2 → uid = BATTERY_UID_OFFSET + 2
    bat_col = f"uid:{BATTERY_UID_OFFSET + 2}"
    assert bat_col in df.columns
    assert float(df[bat_col].iloc[0]) == pytest.approx(50.0)


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
    """plp_min_battery: convert_plp_case produces battery, converter, gen and demand arrays."""
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
    assert bat["vini"] == pytest.approx(0.50)

    # 1 converter
    assert len(sys.get("converter_array", [])) == 1
    conv = sys["converter_array"][0]
    assert conv["battery"] == 2
    assert conv["capacity"] == pytest.approx(50.0)

    # 2 generators: 1 thermal + 1 battery discharge
    gens = sys.get("generator_array", [])
    assert len(gens) == 2
    gen_names = {g["name"] for g in gens}
    assert "Thermal1" in gen_names
    assert "BESS1_disch" in gen_names

    bat_gen = next(g for g in gens if g["name"] == "BESS1_disch")
    assert bat_gen["pmax"] == pytest.approx(50.0)
    assert bat_gen["gcost"] == pytest.approx(0.0)
    assert bat_gen["bus"] == 1

    # 2 demands: 1 thermal + 1 battery charge
    dems = sys.get("demand_array", [])
    assert len(dems) == 2
    dem_names = {d["name"] for d in dems}
    assert "BESS1_chrg" in dem_names

    bat_dem = next(d for d in dems if d["name"] == "BESS1_chrg")
    assert bat_dem["bus"] == 1
    assert bat_dem["lmax"] == "lmax"


@pytest.mark.integration
def test_min_battery_lmax_parquet(tmp_path):
    """plp_min_battery: lmax.parquet contains both thermal and battery charge columns."""
    opts = _make_opts(_PLPMinBattery, tmp_path, "plp_min_battery")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns

    # Thermal demand column (bus uid = 1)
    assert "uid:1" in df.columns
    assert float(df[df["block"] == 1]["uid:1"].iloc[0]) == pytest.approx(80.0)

    # Battery charge column – bat central BESS1 has number=2 → uid = BATTERY_UID_OFFSET + 2
    bat_col = f"uid:{BATTERY_UID_OFFSET + 2}"
    assert bat_col in df.columns
    assert float(df[bat_col].iloc[0]) == pytest.approx(50.0)


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
    """plp_min_ess: convert_plp_case produces battery, converter, gen and demand arrays."""
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
    # ESS capacity = pmax_discharge * hrs_reg = 50.0 * 4.0
    assert bat["capacity"] == pytest.approx(50.0 * 4.0)
    assert bat["vini"] == pytest.approx(0.50)
    # ESS has no active restriction
    assert "active" not in bat

    # 1 converter
    assert len(sys.get("converter_array", [])) == 1

    # 2 generators: 1 thermal + 1 ESS discharge
    gens = sys.get("generator_array", [])
    assert len(gens) == 2
    gen_names = {g["name"] for g in gens}
    assert "Thermal1" in gen_names
    assert "BESS1_disch" in gen_names

    # 2 demands: 1 thermal + 1 ESS charge
    dems = sys.get("demand_array", [])
    assert len(dems) == 2
    dem_names = {d["name"] for d in dems}
    assert "BESS1_chrg" in dem_names


@pytest.mark.integration
def test_min_ess_lmax_parquet(tmp_path):
    """plp_min_ess: lmax.parquet contains both thermal and ESS charge columns."""
    opts = _make_opts(_PLPMinEss, tmp_path, "plp_min_ess")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns

    # Thermal demand column (bus uid = 1)
    assert "uid:1" in df.columns
    assert float(df[df["block"] == 1]["uid:1"].iloc[0]) == pytest.approx(80.0)

    # ESS charge column – bat central BESS1 has number=2 → uid = BATTERY_UID_OFFSET + 2
    ess_col = f"uid:{BATTERY_UID_OFFSET + 2}"
    assert ess_col in df.columns
    assert float(df[ess_col].iloc[0]) == pytest.approx(50.0)


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
    opts["hydrologies"] = "0,1"
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
    opts["hydrologies"] = "0,1"
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
    opts["hydrologies"] = "0,1"
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
