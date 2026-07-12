"""Tests for the multi-bus PSR ``.dat`` network front-end (dbus/dcirc/Volt/cpdexbus)."""

from __future__ import annotations

from pathlib import Path

import pytest

from sddp2gtopt.dat_parsers import (
    BusDemandParser,
    BusParser,
    CircuitParser,
    VoltParser,
    _split_reactance_region,
)
from sddp2gtopt.entities import BusSpec, CircuitSpec, DemandSpec, ThermalSpec
from sddp2gtopt.gtopt_writer import build_planning


def _write(path: Path, text: str) -> Path:
    path.write_text(text, encoding="latin-1")
    return path


def test_split_reactance_region() -> None:
    assert _split_reactance_region("  0.58  3.02") == (0.58, 3.02)
    assert _split_reactance_region("0.100.25") == (0.10, 0.25)  # merged columns
    assert _split_reactance_region("0.118.23") == (0.11, 8.23)
    assert _split_reactance_region("  0.10") == (0.10, 0.0)


def test_bus_parser(tmp_path: Path) -> None:
    dbus = _write(
        tmp_path / "dbus.dat",
        " NUM. Tp ...NombreB.. Id  # tg Plnt Nombre Gener.Area\n"
        " 1000  0 GSU-231      gu                              0\n"
        " 1601  0 CHX-H1       gu  1  1  601 CHX-H1            0\n",
    )
    bp = BusParser(dbus)
    buses = bp.parse()
    by_num = {b.number: b for b in buses}
    assert set(by_num) == {1000, 1601}
    assert by_num[1000].base_kv == pytest.approx(231.0)
    assert by_num[1601].base_kv == pytest.approx(0.0)  # gen bus, no voltage suffix
    assert bp.gen2bus == {"CHX-H1": 1601}


def test_circuit_parser(tmp_path: Path) -> None:
    dcirc = _write(
        tmp_path / "dcirc.dat",
        "#BOR.   #BDE.    .RESIS.REACT Nome........(MVAR)(Tmn)(Tmx)(  MW)(MW)   #\n"
        " 1000    1106      0.58  3.02 LINEA1                       491.6    1000 0 0\n"
        " 1857   16011        0.100.25 MERGED                       100.0    1001 0 0\n",
    )
    circ = CircuitParser(dcirc).parse()
    assert len(circ) == 2
    a, b = circ[0], circ[1]
    assert (a.from_bus, a.to_bus, a.name) == (1000, 1106, "LINEA1")
    assert a.reactance_pu == pytest.approx(3.02)  # raw ohms until loader converts
    assert a.rating == pytest.approx(491.6)
    assert (b.from_bus, b.to_bus) == (1857, 16011)
    assert b.resistance == pytest.approx(0.10) and b.reactance_pu == pytest.approx(0.25)


def test_volt_parser(tmp_path: Path) -> None:
    volt = _write(
        tmp_path / "Volt.dat",
        "(Bus) (...Nome...) O (.kV)\n"
        " 1000 GSU-231          230.\n"
        " 1601 CHX-H1            13.8\n",
    )
    kv = VoltParser(volt).parse()
    assert kv == {1000: pytest.approx(230.0), 1601: pytest.approx(13.8)}


def test_bus_demand_parser(tmp_path: Path) -> None:
    cpx = _write(
        tmp_path / "cpdexbus.dat",
        "Infxbloque :    1\n"
        "dd/mm/aaaa:hh   1000   1106\n"
        "21/06/2026:00   10.0   20.0\n"
        "21/06/2026:01   11.0   21.0\n",
    )
    series = BusDemandParser(cpx).parse()
    assert series[1000] == [10.0, 11.0]
    assert series[1106] == [20.0, 21.0]


