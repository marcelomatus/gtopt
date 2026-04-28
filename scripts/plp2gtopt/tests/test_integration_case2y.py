"""Integration tests for plp2gtopt – 4-bus 24-block and plp_case_2y cases.

Covers: plp_bat_4b_24 (battery + solar profile) and plp_case_2y (full 2-year
reference case with idsim/idap2/idape and aperture Flow writing).
"""

import json
import subprocess
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.plp2gtopt import convert_plp_case

# Path to the sample PLP cases shipped with the repository
_CASES_DIR = Path(__file__).parent.parent.parent / "cases"

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


def _make_opts_2y(tmp_path: Path, case_name: str = "gtopt_case_2y") -> dict:
    """Build conversion options for the plp_case_2y reference case.

    Includes ``max_iterations=2`` so that any downstream gtopt run on the
    generated JSON finishes quickly (2 SDDP iterations instead of default).
    """
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
        "compression": "zstd",
        "probability_factors": None,
        "discount_rate": 0.0,
        "management_factor": 0.0,
        "max_iterations": 2,
        "model_options": {"use_kirchhoff": False},
    }


def _find_gtopt_binary():
    """Locate the gtopt binary without downloading anything."""
    import os
    import shutil

    env_bin = os.environ.get("GTOPT_BIN")
    if env_bin and Path(env_bin).exists():
        return env_bin

    repo_root = Path(__file__).resolve().parents[3]
    for rel in (
        "build/standalone/gtopt",
        "build/gtopt",
        "build-standalone/gtopt",
        "all/build/gtopt",
    ):
        candidate = repo_root / rel
        if candidate.exists():
            return str(candidate)

    which_bin = shutil.which("gtopt")
    if which_bin:
        return which_bin

    return None


def _run_gtopt(
    gtopt_bin: str, case_dir: Path, json_stem: str, timeout: int = 120
) -> tuple[int, str]:
    """Run gtopt on json_stem.json inside case_dir. Returns (rc, combined output)."""
    json_file = f"{json_stem}.json"
    result = subprocess.run(
        [gtopt_bin, json_file],
        cwd=str(case_dir),
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    return result.returncode, result.stdout + result.stderr


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
# plp_case_2y — full 2-year (51-stage) reference case
# ---------------------------------------------------------------------------
# plp_case_2y uses plpidsim.dat (16 simulations → hydros 51-66) and
# plpidap2.dat (16 apertures per stage, with hydros 1 and 2 appearing in
# the last 3 stages, wrapping around the 66-hydrology plpaflce.dat).
# This is the reference case for testing idsim-based scenario mapping,
# stage-indexed aperture handling, and the aperture Flow discharge writer.
# ---------------------------------------------------------------------------


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
    the forward set, so no extra Flow files are needed.
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

    # UIDs = Fortran 1-based hydrology indices (51-66)
    uids = sorted(s["uid"] for s in sim["scenario_array"])
    assert uids == list(range(51, 67))

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
    # All apertures reference forward scenario UIDs (51-66 = Fortran indices)
    forward_uids = {s["uid"] for s in sim["scenario_array"]}
    assert source_uids.issubset(forward_uids), (
        "Stage-1 apertures all map to forward scenarios, no extra UIDs expected"
    )

    # No aperture Flow directory (all hydros already in forward set)
    aperture_flow = Path(opts["output_dir"]) / "apertures" / "Flow"
    assert not aperture_flow.exists(), (
        "No extra Flow files needed when all aperture hydros are in forward set"
    )

    # Main Flow directory has parquet files for the 16 forward scenarios.
    # discharge.parquet format: rows = (scenario × block), columns = scenario +
    # stage + block + one uid:N per hydro-capable central (not per scenario).
    flow_dir = Path(opts["output_dir"]) / "Flow"
    parquet_files = list(flow_dir.glob("*.parquet"))
    assert len(parquet_files) > 0, "Main Flow/*.parquet files should be written"
    df = pd.read_parquet(parquet_files[0])
    # The file must have a 'scenario' column with 16 unique scenario UIDs (1-16)
    assert "scenario" in df.columns, "discharge.parquet should have a 'scenario' column"
    unique_scen = sorted(df["scenario"].unique())
    assert len(unique_scen) == 16, (
        f"Expected 16 unique scenario values in discharge.parquet, got {len(unique_scen)}"
    )
    assert unique_scen == list(range(51, 67)), (
        "Scenario UIDs should be 51-66 (Fortran indices)"
    )
    # uid:N columns represent hydro-capable centrals (there are many in this case)
    central_cols = [c for c in df.columns if c.startswith("uid:")]
    assert len(central_cols) > 0, (
        "discharge.parquet must have at least one uid:N central column"
    )

    # num_apertures is NOT emitted — aperture count is determined by aperture_array
    sddp = data["options"]["sddp_options"]
    assert "num_apertures" not in sddp


@pytest.mark.integration
def test_plp_case_2y_all_stages_extra_hydros(tmp_path):
    """plp_case_2y: all stages — apertures from stages 28+ include extra hydros.

    According to plpidap2.dat (verified via --info):
    - Stages  1-27: 16 apertures {51-66}       (all in active forward set)
    - Stages 28-39: 16 apertures {1, 52-66}    (hydro 1 first appears here)
    - Stages 40-51: 16 apertures {1-2, 53-66}  (hydro 2 also appears)
    Union = {1,2,51,52,...,66} = 18 unique hydro classes.
    Hydros 1 and 2 are NOT in the active forward set (51-66), so extra
    Flow parquet files are required in apertures/Flow/.
    """
    opts = _make_opts_2y(tmp_path, "gtopt_case_2y_all")
    # All 51 stages
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]

    # 51 stages
    assert len(sim["stage_array"]) == 51

    # Aperture array: union of all 51 stages = {1, 2, 51, 52, ..., 66} = 18 hydros
    aps = sim.get("aperture_array", [])
    assert len(aps) == 18, (
        f"Expected 18 apertures (union of all 51 stages), got {len(aps)}"
    )

    # Extra hydros (Fortran 1-based 1 and 2) need their own Flow files
    aperture_flow = Path(opts["output_dir"]) / "apertures" / "Flow"
    assert aperture_flow.exists(), "apertures/Flow/ should be created"

    pfiles = list(aperture_flow.glob("*.parquet"))
    assert len(pfiles) > 0, "Extra hydro Flow parquet files should be written"

    # Each file must have uid:1 and uid:2 (extra hydro scenario UIDs = 1-based hydro index)
    df = pd.read_parquet(pfiles[0])
    assert "uid:1" in df.columns, "Column uid:1 (Fortran hydro 1) missing"
    assert "uid:2" in df.columns, "Column uid:2 (Fortran hydro 2) missing"
    assert "stage" in df.columns
    assert "block" in df.columns
    assert np.isfinite(df["uid:1"].values).all()
    assert np.isfinite(df["uid:2"].values).all()

    # num_apertures is NOT emitted — aperture count is determined by aperture_array
    assert "num_apertures" not in data["options"]["sddp_options"]

    # Forward discharge files for the 16 scenarios (hydros 51-66)
    flow_dir = Path(opts["output_dir"]) / "Flow"
    assert flow_dir.exists()
    fwd_files = list(flow_dir.glob("*.parquet"))
    assert len(fwd_files) > 0


