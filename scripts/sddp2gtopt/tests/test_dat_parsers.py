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
    _interp_cota_vol,
    parse_commitment,
    parse_gen_constraints,
    parse_inflows,
    parse_max_generation,
    parse_renewable_profiles,
    parse_renewables,
    parse_unit_prices,
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


def _fw_chidro_line(
    num: int,
    name: str,
    vaa: int | str,
    pot: float,
    fpmed: float,
    qmax: float,
    vmin: float,
    vmax: float,
    vinic: float,
) -> str:
    """Render one PSR fixed-width ``chidro`` data row at the real offsets."""
    buf = [" "] * 100

    def put_right(val: object, end: int) -> None:
        s = f"{val}"
        buf[end - len(s) : end] = list(s)

    put_right(num, 4)
    s = f"{name}"
    buf[5 : 5 + len(s)] = list(s)  # name left-justified
    put_right(vaa, 27)
    put_right(pot, 50)
    put_right(fpmed, 58)
    put_right(qmax, 74)
    put_right(vmin, 82)
    put_right(vmax, 90)
    put_right(vinic, 98)
    return "".join(buf).rstrip() + "\n"


def test_interp_cota_vol() -> None:
    curve = [(100.0, 10.0), (200.0, 30.0), (300.0, 90.0)]
    assert _interp_cota_vol(150.0, curve) == pytest.approx(20.0)  # midpoint of seg 1
    assert _interp_cota_vol(250.0, curve) == pytest.approx(60.0)
    assert _interp_cota_vol(50.0, curve) == pytest.approx(10.0)  # clamp low
    assert _interp_cota_vol(400.0, curve) == pytest.approx(90.0)  # clamp high
    assert _interp_cota_vol(150.0, [(100.0, 10.0)]) is None  # < 2 points
    assert _interp_cota_vol(150.0, [(None, None), (100.0, 10.0)]) is None


def test_parse_max_generation(tmp_path: Path) -> None:
    """cprmx* parses per-plant hourly max-gen profiles; reports MW vs %G."""
    path = tmp_path / "cprmxhgu_u.dat"
    path.write_text(
        "Unidades   :    1  (1=MW,2=%G)\n"
        "****          601 CHX-H1      \n"
        "dd/mm/aaaa ....00h ....01h ....02h\n"
        "21/06/2026   20.73   20.73   20.73\n"
        "22/06/2026   20.73   20.73   20.73\n"
        "****          602 LPA-B1      \n"
        "dd/mm/aaaa ....00h ....01h ....02h\n"
        "21/06/2026    10.1    10.1    10.1\n"
        "22/06/2026     0.0     0.0     0.0\n",
        encoding="latin-1",
    )
    profiles, is_pct = parse_max_generation(path)
    assert is_pct is False
    assert profiles["CHX-H1"] == [pytest.approx(20.73)] * 6
    assert profiles["LPA-B1"] == [10.1, 10.1, 10.1, 0.0, 0.0, 0.0]


def test_parse_inflows_returns_profile(tmp_path: Path) -> None:
    """inflow.csv → the full per-stage profile, NOT the collapsed horizon mean."""
    path = tmp_path / "inflow.csv"
    path.write_text(
        "Varies per block?,0,Unit,m3/s\n"
        "Varies per sequence?,0\n"
        "# of agents,1\n"
        "Stag,Seq.,Blck,AGU-H1\n"
        "1,1,1,2.0\n2,1,1,4.0\n3,1,1,6.0\n",
        encoding="latin-1",
    )
    prof = parse_inflows(path)
    assert prof["AGU-H1"] == [2.0, 4.0, 6.0]  # profile preserved, not mean 4.0


def test_parse_renewables_and_profiles(tmp_path: Path) -> None:
    """cgndgu → generators (PotIns front-anchored, O&M back-anchored, blanks ok);
    cpgndgu → per-COD hourly forecast concatenated across days."""
    cg = tmp_path / "cgndgu.dat"
    cg.write_text(
        "$version=3\n"
        "!Num Name........ .Bus. Tipo #Uni .PotIns ..FatOpe ProbFal SFal Stat "
        "....O&M CurtCos TechTyp\n"
        "   1 SNT-E        16014    0   16   53.59       1.      0.    0     1"
        "    0.63      0.      0.\n"
        "   9 SDT-F         1445    0    1     40.       1.            0     9"
        "      0.      0.      0.\n",
        encoding="latin-1",
    )
    rens = {r.name: r for r in parse_renewables(cg)}
    assert rens["SNT-E"].pmax == pytest.approx(53.59)
    assert rens["SNT-E"].bus_number == 16014
    assert rens["SNT-E"].g_segments[0][1] == pytest.approx(0.63)  # O&M back-anchored
    assert rens["SNT-E"].is_import is False
    # SDT-F has a blank ProbFal column → O&M still read correctly (back-anchored).
    assert rens["SDT-F"].pmax == pytest.approx(40.0)
    assert rens["SDT-F"].g_segments[0][1] == pytest.approx(0.0)

    cp = tmp_path / "cpgndgu.dat"
    cp.write_text(
        "****    COD:    1 TP: 0\n"
        "dd/mm/aaaa ....00h ....01h ....02h\n"
        "21/06/2026  12.0  13.0  14.0\n"
        "22/06/2026   1.0   2.0   3.0\n"
        "****    COD:    9 TP: 0\n"
        "dd/mm/aaaa ....00h ....01h ....02h\n"
        "21/06/2026   5.0   6.0   7.0\n",
        encoding="latin-1",
    )
    prof = parse_renewable_profiles(cp)
    assert prof[1] == [12.0, 13.0, 14.0, 1.0, 2.0, 3.0]
    assert prof[9] == [5.0, 6.0, 7.0]