def test_build_planning_multibus() -> None:
    from sddp2gtopt.entities import StudySpec  # local import keeps top tidy

    study = StudySpec(stage_type=1, num_stages=1, num_blocks=2, deficit_cost=500.0)
    buses = [
        BusSpec(number=1, name="A-230", base_kv=230.0),
        BusSpec(number=2, name="B-230", base_kv=230.0),
    ]
    circuits = [
        CircuitSpec(from_bus=1, to_bus=2, name="L12", reactance_pu=0.05, rating=300.0)
    ]
    thermals = [
        ThermalSpec(
            code=1,
            name="G1",
            reference_id=1,
            pmin=0.0,
            pmax=200.0,
            g_segments=[(200.0, 30.0)],
            bus_number=1,
        )
    ]
    demands = [
        DemandSpec(
            code=1, name="d2", reference_id=1, bus_number=2, block_values=[120.0, 80.0]
        )
    ]
    planning = build_planning(
        study=study,
        systems=[],
        thermals=thermals,
        hydros=[],
        demands=demands,
        name="mb",
        buses=buses,
        circuits=circuits,
    )
    mo = planning["options"]["model_options"]
    assert mo["use_kirchhoff"] is True and mo["use_single_bus"] is False
    system = planning["system"]
    assert {b["name"] for b in system["bus_array"]} == {"b1", "b2"}
    # Nominal kV from the PSR source (Volt.dat / dbus name suffix /
    # neighbour inference) is emitted as ``Bus.voltage``.
    assert [b["voltage"] for b in system["bus_array"]] == [230.0, 230.0]
    line = system["line_array"][0]
    assert line["bus_a"] == "b1" and line["bus_b"] == "b2"
    assert line["reactance"] == pytest.approx(0.05)
    gen = system["generator_array"][0]
    assert gen["bus"] == "b1" and gen["gcost"] == pytest.approx(30.0)
    dem = system["demand_array"][0]
    assert dem["bus"] == "b2" and dem["lmax"] == [[120.0, 80.0]]


def test_multibus_bus_voltage_omitted_when_unknown() -> None:
    """``base_kv == 0`` (no Volt.dat entry, no name suffix, no neighbour)
    means the kV is genuinely unknown — the ``voltage`` field must be
    OMITTED from the bus entry, never fabricated."""
    from sddp2gtopt.entities import StudySpec  # local import keeps top tidy

    study = StudySpec(stage_type=1, num_stages=1, num_blocks=1, deficit_cost=500.0)
    buses = [
        BusSpec(number=1, name="A-230", base_kv=230.0),
        BusSpec(number=2, name="GENBUS", base_kv=0.0),
    ]
    circuits = [
        CircuitSpec(from_bus=1, to_bus=2, name="L12", reactance_pu=0.05, rating=300.0)
    ]
    demands = [
        DemandSpec(code=1, name="d1", reference_id=1, bus_number=1, block_values=[50.0])
    ]
    planning = build_planning(
        study=study,
        systems=[],
        thermals=[],
        hydros=[],
        demands=demands,
        name="mb",
        buses=buses,
        circuits=circuits,
    )
    by_uid = {b["uid"]: b for b in planning["system"]["bus_array"]}
    assert by_uid[1]["voltage"] == 230.0
    assert "voltage" not in by_uid[2]


