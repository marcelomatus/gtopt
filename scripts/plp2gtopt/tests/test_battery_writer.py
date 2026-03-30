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
    # Unified fields (no maintenance → bus/pmax present)
    assert b["bus"] == 1
    assert b["pmax_discharge"] == pytest.approx(0.0)  # no central_parser → 0
    assert b["pmax_charge"] == pytest.approx(0.0)
    assert b["gcost"] == pytest.approx(0.0)


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
    assert c["battery"] == "BESS1"
    # generator/demand names == battery name
    assert c["generator"] == "BESS1"
    assert c["demand"] == "BESS1"


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
    assert "converter_array" not in result
    assert result["generator_array"] == existing_gen
    assert result["demand_array"] == existing_dem


def test_process_with_battery(tmp_path):
    """process() with one battery uses unified definition (no separate gen/dem/conv)."""
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
    bat = result["battery_array"][0]
    # Unified fields present
    assert "bus" in bat
    assert "pmax_discharge" in bat
    assert "pmax_charge" in bat
    assert "gcost" in bat
    # No separate converter/generator/demand for battery
    assert "converter_array" not in result
    assert len(result["generator_array"]) == 1  # only thermal
    assert len(result["demand_array"]) == 1  # only demand


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
    # Unified fields (no maintenance)
    assert b["bus"] == 1  # default bus
    assert b["pmax_discharge"] == pytest.approx(100.0)
    assert b["pmax_charge"] == pytest.approx(100.0)
    assert b["gcost"] == pytest.approx(0.0)


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
    """process() with one ESS entry uses unified definition (no separate gen/dem/conv)."""
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
    bat = result["battery_array"][0]
    # Unified fields present
    assert "bus" in bat
    assert "pmax_discharge" in bat
    assert "pmax_charge" in bat
    assert "gcost" in bat
    # No separate converter/generator/demand for battery
    assert "converter_array" not in result
    assert len(result["generator_array"]) == 1  # only thermal
    assert len(result["demand_array"]) == 1  # only demand


# ---------------------------------------------------------------------------
# Maintenance schedule tests (plpmanbat.dat / plpmaness.dat)
# ---------------------------------------------------------------------------


def test_battery_maintenance_sets_emin_emax_reference(tmp_path):
    """When manbat provides maintenance, battery emin/emax are file refs.

    plpmanbat.dat modifies Emin/Emax (energy bounds) in Fortran, which map
    to Battery emin/emax schedules in gtopt.  Since manbat does NOT modify
    DC power bounds, the unified battery fields (bus, pmax_*) are still set.
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
    # manbat does NOT affect DC bounds → unified fields present
    assert "bus" in b
    assert "pmax_discharge" in b
    assert "pmax_charge" in b
    assert "gcost" in b


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
    """When maness provides maintenance with DCMax, gen pmax is file ref.

    DC-maintenance entries use the legacy multi-element approach, so the
    battery does NOT have unified fields (bus, pmax_*, gcost).
    """
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

    # Battery should NOT have unified fields (DC maintenance → legacy)
    bats = writer.to_battery_array()
    assert len(bats) == 1
    assert "bus" not in bats[0]
    assert "pmax_discharge" not in bats[0]


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


# ---------------------------------------------------------------------------
# source_generator — generation-coupled battery mode
# ---------------------------------------------------------------------------


def test_battery_cenbat_injection_sets_source_generator(tmp_path):
    """plpcenbat.dat with injection central produces source_generator in battery.

    When NIny > 0, the first injection central name is set as source_generator,
    enabling generation-coupled mode in System::expand_batteries().
    """
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n 1     BESS1\n 1\n SOLAR1     0.95\n 3     0.95     0.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    # source_generator is set to the first injection central name
    assert b.get("source_generator") == "SOLAR1"
    # Unified fields are still present (no DC maintenance)
    assert "bus" in b
    assert b["input_efficiency"] == pytest.approx(0.95)  # FPC from injection


def test_battery_cenbat_no_injection_no_source_generator(tmp_path):
    """plpcenbat.dat with NIny=0 (standalone) does not set source_generator."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n 1     BESS1\n 0\n 3     0.95     0.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    # No injection centrals → no source_generator
    assert b.get("source_generator") is None
    assert "bus" in b  # still unified