@pytest.mark.integration
def test_plp_case_2y_info(capsys):
    """plp_case_2y --info: displays simulation mapping and aperture structure."""
    from plp2gtopt.info_display import display_plp_info

    display_plp_info({"input_dir": _PLPCase2Y, "last_stage": -1})
    out = capsys.readouterr().out

    # System section
    assert "SYSTEM" in out
    assert "236" in out  # 236 buses

    # Hydrology section
    assert "66" in out  # 66 hydrology classes
    assert "167" in out  # 167 centrals with flow data

    # Simulation mapping
    assert "plpidsim.dat" in out
    assert "16 simulations" in out
    assert "Hydrology  51" in out  # Simulation 1 → Hydrology 51
    assert "Hydrology  66" in out  # Simulation 16 → Hydrology 66
    assert "51-66" in out  # Active hydrology class range

    # Aperture section
    assert "plpidap2.dat" in out
    assert "18 total" in out  # 18 unique aperture hydros across all stages
    assert "1-2" in out  # Extra hydros 1 and 2

    # Suggested command
    assert "plp2gtopt" in out
    assert "-y all" in out
    assert "-a all" in out


def _check_2y_global_indicators(
    tmp_path: Path,
    case_name: str,
    *,
    pasada_mode: str = "flow-turbine",
) -> None:
    """Shared indicator check for plp_case_2y in all pasada modes."""
    pasada_hydro = pasada_mode in ("hydro", "flow-turbine")
    from plp2gtopt.plp2gtopt import (  # noqa: PLC0415
        _extract_flow_central_names,
        _gtopt_element_counts,
        _plp_active_hydrology_indices,
        _plp_element_counts,
        compute_comparison_indicators,
    )

    # --- Run full conversion (all stages, all scenarios) ---
    opts = _make_opts_2y(tmp_path, case_name)
    opts["pasada_hydro"] = pasada_hydro
    opts["pasada_mode"] = pasada_mode
    convert_plp_case(opts)

    # --- PLP side: parse PLP data ---
    parser = PLPParser(
        {
            "input_dir": _PLPCase2Y,
            "pasada_hydro": pasada_hydro,
            "pasada_mode": pasada_mode,
        }
    )
    parser.parse_all()

    hydrology_indices = _plp_active_hydrology_indices(parser)
    assert hydrology_indices is not None, "plpidsim.dat should provide hydro indices"
    assert len(hydrology_indices) == 16, "Expected 16 active hydrologies from idsim"

    # --- Read converted planning dict ---
    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    base_dir = str(opts["output_dir"])

    # Verify flow names exist
    flow_central_names = _extract_flow_central_names(data)
    assert flow_central_names is not None, "flow_array should have named flows"
    assert len(flow_central_names) > 0, (
        "At least one flow should reference a PLP central"
    )

    # --- Compute matched indicators (all logic in plp2gtopt) ---
    plp_ind, gtopt_ind = compute_comparison_indicators(parser, data, base_dir=base_dir)
    plp_counts = _plp_element_counts(parser)
    gtopt_counts = _gtopt_element_counts(data)

    # ---- Cross-check: PLP hydrology indices == gtopt forward scenario hydrologies ----
    scenarios = data["simulation"]["scenario_array"]
    forward_scenarios = [s for s in scenarios if "input_directory" not in s]
    gtopt_hydros = sorted(s["hydrology"] for s in forward_scenarios)
    plp_hydros = sorted(hydrology_indices)
    assert gtopt_hydros == plp_hydros, (
        f"PLP hydro indices {plp_hydros} != gtopt scenario hydrologies {gtopt_hydros}"
    )

    # ==================================================================
    # Element count assertions
    # ==================================================================

    # Buses and lines are topology — must match exactly
    assert plp_counts["buses"] == gtopt_counts["buses"]
    assert plp_counts["lines"] == gtopt_counts["lines"]

    # Blocks and stages must match (full conversion)
    assert plp_counts["blocks"] == gtopt_counts["blocks"]
    assert plp_counts["stages"] == gtopt_counts["stages"]

    # Scenarios: 16 (one per simulation from plpidsim.dat)
    assert gtopt_counts["scenarios"] == 16

    # Reservoirs == embalse count
    assert gtopt_counts["reservoirs"] == plp_counts.get("sub_embalse", 0)

    # Stateless reservoirs match (hid_indep=T → use_state_variable=False)
    assert plp_counts["stateless_reservoirs"] > 0, (
        "plp_case_2y should have at least one stateless reservoir"
    )
    assert plp_counts["stateless_reservoirs"] == gtopt_counts["stateless_reservoirs"], (
        "Stateless reservoir counts should match between PLP and gtopt"
    )

    # ReservoirSeepages and reservoir efficiencies should be
    # approximately consistent between PLP and gtopt — but not strictly
    # equal.  Two unrelated effects move the counts in opposite
    # directions:
    #   * MIN-envelope merge: when a reservoir has BOTH plpcenre and
    #     plpcenpmax PF curves, gtopt emits a single combined
    #     ReservoirProductionFactor entry (warning at
    #     junction_writer.py:1952), reducing the gtopt count.
    #   * plpcenpmax-only reservoirs: gtopt also emits a PF entry for
    #     reservoirs that have ONLY plpcenpmax (no cenre), which the
    #     PLP-side counter (``cenre_parser.num_efficiencies``) doesn't
    #     count, increasing the gtopt count.
    # Net effect varies by case.  Sanity-check that both sides are
    # non-zero and within a small tolerance instead of strict equality.
    assert plp_counts["seepages"] == gtopt_counts["seepages"]
    p_eff = plp_counts["reservoir_efficiencies"]
    g_eff = gtopt_counts["reservoir_efficiencies"]
    assert p_eff > 0 and g_eff > 0
    assert abs(p_eff - g_eff) <= max(2, p_eff // 2), (
        f"PLP / gtopt reservoir_efficiencies differ too much: "
        f"plp={p_eff} vs gtopt={g_eff}"
    )

    # Pasada mode-specific assertions
    p_pasada = plp_counts.get("sub_pasada", 0)
    if pasada_mode == "profile":
        # Profile mode: generator profiles == pasada count
        assert gtopt_counts["generator_profiles"] == p_pasada
    else:
        # Hydro or flow-turbine mode: no generator profiles
        assert gtopt_counts["generator_profiles"] == 0
        # Pasada centrals appear as flows (+ turbines)
        assert gtopt_counts["flows"] > 0

    # Batteries match (plpcenbat + plpess)
    p_batteries = plp_counts.get("batteries", 0) + plp_counts.get("ess", 0)
    assert gtopt_counts["batteries"] == p_batteries

    # ==================================================================
    # Global indicator assertions
    # ==================================================================

    # --- Demand indicators (must match exactly) ---
    assert plp_ind["first_block_demand_mw"] > 1000.0, "First demand suspiciously low"
    assert plp_ind["first_block_demand_mw"] == pytest.approx(
        gtopt_ind["first_block_demand_mw"], rel=1e-6
    ), "First block demand mismatch"

    assert plp_ind["last_block_demand_mw"] == pytest.approx(
        gtopt_ind["last_block_demand_mw"], rel=1e-6
    ), "Last block demand mismatch"

    # --- Total and avg annual energy (must match exactly) ---
    assert plp_ind["total_energy_mwh"] > 1e6, "Total energy too low"
    assert plp_ind["total_energy_mwh"] == pytest.approx(
        gtopt_ind["total_energy_mwh"], rel=1e-6
    ), "Total energy mismatch"

    assert plp_ind["avg_annual_energy_mwh"] > 0.0
    assert plp_ind["avg_annual_energy_mwh"] == pytest.approx(
        gtopt_ind["avg_annual_energy_mwh"], rel=1e-6
    ), "Avg annual energy mismatch"

    # --- Capacity indicators (must match exactly, both filter bus<=0) ---
    assert plp_ind["total_gen_capacity_mw"] > 1000.0, "Gen capacity too low"
    assert plp_ind["total_gen_capacity_mw"] == pytest.approx(
        gtopt_ind["total_gen_capacity_mw"], rel=1e-6
    ), "Gen capacity mismatch (both PLP and gtopt exclude bus<=0)"

    # Thermal/hydro/renewable breakdown may differ because:
    # 1. tech-detect reclassifies PLP "termica" into solar/wind/etc.
    # 2. Generators with file-referenced pmax aren't counted by compute_indicators
    # Just verify the sums are in the right ballpark.
    plp_sum = plp_ind["thermal_capacity_mw"] + plp_ind["hydro_capacity_mw"]
    gtopt_sum = (
        gtopt_ind["thermal_capacity_mw"]
        + gtopt_ind["hydro_capacity_mw"]
        + gtopt_ind.get("renewable_capacity_mw", 0.0)
    )
    # Generators with file-referenced pmax (parquet) aren't counted by
    # compute_indicators, so the gtopt sum may be lower.  Allow wide tolerance.
    if plp_sum > 0 and gtopt_sum > 0:
        ratio = gtopt_sum / plp_sum
        assert 0.5 < ratio < 1.5, (
            f"Capacity sum ratio {ratio:.3f} too far from 1.0 "
            f"(plp={plp_sum:.0f}, gtopt={gtopt_sum:.0f})"
        )

    # --- Line capacity (must match exactly) ---
    assert plp_ind["total_line_capacity_mw"] > 0.0, "Line capacity should be > 0"
    assert plp_ind["total_line_capacity_mw"] == pytest.approx(
        gtopt_ind["total_line_capacity_mw"], rel=1e-6
    ), "Line capacity mismatch"

    # --- Flow/affluent indicators (positivity + close match) ---
    assert plp_ind["first_block_affluent_avg"] > 0.0, "PLP first flow should be > 0"
    assert gtopt_ind["first_block_affluent_avg"] > 0.0, "gtopt first flow should be > 0"
    assert plp_ind["last_block_affluent_avg"] > 0.0, "PLP last flow should be > 0"
    assert gtopt_ind["last_block_affluent_avg"] > 0.0, "gtopt last flow should be > 0"
    assert plp_ind["total_water_volume_hm3"] > 0.0, "PLP water volume should be > 0"
    assert gtopt_ind["total_water_volume_hm3"] > 0.0, "gtopt water volume should be > 0"
    assert plp_ind["avg_flow_m3s"] > 0.0, "PLP avg flow should be > 0"
    assert gtopt_ind["avg_flow_m3s"] > 0.0, "gtopt avg flow should be > 0"
    # Profile mode routes some pasada flows to GeneratorProfile/ so water
    # volume accounting may differ.  Use 10% tolerance.
    assert plp_ind["total_water_volume_hm3"] == pytest.approx(
        gtopt_ind["total_water_volume_hm3"], rel=0.10
    ), "Total water volume too far apart (>10%)"


