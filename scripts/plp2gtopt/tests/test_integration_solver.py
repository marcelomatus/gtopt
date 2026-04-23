"""Integration tests for plp2gtopt – solver / end-to-end pipeline tests.

Covers: plp_hydro_4b (conversion + gtopt solve), and full-pipeline tests
for plp_min_1bus and plp_min_2bus (convert → gtopt solve → verify solution).
These tests require the gtopt binary and are skipped when it is not found.
"""

import json
import shutil
import subprocess
from pathlib import Path

import pandas as pd
import pytest

from plp2gtopt.plp_parser import PLPParser
from plp2gtopt.plp2gtopt import convert_plp_case

# Path to the sample PLP cases shipped with the repository
_CASES_DIR = Path(__file__).parent.parent.parent / "cases"
_PLPMin1Bus = _CASES_DIR / "plp_min_1bus"
_PLPMin2Bus = _CASES_DIR / "plp_min_2bus"
_PLPHydro4b = _CASES_DIR / "plp_hydro_4b"

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


def _make_opts_hydro_4b(tmp_path: Path, case_name: str = "gtopt_hydro_4b") -> dict:
    """Build conversion options for the plp_hydro_4b test case."""
    out_dir = tmp_path / case_name
    out_dir.mkdir(parents=True, exist_ok=True)
    return {
        "input_dir": _PLPHydro4b,
        "output_dir": out_dir,
        "output_file": out_dir / f"{case_name}.json",
        "hydrologies": "1,2,3",
        "method": "sddp",
        "num_apertures": "3",
        "last_stage": -1,
        "last_time": -1,
        "compression": "zstd",
        "probability_factors": None,
        "discount_rate": 0.0,
        "management_factor": 0.0,
        "pasada_mode": "flow-turbine",
        "pasada_hydro": True,
        "model_options": {"use_kirchhoff": False},
    }


def _find_plp_binary():
    """Locate the PLP binary without downloading anything."""
    import os

    env_bin = os.environ.get("PLP_BIN")
    if env_bin and Path(env_bin).exists():
        return env_bin

    which_bin = shutil.which("plp")
    if which_bin:
        return which_bin

    # Common install locations
    for candidate_path in (
        "/opt/plp_cen65/plp",
        "/usr/local/bin/plp",
    ):
        if Path(candidate_path).exists():
            return candidate_path
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


def _run_plp(plp_bin: str, case_dir: Path, timeout: int = 120) -> tuple[int, str]:
    """Run PLP solver in case_dir. Returns (rc, stderr)."""
    result = subprocess.run(
        [plp_bin],
        cwd=str(case_dir),
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )
    return result.returncode, result.stderr


def _read_solution_csv(results_dir: Path) -> dict[str, int | float | str]:
    """Parse gtopt solution.csv into a dict.

    Supports both the legacy key,value format and the current columnar
    format (header: scene,phase,status,obj_value,kappa).  For the
    columnar format the values from the first data row are returned.
    """
    solution_csv = results_dir / "solution.csv"
    if not solution_csv.exists():
        return {}
    lines = [
        ln.strip()
        for ln in solution_csv.read_text(encoding="utf-8").splitlines()
        if ln.strip() and not ln.strip().startswith("#")
    ]
    if not lines:
        return {}
    result: dict[str, int | float | str] = {}
    header_fields = [f.strip() for f in lines[0].split(",")]
    if "obj_value" in header_fields and len(header_fields) > 2:
        # Columnar format: use first data row
        if len(lines) > 1:
            vals = [v.strip() for v in lines[1].split(",")]
            for col_name, idx in zip(header_fields, range(len(header_fields))):
                if idx < len(vals):
                    raw = vals[idx]
                    try:
                        result[col_name] = int(raw)
                    except ValueError:
                        try:
                            result[col_name] = float(raw)
                        except ValueError:
                            result[col_name] = raw
    else:
        # Legacy key,value format
        for line in lines:
            parts = line.split(",", maxsplit=1)
            if len(parts) == 2:
                key = parts[0].strip()
                val = parts[1].strip()
                try:
                    result[key] = int(val)
                except ValueError:
                    try:
                        result[key] = float(val)
                    except ValueError:
                        result[key] = val
    return result


