"""Unit tests for :mod:`plexos2gtopt.parsers`.

Each P1 extractor is exercised against a hand-rolled synthetic
``DBSEN_PRGDIARIO.xml`` + minimal CSVs under ``tmp_path``. The fixtures
are deliberately tiny (4 buses / 2 gens / 1 line / 1 battery / 2 loads)
so failures point at one extractor at a time.
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import BundleSpec
from plexos2gtopt.parsers import (
    extract_batteries,
    extract_demands,
    extract_fuels,
    extract_generators,
    extract_lines,
    extract_nodes,
)
from plexos2gtopt.plexos_loader import PlexosBundle
from plexos2gtopt.plexos_xml import NS, load_xml


_MINI_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>4</class_id><name>Fuel</name></t_class>
  <t_class><class_id>7</class_id><name>Battery</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>24</class_id><name>Line</name></t_class>
  <t_object>
    <object_id>1</object_id><class_id>22</class_id><name>bus_north</name>
  </t_object>
  <t_object>
    <object_id>2</object_id><class_id>22</class_id><name>bus_south</name>
  </t_object>
  <t_object>
    <object_id>10</object_id><class_id>2</class_id><name>thermal_a</name>
  </t_object>
  <t_object>
    <object_id>11</object_id><class_id>2</class_id><name>solar_b</name>
  </t_object>
  <t_object>
    <object_id>20</object_id><class_id>24</class_id><name>line_ns</name>
  </t_object>
  <t_object>
    <object_id>30</object_id><class_id>4</class_id><name>diesel</name>
  </t_object>
  <t_object>
    <object_id>40</object_id><class_id>7</class_id><name>bess_a</name>
  </t_object>
  <t_collection>
    <collection_id>12</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>22</child_class_id>
    <name>Nodes</name>
  </t_collection>
  <t_collection>
    <collection_id>7</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>4</child_class_id>
    <name>Fuels</name>
  </t_collection>
  <t_collection>
    <collection_id>306</collection_id>
    <parent_class_id>24</parent_class_id>
    <child_class_id>22</child_class_id>
    <name>Node From</name>
  </t_collection>
  <t_collection>
    <collection_id>307</collection_id>
    <parent_class_id>24</parent_class_id>
    <child_class_id>22</child_class_id>
    <name>Node To</name>
  </t_collection>
  <t_collection>
    <collection_id>83</collection_id>
    <parent_class_id>7</parent_class_id>
    <child_class_id>22</child_class_id>
    <name>Nodes</name>
  </t_collection>
  <t_membership>
    <membership_id>900</membership_id>
    <collection_id>12</collection_id>
    <parent_object_id>10</parent_object_id>
    <child_object_id>1</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>901</membership_id>
    <collection_id>12</collection_id>
    <parent_object_id>11</parent_object_id>
    <child_object_id>2</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>910</membership_id>
    <collection_id>7</collection_id>
    <parent_object_id>10</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>920</membership_id>
    <collection_id>306</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>1</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>921</membership_id>
    <collection_id>307</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>2</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>930</membership_id>
    <collection_id>83</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>1</child_object_id>
  </t_membership>
</MasterDataSet>
"""


def _build_bundle(tmp_path: Path) -> tuple[PlexosBundle, Path]:
    """Materialise the synthetic bundle on disk and return a bundle handle."""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_MINI_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    return bundle, xml_path


def _write_csv(tmp_path: Path, name: str, body: str) -> None:
    (tmp_path / name).write_text(body)


def test_extract_nodes(tmp_path: Path) -> None:
    """One NodeSpec per Node object."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    nodes = extract_nodes(db)
    assert {n.name for n in nodes} == {"bus_north", "bus_south"}
    _ = bundle  # unused — extractor needs only the db


def test_extract_fuels(tmp_path: Path) -> None:
    """One FuelSpec per Fuel object; price=0 when Fuel_Price.csv absent."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert [f.name for f in fuels] == ["diesel"]
    assert fuels[0].price == 0.0