def _write_mini_multibus_case(root: Path) -> Path:
    """Write a complete 2-bus PSR .dat case (network + economics) to ``root``."""
    root.mkdir(parents=True, exist_ok=True)
    _write(
        root / "sddp.dat",
        "Mini multibus case\nETAPA  1\nNUMERO DE ETAPAS  1\n"
        "NUMERO DE SISTEMAS  1\nNUMERO DE BLOQUES DEMANDA  1\n"
        "TASA DE DESCUENTO  0.10\n"
        "----- Costo Deficit -----\n.....% c1\n   100   500.0\n",
    )
    _write(root / "sistem.dat", " NUM NOMBRE  ID\n0001 TESTSYS  TS\n")
    _write(
        root / "ccombuTS.dat",
        "!Num Nome UComb Custo\n   1 GAS  MWh  50.00\n",
    )
    _write(
        root / "ctermiTS.dat",
        "$version=1\n!Num Nombre #Uni Tipo PotIns GerMin GerMax Teif Ih CVaria MR Comb G1 CEsp1\n"
        "  10 GASPLANT  1 0  300.0  0.0  300.0  0. 0. 0. 0  1  100  2\n",
    )
    _write(
        root / "chidroTS.dat",
        " NUM Nombre PV VAA TAA #Uni Tipo Pot FPMed\n"
        " 201 HYDRO1  201 202  1 0  60.0 1.5\n",
    )
    _write(
        root / "dbus.dat",
        " NUM. Tp NombreB Id  # tg Plnt Nombre\n"
        " 1 0 GEN-230 ts  1 0 10 GASPLANT\n"
        " 1 0 GEN-230 ts  2 0 201 HYDRO1\n"
        " 2 0 LOAD-230 ts  0\n",
    )
    _write(
        root / "dcirc.dat",
        "#BOR.   #BDE.    .RESIS.REACT Nome (MW)   #\n"
        " 1       2         1.00  5.00 L12                          300.0    1 0 0\n",
    )
    _write(root / "Volt.dat", "(Bus) Nome O kV\n 1 GEN-230 230.\n 2 LOAD-230 230.\n")
    _write(
        root / "cpdexbus.dat",
        "Infxbloque : 1\ndd/mm/aaaa:hh   2\n01/01/2026:00   150.0\n",
    )
    return root


def test_load_dat_case_multibus(tmp_path: Path) -> None:
    from sddp2gtopt.dat_loader import load_dat_case

    case = load_dat_case(_write_mini_multibus_case(tmp_path))
    assert case.multi_bus is True
    assert len(case.buses) == 2
    assert len(case.circuits) == 1
    # ohm → per-unit: 5 Ω on 230 kV, 100 MVA base → 5·100/230² ≈ 0.00945.
    assert case.circuits[0].reactance_pu == pytest.approx(5 * 100 / 230**2, rel=1e-3)
    # generators routed to bus 1, demand to bus 2.
    assert {t.bus_number for t in case.thermals} == {1}
    assert {h.bus_number for h in case.hydros} == {1}
    assert case.demands[0].bus_number == 2
    assert case.demands[0].block_values == [150.0]


@pytest.mark.integration
def test_multibus_case_solves(tmp_path: Path, gtopt_bin: str) -> None:
    import json  # pylint: disable=import-outside-toplevel
    import subprocess  # pylint: disable=import-outside-toplevel

    from sddp2gtopt.sddp2gtopt import convert_sddp_case

    case_dir = _write_mini_multibus_case(tmp_path / "case")
    out = tmp_path / "out"
    assert convert_sddp_case({"input_dir": case_dir, "output_dir": out}) == 0
    data = json.loads((out / "out.json").read_text(encoding="utf-8"))
    assert data["options"]["model_options"]["use_kirchhoff"] is True
    assert "line_array" in data["system"]
    solve = tmp_path / "solve"
    subprocess.run(
        [gtopt_bin, "-s", str(out / "out.json"), "-d", str(solve), "-l", "ERROR"],
        check=True,
        timeout=120,
    )
    status = json.loads((solve / "solver_status.json").read_text(encoding="utf-8"))
    assert status["status"] == "done"


def test_parse_water_values(tmp_path: Path) -> None:
    from sddp2gtopt.dat_parsers import parse_water_values

    wv = _write(
        tmp_path / "watervcp.csv",
        "Varies per block?,0,Unit,k$/hm3\n"
        "Varies per sequence?,0\n"
        "# of agents,2\n"
        "Stag,Seq.,Blck,HYDRO1      ,HYDRO2      \n"
        "   1,   1,   1,  50.0      , 0.0\n"
        "   1,   1,   2,  70.0      , 0.0\n",
    )
    out = parse_water_values(wv)
    assert out["HYDRO1"] == pytest.approx(60.0)  # mean of 50, 70
    assert out["HYDRO2"] == pytest.approx(0.0)