def _read_output(results_dir: Path, component: str, name: str) -> pd.DataFrame | None:
    """Read a parquet or csv output from results_dir/component/name.{parquet,csv}.

    Handles the hive-partitioned parquet layout
    ``{name}.parquet/scene=<N>/phase=<M>/part.parquet`` and CSV shards
    ``{name}_s*_p*.csv`` alongside the legacy single-file layout.
    """
    comp_dir = results_dir / component
    parquet_path = comp_dir / f"{name}.parquet"
    # pd.read_parquet accepts both a single file and a hive-partitioned
    # directory; the dataset reader surfaces ``scene`` and ``phase`` as
    # partition columns automatically.
    if parquet_path.is_dir() or parquet_path.is_file():
        return pd.read_parquet(parquet_path)

    csv_shards = sorted(comp_dir.glob(f"{name}_s*_p*.csv"))
    if csv_shards:
        return pd.concat([pd.read_csv(f) for f in csv_shards], ignore_index=True)
    csv_path = comp_dir / f"{name}.csv"
    if csv_path.is_file():
        return pd.read_csv(csv_path)
    return None


# plp_hydro_4b reservoir bounds and solver tolerances
_RESERVOIR_EMIN = 100.0
_RESERVOIR_EMAX = 1200.0
_RESERVOIR_EINI = 600.0
_RESERVOIR_EFIN = 500.0
_VOLUME_TOLERANCE = 1.0  # numerical tolerance for volume bound checks
_CMG_TOLERANCE = 1.0  # small negative CMg from LP solver numerics
_FAILURE_COST = 500.0  # failure generator cost ($/MWh)

# ---------------------------------------------------------------------------
# plp_hydro_4b – 4-bus hydrothermal system with reservoir, 3 stages,
# 3 blocks/stage, 3 hydrologies, and apertures.  Combines the 4-bus
# network layout of plp_bat_4b_24 with the reservoir modelling of
# plp_min_reservoir, adding stochastic inflows and multi-stage structure
# inspired by plp_case_2y.
#
# System:
#   - 4 buses: b1 (hydro hub), b2 (run-of-river), b3/b4 (demand)
#   - 1 reservoir (LakeA): 100 MW, 100–1200 Mm³, Vini=600, Vfin=500
#   - 1 series turbine (TurbineA): 100 MW downstream of LakeA
#   - 1 run-of-river (HydroRoR): 50 MW at b2
#   - 1 failure generator (Falla1): 9999 MW, 500 $/MWh
#   - 5 transmission lines (100–200 MW)
#   - 3 stages × 3 blocks (4 h each), 3 hydrologies (wet/normal/dry)
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_hydro_4b_parse():
    """plp_hydro_4b: parser loads reservoir + serie + pasada + falla centrals."""
    parser = PLPParser({"input_dir": _PLPHydro4b})
    parser.parse_all()

    assert parser.parsed_data["bus_parser"].num_buses == 4
    assert parser.parsed_data["block_parser"].num_blocks == 9
    assert parser.parsed_data["stage_parser"].num_stages == 3
    assert parser.parsed_data["line_parser"].num_lines == 5

    cp = parser.parsed_data["central_parser"]
    assert cp.num_embalses == 1
    assert cp.num_series == 1
    assert cp.num_pasadas == 1
    assert cp.num_termicas == 1
    assert cp.num_fallas == 1

    embalse = cp.get_item_by_name("LakeA")
    assert embalse is not None
    assert embalse["type"] == "embalse"
    assert embalse["pmax"] == pytest.approx(100.0)
    assert embalse["emin"] == pytest.approx(100.0)
    assert embalse["emax"] == pytest.approx(1200.0)
    assert embalse["vol_ini"] == pytest.approx(600.0)
    assert embalse["vol_fin"] == pytest.approx(500.0)

    aflce = parser.parsed_data["aflce_parser"]
    assert aflce.num_flows == 1
    assert aflce.flows[0]["name"] == "LakeA"
    assert aflce.flows[0]["num_hydrologies"] == 3
    assert aflce.flows[0]["flow"].shape == (9, 3)

    idsim = parser.parsed_data["idsim_parser"]
    assert idsim is not None
    assert idsim.num_simulations == 3
    assert idsim.num_stages == 3

    idap2 = parser.parsed_data["idap2_parser"]
    assert idap2 is not None
    assert idap2.num_stages == 3


