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
    line = system["line_array"][0]
    assert line["bus_a"] == "b1" and line["bus_b"] == "b2"
    assert line["reactance"] == pytest.approx(0.05)
    gen = system["generator_array"][0]
    assert gen["bus"] == "b1" and gen["gcost"] == pytest.approx(30.0)
    dem = system["demand_array"][0]
    assert dem["bus"] == "b2" and dem["lmax"] == [[120.0, 80.0]]


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