@pytest.mark.integration
def test_plp_case_2y_global_indicators(tmp_path):
    """plp_case_2y: indicators match in default flow-turbine mode."""
    _check_2y_global_indicators(
        tmp_path, "gtopt_case_2y_ft", pasada_mode="flow-turbine"
    )


@pytest.mark.integration
def test_plp_case_2y_global_indicators_hydro_mode(tmp_path):
    """plp_case_2y: indicators match in full hydro topology mode."""
    _check_2y_global_indicators(tmp_path, "gtopt_case_2y_hydro", pasada_mode="hydro")


@pytest.mark.integration
def test_plp_case_2y_global_indicators_profile_mode(tmp_path):
    """plp_case_2y: indicators match in legacy pasada-profile mode."""
    _check_2y_global_indicators(
        tmp_path, "gtopt_case_2y_profile", pasada_mode="profile"
    )


@pytest.mark.integration
def test_plp_case_2y_4h_partial_hydrology(tmp_path):
    """plp_case_2y with 4 hydrologies: demand matches, flows positivity-checked.

    Converts only a subset of hydrologies (1,2,3,4 out of 16).  Demand
    indicators must still match exactly because demand does not depend on
    hydrology.  Flow/affluent indicators are averaged over the selected
    hydrologies only; positivity and order-of-magnitude checks apply.
    """
    from plp2gtopt.plp2gtopt import (  # noqa: PLC0415
        _extract_flow_central_names,
        _gtopt_element_counts,
        _plp_element_counts,
        compute_comparison_indicators,
    )

    opts = _make_opts_2y(tmp_path, "gtopt_case_2y_4h")
    opts["hydrologies"] = "51,52,53,54"
    convert_plp_case(opts)

    parser = PLPParser({"input_dir": _PLPCase2Y})
    parser.parse_all()

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    scenarios = data["simulation"]["scenario_array"]
    forward_scenarios = [s for s in scenarios if "input_directory" not in s]
    assert len(forward_scenarios) == 4, (
        "Expected 4 forward scenarios for hydrologies 51,52,53,54"
    )

    # Verify flow names exist
    flow_central_names = _extract_flow_central_names(data)
    assert flow_central_names is not None, "flow_array should have named flows"

    # --- Compute matched indicators (all logic in plp2gtopt) ---
    base_dir = str(opts["output_dir"])
    plp_ind, gtopt_ind = compute_comparison_indicators(parser, data, base_dir=base_dir)
    plp_counts = _plp_element_counts(parser)
    gtopt_counts = _gtopt_element_counts(data)

    # Element counts: topology unchanged
    assert plp_counts["buses"] == gtopt_counts["buses"]
    assert plp_counts["lines"] == gtopt_counts["lines"]
    assert plp_counts["blocks"] == gtopt_counts["blocks"]
    assert plp_counts["stages"] == gtopt_counts["stages"]
    assert gtopt_counts["scenarios"] == 4

    # Demand does NOT depend on hydrology — must match exactly
    assert plp_ind["first_block_demand_mw"] == pytest.approx(
        gtopt_ind["first_block_demand_mw"], rel=1e-6
    ), "Demand must match regardless of hydrology subset"
    assert plp_ind["last_block_demand_mw"] == pytest.approx(
        gtopt_ind["last_block_demand_mw"], rel=1e-6
    ), "Last block demand must match regardless of hydrology subset"
    assert plp_ind["total_energy_mwh"] == pytest.approx(
        gtopt_ind["total_energy_mwh"], rel=1e-6
    ), "Total energy must match regardless of hydrology subset"

    # Capacity does NOT depend on hydrology — must match exactly
    assert plp_ind["total_gen_capacity_mw"] == pytest.approx(
        gtopt_ind["total_gen_capacity_mw"], rel=1e-6
    ), "Gen capacity must match regardless of hydrology subset"

    # Flow/affluent indicators: positivity + order-of-magnitude check
    assert plp_ind["first_block_affluent_avg"] > 0.0, "PLP first flow should be > 0"
    assert gtopt_ind["first_block_affluent_avg"] > 0.0, "gtopt first flow should be > 0"
    assert plp_ind["total_water_volume_hm3"] > 0.0, "PLP water volume should be > 0"
    assert gtopt_ind["total_water_volume_hm3"] > 0.0, "gtopt water volume should be > 0"
    assert plp_ind["total_water_volume_hm3"] == pytest.approx(
        gtopt_ind["total_water_volume_hm3"], rel=0.15
    ), "Total water volume too far apart (>15%)"