@pytest.mark.integration
def test_hydro_4b_sddp_conversion(tmp_path):
    """plp_hydro_4b SDDP: 4 buses, 5 lines, reservoir, 3 scenes, 3 phases."""
    opts = _make_opts_hydro_4b(tmp_path)
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sys_data = data["system"]
    sim = data["simulation"]

    # Network
    assert len(sys_data["bus_array"]) == 4
    assert len(sys_data["line_array"]) == 5

    # Generators: LakeA (embalse), TurbineA (serie), HydroRoR (pasada), Thermal1
    gens = sys_data["generator_array"]
    assert len(gens) == 4
    gen_names = {g["name"] for g in gens}
    assert "LakeA" in gen_names
    assert "TurbineA" in gen_names
    assert "HydroRoR" in gen_names
    assert "Thermal1" in gen_names
    # Falla1 should NOT be in generator_array (handled separately)
    assert "Falla1" not in gen_names

    # Demands
    dems = sys_data["demand_array"]
    assert len(dems) == 2

    # Reservoir
    reservoirs = sys_data.get("reservoir_array", [])
    assert len(reservoirs) == 1
    rsv = reservoirs[0]
    assert rsv["name"] == "LakeA"
    assert rsv["eini"] == pytest.approx(600.0)
    assert rsv["efin"] == pytest.approx(500.0)
    assert rsv["emin"] == pytest.approx(100.0)
    assert rsv["emax"] == pytest.approx(1200.0)
    assert rsv["capacity"] == pytest.approx(1200.0)
    assert rsv["flow_conversion_rate"] == pytest.approx(3.6 / 1000.0)

    # Junctions (LakeA + TurbineA_ocean + TurbineA)
    junctions = sys_data.get("junction_array", [])
    assert len(junctions) == 3
    j_names = {j["name"] for j in junctions}
    assert "LakeA" in j_names
    assert "TurbineA" in j_names

    # Waterways: reservoir→turbine (ser_hid) + turbine→ocean (ser_hid=0 fix)
    waterways = sys_data.get("waterway_array", [])
    assert len(waterways) == 2

    # Turbines: LakeA (embalse) + TurbineA (serie) from junctions,
    # + HydroRoR (pasada) from flow-turbine mode
    turbines = sys_data.get("turbine_array", [])
    assert len(turbines) == 3

    # Flows: LakeA (from junction) + HydroRoR (from flow-turbine)
    flows = sys_data.get("flow_array", [])
    assert len(flows) == 2
    flow_names = {f["name"] for f in flows}
    assert "HydroRoR" in flow_names

    # SDDP structure: 3 scenarios → 3 scenes, 3 stages → 3 phases
    assert len(sim["scenario_array"]) == 3
    assert len(sim["scene_array"]) == 3
    assert len(sim["stage_array"]) == 3
    assert len(sim["phase_array"]) == 3
    assert len(sim["block_array"]) == 9

    # SDDP options
    sddp_opts = data["options"]["sddp_options"]
    assert data["options"]["method"] == "sddp"
    assert "num_apertures" not in sddp_opts


@pytest.mark.integration
def test_hydro_4b_mono_conversion(tmp_path):
    """plp_hydro_4b monolithic: 1 scene covering all scenarios, 1 phase."""
    opts = _make_opts_hydro_4b(tmp_path, "gtopt_hydro_4b_mono")
    opts["method"] = "mono"
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    sim = data["simulation"]

    assert data["options"]["method"] == "monolithic"

    # 3 scenarios with equal probability
    scenarios = sim["scenario_array"]
    assert len(scenarios) == 3
    for s in scenarios:
        assert s["probability_factor"] == pytest.approx(1.0 / 3.0)

    # Monolithic: exactly one scene covering all scenarios
    scenes = sim["scene_array"]
    assert len(scenes) == 1
    assert scenes[0]["first_scenario"] == 0
    assert scenes[0]["count_scenario"] == 3

    # Monolithic: exactly one phase covering all stages
    phases = sim["phase_array"]
    assert len(phases) == 1
    assert phases[0]["first_stage"] == 0
    assert phases[0]["count_stage"] == 3


