"""Unit tests for BatteryWriter class."""

import pytest
import pandas as pd

from ..battery_parser import BatteryParser
from ..ess_parser import EssParser
from ..central_parser import CentralParser
from ..manbat_parser import ManbatParser
from ..maness_parser import ManessParser
from ..battery_writer import BatteryWriter


def _make_battery_parser(tmp_path, content: str) -> BatteryParser:
    f = tmp_path / "plpcenbat.dat"
    f.write_text(content)
    p = BatteryParser(f)
    p.parse()
    return p


def _make_ess_parser(tmp_path, content: str) -> EssParser:
    f = tmp_path / "plpess.dat"
    f.write_text(content)
    p = EssParser(f)
    p.parse()
    return p


def _make_central_parser(tmp_path, content: str) -> CentralParser:
    f = tmp_path / "plpcnfce.dat"
    f.write_text(content)
    p = CentralParser(f)
    p.parse()
    return p


def _make_manbat_parser(tmp_path, content: str) -> ManbatParser:
    f = tmp_path / "plpmanbat.dat"
    f.write_text(content)
    p = ManbatParser(f)
    p.parse()
    return p


def _make_maness_parser(tmp_path, content: str) -> ManessParser:
    f = tmp_path / "plpmaness.dat"
    f.write_text(content)
    p = ManessParser(f)
    p.parse()
    return p


# ---------------------------------------------------------------------------
# battery_array from plpcenbat.dat
# ---------------------------------------------------------------------------


def test_battery_array_from_cenbat(tmp_path):
    """BatteryWriter produces correct battery_array from a plpcenbat.dat entry."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 1     0.95     0.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b["uid"] == 1  # BatInd (no central_parser to override)
    assert b["name"] == "BESS1"
    assert b["input_efficiency"] == pytest.approx(0.95)  # FPC from injection
    assert b["output_efficiency"] == pytest.approx(0.95)  # FPD
    assert b["emin"] == pytest.approx(0.0)  # absolute emin value
    assert b["emax"] == pytest.approx(200.0)  # absolute emax value
    assert "eini" not in b  # eini not read from PLP files
    assert b["capacity"] == pytest.approx(200.0)  # emax directly


def test_battery_array_emin_from_emin_emax(tmp_path):
    """emin is the absolute value from plpcenbat.dat."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n 1     BAT1\n 1\n BAT1_C     0.90\n 5     0.90     50.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b["emin"] == pytest.approx(50.0)  # absolute emin value


# ---------------------------------------------------------------------------
# generator_array (discharge path)
# ---------------------------------------------------------------------------


def test_generator_array(tmp_path):
    """BatteryWriter produces correct discharge generator entry."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 3     0.95     0.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp)
    gens = writer.to_generator_array()

    assert len(gens) == 1
    g = gens[0]
    # uid == battery central number (no offset)
    assert g["uid"] == 1
    assert g["name"] == "BESS1_disch"
    assert g["bus"] == 3
    assert g["pmin"] == pytest.approx(0.0)
    assert "pmax" in g
    assert g["gcost"] == pytest.approx(0.0)


# ---------------------------------------------------------------------------
# demand_array (charge path)
# ---------------------------------------------------------------------------


def test_demand_array(tmp_path):
    """BatteryWriter produces correct charge demand entry."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 2     0.95     0.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp)
    dems = writer.to_demand_array()

    assert len(dems) == 1
    d = dems[0]
    # uid == battery central number (no offset)
    assert d["uid"] == 1
    assert d["name"] == "BESS1_chrg"
    assert d["bus"] == 2
    # Without maintenance, lmax is a scalar (pmax_charge)
    assert isinstance(d["lmax"], float)


# ---------------------------------------------------------------------------
# converter_array
# ---------------------------------------------------------------------------