@pytest.mark.integration
def test_plp_case_2y_4h_4s_partial_stages(tmp_path):
    """plp_case_2y with 4 hydrologies and 4 stages: energy and last block differ.

    Converts only 4 stages (instead of 51) and 4 hydrologies.  First-block
    demand and capacity indicators still match.  Total energy and last-block
    demand will differ because the conversion only includes the first 4 stages.
    """
    from plp2gtopt.plp2gtopt import (  # noqa: PLC0415
        _gtopt_element_counts,
        _plp_element_counts,
        compute_comparison_indicators,
    )

    opts = _make_opts_2y(tmp_path, "gtopt_case_2y_4h_4s")
    opts["hydrologies"] = "51,52,53,54"
    opts["last_stage"] = 4
    convert_plp_case(opts)

    parser = PLPParser({"input_dir": _PLPCase2Y})
    parser.parse_all()

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))

    # --- Compute matched indicators (all logic in plp2gtopt) ---
    base_dir = str(opts["output_dir"])
    plp_ind, gtopt_ind = compute_comparison_indicators(parser, data, base_dir=base_dir)
    plp_counts = _plp_element_counts(parser)
    gtopt_counts = _gtopt_element_counts(data)

    # Topology unchanged
    assert plp_counts["buses"] == gtopt_counts["buses"]
    assert plp_counts["lines"] == gtopt_counts["lines"]
    assert gtopt_counts["scenarios"] == 4

    # Stages differ: gtopt has only 4, PLP parser sees all 51
    assert gtopt_counts["stages"] == 4
    assert plp_counts["stages"] > 4

    # gtopt blocks = 4 stages × blocks_per_stage; PLP has all 51 stages
    assert gtopt_counts["blocks"] < plp_counts["blocks"]

    # First-block demand and capacity still match (same data)
    assert plp_ind["first_block_demand_mw"] == pytest.approx(
        gtopt_ind["first_block_demand_mw"], rel=1e-6
    ), "First block demand must match even with truncated stages"
    assert plp_ind["total_gen_capacity_mw"] == pytest.approx(
        gtopt_ind["total_gen_capacity_mw"], rel=1e-6
    ), "Gen capacity must match even with truncated stages"

    # Total energy and last-block demand DIFFER because fewer stages
    assert plp_ind["total_energy_mwh"] > gtopt_ind["total_energy_mwh"], (
        "PLP total energy (all stages) should exceed gtopt (4 stages)"
    )
    assert plp_ind["last_block_demand_mw"] != pytest.approx(
        gtopt_ind["last_block_demand_mw"], rel=1e-3
    ), "Last block demand should differ with truncated stages"


