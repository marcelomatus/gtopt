"""Integration tests for plp2gtopt – hydro cases.

Covers: plp_min_hydro, plp_min_mance, plp_min_manli, plp_min_reservoir,
and plp_min_hydro_ms.
"""

import json
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.plp2gtopt import convert_plp_case

# Path to the sample PLP cases shipped with the repository
_CASES_DIR = Path(__file__).parent.parent.parent / "cases"

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
    assert g["type"] == "hydro_ror"
    assert g["gcost"] == pytest.approx(0.0)
    assert g["pmax"] == pytest.approx(200.0)
    assert g["bus"] == 1

    # HydroGen is detected as hydro_ror → flow+turbine mode (not profile)
    profiles = sys.get("generator_profile_array", [])
    assert len(profiles) == 0, "hydro_ror pasada should use flow+turbine, not profile"

    # flow+turbine elements created for hydro pasada
    flows = sys.get("flow_array", [])
    turbines = sys.get("turbine_array", [])
    assert len(flows) == 1
    assert flows[0]["name"] == "HydroGen"
    assert len(turbines) == 1
    assert turbines[0]["name"] == "HydroGen"
    assert turbines[0]["flow"] == "HydroGen"


@pytest.mark.integration
def test_min_hydro_afluent_parquet(tmp_path):
    """plp_min_hydro: Flow/discharge.parquet contains normalised per-scenario flows."""

    opts = _make_opts(_PLPMinHydro, tmp_path, "plp_min_hydro")
    opts["hydrologies"] = "1,2"
    convert_plp_case(opts)

    discharge_path = Path(opts["output_dir"]) / "Flow" / "discharge.parquet"
    assert discharge_path.exists(), "Flow/discharge.parquet not written"

    df = pd.read_parquet(discharge_path)
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
    assert rsv["junction"] == "Reservoir1"
    assert rsv["emin"] == pytest.approx(100.0)
    assert rsv["emax"] == pytest.approx(1000.0)
    assert rsv["capacity"] == pytest.approx(1000.0)
    assert rsv["eini"] == pytest.approx(500.0)
    assert rsv["efin"] == pytest.approx(400.0)
    assert rsv["flow_conversion_rate"] == pytest.approx(3.6 / 1000.0)

    # Junctions (embalse + serie + ocean junctions for ser_hid=0 centrals)
    junctions = sys_data.get("junction_array", [])
    assert len(junctions) == 3  # Reservoir1 + TurbineGen_ocean + TurbineGen
    j_names = {j["name"] for j in junctions}
    assert "Reservoir1" in j_names
    assert "TurbineGen" in j_names
    # Post-86616b80 (junction_writer.py:979): drain is True only when BOTH
    # gen and ver waterways are absent.  Reservoir1 has gen waterway →
    # TurbineGen, and TurbineGen has gen → ocean.  Both source junctions
    # therefore have drain=False — excess water leaves through explicit
    # arcs (the `_ver` ocean fallback for terminal centrals), not via a
    # zero-cost teleport on the source junction.
    rsv_j = next(j for j in junctions if j["name"] == "Reservoir1")
    assert rsv_j["drain"] is False
    turb_j = next(j for j in junctions if j["name"] == "TurbineGen")
    assert turb_j["drain"] is False

    # Waterways: embalse→serie (ser_hid=2) + serie→ocean (ser_hid=0 fix)
    waterways = sys_data.get("waterway_array", [])
    assert len(waterways) == 2
    # The original waterway from Reservoir1 to TurbineGen
    ww_res = next(w for w in waterways if w["junction_a"] == "Reservoir1")
    assert ww_res["junction_b"] == "TurbineGen"

    # Turbines: Reservoir1 (embalse, bus>0) + TurbineGen (serie, bus>0, ocean fix)
    turbines = sys_data.get("turbine_array", [])
    assert len(turbines) == 2
    rsv_turb = next(t for t in turbines if t["generator"] == "Reservoir1")
    assert rsv_turb["waterway"] == ww_res["name"]

    # Flow (discharge) is a parquet reference
    flows = sys_data.get("flow_array", [])
    assert len(flows) == 1
    assert flows[0]["junction"] == "Reservoir1"
    assert flows[0]["discharge"] == "discharge"

    # Generators: embalse + serie (no falla)
    gens = sys_data.get("generator_array", [])
    assert len(gens) == 2
    gen_names = {g["name"] for g in gens}
    assert "Reservoir1" in gen_names
    assert "TurbineGen" in gen_names
    assert "Falla1" not in gen_names

    # Generator types preserved
    rsv_g = next(g for g in gens if g["name"] == "Reservoir1")
    assert rsv_g["type"] == "hydro_reservoir"
    turb_g = next(g for g in gens if g["name"] == "TurbineGen")
    assert turb_g["type"] == "hydro_ror"