def test_converter_array(tmp_path):
    """BatteryWriter produces correct converter entry linking bat/gen/dem."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 1     0.95     0.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp)
    convs = writer.to_converter_array()

    assert len(convs) == 1
    c = convs[0]
    assert c["uid"] == 1
    assert c["name"] == "BESS1"
    assert c["battery"] == 1
    # generator/demand uids == battery uid (no offset)
    assert c["generator"] == 1
    assert c["demand"] == 1


# ---------------------------------------------------------------------------
# process() - combined result
# ---------------------------------------------------------------------------


def test_process_no_battery(tmp_path):
    """process() with no battery/BAT centrals returns unchanged arrays."""
    writer = BatteryWriter()
    existing_gen = [{"uid": 1, "name": "Gen1"}]
    existing_dem = [{"uid": 1, "name": "Dem1"}]
    result = writer.process(existing_gen, existing_dem, tmp_path)
    assert not result["battery_array"]
    assert not result["converter_array"]
    assert result["generator_array"] == existing_gen
    assert result["demand_array"] == existing_dem


def test_process_with_battery(tmp_path):
    """process() with one battery produces correct combined arrays."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 1     0.95     0.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp, options={"output_dir": tmp_path})
    result = writer.process(
        [{"uid": 1, "name": "Thermal1"}],
        [{"uid": 1, "name": "DemBus1"}],
        tmp_path,
    )

    assert len(result["battery_array"]) == 1
    assert len(result["converter_array"]) == 1
    assert len(result["generator_array"]) == 2  # 1 thermal + 1 battery discharge
    assert len(result["demand_array"]) == 2  # 1 demand + 1 battery charge


# ---------------------------------------------------------------------------
# ESS path – plpess.dat format: Nombre nd nc mloss emax dcmax [dcmod] [cenpc]
# ---------------------------------------------------------------------------


def test_battery_array_from_ess(tmp_path):
    """BatteryWriter produces correct battery_array from ESS entry."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.90  0.90  1.0  200.0  100.0  0\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b["name"] == "ESS1"
    assert b["input_efficiency"] == pytest.approx(0.90)  # nc
    assert b["output_efficiency"] == pytest.approx(0.90)  # nd
    assert b["capacity"] == pytest.approx(200.0)  # emax from plpess.dat
    assert b["emin"] == pytest.approx(0.0)  # no emin in ESS
    assert b["emax"] == pytest.approx(200.0)  # absolute emax from plpess.dat
    assert "eini" not in b  # eini not read from PLP files


def test_ess_generator_array(tmp_path):
    """BatteryWriter produces discharge generator from ESS (dcmax = pmax)."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.95  0.95  0.0  200.0  60.0  0\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    gens = writer.to_generator_array()

    assert len(gens) == 1
    g = gens[0]
    assert g["name"] == "ESS1_disch"
    assert g["pmax"] == pytest.approx(60.0)  # dcmax from plpess.dat
    assert g["gcost"] == pytest.approx(0.0)


def test_ess_demand_array(tmp_path):
    """BatteryWriter produces charge demand from ESS (dcmax = lmax)."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.95  0.95  0.0  200.0  50.0  0\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    dems = writer.to_demand_array()

    assert len(dems) == 1
    d = dems[0]
    assert d["name"] == "ESS1_chrg"
    # Without maintenance, lmax is a scalar (pmax_charge = dcmax)
    assert d["lmax"] == pytest.approx(50.0)


def test_ess_takes_priority_over_battery(tmp_path):
    """When both ESS and battery parsers provided, ESS takes priority."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BAT1\n"
        " 1\n"
        " BAT1_NEG     0.95\n"
        " 1     0.95     0.0     200.0\n",
    )
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.90  0.90  1.0  300.0  60.0  0\n",
    )
    writer = BatteryWriter(battery_parser=bp, ess_parser=ep)
    bats = writer.to_battery_array()

    # Only ESS1 – battery is silently ignored when ESS parser has entries
    assert len(bats) == 1
    assert bats[0]["name"] == "ESS1"
    assert bats[0]["capacity"] == pytest.approx(300.0)  # ESS emax