def test_extract_fuels_with_price(tmp_path: Path) -> None:
    """P3: Fuel_Price.csv populates FuelSpec.price (period-1 scalar)."""
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Fuel_Price.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\ndiesel,2026,1,1,1,1234.5\n",
    )
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert fuels[0].price == 1234.5


def test_extract_fuels_with_co2_membership(tmp_path: Path) -> None:
    """Emission→Fuel membership populates FuelSpec.co2_rate.

    Builds a synthetic bundle with one Emission object ``CO2``
    targeting one Fuel ``diesel`` via the canonical PLEXOS
    ``Emission → Fuels`` collection (parent_class=10, child_class=4).
    The membership carries one t_data row for ``Production Rate``
    with value 0.0742 tCO₂/GJ (IPCC-default diesel combustion).
    """
    xml = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>4</class_id><name>Fuel</name></t_class>
  <t_class><class_id>10</class_id><name>Emission</name></t_class>
  <t_object>
    <object_id>30</object_id><class_id>4</class_id><name>diesel</name>
  </t_object>
  <t_object>
    <object_id>200</object_id><class_id>10</class_id><name>CO2</name>
  </t_object>
  <t_collection>
    <collection_id>112</collection_id>
    <parent_class_id>10</parent_class_id>
    <child_class_id>4</child_class_id>
    <name>Fuels</name>
  </t_collection>
  <t_membership>
    <membership_id>9000</membership_id>
    <collection_id>112</collection_id>
    <parent_object_id>200</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_property>
    <property_id>1219</property_id>
    <collection_id>112</collection_id>
    <name>Production Rate</name>
  </t_property>
  <t_data>
    <data_id>1</data_id>
    <membership_id>9000</membership_id>
    <property_id>1219</property_id>
    <value>0.0742</value>
  </t_data>
</MasterDataSet>
"""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(xml)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert [f.name for f in fuels] == ["diesel"]
    assert fuels[0].co2_rate == 0.0742
    assert fuels[0].co2_upstream_rate == 0.0


def test_extract_fuels_co2_upstream_membership(tmp_path: Path) -> None:
    """An Emission named ``CO2 (Upstream)`` routes to co2_upstream_rate.

    The combustion component stays zero — proving the
    upstream-detection heuristic doesn't pollute the combustion
    slot, and that two separate Emission objects can split CO₂
    across components.
    """
    xml = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>4</class_id><name>Fuel</name></t_class>
  <t_class><class_id>10</class_id><name>Emission</name></t_class>
  <t_object>
    <object_id>31</object_id><class_id>4</class_id><name>gas</name>
  </t_object>
  <t_object>
    <object_id>201</object_id>
    <class_id>10</class_id><name>CO2 (Upstream)</name>
  </t_object>
  <t_collection>
    <collection_id>112</collection_id>
    <parent_class_id>10</parent_class_id>
    <child_class_id>4</child_class_id>
    <name>Fuels</name>
  </t_collection>
  <t_membership>
    <membership_id>9001</membership_id>
    <collection_id>112</collection_id>
    <parent_object_id>201</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
  <t_property>
    <property_id>1219</property_id>
    <collection_id>112</collection_id>
    <name>Production Rate</name>
  </t_property>
  <t_data>
    <data_id>1</data_id>
    <membership_id>9001</membership_id>
    <property_id>1219</property_id>
    <value>0.008</value>
  </t_data>
</MasterDataSet>
"""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(xml)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert fuels[0].co2_rate == 0.0
    assert fuels[0].co2_upstream_rate == 0.008


def test_extract_fuels_skips_non_co2_emission(tmp_path: Path) -> None:
    """An Emission named ``NOx`` is ignored (only CO₂ is wired today)."""
    xml = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>4</class_id><name>Fuel</name></t_class>
  <t_class><class_id>10</class_id><name>Emission</name></t_class>
  <t_object>
    <object_id>30</object_id><class_id>4</class_id><name>diesel</name>
  </t_object>
  <t_object>
    <object_id>202</object_id><class_id>10</class_id><name>NOx</name>
  </t_object>
  <t_collection>
    <collection_id>112</collection_id>
    <parent_class_id>10</parent_class_id>
    <child_class_id>4</child_class_id>
    <name>Fuels</name>
  </t_collection>
  <t_membership>
    <membership_id>9002</membership_id>
    <collection_id>112</collection_id>
    <parent_object_id>202</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_property>
    <property_id>1219</property_id>
    <collection_id>112</collection_id>
    <name>Production Rate</name>
  </t_property>
  <t_data>
    <data_id>1</data_id>
    <membership_id>9002</membership_id>
    <property_id>1219</property_id>
    <value>0.005</value>
  </t_data>
