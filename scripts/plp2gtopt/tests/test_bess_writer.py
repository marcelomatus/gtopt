"""Unit tests for BessWriter class."""

from pathlib import Path
import pytest
import pandas as pd

from ..bess_parser import BessParser
from ..ess_parser import EssParser
from ..manbess_parser import ManbessParser
from ..bess_writer import BessWriter, BESS_UID_OFFSET


def _make_bess_parser(tmp_path, content: str) -> BessParser:
    f = tmp_path / "plpbess.dat"
    f.write_text(content)
    p = BessParser(f)
    p.parse()
    return p


def _make_ess_parser(tmp_path, content: str) -> EssParser:
    f = tmp_path / "plpess.dat"
    f.write_text(content)
    p = EssParser(f)
    p.parse()
    return p


def _make_manbess_parser(tmp_path, content: str) -> ManbessParser:
    f = tmp_path / "plpmanbess.dat"
    f.write_text(content)
    p = ManbessParser(f)
    p.parse()
    return p


# ---------------------------------------------------------------------------
# battery_array
# ---------------------------------------------------------------------------


def test_battery_array_from_bess(tmp_path):
    """BessWriter produces correct battery_array from a BESS entry."""
    bp = _make_bess_parser(
        tmp_path,
        " 1\n    1  'BESS1'     1   50.0   50.0  0.95  0.95  4.0    1    1    0.50    1.0\n",
    )
    writer = BessWriter(bess_parser=bp)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b["uid"] == 1
    assert b["name"] == "BESS1"
    assert b["input_efficiency"] == pytest.approx(0.95)
    assert b["output_efficiency"] == pytest.approx(0.95)
    assert b["vmin"] == pytest.approx(0.0)
    assert b["vmax"] == pytest.approx(1.0)
    assert b["vini"] == pytest.approx(0.50)
    assert b["capacity"] == pytest.approx(50.0 * 4.0)  # pmax_discharge * hrs_reg


def test_battery_array_from_ess(tmp_path):
    """BessWriter produces correct battery_array from an ESS entry (no active)."""
    ep = _make_ess_parser(
        tmp_path,
        " 1\n    1  'ESS1'      1   100.0  100.0  0.90  0.90  2.0   0.40\n",
    )
    writer = BessWriter(ess_parser=ep)
    bats = writer.to_battery_array()

    assert len(bats) == 1
    b = bats[0]
    assert b["uid"] == 1
    assert b["name"] == "ESS1"
    assert b["capacity"] == pytest.approx(100.0 * 2.0)
    # ESS has no active restriction
    assert "active" not in b


# ---------------------------------------------------------------------------
# generator_array (discharge path)
# ---------------------------------------------------------------------------


def test_generator_array(tmp_path):
    """BessWriter produces correct discharge generator entry."""
    bp = _make_bess_parser(
        tmp_path,
        " 1\n    1  'BESS1'     3   50.0   60.0  0.95  0.95  4.0    1    1    0.50    1.0\n",
    )
    writer = BessWriter(bess_parser=bp)
    gens = writer.to_generator_array()

    assert len(gens) == 1
    g = gens[0]
    assert g["uid"] == BESS_UID_OFFSET + 1
    assert g["name"] == "BESS1_disch"
    assert g["bus"] == 3
    assert g["pmin"] == pytest.approx(0.0)
    assert g["pmax"] == pytest.approx(60.0)
    assert g["gcost"] == pytest.approx(0.0)
    assert g["capacity"] == pytest.approx(60.0)


# ---------------------------------------------------------------------------
# demand_array (charge path)
# ---------------------------------------------------------------------------


def test_demand_array(tmp_path):
    """BessWriter produces correct charge demand entry."""
    bp = _make_bess_parser(
        tmp_path,
        " 1\n    1  'BESS1'     2   50.0   60.0  0.95  0.95  4.0    1    1    0.50    1.0\n",
    )
    writer = BessWriter(bess_parser=bp)
    dems = writer.to_demand_array()

    assert len(dems) == 1
    d = dems[0]
    assert d["uid"] == BESS_UID_OFFSET + 1
    assert d["name"] == "BESS1_chrg"
    assert d["bus"] == 2
    assert d["lmax"] == "lmax"