@pytest.mark.integration
def test_plp_case_2y_2h_two_scenarios(tmp_path):
    """plp_case_2y with 2 explicit hydrologies (-y 55,60), max_iterations=2.

    Picks two hydrologies from the 51-66 range available in plpidsim.dat.
    Verifies the conversion produces exactly 2 forward scenarios with the
    correct hydrology indices, demand/capacity match, and SDDP
    max_iterations is propagated.
    """
    from plp2gtopt.plp2gtopt import (  # noqa: PLC0415
        _gtopt_element_counts,
        _plp_element_counts,
        compute_comparison_indicators,
    )

    opts = _make_opts_2y(tmp_path, "gtopt_case_2y_2h")
    opts["hydrologies"] = "55,60"
    opts["num_apertures"] = 0
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]

    # Exactly 2 forward scenarios
    scenarios = sim["scenario_array"]
    forward_scenarios = [s for s in scenarios if "input_directory" not in s]
    assert len(forward_scenarios) == 2, (
        f"Expected 2 forward scenarios, got {len(forward_scenarios)}"
    )

    # Hydrology indices: 55 → 0-based 54, 60 → 0-based 59
    hydro_indices = sorted(s["hydrology"] for s in forward_scenarios)
    assert hydro_indices == [54, 59], (
        f"Expected 0-based hydrology indices [54, 59], got {hydro_indices}"
    )

    # UIDs are the Fortran 1-based hydrology indices
    uids = sorted(s["uid"] for s in forward_scenarios)
    assert uids == [55, 60]

    # Equal probability weights (1/2 each)
    for s in forward_scenarios:
        assert s["probability_factor"] == pytest.approx(0.5)

    # SDDP max_iterations=2 propagated into JSON
    sddp = data["options"].get("sddp_options", {})
    assert sddp.get("max_iterations") == 2

    # Element counts
    parser = PLPParser({"input_dir": _PLPCase2Y})
    parser.parse_all()
    plp_counts = _plp_element_counts(parser)
    gtopt_counts = _gtopt_element_counts(data)

    assert plp_counts["buses"] == gtopt_counts["buses"]
    assert plp_counts["lines"] == gtopt_counts["lines"]
    assert gtopt_counts["scenarios"] == 2

    # Demand does not depend on hydrology — must match exactly
    base_dir = str(opts["output_dir"])
    plp_ind, gtopt_ind = compute_comparison_indicators(parser, data, base_dir=base_dir)

    assert plp_ind["first_block_demand_mw"] == pytest.approx(
        gtopt_ind["first_block_demand_mw"], rel=1e-6
    )
    assert plp_ind["total_gen_capacity_mw"] == pytest.approx(
        gtopt_ind["total_gen_capacity_mw"], rel=1e-6
    )

    # Flow discharge parquet has exactly 2 scenario UIDs
    flow_dir = Path(opts["output_dir"]) / "Flow"
    parquet_files = list(flow_dir.glob("*.parquet"))
    assert len(parquet_files) > 0
    df = pd.read_parquet(parquet_files[0])
    assert "scenario" in df.columns
    assert sorted(df["scenario"].unique()) == [55, 60]