def test_ess_annual_loss(tmp_path):
    """annual_loss is mloss * 12."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.90  0.90  1.5  200.0  50.0  0\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    entries = writer._all_entries()  # pylint: disable=protected-access

    assert len(entries) == 1
    assert entries[0]["annual_loss"] == pytest.approx(1.5 * 12)


def test_process_with_ess(tmp_path):
    """process() with one ESS entry produces correct combined arrays."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.95  0.95  0.0  200.0  50.0  0\n",
    )
    writer = BatteryWriter(ess_parser=ep, options={"output_dir": tmp_path})
    result = writer.process(
        [{"uid": 1, "name": "Thermal1"}],
        [{"uid": 1, "name": "DemBus1"}],
        tmp_path,
    )

    assert len(result["battery_array"]) == 1
    assert len(result["converter_array"]) == 1
    assert len(result["generator_array"]) == 2  # 1 thermal + 1 ESS discharge
    assert len(result["demand_array"]) == 2  # 1 demand + 1 ESS charge


# ---------------------------------------------------------------------------
# Maintenance schedule tests (plpmanbat.dat / plpmaness.dat)
# ---------------------------------------------------------------------------


def test_battery_maintenance_sets_emin_emax_reference(tmp_path):
    """When manbat provides maintenance, battery emin/emax are file refs.

    plpmanbat.dat modifies Emin/Emax (energy bounds) in Fortran, which map
    to Battery emin/emax schedules in gtopt.
    """
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 1     0.95     0.0     200.0\n",
    )
    mp = _make_manbat_parser(
        tmp_path,
        "1\nBESS1\n2\n1  0.0  180.0\n2  10.0  150.0\n",
    )
    writer = BatteryWriter(battery_parser=bp, manbat_parser=mp)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    # With maintenance, emin/emax are file references
    assert b["emin"] == "emin"
    assert b["emax"] == "emax"


def test_battery_maintenance_gen_pmax_is_scalar(tmp_path):
    """plpmanbat.dat does NOT affect generator pmax (only Emin/Emax).

    Discharge generator pmax should remain a scalar value.
    """
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 1     0.95     0.0     200.0\n",
    )
    mp = _make_manbat_parser(
        tmp_path,
        "1\nBESS1\n2\n1  0.0  180.0\n2  10.0  150.0\n",
    )
    writer = BatteryWriter(battery_parser=bp, manbat_parser=mp)
    gens = writer.to_generator_array()

    assert len(gens) == 1
    g = gens[0]
    # plpmanbat only modifies energy bounds, NOT pmax
    assert isinstance(g["pmax"], float)


def test_battery_maintenance_dem_lmax_is_scalar(tmp_path):
    """plpmanbat.dat does NOT affect demand lmax (only Emin/Emax).

    Charge demand lmax should remain a scalar value.
    """
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 1     0.95     0.0     200.0\n",
    )
    mp = _make_manbat_parser(
        tmp_path,
        "1\nBESS1\n2\n1  0.0  180.0\n2  10.0  150.0\n",
    )
    writer = BatteryWriter(battery_parser=bp, manbat_parser=mp)
    dems = writer.to_demand_array()

    assert len(dems) == 1
    d = dems[0]
    # plpmanbat only modifies energy bounds, NOT pmax/lmax
    assert isinstance(d["lmax"], float)