# ---------------------------------------------------------------------------
# converter_array
# ---------------------------------------------------------------------------


def test_converter_array(tmp_path):
    """BessWriter produces correct converter entry."""
    bp = _make_bess_parser(
        tmp_path,
        " 1\n    1  'BESS1'     1   50.0   50.0  0.95  0.95  4.0    1    1    0.50    1.0\n",
    )
    writer = BessWriter(bess_parser=bp)
    convs = writer.to_converter_array()

    assert len(convs) == 1
    c = convs[0]
    assert c["uid"] == 1
    assert c["name"] == "BESS1"
    assert c["battery"] == 1
    assert c["generator"] == BESS_UID_OFFSET + 1
    assert c["demand"] == BESS_UID_OFFSET + 1
    assert c["capacity"] == pytest.approx(50.0)


# ---------------------------------------------------------------------------
# lmax parquet
# ---------------------------------------------------------------------------


def test_lmax_parquet_written(tmp_path):
    """BessWriter writes lmax.parquet column for charge demand."""
    bp = _make_bess_parser(
        tmp_path,
        " 1\n    1  'BESS1'     1   50.0   50.0  0.95  0.95  4.0    1    1    0.50    1.0\n",
    )

    # Pre-populate a demand lmax parquet (as DemandWriter would)
    demand_dir = tmp_path / "Demand"
    demand_dir.mkdir()
    existing = pd.DataFrame({"block": [1], "uid:1": [80.0]})
    existing.to_parquet(demand_dir / "lmax.parquet", index=False)

    writer = BessWriter(bess_parser=bp, options={"output_dir": tmp_path})
    writer.process([], [], tmp_path)

    df = pd.read_parquet(demand_dir / "lmax.parquet")
    bess_col = f"uid:{BESS_UID_OFFSET + 1}"
    assert bess_col in df.columns
    assert float(df[bess_col].iloc[0]) == pytest.approx(50.0)
    # Original demand column preserved
    assert "uid:1" in df.columns


# ---------------------------------------------------------------------------
# process() – combined result
# ---------------------------------------------------------------------------


def test_process_no_bess(tmp_path):
    """process() with no BESS/ESS returns unchanged arrays."""
    writer = BessWriter()
    existing_gen = [{"uid": 1, "name": "Gen1"}]
    existing_dem = [{"uid": 1, "name": "Dem1"}]
    result = writer.process(existing_gen, existing_dem, tmp_path)
    assert result["battery_array"] == []
    assert result["converter_array"] == []
    assert result["generator_array"] == existing_gen
    assert result["demand_array"] == existing_dem


def test_process_with_bess(tmp_path):
    """process() with one BESS produces correct combined arrays."""
    bp = _make_bess_parser(
        tmp_path,
        " 1\n    1  'BESS1'     1   50.0   50.0  0.95  0.95  4.0    1    1    0.50    1.0\n",
    )
    writer = BessWriter(bess_parser=bp, options={"output_dir": tmp_path})
    result = writer.process(
        [{"uid": 1, "name": "Thermal1"}],
        [{"uid": 1, "name": "DemBus1"}],
        tmp_path,
    )

    assert len(result["battery_array"]) == 1
    assert len(result["converter_array"]) == 1
    assert len(result["generator_array"]) == 2  # 1 thermal + 1 BESS discharge
    assert len(result["demand_array"]) == 2  # 1 thermal + 1 BESS charge


def test_process_bess_and_ess(tmp_path):
    """process() combines BESS and ESS entries."""
    bp = _make_bess_parser(
        tmp_path,
        " 1\n    1  'BESS1'     1   50.0   50.0  0.95  0.95  4.0    1    1    0.50    1.0\n",
    )
    ep = _make_ess_parser(
        tmp_path,
        " 1\n    2  'ESS2'      1   30.0   30.0  0.90  0.90  2.0   0.60\n",
    )
    writer = BessWriter(
        bess_parser=bp, ess_parser=ep, options={"output_dir": tmp_path}
    )
    result = writer.process([], [], tmp_path)

    assert len(result["battery_array"]) == 2
    assert len(result["converter_array"]) == 2
    assert len(result["generator_array"]) == 2
    assert len(result["demand_array"]) == 2