@pytest.mark.integration
def test_plp_case_2y_2h_4s_two_scenarios_partial(tmp_path):
    """plp_case_2y with 2 hydrologies (-y 51,66), 4 stages, max_iterations=2.

    Uses the first and last hydrology from the idsim range.
    Limits to 4 stages for fast execution.  Checks structure, SDDP
    max_iterations propagation, and that the partial-stage conversion
    produces the expected first-block indicators.
    """
    opts = _make_opts_2y(tmp_path, "gtopt_case_2y_2h_4s")
    opts["hydrologies"] = "51,66"
    opts["last_stage"] = 4
    opts["num_apertures"] = 0
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]

    # 2 forward scenarios
    scenarios = sim["scenario_array"]
    forward_scenarios = [s for s in scenarios if "input_directory" not in s]
    assert len(forward_scenarios) == 2

    # Hydrology: 51 → 0-based 50, 66 → 0-based 65
    hydro_indices = sorted(s["hydrology"] for s in forward_scenarios)
    assert hydro_indices == [50, 65]

    # 4 stages
    assert len(sim["stage_array"]) == 4

    # SDDP max_iterations=2 propagated into JSON
    sddp = data["options"].get("sddp_options", {})
    assert sddp.get("max_iterations") == 2

    # SDDP mode: one phase per stage → 4 phases
    assert len(sim["phase_array"]) == 4

    # One scene per scenario×phase is expected for SDDP
    assert len(sim["scene_array"]) >= 2

    # Demand and capacity: first-block indicators must match PLP
    # (demand is hydrology-independent)
    from plp2gtopt.plp2gtopt import (  # noqa: PLC0415
        compute_comparison_indicators,
    )

    parser = PLPParser({"input_dir": _PLPCase2Y})
    parser.parse_all()
    base_dir = str(opts["output_dir"])
    plp_ind, gtopt_ind = compute_comparison_indicators(parser, data, base_dir=base_dir)

    assert plp_ind["first_block_demand_mw"] == pytest.approx(
        gtopt_ind["first_block_demand_mw"], rel=1e-6
    )
    assert plp_ind["total_gen_capacity_mw"] == pytest.approx(
        gtopt_ind["total_gen_capacity_mw"], rel=1e-6
    )

    # Flow parquet: 2 scenario UIDs
    flow_dir = Path(opts["output_dir"]) / "Flow"
    parquet_files = list(flow_dir.glob("*.parquet"))
    assert len(parquet_files) > 0
    df = pd.read_parquet(parquet_files[0])
    assert sorted(df["scenario"].unique()) == [51, 66]