@pytest.mark.integration
def test_min_reservoir_afluent_parquet(tmp_path):
    """plp_min_reservoir: Flow/discharge.parquet contains reservoir inflows."""
    opts = _make_opts(_PLPMinReservoir, tmp_path, "plp_min_reservoir")
    convert_plp_case(opts)

    discharge_path = Path(opts["output_dir"]) / "Flow" / "discharge.parquet"
    assert discharge_path.exists(), "Flow/discharge.parquet not written"

    df = pd.read_parquet(discharge_path)
    assert "block" in df.columns
    assert "uid:1" in df.columns  # Reservoir1 uid=1

    # 1 scenario × 2 blocks = 2 rows
    assert len(df) == 2

    # Embalse afluent values are raw water inflows (not normalised)
    values = sorted(df["uid:1"].tolist())
    assert values[0] == pytest.approx(20.0)
    assert values[1] == pytest.approx(25.0)


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
    """plp_min_hydro_ms: discharge parquet has unique scenario UIDs across 3 stages."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms")
    opts["hydrologies"] = "1,2"
    convert_plp_case(opts)

    discharge_path = Path(opts["output_dir"]) / "Flow" / "discharge.parquet"
    assert discharge_path.exists(), "Flow/discharge.parquet not written"

    df = pd.read_parquet(discharge_path)
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
    """plp_min_hydro_ms + --method mono: one scene (all scenarios), one phase (all stages)."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms_mono")
    opts["hydrologies"] = "1,2"
    opts["method"] = "mono"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]

    # method in options must be "monolithic" (top-level field)
    assert data["options"]["method"] == "monolithic"

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
    """plp_min_hydro_ms + --method sddp: options must have method='sddp'."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms_sddp")
    opts["hydrologies"] = "1,2"
    opts["method"] = "sddp"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    assert data["options"]["method"] == "sddp"

    sim = data["simulation"]
    # SDDP: one scene per scenario
    assert len(sim["scene_array"]) == 2
    # SDDP: one phase per stage
    assert len(sim["phase_array"]) == 3


@pytest.mark.integration
def test_min_hydro_ms_num_apertures(tmp_path):
    """plp_min_hydro_ms + --num-apertures: num_apertures NOT in sddp_options (C++ ignores it)."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms_apertures")
    opts["hydrologies"] = "1,2"
    opts["method"] = "sddp"
    opts["num_apertures"] = "2"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sddp = data["options"]["sddp_options"]
    assert data["options"]["method"] == "sddp"
    # num_apertures is NOT emitted — apertures are configured via aperture_array
    assert "num_apertures" not in sddp


@pytest.mark.integration
def test_min_hydro_ms_num_apertures_all(tmp_path):
    """plp_min_hydro_ms + --num-apertures all/-1: no num_apertures in sddp_options."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms_apertures_all")
    opts["hydrologies"] = "1,2"
    opts["method"] = "sddp"
    opts["num_apertures"] = "-1"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sddp = data["options"].get("sddp_options", {})
    assert "num_apertures" not in sddp


@pytest.mark.integration
def test_min_hydro_ms_no_apertures_by_default(tmp_path):
    """plp_min_hydro_ms without --num-apertures: num_apertures absent in output."""
    opts = _make_opts(_PLPMinHydroMs, tmp_path, "plp_min_hydro_ms_no_apertures")
    opts["hydrologies"] = "1,2"
    opts["method"] = "sddp"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sddp = data["options"].get("sddp_options", {})
    assert "num_apertures" not in sddp