</MasterDataSet>
"""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(xml)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert fuels[0].co2_rate == 0.0
    assert fuels[0].co2_upstream_rate == 0.0


def test_extract_generators_with_costs(tmp_path: Path) -> None:
    """P3: Gen_HeatRate / Gen_VOMCharge populate thermal cost fields."""
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nthermal_a,2026,1,1,1,1,100\n",
    )
    _write_csv(
        tmp_path,
        "Gen_HeatRate.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\nthermal_a,2026,1,1,1,0.25\n",
    )
    _write_csv(
        tmp_path,
        "Gen_VOMCharge.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\nthermal_a,2026,1,1,1,8.5\n",
    )
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    thermal = next(g for g in gens if g.name == "thermal_a")
    assert thermal.heat_rate == 0.25
    assert thermal.vom_charge == 8.5
    # The renewable (solar_b) is not in the cost CSVs → zero cost.
    solar = next(g for g in gens if g.name == "solar_b")
    assert solar.heat_rate == 0.0
    assert solar.vom_charge == 0.0


def test_extract_generators_fuel_transport(tmp_path: Path) -> None:
    """Gen_FuelTransportCharge.csv populates GeneratorSpec.fuel_transport.

    PLEXOS keys each row by the ``<gen_name><fuel_name>`` concatenation
    (one row per gen × fuel attachment).  The extractor picks the
    primary-fuel match (same convention as the writer uses for gcost).
    Only PERIOD=1 is populated in daily PCP bundles.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nthermal_a,2026,1,1,1,1,100\n",
    )
    # ``thermal_a`` is wired to ``diesel`` in _MINI_XML.  The transport
    # CSV key is therefore ``thermal_a`` + ``diesel`` = ``thermal_adiesel``.
    # An unrelated stale row must not match (no fuel suffix on a real gen).
    _write_csv(
        tmp_path,
        "Gen_FuelTransportCharge.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\n"
        "thermal_adiesel,2026,1,1,1,12.5\n"
        "phantom_unitsome_fuel,2026,1,1,1,99.0\n",
    )
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    by_name = {g.name: g for g in gens}
    assert by_name["thermal_a"].fuel_transport == 12.5
    # Renewable solar_b has no fuel membership → no key matches → 0.
    assert by_name["solar_b"].fuel_transport == 0.0


def test_extract_generators_fuel_transport_absent(tmp_path: Path) -> None:
    """Missing Gen_FuelTransportCharge.csv leaves fuel_transport=0."""
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nthermal_a,2026,1,1,1,1,100\n",
    )
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    thermal = next(g for g in gens if g.name == "thermal_a")
    assert thermal.fuel_transport == 0.0


def test_extract_generators(tmp_path: Path) -> None:
    """Generators attach to the right bus; thermal carries a fuel."""
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,100\n"
        "thermal_a,2026,1,1,2,1,100\n"
        "solar_b,2026,1,1,1,1,10\n"
        "solar_b,2026,1,1,12,1,80\n",
    )
    _write_csv(
        tmp_path,
        "Gen_MinStableLevel.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nthermal_a,2026,1,1,1,1,20\n",
    )
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    by_name = {g.name: g for g in gens}
    assert set(by_name) == {"thermal_a", "solar_b"}
    assert by_name["thermal_a"].bus_name == "bus_north"
    assert by_name["thermal_a"].pmax == 100.0
    # GeneratorSpec.pmin is the always-on hard floor and stays 0
    # post-2026-05-20.  PLEXOS Min Stable Level (20 MW for thermal_a)
    # is per-docs commitment-conditional and now travels via
    # CommitmentSpec.pmin instead (verified by test_extract_commitments).
    assert by_name["thermal_a"].pmin == 0.0
    assert by_name["thermal_a"].fuel_names == ("diesel",)
    assert by_name["solar_b"].bus_name == "bus_south"
    # Renewable: no fuel membership.
    assert by_name["solar_b"].fuel_names == ()
    # Profile peaks at hour 12 (idx 11).
    assert by_name["solar_b"].pmax == 80.0
    assert by_name["solar_b"].pmax_profile[11] == 80.0