def test_ess_dcmod1_sets_source_generator(tmp_path):
    """ESS with dcmod=1 and cenpc produces source_generator in battery.

    When dcmod=1, the cenpc (primary charge central) is set as
    source_generator, enabling generation-coupled mode in
    System::expand_batteries().
    """
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.90  0.90  1.0  200.0  100.0  1  SOLAR1\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    # source_generator is set to cenpc name
    assert b.get("source_generator") == "SOLAR1"
    # Unified fields present (no DC maintenance)
    assert "bus" in b
    assert "pmax_discharge" in b


def test_ess_dcmod0_no_source_generator(tmp_path):
    """ESS with dcmod=0 (standalone) does not set source_generator."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.90  0.90  1.0  200.0  100.0  0\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b.get("source_generator") is None


def test_ess_dcmod1_empty_cenpc_no_source_generator(tmp_path):
    """ESS with dcmod=1 but empty cenpc does not set source_generator."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.90  0.90  1.0  200.0  100.0  1\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    # cenpc is empty → no source_generator
    assert b.get("source_generator") is None


def test_source_generator_not_set_in_dc_maintenance(tmp_path):
    """source_generator is not set for entries with DC-maintenance (legacy path)."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ESS1  0.95  0.95  0.0  200.0  50.0  1  SOLAR1\n",
    )
    mp = _make_maness_parser(
        tmp_path,
        "1\n'ESS1'\n2\n1  0.0  200.0  0.0  40.0  1\n2  0.0  180.0  0.0  35.0  1\n",
    )
    writer = BatteryWriter(ess_parser=ep, maness_parser=mp)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    # DC-maintenance path → no unified fields → no source_generator in battery
    assert "bus" not in b
    assert "source_generator" not in b


# ---------------------------------------------------------------------------
# DCMod=2: regulation tank → hydro reservoir, not battery
# ---------------------------------------------------------------------------


def test_ess_dcmod2_excluded_from_battery_array(tmp_path):
    """ESS with dcmod=2 (regulation tank) is not included in battery_array.

    DCMod=2 entries have nc=nd=1.0 (water storage, no electrical losses)
    and are mapped to hydro Reservoir elements instead of Battery.
    """
    ep = _make_ess_parser(
        tmp_path,
        " 2\n"
        "  ANGOSTURA_ERD  1.0  1.0  2.0  800.0  328.1  2  ANGOSTURA\n"
        "  ALFALFAL_BESS  0.9  0.9  1.0  244.3   59.3  1  ALFALFAL\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    bats = writer.to_battery_array()

    # Only ALFALFAL_BESS should appear (dcmod=1), not ANGOSTURA_ERD (dcmod=2)
    assert len(bats) == 1
    assert bats[0]["name"] == "ALFALFAL_BESS"


def test_ess_dcmod2_generates_regulation_reservoir(tmp_path):
    """ESS with dcmod=2 produces a reservoir via get_regulation_reservoirs().

    The reservoir attaches to the paired generator's junction and uses
    the ESS emax as energy capacity.
    """
    # ESS with dcmod=2 paired to ANGOSTURA
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ANGOSTURA_ERD  1.0  1.0  2.0  800.0  328.1  2  ANGOSTURA\n",
    )
    # Mock central_parser: provides centrals list with number (junction UID)
    # and type for the paired generator and the BAT central
    mock_cp = type(
        "MockCentralParser",
        (),
        {
            "centrals": [
                {"name": "ANGOSTURA", "number": 68, "type": "serie", "pmax": 328.1},
                {
                    "name": "ANGOSTURA_ERD",
                    "number": 1188,
                    "type": "bateria",
                    "pmax": 328.1,
                },
            ]
        },
    )()
    writer = BatteryWriter(ess_parser=ep, central_parser=mock_cp)
    reservoirs = writer.get_regulation_reservoirs()

    assert len(reservoirs) == 1
    r = reservoirs[0]
    assert r["name"] == "ANGOSTURA_ERD"
    assert r["uid"] == 1188
    assert r["junction"] == 68  # paired generator's central number
    assert r["emax"] == pytest.approx(800.0)
    assert r["emin"] == pytest.approx(0.0)
    assert r["annual_loss"] == pytest.approx(24.0)  # 2.0 * 12
    assert r["daily_cycle"] is True


def test_ess_dcmod2_no_reservoir_without_cenpc(tmp_path):
    """ESS with dcmod=2 but no cenpc does not generate a reservoir."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  ORPHAN_ERD  1.0  1.0  2.0  500.0  100.0  2\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    reservoirs = writer.get_regulation_reservoirs()

    assert len(reservoirs) == 0