def test_ess_maintenance_sets_pmax_reference(tmp_path):
    """When maness provides maintenance with DCMax, gen pmax is file ref."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.95  0.95  0.0  200.0  50.0  0\n",
    )
    mp = _make_maness_parser(
        tmp_path,
        "1\n'ESS1'\n2\n1  0.0  200.0  0.0  40.0  1\n2  0.0  180.0  0.0  35.0  1\n",
    )
    writer = BatteryWriter(ess_parser=ep, maness_parser=mp)
    gens = writer.to_generator_array()

    assert len(gens) == 1
    g = gens[0]
    assert g["pmax"] == "pmax"


def test_ess_maintenance_sets_lmax_reference(tmp_path):
    """When maness provides maintenance with DCMax, dem lmax is file ref."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.95  0.95  0.0  200.0  50.0  0\n",
    )
    mp = _make_maness_parser(
        tmp_path,
        "1\n'ESS1'\n2\n1  0.0  200.0  0.0  40.0  1\n2  0.0  180.0  0.0  35.0  1\n",
    )
    writer = BatteryWriter(ess_parser=ep, maness_parser=mp)
    dems = writer.to_demand_array()

    assert len(dems) == 1
    d = dems[0]
    assert d["lmax"] == "lmax"


def test_maintenance_parquet_battery(tmp_path):
    """process() with battery maintenance writes Battery/emin.parquet and emax.parquet."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 1     0.95     0.0     200.0\n",
    )
    mp = _make_manbat_parser(
        tmp_path,
        "1\nBESS1\n2\n1  0.0  180.0\n2  10.0  150.0\n",
    )
    writer = BatteryWriter(battery_parser=bp, manbat_parser=mp)
    writer.process([], [], tmp_path)

    emin_path = tmp_path / "Battery" / "emin.parquet"
    assert emin_path.exists(), "Battery/emin.parquet not written"
    df_emin = pd.read_parquet(emin_path)
    assert "uid:1" in df_emin.columns
    assert len(df_emin) == 2
    # emin values are absolute (not normalized by capacity)
    assert df_emin["uid:1"].iloc[0] == pytest.approx(0.0)
    assert df_emin["uid:1"].iloc[1] == pytest.approx(10.0)

    emax_path = tmp_path / "Battery" / "emax.parquet"
    assert emax_path.exists(), "Battery/emax.parquet not written"
    df_emax = pd.read_parquet(emax_path)
    assert "uid:1" in df_emax.columns
    # emax values are absolute (not normalized by capacity)
    assert df_emax["uid:1"].iloc[0] == pytest.approx(180.0)
    assert df_emax["uid:1"].iloc[1] == pytest.approx(150.0)


def test_maintenance_parquet_ess(tmp_path):
    """process() with ESS maintenance writes Battery+Generator+Demand parquet."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.95  0.95  0.0  200.0  50.0  0\n",
    )
    mp = _make_maness_parser(
        tmp_path,
        "1\n'ESS1'\n2\n1  0.0  200.0  0.0  40.0  1\n2  0.0  180.0  0.0  35.0  1\n",
    )
    writer = BatteryWriter(ess_parser=ep, maness_parser=mp)
    writer.process([], [], tmp_path)

    # Battery energy bounds
    emin_path = tmp_path / "Battery" / "emin.parquet"
    assert emin_path.exists()
    emax_path = tmp_path / "Battery" / "emax.parquet"
    assert emax_path.exists()

    # DC power bounds
    pmax_path = tmp_path / "Generator" / "pmax.parquet"
    assert pmax_path.exists(), "Generator/pmax.parquet not written"
    df_pmax = pd.read_parquet(pmax_path)
    assert "uid:0" in df_pmax.columns  # uid=0 because no central_parser
    assert len(df_pmax) == 2

    lmax_path = tmp_path / "Demand" / "lmax.parquet"
    assert lmax_path.exists(), "Demand/lmax.parquet not written"
    df_lmax = pd.read_parquet(lmax_path)
    assert len(df_lmax) == 2


def test_no_maintenance_no_parquet(tmp_path):
    """process() without maintenance does not write maintenance parquet files."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 1     0.95     0.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp)
    writer.process([], [], tmp_path)

    pmax_path = tmp_path / "Generator" / "pmax.parquet"
    assert not pmax_path.exists()
    emin_path = tmp_path / "Battery" / "emin.parquet"
    assert not emin_path.exists()