def test_water_value_pricing(tmp_path: Path) -> None:
    """watervcp k$/hm³ → per-hydro gcost via FPMed (= WV·3.6/FPMed)."""
    from sddp2gtopt.dat_loader import load_dat_case

    case_dir = _write_mini_multibus_case(tmp_path)
    _write(
        case_dir / "watervcp.csv",
        "Unit,k$/hm3\nStag,Seq.,Blck,HYDRO1\n   1,   1,   1,  50.0\n",
    )
    case = load_dat_case(case_dir)
    h1 = next(h for h in case.hydros if h.name == "HYDRO1")
    # FPMed = 1.5 (fixture) → gcost = 50·3.6/1.5 = 120 $/MWh.
    assert h1.fp_med == pytest.approx(1.5)
    assert h1.gcost == pytest.approx(120.0)
    # Writer emits the per-plant water-value gcost.
    planning = build_planning(
        study=case.study,
        systems=case.systems,
        thermals=case.thermals,
        hydros=case.hydros,
        demands=case.demands,
        name="wv",
        buses=case.buses,
        circuits=case.circuits,
    )
    hyd = next(
        g for g in planning["system"]["generator_array"] if g["name"] == "HYDRO1"
    )
    assert hyd["gcost"] == pytest.approx(120.0)


def test_import_limit_caps_aggregate(tmp_path: Path) -> None:
    """--import-limit scales the aggregate Mexico/import-fuel pmax to the tie cap."""
    from sddp2gtopt.dat_loader import load_dat_case

    cd = tmp_path
    _write(
        cd / "sddp.dat",
        "x\nETAPA 1\nNUMERO DE ETAPAS 1\nNUMERO DE SISTEMAS 1\n"
        "NUMERO DE BLOQUES DEMANDA 1\nTASA DE DESCUENTO 0.1\n"
        "----- Costo Deficit -----\n.% c1\n 100 500.0\n",
    )
    _write(cd / "sistem.dat", " NUM NOMBRE ID\n0001 SYS TS\n")
    # Two import fuels (MEX-I, IMP-01, $0) + one domestic (GAS).
    _write(
        cd / "ccombuTS.dat",
        "!Num Nome UComb Custo\n   1 GAS  MWh 50.0\n"
        "   2 MEX-I MWh 0.0\n   3 IMP-01 MWh 0.0\n",
    )
    # G1 (domestic), M1+M2 (imports, 600+400 = 1000 MW).
    _write(
        cd / "ctermiTS.dat",
        "$version=1\n!Num Nombre #Uni Tipo PotIns GerMin GerMax Teif Ih CVaria MR Comb G1 CEsp1\n"
        "  10 G1  1 0 100.0 0.0 100.0 0. 0. 0. 0 1 100 1\n"
        "  11 M1  1 0 600.0 0.0 600.0 0. 0. 0. 0 2 100 1\n"
        "  12 M2  1 0 400.0 0.0 400.0 0. 0. 0. 0 3 100 1\n",
    )
    _write(
        cd / "cpdeTS.dat",
        "Infxbloque : 1\ndd/mm/aaaa h\n01/01/2026 120.0\n",
    )
    case = load_dat_case(cd, import_limit=200.0)
    by = {t.name: t for t in case.thermals}
    assert by["M1"].is_import and by["M2"].is_import and not by["G1"].is_import
    imp_total = by["M1"].pmax + by["M2"].pmax
    assert imp_total == pytest.approx(200.0)  # scaled 1000 → 200
    assert by["M1"].pmax == pytest.approx(120.0)  # 600·0.2
    assert by["G1"].pmax == pytest.approx(100.0)  # domestic untouched


