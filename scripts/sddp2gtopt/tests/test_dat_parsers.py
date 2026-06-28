"""Tests for the PSR SDDP/NCP ``.dat`` front-end."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from sddp2gtopt.dat_loader import is_dat_case, load_dat_case
from sddp2gtopt.dat_parsers import (
    ControlParser,
    DemandParser,
    FuelParser,
    HydroParser,
    SystemParser,
    ThermalParser,
)
from sddp2gtopt.gtopt_writer import build_planning
from sddp2gtopt.sddp2gtopt import convert_sddp_case, validate_sddp_case


def test_control_parser(dat_case_dir: Path) -> None:
    study = ControlParser(dat_case_dir / "sddp.dat").parse()
    assert study.stage_type == 1  # ETAPA = weekly
    assert study.num_stages == 2  # NUMERO DE ETAPAS
    assert study.num_systems == 1
    assert study.num_blocks == 1
    assert study.discount_rate == pytest.approx(0.10)
    assert study.deficit_cost == pytest.approx(500.0)


def test_system_parser(dat_case_dir: Path) -> None:
    systems = SystemParser(dat_case_dir / "sistem.dat").parse()
    assert len(systems) == 1
    assert systems[0].code == 1
    assert systems[0].name == "TESTSYS"


def test_fuel_parser(dat_case_dir: Path) -> None:
    fuels = FuelParser(dat_case_dir / "ccombuTS.dat").parse()
    by_code = {f.code: f for f in fuels}
    assert by_code[1].name == "GAS" and by_code[1].cost == pytest.approx(50.0)
    assert by_code[2].cost == pytest.approx(120.0)


def test_thermal_parser_cost_from_fuel(dat_case_dir: Path) -> None:
    fuels = {f.code: f.cost for f in FuelParser(dat_case_dir / "ccombuTS.dat").parse()}
    thermals = ThermalParser(dat_case_dir / "ctermiTS.dat").parse(fuels=fuels)
    by_name = {t.name: t for t in thermals}
    gas = by_name["GASPLANT"]
    # CEsp1=2 × fuel GAS 50 + CVaria 0 = 100; capacity = 100% × pmax 100.
    assert gas.pmax == pytest.approx(100.0)
    assert gas.g_segments[0] == pytest.approx((100.0, 100.0))
    oil = by_name["OILPLANT"]
    # CEsp1=3 × fuel OIL 120 = 360; pmin honoured.
    assert oil.pmin == pytest.approx(10.0)
    assert oil.g_segments[0][1] == pytest.approx(360.0)


def test_hydro_parser_picks_pot(dat_case_dir: Path) -> None:
    hydros = HydroParser(dat_case_dir / "chidroTS.dat").parse()
    by_name = {h.name: h for h in hydros}
    # Pot is the first decimal value, robust to blank fixed-width columns.
    assert by_name["HYDRO1"].p_inst == pytest.approx(30.0)
    assert by_name["HYDRO2"].p_inst == pytest.approx(20.0)


def test_demand_parser_flattens_hours(dat_case_dir: Path) -> None:
    blocks = DemandParser(dat_case_dir / "cpdeTS.dat").parse()
    assert blocks == [100.0, 120.0, 110.0, 130.0, 140.0, 135.0]


def test_is_dat_case(dat_case_dir: Path, tmp_path: Path) -> None:
    assert is_dat_case(dat_case_dir)
    assert not is_dat_case(tmp_path)  # empty dir
    # A psrclasses.json shadows the .dat path.
    (tmp_path / "sddp.dat").write_text("x\n")
    (tmp_path / "psrclasses.json").write_text("{}")
    assert not is_dat_case(tmp_path)


def test_load_dat_case(dat_case_dir: Path) -> None:
    case = load_dat_case(dat_case_dir)
    assert len(case.systems) == 1
    assert len(case.thermals) == 2
    assert len(case.hydros) == 2
    assert len(case.demands) == 1
    # Single-stage model: one block per demand hour.
    assert case.study.num_stages == 1
    assert case.study.num_blocks == 6
    assert case.demands[0].block_values == [100.0, 120.0, 110.0, 130.0, 140.0, 135.0]


def test_build_planning_from_dat(dat_case_dir: Path) -> None:
    case = load_dat_case(dat_case_dir)
    planning = build_planning(
        study=case.study,
        systems=case.systems,
        thermals=case.thermals,
        hydros=case.hydros,
        demands=case.demands,
        name="dat_test",
    )
    sim = planning["simulation"]
    assert len(sim["block_array"]) == 6
    assert len(sim["stage_array"]) == 1
    system = planning["system"]
    # 2 thermal + 2 hydro generators on the single TESTSYS bus.
    assert len(system["generator_array"]) == 4
    demand = system["demand_array"][0]
    assert demand["lmax"] == [[100.0, 120.0, 110.0, 130.0, 140.0, 135.0]]
    # Merit cost: gas plant (CEsp×50) cheaper than oil plant (CEsp×120).
    gcosts = {g["name"]: g["gcost"] for g in system["generator_array"]}
    assert gcosts["GASPLANT"] == pytest.approx(100.0)
    assert gcosts["OILPLANT"] == pytest.approx(360.0)


@pytest.mark.integration
def test_dat_case_solves(dat_case_dir: Path, gtopt_bin: str, tmp_path: Path) -> None:
    """gtopt builds + solves the converted .dat case to optimality."""
    import subprocess  # pylint: disable=import-outside-toplevel

    out = tmp_path / "out"
    assert convert_sddp_case({"input_dir": dat_case_dir, "output_dir": out}) == 0
    solve_out = tmp_path / "solve"
    subprocess.run(
        [gtopt_bin, "-s", str(out / "out.json"), "-d", str(solve_out), "-l", "ERROR"],
        check=True,
        timeout=120,
    )
    status = json.loads((solve_out / "solver_status.json").read_text(encoding="utf-8"))
    assert status["status"] == "done"


def test_validate_and_convert_dat(dat_case_dir: Path, tmp_path: Path) -> None:
    assert validate_sddp_case({"input_dir": dat_case_dir}) is True
    out = tmp_path / "out"
    rc = convert_sddp_case({"input_dir": dat_case_dir, "output_dir": out})
    assert rc == 0
    planning_file = out / "out.json"
    assert planning_file.is_file()
    data = json.loads(planning_file.read_text(encoding="utf-8"))
    assert len(data["system"]["generator_array"]) == 4