def test_parse_gen_constraints(tmp_path: Path) -> None:
    """RESTMEX → {unit: (type, [hourly RHS|None])}; labels normalised, blanks None."""
    path = tmp_path / "RESTMEX.csv"
    path.write_text(
        "#1,,,\n"
        "Tipo,<,>,=\n"
        "dd/mm/aaaa:hh,2-ORT-G DG,6-TER-B V,61-EDC-I\n"
        "21/06/2026:00h,14.6,7.0,\n"
        "21/06/2026:01h,14.6,,5.0\n",
        encoding="latin-1",
    )
    c = parse_gen_constraints(path)
    assert c["ORT-G"] == ("<", [14.6, 14.6])
    assert c["TER-B"] == (">", [7.0, None])  # blank → None (inactive that hour)
    assert c["EDC-I"] == ("=", [None, 5.0])


def test_parse_unit_prices(tmp_path: Path) -> None:
    """PRECIOSMEX.csv → per-unit hourly bids; quoted headers, blanks → 0."""
    path = tmp_path / "PRECIOSMEX.csv"
    path.write_text(
        "dd/mm/aaaa:hh,'ARI-O','MEX-I','MEX-I2'\n"
        "21/06/2026:00h,178.0,0,250\n"
        "21/06/2026:01h,178.0,0,\n",
        encoding="latin-1",
    )
    pr = parse_unit_prices(path)
    assert pr["ARI-O"] == [178.0, 178.0]
    assert pr["MEX-I"] == [0.0, 0.0]
    assert pr["MEX-I2"] == [250.0, 0.0]  # blank cell → 0


def test_parse_commitment(tmp_path: Path) -> None:
    """commith parses per-hydro on/off flags."""
    path = tmp_path / "commith.dat"
    path.write_text(
        "1! 601,!CHX-H1\n0! 603,!CHX-H3\n1! 605,!CHX-H5\n",
        encoding="latin-1",
    )
    commit = parse_commitment(path)
    assert commit == {"CHX-H1": True, "CHX-H3": False, "CHX-H5": True}


def test_hydro_parser_fixed_width_storage(tmp_path: Path) -> None:
    """Fixed-width chidro recovers storage (VMin/VMax), eini, QMax, topology."""
    header = (
        " NUM ...Nombre... .PV. .VAA .TAA #Uni Tipo ....Pot .FPMed. "
        ".QMin.. .QMax.. .VMin.. .VMax.. .VInic.\n"
    )
    rows = _fw_chidro_line(
        601, "RES-H1", 602, 50.73, 3.33333, 15.0, 90.0, 340.0, 200.0
    ) + _fw_chidro_line(602, "ROR-H2", "", 50.94, 3.34666, 15.0, 0.0, 0.0, 0.0)
    path = tmp_path / "chidroGU.dat"
    path.write_text(header + rows, encoding="latin-1")
    hydros = {h.name: h for h in HydroParser(path).parse()}

    res = hydros["RES-H1"]
    assert res.p_inst == pytest.approx(50.73)
    assert res.fp_med == pytest.approx(3.33333)
    assert res.qmax == pytest.approx(15.0)
    assert (res.vmin, res.vmax) == (pytest.approx(90.0), pytest.approx(340.0))
    assert res.downstream_code == 602
    # No cota–vol table on this short line → eini = midpoint, clamped to range.
    assert res.eini == pytest.approx(0.5 * (90.0 + 340.0))

    ror = hydros["ROR-H2"]
    assert ror.vmax == pytest.approx(0.0)  # run-of-river: no reservoir
    assert ror.downstream_code is None  # blank VAA → terminal


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


def test_load_dat_case_xz(dat_case_dir: Path, tmp_path: Path) -> None:
    """A .xz-compressed PSR .dat case reads transparently."""
    import lzma  # pylint: disable=import-outside-toplevel

    xz_dir = tmp_path / "dat_xz"
    xz_dir.mkdir()
    for f in dat_case_dir.iterdir():
        if f.is_file():
            (xz_dir / (f.name + ".xz")).write_bytes(lzma.compress(f.read_bytes()))
    assert is_dat_case(xz_dir)
    case = load_dat_case(xz_dir)
    assert len(case.thermals) == 2
    assert len(case.hydros) == 2
    assert len(case.demands) == 1
    assert case.study.num_blocks == 6


def test_validate_and_convert_dat(dat_case_dir: Path, tmp_path: Path) -> None:
    assert validate_sddp_case({"input_dir": dat_case_dir}) is True
    out = tmp_path / "out"
    rc = convert_sddp_case({"input_dir": dat_case_dir, "output_dir": out})
    assert rc == 0
    planning_file = out / "out.json"
    assert planning_file.is_file()
    data = json.loads(planning_file.read_text(encoding="utf-8"))
    assert len(data["system"]["generator_array"]) == 4
