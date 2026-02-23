"""Integration tests for plp2gtopt using the plp_dat_ex sample case."""

import json
from pathlib import Path

import pytest

from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.plp2gtopt import convert_plp_case

# Path to the sample PLP case shipped with the repository
_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPDatEx = _CASES_DIR / "plp_dat_ex"
_PLPMin1Bus = _CASES_DIR / "plp_min_1bus"
_PLPMin2Bus = _CASES_DIR / "plp_min_2bus"
_PLPMinBess = _CASES_DIR / "plp_min_bess"


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
def test_plp_parser_parses_dat_ex(tmp_path):
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

    data = json.loads(Path(opts["output_file"]).read_text())
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
    import pandas as pd

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

    data = json.loads(Path(opts["output_file"]).read_text())
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
    import pandas as pd

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

    bess = parser.parsed_data.get("bess_parser")
    assert bess is not None
    assert bess.num_besses == 1
    assert bess.besses[0]["name"] == "BESS1"


@pytest.mark.integration
def test_min_bess_conversion(tmp_path):
    """plp_min_bess: convert_plp_case produces battery, converter, gen and demand arrays."""
    opts = _make_opts(_PLPMinBess, tmp_path, "plp_min_bess")
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text())
    sys = data["system"]

    # 1 battery
    assert len(sys.get("battery_array", [])) == 1
    bat = sys["battery_array"][0]
    assert bat["uid"] == 1
    assert bat["name"] == "BESS1"
    assert bat["input_efficiency"] == pytest.approx(0.95)
    assert bat["output_efficiency"] == pytest.approx(0.95)
    assert bat["capacity"] == pytest.approx(50.0 * 4.0)  # pmax_discharge * hrs_reg
    assert bat["vini"] == pytest.approx(0.50)

    # 1 converter
    assert len(sys.get("converter_array", [])) == 1
    conv = sys["converter_array"][0]
    assert conv["battery"] == 1
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
    import pandas as pd
    from plp2gtopt.bess_writer import BESS_UID_OFFSET

    opts = _make_opts(_PLPMinBess, tmp_path, "plp_min_bess")
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns

    # Thermal demand column (bus uid = 1)
    assert "uid:1" in df.columns
    assert float(df[df["block"] == 1]["uid:1"].iloc[0]) == pytest.approx(80.0)

    # BESS charge column
    bess_col = f"uid:{BESS_UID_OFFSET + 1}"
    assert bess_col in df.columns
    assert float(df[bess_col].iloc[0]) == pytest.approx(50.0)