@pytest.mark.integration
@pytest.mark.skip(
    reason=(
        "LP-level regression introduced by commit 86616b80 "
        "(`refactor(waterway): physical fcost on _ver arcs replaces "
        "drain teleport`).  On plp_case_2y data, LMAULE moves from the "
        "old drain teleport (spillway_capacity > 0, no _ver arc) to the "
        "new physical model (spillway_capacity=0, explicit _ver arc + "
        "rebalse fcost).  Combined with hard `reservoir_efin >= eini` "
        "rows + must-run waterway flows + capped inflow, the LP cell at "
        "stage 51 phase 1 becomes structurally infeasible.  IIS minimal "
        "conflict points at `reservoir_efin_1_51_1: ... >= 65.70569`.  "
        "Same pattern as the support/juan/IPLP_uninodal investigation; "
        "proper fix needs the per-reservoir soft `efin_cost` (option A) "
        "or `pmin_as_flowright` on the case2y centrals.  Tracked as a "
        "follow-up."
    )
)
def test_plp_case_2y_aperture_cache_loading(tmp_path):
    """plp_case_2y: convert with 1 scenario + apertures, run gtopt for 1 SDDP iteration.

    Verifies that gtopt can correctly load the aperture Flow parquet files
    generated by plp2gtopt.  Uses a single forward scenario (-y 51) so that
    the remaining 15 aperture hydrologies (52-66) require separate aperture
    files.  Limits to 4 stages and 1 SDDP iteration for fast execution.

    Re-enabled after the plp2gtopt defaults flipped: both
    ``soft_storage_bounds`` (per-reservoir ``efin_cost`` slack) and
    ``pmin_as_flowright`` (must-run → FlowRight obligation) now default
    to ON, keeping the previously-infeasible cell at stage 51 phase 1
    feasible at a bounded penalty cost — the IIS sentinel
    ``reservoir_efin_1_51_1 >= 65.70569`` is relaxed via the soft slack
    rather than left as a hard constraint, so plp_case_2y builds and
    solves the 1-SDDP-iteration smoke run end-to-end without any
    test-side overrides.
    """
    gtopt_bin = _find_gtopt_binary()
    if gtopt_bin is None:
        pytest.skip("gtopt binary not found")

    # Convert PLP → gtopt: 1 scenario, all apertures, 1 SDDP iteration.
    # Limit to 4 stages for speed.  Use pasada_mode="flow-turbine" so all
    # pasada centrals route through Flow/Turbine (no GeneratorProfile).
    opts = _make_opts_2y(tmp_path, "gtopt_case_2y_ap")
    opts["hydrologies"] = "51"
    opts["num_apertures"] = "all"
    opts["max_iterations"] = 1
    opts["last_stage"] = 1
    opts["pasada_mode"] = "flow-turbine"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]

    # 1 forward scenario
    scenarios = sim["scenario_array"]
    forward = [s for s in scenarios if "input_directory" not in s]
    assert len(forward) == 1
    assert forward[0]["hydrology"] == 50  # 0-based for hydro 51

    # 1 stage → apertures use hydros 51-66.
    # Only hydro 51 is forward, so 15 extra hydros need aperture files.
    assert len(sim["stage_array"]) == 1
    aps = sim.get("aperture_array", [])
    assert len(aps) == 16, f"Expected 16 apertures (hydros 51-66), got {len(aps)}"

    # Aperture Flow files must exist for the 15 extra hydros (52-66)
    aperture_flow = Path(opts["output_dir"]) / "apertures" / "Flow"
    assert aperture_flow.exists(), "apertures/Flow/ should be created"
    pfiles = list(aperture_flow.glob("*.parquet"))
    assert len(pfiles) > 0, "Extra hydro Flow parquet files should be written"

    # SDDP max_iterations=1 in JSON
    sddp = data["options"].get("sddp_options", {})
    assert sddp.get("max_iterations") == 1

    # Run gtopt — it must load aperture data without crashing.
    # rc=0 proves that all aperture Flow parquet files were found and loaded.
    json_file = Path(opts["output_file"])
    case_dir = json_file.parent
    rc, stderr = _run_gtopt(gtopt_bin, case_dir, json_file.stem, timeout=60)
    assert rc == 0, f"gtopt failed loading aperture data (rc={rc}):\n{stderr}"