def test_ess_dcmod0_no_regulation_reservoir(tmp_path):
    """ESS with dcmod=0 does not generate regulation reservoirs."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  BAT_ARICA  0.9  0.9  1.0  2.0  2.0  0\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    reservoirs = writer.get_regulation_reservoirs()

    assert len(reservoirs) == 0


def test_ess_dcmod2_rejects_non_hydro_central(tmp_path):
    """DCMod=2 paired with a non-hydro central (termica) raises ValueError."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  FAKE_ERD  1.0  1.0  2.0  100.0  50.0  2  THERMAL1\n",
    )
    mock_cp = type(
        "MockCentralParser",
        (),
        {
            "centrals": [
                {"name": "THERMAL1", "number": 99, "type": "termica", "pmax": 50.0},
                {"name": "FAKE_ERD", "number": 1000, "type": "bateria", "pmax": 50.0},
            ]
        },
    )()
    writer = BatteryWriter(ess_parser=ep, central_parser=mock_cp)

    with pytest.raises(ValueError, match="type 'termica'"):
        writer.get_regulation_reservoirs()


def test_ess_dcmod2_rejects_missing_central(tmp_path):
    """DCMod=2 paired with an unknown central raises ValueError."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  FAKE_ERD  1.0  1.0  2.0  100.0  50.0  2  UNKNOWN\n",
    )
    mock_cp = type(
        "MockCentralParser",
        (),
        {
            "centrals": [
                {"name": "FAKE_ERD", "number": 1000, "type": "bateria", "pmax": 50.0},
            ]
        },
    )()
    writer = BatteryWriter(ess_parser=ep, central_parser=mock_cp)

    with pytest.raises(ValueError, match="not found"):
        writer.get_regulation_reservoirs()


def test_ess_dcmod2_works_with_pasada_central(tmp_path):
    """DCMod=2 paired with a pasada (run-of-river) hydro central is valid.

    A pasada central has a junction and turbine but no reservoir.
    Adding a regulation reservoir to its junction correctly models
    the regulation tank: water balance at the junction sums the
    affluent inflow + reservoir extraction, and the turbine capacity
    enforces the joint power limit.
    """
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  PASADA_ERD  1.0  1.0  1.5  200.0  80.0  2  PASADA1\n",
    )
    mock_cp = type(
        "MockCentralParser",
        (),
        {
            "centrals": [
                {"name": "PASADA1", "number": 42, "type": "pasada", "pmax": 80.0},
                {
                    "name": "PASADA_ERD",
                    "number": 900,
                    "type": "bateria",
                    "pmax": 80.0,
                },
            ]
        },
    )()
    writer = BatteryWriter(ess_parser=ep, central_parser=mock_cp)
    reservoirs = writer.get_regulation_reservoirs()

    assert len(reservoirs) == 1
    r = reservoirs[0]
    assert r["name"] == "PASADA_ERD"
    assert r["uid"] == 900
    assert r["junction"] == 42  # pasada central's junction
    assert r["emax"] == pytest.approx(200.0)
    assert r["annual_loss"] == pytest.approx(18.0)  # 1.5 * 12


# ---------------------------------------------------------------------------
# Efficiency clamping: values > 1.0 are warned and clamped to 1.0
# ---------------------------------------------------------------------------


def test_ess_input_efficiency_clamped_to_one(tmp_path, caplog):
    """ESS with input_efficiency (nc) > 1.0 emits a warning and clamps to 1.0."""
    import logging

    # Format: Nombre nd nc mloss Emax DCMax DCMod — nc=5.23 (charge eff)
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  BAT_ALFALFAL  0.90  5.23  1.0  200.0  100.0  0\n",
    )
    writer = BatteryWriter(ess_parser=ep)

    with caplog.at_level(logging.DEBUG, logger="plp2gtopt.battery_writer"):
        bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b["input_efficiency"] == pytest.approx(1.0)
    assert b["output_efficiency"] == pytest.approx(0.90)

    # Check debug message was emitted
    warn_records = [r for r in caplog.records if "input_efficiency" in r.message]
    assert len(warn_records) == 1
    assert "5.23" in warn_records[0].message


def test_ess_efficiency_not_clamped_when_disabled(tmp_path, caplog):
    """With clamp_battery_efficiency=False, raw values pass through."""
    import logging

    ep = _make_ess_parser(
        tmp_path,
        " 1\n  BAT_X  0.90  5.23  1.0  200.0  100.0  0\n",
    )
    writer = BatteryWriter(ess_parser=ep, options={"clamp_battery_efficiency": False})

    with caplog.at_level(logging.DEBUG, logger="plp2gtopt.battery_writer"):
        bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    # Not clamped — raw value passes through
    assert b["input_efficiency"] == pytest.approx(5.23)
    assert b["output_efficiency"] == pytest.approx(0.90)

    # Debug message is still emitted
    warn_records = [r for r in caplog.records if "input_efficiency" in r.message]
    assert len(warn_records) == 1


def test_ess_output_efficiency_clamped_to_one(tmp_path, caplog):
    """ESS with output_efficiency (nd) > 1.0 emits a debug message and clamps to 1.0."""
    import logging

    # Format: Nombre nd nc mloss Emax DCMax DCMod — nd=1.50 (discharge eff)
    ep = _make_ess_parser(
        tmp_path,
        " 1\n  BAT_TEST  1.50  0.95  1.0  200.0  100.0  0\n",
    )
    writer = BatteryWriter(ess_parser=ep)

    with caplog.at_level(logging.DEBUG, logger="plp2gtopt.battery_writer"):
        bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b["input_efficiency"] == pytest.approx(0.95)
    assert b["output_efficiency"] == pytest.approx(1.0)

    warn_records = [r for r in caplog.records if "output_efficiency" in r.message]
    assert len(warn_records) == 1
    assert "1.5" in warn_records[0].message


def test_cenbat_efficiency_clamped_to_one(tmp_path, caplog):
    """plpcenbat.dat with FPC > 1.0 emits a debug message and clamps to 1.0."""
    import logging

    # Format: NBat BatBus / BatInd BatName / NIny / InjName FPC / BatBar FPD EMin EMax
    # FPC=5.23 in injection, FPD=0.95
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n 1     BAT1\n 1\n SOLAR1     5.23\n 1     0.95     0.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp)

    with caplog.at_level(logging.DEBUG, logger="plp2gtopt.battery_writer"):
        bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b["input_efficiency"] == pytest.approx(1.0)
    assert b["output_efficiency"] == pytest.approx(0.95)

    warn_records = [r for r in caplog.records if "input_efficiency" in r.message]
    assert len(warn_records) == 1
    assert "5.23" in warn_records[0].message
