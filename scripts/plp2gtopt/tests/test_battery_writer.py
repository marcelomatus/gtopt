"""Unit tests for BatteryWriter class."""

import pytest
import pandas as pd

from ..battery_parser import BatteryParser
from ..ess_parser import EssParser
from ..manbat_parser import ManbatParser
from ..battery_writer import BatteryWriter, BATTERY_UID_OFFSET


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


def _make_manbat_parser(tmp_path, content: str) -> ManbatParser:
    f = tmp_path / "plpmanbat.dat"
    f.write_text(content)
    p = ManbatParser(f)
    p.parse()
    return p


# ---------------------------------------------------------------------------
# battery_array
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
    assert b["input_efficiency"] == pytest.approx(0.95)
    assert b["output_efficiency"] == pytest.approx(0.95)
    assert b["vmin"] == pytest.approx(0.0)
    assert b["vmax"] == pytest.approx(1.0)
    assert b["vini"] == pytest.approx(0.5)
    assert b["capacity"] == pytest.approx(200.0)  # emax directly


def test_battery_array_vmin_from_emin_emax(tmp_path):
    """vmin is computed as emin/emax."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n 1     BAT1\n 1\n BAT1_C     0.90\n 5     0.90     50.0     200.0\n",
    )
    writer = BatteryWriter(battery_parser=bp)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b["vmin"] == pytest.approx(50.0 / 200.0)  # emin/emax = 0.25


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
    assert g["uid"] == BATTERY_UID_OFFSET + 1
    assert g["name"] == "BESS1_disch"
    assert g["bus"] == 3
    assert g["pmin"] == pytest.approx(0.0)
    # pmax_discharge: no central_parser -> defaults to 0.0; we just check the field exists
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
    assert d["uid"] == BATTERY_UID_OFFSET + 1
    assert d["name"] == "BESS1_chrg"
    assert d["bus"] == 2
    assert d["lmax"] == "lmax"


# ---------------------------------------------------------------------------
# converter_array
# ---------------------------------------------------------------------------


def test_converter_array(tmp_path):
    """BatteryWriter produces correct converter entry."""
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
    assert c["generator"] == BATTERY_UID_OFFSET + 1
    assert c["demand"] == BATTERY_UID_OFFSET + 1


# ---------------------------------------------------------------------------
# lmax parquet
# ---------------------------------------------------------------------------


def test_lmax_parquet_written(tmp_path):
    """BatteryWriter writes lmax.parquet column for charge demand."""
    bp = _make_battery_parser(
        tmp_path,
        " 1     1\n"
        " 1     BESS1\n"
        " 1\n"
        " BESS1_NEG     0.95\n"
        " 1     0.95     0.0     200.0\n",
    )

    # Pre-populate a demand lmax parquet (as DemandWriter would)
    demand_dir = tmp_path / "Demand"
    demand_dir.mkdir()
    existing = pd.DataFrame({"block": [1], "uid:1": [80.0]})
    existing.to_parquet(demand_dir / "lmax.parquet", index=False)

    writer = BatteryWriter(battery_parser=bp, options={"output_dir": tmp_path})
    writer.process([], [], tmp_path)

    df = pd.read_parquet(demand_dir / "lmax.parquet")
    bat_col = f"uid:{BATTERY_UID_OFFSET + 1}"
    assert bat_col in df.columns
    # Original demand column preserved
    assert "uid:1" in df.columns


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
# ESS path – capacity = pmax_discharge * hrs_reg
# ---------------------------------------------------------------------------


def test_battery_array_from_ess(tmp_path):
    """BatteryWriter produces correct battery_array from an ESS entry (no active)."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n    1  'ESS1'      1   100.0  100.0  0.90  0.90  2.0   0.40\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b["uid"] == 1
    assert b["name"] == "ESS1"
    assert b["input_efficiency"] == pytest.approx(0.90)
    assert b["output_efficiency"] == pytest.approx(0.90)
    assert b["capacity"] == pytest.approx(100.0 * 2.0)  # pmax_discharge * hrs_reg
    assert b["vmin"] == pytest.approx(0.0)
    assert b["vini"] == pytest.approx(0.40)
    # ESS has no active restriction
    assert "active" not in b


def test_ess_generator_array(tmp_path):
    """BatteryWriter produces discharge generator entry from ESS."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n    1  'ESS1'      3   50.0   60.0  0.95  0.95  4.0   0.50\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    gens = writer.to_generator_array()

    assert len(gens) == 1
    g = gens[0]
    assert g["uid"] == BATTERY_UID_OFFSET + 1
    assert g["name"] == "ESS1_disch"
    assert g["bus"] == 3
    assert g["pmax"] == pytest.approx(60.0)
    assert g["gcost"] == pytest.approx(0.0)


def test_ess_demand_array(tmp_path):
    """BatteryWriter produces charge demand entry from ESS."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n    1  'ESS1'      2   50.0   60.0  0.95  0.95  4.0   0.50\n",
    )
    writer = BatteryWriter(ess_parser=ep)
    dems = writer.to_demand_array()

    assert len(dems) == 1
    d = dems[0]
    assert d["uid"] == BATTERY_UID_OFFSET + 1
    assert d["name"] == "ESS1_chrg"
    assert d["bus"] == 2
    assert d["lmax"] == "lmax"


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
        " 1\n    1  'ESS1'      1   30.0   30.0  0.90  0.90  2.0   0.60\n",
    )
    writer = BatteryWriter(battery_parser=bp, ess_parser=ep)
    bats = writer.to_battery_array()

    # Only ESS1 – battery is silently ignored when ESS parser has entries
    assert len(bats) == 1
    assert bats[0]["name"] == "ESS1"
    assert bats[0]["capacity"] == pytest.approx(30.0 * 2.0)  # ESS formula


def test_process_with_ess(tmp_path):
    """process() with one ESS entry produces correct combined arrays."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n    1  'ESS1'      1   50.0   50.0  0.95  0.95  4.0   0.50\n",
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