def test_water_network_and_boundary_cut() -> None:
    """hydro_topology emits junctions/reservoirs/turbines/inflow + a boundary cut."""
    from sddp2gtopt.entities import HydroSpec, StudySpec

    study = StudySpec(stage_type=1, num_stages=1, num_blocks=2, deficit_cost=500.0)
    buses = [BusSpec(number=1, name="A-230", base_kv=230.0)]
    circuits = [CircuitSpec(from_bus=1, to_bus=1, reactance_pu=0.05, rating=300.0)]
    hydros = [
        # Storage plant (reservoir + water value) draining to the RoR plant.
        HydroSpec(
            code=601,
            name="RES-H1",
            reference_id=601,
            p_inst=50.0,
            fp_med=3.0,
            qmax=15.0,
            vmin=90.0,
            vmax=340.0,
            eini=200.0,
            efin=150.0,  # expected end volume (distinct from eini → used by the cut)
            inflow=33.5,
            water_value=185907.0,  # $/hm³
            downstream_code=602,
            bus_number=1,
        ),
        HydroSpec(
            code=602,
            name="ROR-H2",
            reference_id=602,
            p_inst=50.0,
            fp_med=3.0,
            qmax=15.0,
            inflow=5.0,
            bus_number=1,
        ),
    ]
    planning = build_planning(
        study=study,
        systems=[],
        thermals=[],
        hydros=hydros,
        demands=[],
        name="wn",
        buses=buses,
        circuits=circuits,
        hydro_topology=True,
    )
    system = planning["system"]
    # One junction + turbine per plant; one reservoir (only RES-H1 has storage).
    assert len(system["junction_array"]) == 2
    assert len(system["turbine_array"]) == 2
    assert len(system["reservoir_array"]) == 1
    assert len(system["flow_array"]) == 2  # both have inflow

    res = system["reservoir_array"][0]
    assert res["name"] == "rs_RES-H1"
    assert (res["emin"], res["emax"], res["eini"]) == (90.0, 340.0, 200.0)
    assert res["use_state_variable"] is True
    # hm³/(m³/s·h) conversion must be pinned (gtopt LP defaults absent → 3.6).
    assert res["flow_conversion_rate"] == pytest.approx(0.0036)
    # Soft end-of-horizon target at the expected end volume efin (150, NOT eini),
    # priced at the water value.
    assert res["efin"] == pytest.approx(150.0)
    assert res["efin_cost"] == pytest.approx(185907.0)

    # RES-H1 turbine routes to ROR-H2's junction (cascade); ROR-H2 is terminal.
    tb = {t["name"]: t for t in system["turbine_array"]}
    assert tb["tb_RES-H1"]["junction_b"] == "jn_ROR-H2"
    assert "junction_b" not in tb["tb_ROR-H2"]
    assert tb["tb_RES-H1"]["generator"] == "RES-H1"

    # Hydro generators are free (water value rides the cut, not gcost).
    gens = {g["name"]: g for g in system["generator_array"]}
    assert gens["RES-H1"]["gcost"] == pytest.approx(0.0)

    # Single-week dispatch → monolithic (default method, cut in monolithic_options).
    assert "method" not in planning["options"]
    assert planning["options"]["monolithic_options"]["boundary_cuts_file"]
    # The cut CSV carries -water_value for the priced reservoir.
    cut = planning["_boundary_cuts"]
    header, row = cut.strip().splitlines()
    assert header == "iteration,scene,rhs,rs_RES-H1"
    cells = row.split(",")
    assert float(cells[-1]) == pytest.approx(-185907.0)  # -WV
    assert float(cells[2]) == pytest.approx(185907.0 * 150.0)  # rhs = WV·efin (150)