def test_extract_lines(tmp_path: Path) -> None:
    """Line endpoints picked up from Node From / Node To."""
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Lin_MaxRating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nline_ns,2026,1,1,1,1,200\n",
    )
    _write_csv(
        tmp_path,
        "Lin_MinRating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nline_ns,2026,1,1,1,1,-150\n",
    )
    _write_csv(
        tmp_path,
        "Lin_Units.csv",
        "YEAR,MONTH,DAY,PERIOD,BAND,line_ns\n2026,1,1,1,1,1\n",
    )
    db = load_xml(xml_path)
    lines = extract_lines(db, bundle)
    assert len(lines) == 1
    line = lines[0]
    assert line.bus_from == "bus_north"
    assert line.bus_to == "bus_south"
    assert line.tmax_ab == 200.0
    assert line.tmin_ab == -150.0
    assert line.units == 1


def test_extract_lines_skips_offline(tmp_path: Path) -> None:
    """A line with Lin_Units=0 all day is dropped."""
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Lin_MaxRating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nline_ns,2026,1,1,1,1,200\n",
    )
    _write_csv(
        tmp_path,
        "Lin_Units.csv",
        "YEAR,MONTH,DAY,PERIOD,BAND,line_ns\n2026,1,1,1,1,0\n",
    )
    db = load_xml(xml_path)
    lines = extract_lines(db, bundle)
    assert not lines


def test_extract_demands(tmp_path: Path) -> None:
    """One DemandSpec per non-zero bus column in Nod_Load.csv."""
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Nod_Load.csv",
        "YEAR,MONTH,DAY,PERIOD,bus_north,bus_south,bus_dark\n"
        "2026,1,1,1,50,30,0\n"
        "2026,1,1,2,55,32,0\n",
    )
    db = load_xml(xml_path)
    demands = extract_demands(db, bundle)
    names = {d.bus_name for d in demands}
    assert names == {"bus_north", "bus_south"}
    by_bus = {d.bus_name: d for d in demands}
    assert by_bus["bus_north"].lmax_profile[0] == 50.0
    assert by_bus["bus_north"].lmax_profile[1] == 55.0


def test_extract_batteries(tmp_path: Path) -> None:
    """One BatterySpec per Battery; initial SOC from BESS_IniValue.

    PLEXOS ``Initial SoC`` is a percentage (0-100), not absolute MWh.
    The extractor multiplies by ``Capacity / 100`` to obtain MWh.
    Without a Capacity property in the mini bundle, the absolute
    energy collapses to 0 — same as PLEXOS itself.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "BESS_IniValue.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\nbess_a,2026,1,1,1,12.5\n",
    )
    db = load_xml(xml_path)
    batts = extract_batteries(db, bundle)
    assert len(batts) == 1
    assert batts[0].bus_name == "bus_north"
    # No Capacity property in the mini XML → SoC×Capacity/100 = 0.
    assert batts[0].eini == 0.0
    assert batts[0].emax == 0.0


def test_extract_bundle_spec_defaults() -> None:
    """`extract_bundle_spec` returns safe defaults when Param.xml absent."""
    bundle = PlexosBundle(root=Path("/nonexistent"), source=Path("/nonexistent"))
    # Bundle dataclass keeps its defaults; the function shouldn't crash even
    # though there is no PLEXOS_Param.xml on disk.
    from plexos2gtopt.parsers import extract_bundle_spec

    spec = extract_bundle_spec(bundle)
    assert isinstance(spec, BundleSpec)
    assert spec.step_count == 24
    assert spec.step_type == "Hour"