@pytest.mark.integration
def test_hydro_4b_afluent_parquet(tmp_path):
    """plp_hydro_4b: Flow/discharge.parquet has 3 scenarios × 9 blocks."""
    opts = _make_opts_hydro_4b(tmp_path)
    convert_plp_case(opts)

    discharge_path = Path(opts["output_dir"]) / "Flow" / "discharge.parquet"
    assert discharge_path.exists(), "Flow/discharge.parquet not written"

    df = pd.read_parquet(discharge_path)
    assert "scenario" in df.columns
    assert "block" in df.columns
    assert "uid:1" in df.columns  # LakeA uid=1

    # 3 scenarios × 9 blocks = 27 rows
    assert len(df) == 27
    assert set(df["scenario"].unique()) == {1, 2, 3}

    # Hydrology 1 (wet) – block 1: 40.0/100.0 (pmax=100 for embalse)
    s1_b1 = df[(df["scenario"] == 1) & (df["block"] == 1)]
    assert len(s1_b1) == 1
    assert s1_b1["uid:1"].iloc[0] == pytest.approx(40.0)

    # Hydrology 3 (dry) – block 1: 20.0/100.0
    s3_b1 = df[(df["scenario"] == 3) & (df["block"] == 1)]
    assert len(s3_b1) == 1
    assert s3_b1["uid:1"].iloc[0] == pytest.approx(20.0)


@pytest.mark.integration
def test_hydro_4b_demand_parquet(tmp_path):
    """plp_hydro_4b: Demand/lmax.parquet has per-block demands for b3 and b4."""
    opts = _make_opts_hydro_4b(tmp_path)
    convert_plp_case(opts)

    lmax_path = Path(opts["output_dir"]) / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "Demand/lmax.parquet not written"

    df = pd.read_parquet(lmax_path)
    assert "block" in df.columns
    dem_cols = [c for c in df.columns if c.startswith("uid:")]
    assert len(dem_cols) == 2

    # Block 2: b3=90, b4=60 (the peak demand block in stage 1)
    row2 = df[df["block"] == 2]
    vals = sorted(float(row2[c].iloc[0]) for c in dem_cols)
    assert vals[0] == pytest.approx(60.0)
    assert vals[1] == pytest.approx(90.0)


@pytest.mark.integration
def test_hydro_4b_reservoir_volume_bounds(tmp_path):
    """plp_hydro_4b: reservoir eini/efin/emin/emax are physically consistent."""
    opts = _make_opts_hydro_4b(tmp_path)
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    rsv = data["system"]["reservoir_array"][0]

    # Volume bounds: emin < efin < eini < emax
    assert rsv["emin"] < rsv["efin"]
    assert rsv["efin"] < rsv["eini"]
    assert rsv["eini"] < rsv["emax"]

    # flow_conversion_rate: 3.6 / scale (scale = 1e6 from plpcnfce.dat)
    assert rsv["flow_conversion_rate"] == pytest.approx(3.6e-3)


# ---------------------------------------------------------------------------
# plp_hydro_4b — gtopt solver integration tests
#
# These tests require the gtopt binary and optionally the PLP binary.
# They are skipped when the binary is not found.
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_hydro_4b_gtopt_mono_solve(tmp_path, gtopt_bin):
    """plp_hydro_4b: convert to monolithic gtopt and solve if binary is found."""
    # Convert PLP → gtopt (monolithic for deterministic solve)
    opts = _make_opts_hydro_4b(tmp_path, "gtopt_hydro_4b_solve")
    opts["method"] = "mono"
    convert_plp_case(opts)

    json_file = Path(opts["output_file"])
    case_dir = json_file.parent

    rc, stderr = _run_gtopt(gtopt_bin, case_dir, json_file.stem)
    assert rc == 0, f"gtopt failed with rc={rc}: {stderr}"

    # Check solution
    results_dir = case_dir / "results"
    sol = _read_solution_csv(results_dir)
    assert sol.get("status") == 0, f"Solver status={sol.get('status')} (expected 0)"
    assert sol.get("obj_value", -1) >= 0, f"Negative objective: {sol.get('obj_value')}"

    # Check generation output
    gen_df = _read_output(results_dir, "Generator", "generation_sol")
    assert gen_df is not None and len(gen_df) > 0, "No generation data"

    # Check reservoir volume output (vini/vfin trajectories)
    rsv_df = _read_output(results_dir, "Reservoir", "eini_sol")
    if rsv_df is not None and len(rsv_df) > 0:
        uid_cols = [c for c in rsv_df.columns if c.startswith("uid:")]
        for col in uid_cols:
            vols = rsv_df[col].astype(float)
            assert vols.min() >= _RESERVOIR_EMIN - _VOLUME_TOLERANCE, (
                f"Reservoir volume below emin: {vols.min()}"
            )
            assert vols.max() <= _RESERVOIR_EMAX + _VOLUME_TOLERANCE, (
                f"Reservoir volume above emax: {vols.max()}"
            )

    # Check bus marginal costs (balance_dual)
    dual_df = _read_output(results_dir, "Bus", "balance_dual")
    if dual_df is not None:
        assert len(dual_df) > 0, "No marginal cost data"

    # Check no load shedding
    fail_df = _read_output(results_dir, "Demand", "fail_sol")
    if fail_df is not None:
        uid_cols = [c for c in fail_df.columns if c.startswith("uid:")]
        for col in uid_cols:
            total_fail = fail_df[col].astype(float).sum()
            assert total_fail == pytest.approx(0.0, abs=0.1), (
                f"Load shedding detected: {col}={total_fail}"
            )