def test_no_vfin_reservoir_priced_in_cut_without_efin_target() -> None:
    """No shipped vfin → no efin target (vfin free in [vmin,vmax]); still in cut."""
    from sddp2gtopt.entities import HydroSpec, StudySpec

    study = StudySpec(stage_type=1, num_stages=1, num_blocks=2, deficit_cost=500.0)
    buses = [BusSpec(number=1, name="A-230", base_kv=230.0)]
    circuits = [CircuitSpec(from_bus=1, to_bus=1, reactance_pu=0.05, rating=300.0)]
    # water_value > 0 but efin == 0 (volfincp absent) → must NOT assume vfin=vini.
    hydros = [
        HydroSpec(
            code=701,
            name="NOVF-H1",
            reference_id=701,
            p_inst=40.0,
            fp_med=2.0,
            qmax=10.0,
            vmin=50.0,
            vmax=200.0,
            eini=120.0,
            efin=0.0,  # no shipped end-volume
            water_value=90000.0,
            bus_number=1,
        )
    ]
    planning = build_planning(
        study=study,
        systems=[],
        thermals=[],
        hydros=hydros,
        demands=[],
        name="nv",
        buses=buses,
        circuits=circuits,
        hydro_topology=True,
    )
    res = planning["system"]["reservoir_array"][0]
    # Bounds are the natural emin/emax (vmin<=vfin<=vmax); no end target imposed.
    assert (res["emin"], res["emax"]) == (50.0, 200.0)
    assert "efin" not in res and "efin_cost" not in res
    # Still priced via the single cut (water-value coefficient), linearised at eini.
    cut = planning["_boundary_cuts"]
    header, row = cut.strip().splitlines()
    assert header == "iteration,scene,rhs,rs_NOVF-H1"
    cells = row.split(",")
    assert float(cells[-1]) == pytest.approx(-90000.0)  # -WV
    assert float(cells[2]) == pytest.approx(90000.0 * 120.0)  # rhs = WV·eini


def test_max_gen_cap_sets_pmax_pmin_zero_no_uc() -> None:
    """cprmx* cap drives generator pmax (not capacity); pmin is relaxed to 0.

    The base dispatch has no unit commitment, so a forced ``GerMin`` must-run
    would over-supply (free hydro becomes marginal, crashing the LMP).  pmin is
    therefore emitted as 0 regardless of the parsed minimum; capacity stays at
    installed and the pmax cap still follows the operational ``cprmx*`` profile.
    """
    from sddp2gtopt.entities import StudySpec  # ThermalSpec is imported above

    study = StudySpec(stage_type=1, num_stages=1, num_blocks=4, deficit_cost=500.0)
    buses = [BusSpec(number=1, name="A-230", base_kv=230.0)]
    circuits = [CircuitSpec(from_bus=1, to_bus=1, reactance_pu=0.05, rating=300.0)]
    thermals = [
        # Constant cap below installed → scalar pmax = cap; capacity = installed.
        ThermalSpec(
            code=1,
            name="GCAP",
            reference_id=1,
            pmin=0.0,
            pmax=50.0,
            g_segments=[(50.0, 30.0)],
            bus_number=1,
            max_gen=[20.0, 20.0, 20.0, 20.0],
        ),
        # Cap drops to 0 (maintenance); parsed pmin=4 is relaxed to 0 (no UC).
        ThermalSpec(
            code=2,
            name="GMNT",
            reference_id=2,
            pmin=4.0,
            pmax=30.0,
            g_segments=[(30.0, 40.0)],
            bus_number=1,
            max_gen=[10.0, 10.0, 0.0, 0.0],
        ),
    ]
    planning = build_planning(
        study=study,
        systems=[],
        thermals=thermals,
        hydros=[],
        demands=[],
        name="cap",
        buses=buses,
        circuits=circuits,
    )
    g = {x["name"]: x for x in planning["system"]["generator_array"]}
    # GCAP: pmax = cap 20 (scalar), capacity stays installed 50, pmin 0.
    assert g["GCAP"]["pmax"] == pytest.approx(20.0)
    assert g["GCAP"]["capacity"] == pytest.approx(50.0)
    assert g["GCAP"]["pmin"] == pytest.approx(0.0)
    # GMNT: pmax is the per-block cap schedule; pmin relaxed to 0 (no must-run).
    assert g["GMNT"]["pmax"] == [[10.0, 10.0, 0.0, 0.0]]
    assert g["GMNT"]["pmin"] == pytest.approx(0.0)
    assert g["GMNT"]["capacity"] == pytest.approx(30.0)