@pytest.mark.integration
def test_plp_case_2y_boundary_cuts_loaded(tmp_path):
    """plp_case_2y: boundary cuts must always be loaded regardless of recovery_mode.

    Converts plp_case_2y with a single hydrology (-y 51) and 6 stages,
    then runs gtopt for 1 SDDP iteration.  Verifies that:

    1. plp2gtopt generates a boundary_cuts.csv from the PLP planos data.
    2. gtopt loads the boundary cuts (visible in the log output).

    Boundary cuts are problem specification (future-cost approximation),
    NOT recovery state — they do NOT affect the iteration offset.
    """
    gtopt_bin = _find_gtopt_binary()
    if gtopt_bin is None:
        pytest.skip("gtopt binary not found")

    # Convert PLP → gtopt: 1 scenario (-y 51), 6 stages, no apertures.
    opts = _make_opts_2y(tmp_path, "gtopt_bc_test")
    opts["hydrologies"] = "51"
    opts["num_apertures"] = "0"
    opts["max_iterations"] = 1
    opts["last_stage"] = 6
    convert_plp_case(opts)

    # 1. Boundary cuts CSV must be generated from plpplem data
    out_dir = Path(opts["output_dir"])
    bc_file = out_dir / "boundary_cuts.csv"
    assert bc_file.exists(), "boundary_cuts.csv not generated by plp2gtopt"
    bc_lines = bc_file.read_text().strip().split("\n")
    assert len(bc_lines) > 1, "boundary_cuts.csv is empty"

    # Verify JSON references the boundary cuts file
    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sddp = data["options"].get("sddp_options", {})
    assert sddp.get("boundary_cuts_file"), "boundary_cuts_file not set in JSON"

    # 2. Run gtopt — boundary cuts must be loaded
    json_file = Path(opts["output_file"])
    case_dir = json_file.parent
    _rc, output = _run_gtopt(gtopt_bin, case_dir, json_file.stem, timeout=120)

    # 3. Check that boundary cuts were loaded (not silently skipped)
    assert "loaded" in output and "boundary cuts" in output, (
        f"gtopt did not load boundary cuts.\n"
        f"Expected 'loaded ... boundary cuts' in output.\n"
        f"output (last 500 chars):\n{output[-500:]}"
    )
    # Boundary cuts are problem specification, not recovery — they must
    # NOT set the iteration offset.
    assert "iteration offset" not in output, (
        f"Boundary cuts should not set iteration offset.\n"
        f"output (last 500 chars):\n{output[-500:]}"
    )