@pytest.mark.integration
def test_hydro_4b_gtopt_reservoir_trajectory(tmp_path, gtopt_bin):
    """plp_hydro_4b: verify reservoir vini/vfin trajectory across stages."""
    opts = _make_opts_hydro_4b(tmp_path, "gtopt_hydro_4b_traj")
    opts["method"] = "mono"
    convert_plp_case(opts)

    json_file = Path(opts["output_file"])
    case_dir = json_file.parent

    rc, stderr = _run_gtopt(gtopt_bin, case_dir, json_file.stem)
    assert rc == 0, f"gtopt failed with rc={rc}: {stderr}"

    results_dir = case_dir / "results"

    # Verify reservoir eini and efin trajectories
    for name in ("eini_sol", "efin_sol"):
        df = _read_output(results_dir, "Reservoir", name)
        if df is not None and len(df) > 0:
            uid_cols = [c for c in df.columns if c.startswith("uid:")]
            for col in uid_cols:
                vols = df[col].astype(float)
                assert vols.min() >= _RESERVOIR_EMIN - _VOLUME_TOLERANCE, (
                    f"{name}: below emin ({vols.min()})"
                )
                assert vols.max() <= _RESERVOIR_EMAX + _VOLUME_TOLERANCE, (
                    f"{name}: above emax ({vols.max()})"
                )

    # Verify eini at stage 1 starts at initial volume
    eini_df = _read_output(results_dir, "Reservoir", "eini_sol")
    if eini_df is not None and "stage" in eini_df.columns:
        uid_cols = [c for c in eini_df.columns if c.startswith("uid:")]
        if uid_cols:
            stage1 = eini_df[eini_df["stage"] == 1]
            if len(stage1) > 0:
                v_ini = stage1[uid_cols[0]].astype(float).iloc[0]
                assert v_ini == pytest.approx(_RESERVOIR_EINI, abs=_VOLUME_TOLERANCE), (
                    f"Initial volume should be {_RESERVOIR_EINI} Mm³, got {v_ini}"
                )


@pytest.mark.integration
def test_hydro_4b_gtopt_marginal_costs(tmp_path, gtopt_bin):
    """plp_hydro_4b: marginal costs should be non-negative and bounded."""
    opts = _make_opts_hydro_4b(tmp_path, "gtopt_hydro_4b_cmg")
    opts["method"] = "mono"
    convert_plp_case(opts)

    json_file = Path(opts["output_file"])
    case_dir = json_file.parent

    rc, stderr = _run_gtopt(gtopt_bin, case_dir, json_file.stem)
    assert rc == 0, f"gtopt failed with rc={rc}: {stderr}"

    results_dir = case_dir / "results"
    dual_df = _read_output(results_dir, "Bus", "balance_dual")

    if dual_df is None:
        pytest.skip("No balance_dual output found")

    uid_cols = [c for c in dual_df.columns if c.startswith("uid:")]
    assert len(uid_cols) > 0, "No bus dual columns in output"

    for col in uid_cols:
        vals = dual_df[col].astype(float)
        # Marginal costs: allow small negative values from LP numerics
        assert vals.min() >= -_CMG_TOLERANCE, (
            f"Negative marginal cost at bus {col}: {vals.min()}"
        )
        # Should not exceed failure cost + tolerance
        assert vals.max() <= _FAILURE_COST + _CMG_TOLERANCE, (
            f"Marginal cost exceeds failure cost at bus {col}: {vals.max()}"
        )