def test_import_gcost_profile_and_renewable_emit() -> None:
    """An import's PRECIOSMEX bid emits a time-varying gcost; a renewable
    (capped by max_gen) emits at its O&M cost; both carry pmin 0 (no UC)."""
    from sddp2gtopt.entities import StudySpec  # ThermalSpec is imported above

    study = StudySpec(stage_type=1, num_stages=1, num_blocks=3, deficit_cost=500.0)
    buses = [BusSpec(number=1, name="A-230", base_kv=230.0)]
    circuits = [CircuitSpec(from_bus=1, to_bus=1, reactance_pu=0.05, rating=300.0)]
    thermals = [
        ThermalSpec(
            code=1,
            name="MEX-I2",
            reference_id=1,
            pmin=0.0,
            pmax=30.0,
            g_segments=[(30.0, 0.0)],
            bus_number=1,
            is_import=True,
            gcost_profile=[250.0, 250.0, 0.0],  # 250 on the billed day, 0 ahead
        ),
        ThermalSpec(
            code=2,
            name="SOL-F",
            reference_id=2,
            pmin=0.0,
            pmax=50.0,
            g_segments=[(50.0, 1.5)],
            bus_number=1,
            max_gen=[0.0, 40.0, 20.0],  # hourly availability forecast
        ),
    ]
    planning = build_planning(
        study=study,
        systems=[],
        thermals=thermals,
        hydros=[],
        demands=[],
        name="px",
        buses=buses,
        circuits=circuits,
    )
    g = {x["name"]: x for x in planning["system"]["generator_array"]}
    assert g["MEX-I2"]["gcost"] == [[250.0, 250.0, 0.0]]  # time-varying import bid
    assert g["MEX-I2"]["pmin"] == pytest.approx(0.0)
    assert g["SOL-F"]["pmax"] == [[0.0, 40.0, 20.0]]  # forecast cap as pmax
    assert g["SOL-F"]["gcost"] == pytest.approx(1.5)  # O&M cost


def test_amm_constraint_overrides_bounds() -> None:
    """A RESTMEX constraint overrides bounds: ``<``→pmax, ``>``→pmin (None→0)."""
    from sddp2gtopt.entities import StudySpec  # ThermalSpec is imported above

    study = StudySpec(stage_type=1, num_stages=1, num_blocks=2, deficit_cost=500.0)
    buses = [BusSpec(number=1, name="A-230", base_kv=230.0)]
    circuits = [CircuitSpec(from_bus=1, to_bus=1, reactance_pu=0.05, rating=300.0)]
    thermals = [
        ThermalSpec(
            code=1,
            name="ORT-G",
            reference_id=1,
            pmin=0.0,
            pmax=15.84,
            g_segments=[(15.84, 30.0)],
            bus_number=1,
            amm_tipo="<",
            amm_profile=[14.6, 14.6],
        ),
        ThermalSpec(
            code=2,
            name="TER-B",
            reference_id=2,
            pmin=0.0,
            pmax=50.0,
            g_segments=[(50.0, 40.0)],
            bus_number=1,
            amm_tipo=">",
            amm_profile=[7.0, None],
        ),
    ]
    planning = build_planning(
        study=study,
        systems=[],
        thermals=thermals,
        hydros=[],
        demands=[],
        name="amm",
        buses=buses,
        circuits=circuits,
    )
    g = {x["name"]: x for x in planning["system"]["generator_array"]}
    assert g["ORT-G"]["pmax"] == pytest.approx(14.6)  # < cap (constant → scalar)
    assert g["ORT-G"]["pmin"] == pytest.approx(0.0)
    assert g["TER-B"]["pmin"] == [[7.0, 0.0]]  # > floor; blank hour → 0
    assert g["TER-B"]["pmax"] == pytest.approx(50.0)  # unchanged (installed)