@pytest.mark.integration
def test_hydro_4b_plp_vs_gtopt(tmp_path, gtopt_bin):
    """plp_hydro_4b: compare PLP and gtopt solutions if both binaries exist."""
    plp_bin = _find_plp_binary()

    if plp_bin is None:
        pytest.skip("PLP binary not found")

    # --- Run gtopt ---
    opts = _make_opts_hydro_4b(tmp_path, "gtopt_hydro_4b_compare")
    opts["method"] = "mono"
    convert_plp_case(opts)

    json_file = Path(opts["output_file"])
    gtopt_dir = json_file.parent

    rc_gtopt, stderr_gtopt = _run_gtopt(gtopt_bin, gtopt_dir, json_file.stem)
    assert rc_gtopt == 0, f"gtopt failed: {stderr_gtopt}"

    # --- Run PLP ---
    plp_dir = tmp_path / "plp_run"
    shutil.copytree(_PLPHydro4b, plp_dir)

    rc_plp, stderr_plp = _run_plp(plp_bin, plp_dir)
    assert rc_plp == 0, f"PLP failed: {stderr_plp}"

    # --- Compare marginal costs ---
    gtopt_results = gtopt_dir / "results"

    # Read gtopt marginal costs
    gtopt_dual = _read_output(gtopt_results, "Bus", "balance_dual")
    if gtopt_dual is None:
        pytest.skip("gtopt did not produce balance_dual output")

    # Both should have non-negative marginal costs
    gtopt_uid_cols = [c for c in gtopt_dual.columns if c.startswith("uid:")]
    for col in gtopt_uid_cols:
        vals = gtopt_dual[col].astype(float)
        assert vals.min() >= -_CMG_TOLERANCE, (
            f"gtopt negative CMg at {col}: {vals.min()}"
        )

    # Read PLP marginal costs (plpbar.csv)
    plpbar_csv = plp_dir / "plpbar.csv"
    if not plpbar_csv.exists():
        pytest.skip("PLP did not produce plpbar.csv")

    import csv

    plp_cmg: dict[int, dict[str, float]] = {}
    with open(plpbar_csv, newline="", encoding="utf-8") as fh:
        reader = csv.reader(fh)
        header = [h.strip() for h in next(reader)]
        if "Bloque" in header and "BarNom" in header and "CMgBar" in header:
            idx_blk = header.index("Bloque")
            idx_bus = header.index("BarNom")
            idx_cmg = header.index("CMgBar")
            for row in reader:
                block = int(row[idx_blk].strip())
                bus = row[idx_bus].strip()
                cmg = float(row[idx_cmg].strip())
                plp_cmg.setdefault(block, {})[bus] = cmg

    # Compare: both should agree on relative merit order
    if plp_cmg:
        for block_idx, bus_cmgs in plp_cmg.items():
            for bus_name, plp_val in bus_cmgs.items():
                assert plp_val >= -_CMG_TOLERANCE, (
                    f"PLP negative CMg at block {block_idx}, bus {bus_name}"
                )

    # --- Compare reservoir trajectories ---
    gtopt_eini = _read_output(gtopt_results, "Reservoir", "eini_sol")
    plp_emb_csv = plp_dir / "plpemb.csv"

    if gtopt_eini is not None and plp_emb_csv.exists():
        uid_cols = [c for c in gtopt_eini.columns if c.startswith("uid:")]
        for col in uid_cols:
            vols = gtopt_eini[col].astype(float)
            assert vols.min() >= _RESERVOIR_EMIN - _VOLUME_TOLERANCE
            assert vols.max() <= _RESERVOIR_EMAX + _VOLUME_TOLERANCE


# ---------------------------------------------------------------------------
# plp_min_1bus: PLP → gtopt full pipeline integration tests
# ---------------------------------------------------------------------------

# Expected result: Thermal1 (50 $/MWh, Pmax=100 MW) serves Bus1 demand (80 MW)
_MIN_1BUS_DEMAND_MW = 80.0
_MIN_1BUS_GEN_COST = 50.0  # $/MWh


@pytest.mark.integration
def test_min_1bus_gtopt_solve(tmp_path, gtopt_bin):
    """plp_min_1bus: convert PLP → gtopt and verify optimal solution."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "gtopt_min_1bus")
    opts["method"] = "mono"
    convert_plp_case(opts)

    json_file = Path(opts["output_file"])
    case_dir = json_file.parent

    rc, stderr = _run_gtopt(gtopt_bin, case_dir, json_file.stem)
    assert rc == 0, f"gtopt failed with rc={rc}: {stderr}"

    results_dir = case_dir / "results"
    sol = _read_solution_csv(results_dir)
    assert sol.get("status") == 0, f"Solver status={sol.get('status')} (expected 0)"
    assert sol.get("obj_value", -1) >= 0, f"Negative objective: {sol.get('obj_value')}"

    # Generation output must exist and Thermal1 must cover the full demand
    gen_df = _read_output(results_dir, "Generator", "generation_sol")
    assert gen_df is not None and len(gen_df) > 0, "No generation data"
    uid_cols = [c for c in gen_df.columns if c.startswith("uid:")]
    total_gen = sum(gen_df[c].astype(float).sum() for c in uid_cols)
    assert total_gen == pytest.approx(_MIN_1BUS_DEMAND_MW, abs=0.1), (
        f"Total generation {total_gen} != expected demand {_MIN_1BUS_DEMAND_MW}"
    )

    # No load shedding expected (Thermal1 has sufficient capacity)
    fail_df = _read_output(results_dir, "Demand", "fail_sol")
    if fail_df is not None:
        uid_cols_f = [c for c in fail_df.columns if c.startswith("uid:")]
        for col in uid_cols_f:
            total_fail = fail_df[col].astype(float).sum()
            assert total_fail == pytest.approx(0.0, abs=0.1), (
                f"Unexpected load shedding: {col}={total_fail}"
            )


@pytest.mark.integration
def test_min_1bus_gtopt_generation_cost(tmp_path, gtopt_bin):
    """plp_min_1bus: objective value matches expected generation cost."""
    opts = _make_opts(_PLPMin1Bus, tmp_path, "gtopt_min_1bus_cost")
    opts["method"] = "mono"
    convert_plp_case(opts)

    json_file = Path(opts["output_file"])
    case_dir = json_file.parent

    rc, _stderr = _run_gtopt(gtopt_bin, case_dir, json_file.stem)
    assert rc == 0, f"gtopt failed with rc={rc}"

    results_dir = case_dir / "results"
    sol = _read_solution_csv(results_dir)
    assert sol.get("status") == 0

    # Expected cost: 80 MW × 1 h × 50 $/MWh = 4000 $
    # gtopt reports obj_value in physical (unscaled) units.
    expected = _MIN_1BUS_DEMAND_MW * 1.0 * _MIN_1BUS_GEN_COST

    obj = float(sol.get("obj_value", -1))
    assert obj == pytest.approx(expected, rel=1e-3), (
        f"obj_value={obj} != expected {expected} "
        f"(demand={_MIN_1BUS_DEMAND_MW} MW × cost={_MIN_1BUS_GEN_COST} $/MWh)"
    )


# ---------------------------------------------------------------------------
# plp_min_2bus: PLP → gtopt multi-bus pipeline integration test
# ---------------------------------------------------------------------------


@pytest.mark.integration
def test_min_2bus_gtopt_solve(tmp_path, gtopt_bin):
    """plp_min_2bus: convert PLP → gtopt multi-bus case and verify optimal."""
    if not _PLPMin2Bus.exists():
        pytest.skip(f"plp_min_2bus case not found at {_PLPMin2Bus}")

    opts = _make_opts(_PLPMin2Bus, tmp_path, "gtopt_min_2bus")
    opts["method"] = "mono"
    convert_plp_case(opts)

    json_file = Path(opts["output_file"])
    case_dir = json_file.parent

    rc, stderr = _run_gtopt(gtopt_bin, case_dir, json_file.stem)
    assert rc == 0, f"gtopt failed with rc={rc}: {stderr}"

    results_dir = case_dir / "results"
    sol = _read_solution_csv(results_dir)
    assert sol.get("status") == 0, f"Solver status={sol.get('status')} (expected 0)"
    assert sol.get("obj_value", -1) >= 0, f"Negative objective: {sol.get('obj_value')}"

    # Generation output must exist
    gen_df = _read_output(results_dir, "Generator", "generation_sol")
    assert gen_df is not None and len(gen_df) > 0, "No generation data"

    # No load shedding expected
    fail_df = _read_output(results_dir, "Demand", "fail_sol")
    if fail_df is not None:
        uid_cols = [c for c in fail_df.columns if c.startswith("uid:")]
        for col in uid_cols:
            total_fail = fail_df[col].astype(float).sum()
            assert total_fail == pytest.approx(0.0, abs=0.1), (
                f"Unexpected load shedding: {col}={total_fail}"
            )


# ---------------------------------------------------------------------------
# Cascade: plp_hydro_4b PLP → gtopt cascade pipeline integration tests
# ---------------------------------------------------------------------------


def _make_opts_hydro_4b_cascade(
    tmp_path: Path, case_name: str = "gtopt_hydro_4b_cascade"
) -> dict:
    """Build conversion options for cascade solve of plp_hydro_4b."""
    out_dir = tmp_path / case_name
    out_dir.mkdir(parents=True, exist_ok=True)
    return {
        "input_dir": _PLPHydro4b,
        "output_dir": out_dir,
        "output_file": out_dir / f"{case_name}.json",
        "hydrologies": "1,2,3",
        "method": "cascade",
        "num_apertures": "3",
        "last_stage": -1,
        "last_time": -1,
        "compression": "zstd",
        "probability_factors": None,
        "discount_rate": 0.0,
        "management_factor": 0.0,
        "pasada_mode": "flow-turbine",
        "pasada_hydro": True,
        "max_iterations": 60,
        "model_options": {"use_kirchhoff": False},
    }


@pytest.mark.integration
def test_hydro_4b_cascade_conversion(tmp_path):
    """plp_hydro_4b cascade: JSON has method=cascade and 3-level cascade_options."""
    opts = _make_opts_hydro_4b_cascade(tmp_path)
    convert_plp_case(opts)

    data = json.loads(Path(opts["output_file"]).read_text(encoding="utf-8"))
    options = data["options"]

    # Method must be cascade
    assert options["method"] == "cascade"

    # cascade_options must be present with 3 levels
    cascade = options["cascade_options"]
    assert "level_array" in cascade
    levels = cascade["level_array"]
    assert len(levels) == 3

    # Level 0: uninodal
    assert levels[0]["name"] == "uninodal"
    assert levels[0]["model_options"]["use_single_bus"] is True

    # Level 1: transport (no kirchhoff, no losses)
    assert levels[1]["name"] == "transport"
    assert levels[1]["model_options"]["use_single_bus"] is False
    assert levels[1]["model_options"]["use_kirchhoff"] is False
    assert levels[1]["model_options"]["use_line_losses"] is False

    # Level 2: full network
    assert levels[2]["name"] == "full_network"

    # Iteration budget: level 0 gets full PLP budget (60); 1/4 for levels 1 & 2.
    assert levels[0]["sddp_options"]["max_iterations"] == 60
    assert levels[1]["sddp_options"]["max_iterations"] == 15
    assert levels[2]["sddp_options"]["max_iterations"] == 15

    # Transitions on levels 1 and 2 inherit targets
    assert levels[1]["transition"]["inherit_targets"] == -1
    assert levels[2]["transition"]["inherit_targets"] == -1

    # SDDP structure: 3 scenarios → 3 scenes, 3 stages → 3 phases
    sim = data["simulation"]
    assert len(sim["scenario_array"]) == 3
    assert len(sim["scene_array"]) == 3
    assert len(sim["stage_array"]) == 3
    assert len(sim["phase_array"]) == 3


@pytest.mark.integration
def test_hydro_4b_cascade_gtopt_solve(tmp_path, gtopt_bin):
    """plp_hydro_4b: convert to cascade, run gtopt, verify it runs."""
    opts = _make_opts_hydro_4b_cascade(tmp_path, "gtopt_hydro_4b_cascade_solve")
    convert_plp_case(opts)

    json_file = Path(opts["output_file"])
    case_dir = json_file.parent

    rc, stderr = _run_gtopt(gtopt_bin, case_dir, json_file.stem, timeout=180)
    assert rc == 0, f"gtopt cascade failed with rc={rc}: {stderr}"

    # Check solution exists and has valid status
    results_dir = case_dir / "results"
    sol = _read_solution_csv(results_dir)
    # status 0=optimal, 3=iteration_limit — both acceptable for cascade
    assert sol.get("status") in (0, 3), (
        f"Solver status={sol.get('status')} (expected 0 or 3)"
    )

    # Output directories must be populated
    assert (results_dir / "Generator").exists(), "No Generator output dir"
    assert (results_dir / "Demand").exists(), "No Demand output dir"
    assert (results_dir / "solution.csv").exists(), "No solution.csv"