def test_committed_off_hydro_pmax_zero_turbine_drains() -> None:
    """A committed-off hydro gets pmax 0 and a drain turbine (water bypasses)."""
    from sddp2gtopt.entities import HydroSpec, StudySpec

    study = StudySpec(stage_type=1, num_stages=1, num_blocks=2, deficit_cost=500.0)
    buses = [BusSpec(number=1, name="A-230", base_kv=230.0)]
    circuits = [CircuitSpec(from_bus=1, to_bus=1, reactance_pu=0.05, rating=300.0)]
    hydros = [
        HydroSpec(
            code=601,
            name="ON-H1",
            reference_id=601,
            p_inst=50.0,
            fp_med=3.0,
            qmax=15.0,
            downstream_code=603,
            inflow=30.0,
            bus_number=1,
            committed=True,
        ),
        HydroSpec(
            code=603,
            name="OFF-H3",  # committed off → pmax 0, turbine drains downstream
            reference_id=603,
            p_inst=50.0,
            fp_med=3.0,
            qmax=15.0,
            bus_number=1,
            committed=False,
        ),
    ]
    planning = build_planning(
        study=study,
        systems=[],
        thermals=[],
        hydros=hydros,
        demands=[],
        name="cm",
        buses=buses,
        circuits=circuits,
        hydro_topology=True,
    )
    g = {x["name"]: x for x in planning["system"]["generator_array"]}
    assert g["ON-H1"]["pmax"] == pytest.approx(50.0)
    assert g["OFF-H3"]["pmax"] == pytest.approx(0.0)  # off → cannot generate
    tb = {t["name"]: t for t in planning["system"]["turbine_array"]}
    assert "drain" not in tb["tb_ON-H1"]  # on: power = pf·flow
    assert tb["tb_OFF-H3"]["drain"] is True  # off: power ≤ pf·flow → flow bypasses


def test_multistage_emits_sddp_simulation() -> None:
    """Daily staging emits phase/scene/aperture arrays for the SDDP run."""
    from sddp2gtopt.entities import StudySpec

    study = StudySpec(
        stage_type=1, num_stages=3, num_blocks=24, block_hours=1.0, deficit_cost=500.0
    )
    buses = [BusSpec(number=1, name="A-230", base_kv=230.0)]
    circuits = [CircuitSpec(from_bus=1, to_bus=1, reactance_pu=0.05, rating=300.0)]
    demands = [
        DemandSpec(
            code=1,
            name="d1",
            reference_id=1,
            bus_number=1,
            block_values=[100.0] * 72,  # 3 stages × 24 blocks
        )
    ]
    planning = build_planning(
        study=study,
        systems=[],
        thermals=[],
        hydros=[],
        demands=demands,
        name="ms",
        buses=buses,
        circuits=circuits,
    )
    sim = planning["simulation"]
    assert len(sim["phase_array"]) == 3
    assert sim["phase_array"][1] == {
        "uid": 2,
        "first_stage": 1,
        "count_stage": 1,
        "apertures": [1],
    }
    assert len(sim["scene_array"]) == 1
    assert len(sim["aperture_array"]) == 1
    assert sim["block_array"][0]["duration"] == pytest.approx(1.0)
    # Demand reshaped into [stage][block].
    lmax = planning["system"]["demand_array"][0]["lmax"]
    assert len(lmax) == 3 and all(len(r) == 24 for r in lmax)


def test_build_planning_hydro_cost() -> None:
    from sddp2gtopt.entities import HydroSpec, StudySpec

    study = StudySpec(stage_type=1, num_stages=1, num_blocks=1)
    buses = [
        BusSpec(number=1, name="A-230", base_kv=230.0),
        BusSpec(number=2, name="B-230", base_kv=230.0),
    ]
    circuits = [CircuitSpec(from_bus=1, to_bus=2, reactance_pu=0.05, rating=300.0)]
    hydros = [HydroSpec(code=1, name="H1", reference_id=1, p_inst=50.0, bus_number=1)]
    planning = build_planning(
        study=study,
        systems=[],
        thermals=[],
        hydros=hydros,
        demands=[],
        name="mb",
        buses=buses,
        circuits=circuits,
        hydro_cost=92.0,
    )
    gen = planning["system"]["generator_array"][0]
    assert gen["gcost"] == pytest.approx(92.0)
