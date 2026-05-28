"""Unit tests for :mod:`plexos2gtopt.parsers`.

Each P1 extractor is exercised against a hand-rolled synthetic
``DBSEN_PRGDIARIO.xml`` + minimal CSVs under ``tmp_path``. The fixtures
are deliberately tiny (4 buses / 2 gens / 1 line / 1 battery / 2 loads)
so failures point at one extractor at a time.
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import BundleSpec
from plexos2gtopt.entities import GeneratorSpec, PlexosCase
from plexos2gtopt.parsers import (
    _build_plant_cap_ucs,
    _extract_config_mutex_groups,
    extract_batteries,
    extract_demands,
    extract_fuels,
    extract_generators,
    extract_lines,
    extract_nodes,
    extract_reservoirs,
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


def test_extract_fuels_max_offtake_week_binding_week(tmp_path: Path) -> None:
    """``Fuel_MaxOfftakeWeek.csv`` populates ``FuelSpec.max_offtake``.

    Two week-start dates (Apr 16 and Apr 23) ship per fuel; the
    binding week is the FIRST one seen by ``read_long`` (Apr 16 in
    calendar order — the week containing the Apr 22 bundle date).
    The Apr 23 row is ignored because it's outside the bundle's
    1-day window.

    Mirrors PLEXOS's ``FueMaxOffWeek_<fuel>`` Constraint pattern.

    The CSV value is in **TJ/week** and is scaled to **GJ/week**
    by the ``_TJ_TO_GJ`` factor (= 1000) so the cap units match
    the per-block LHS basis (heat_rate × MWh in GJ).  Verified
    against PLEXOS solution `.accdb`:
    ``FueMaxOffWeek_Gas_NuevaRenca_GN_A`` ships 9.8 TJ in the CSV
    and 58.33 GJ × 168 hours = 9800 GJ in the constraint's t_data.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Fuel_MaxOfftakeWeek.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\n"
        "diesel,2026,4,16,1,5.0\n"  # ← binding week (5 TJ)
        "diesel,2026,4,23,1,1.0\n",  # ← ignored (next week)
    )
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    # 5.0 TJ × 1000 = 5000 GJ (unit matching the gtopt FuelLP basis).
    assert fuels[0].max_offtake == 5000.0


def test_extract_fuels_max_offtake_week_absent(tmp_path: Path) -> None:
    """No CSV → ``max_offtake = None`` (no cap binds)."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert fuels[0].max_offtake is None


def test_extract_fuels_max_offtake_week_explicit_zero(tmp_path: Path) -> None:
    """Explicit 0 in the CSV → ``max_offtake = 0.0`` (PLEXOS "shut").

    Distinct from "absent" (= no cap).  An explicit-zero cap propagates
    so the gtopt LP forces every generator on this fuel band to
    dispatch 0 within the stage.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Fuel_MaxOfftakeWeek.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\ndiesel,2026,4,16,1,0\n",
    )
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert fuels[0].max_offtake == 0.0


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


def test_extract_generators_units_out_overrides_rating(tmp_path: Path) -> None:
    """``Gen_UnitsOut[t] = 1`` forces ``pmax_profile[t] = 0`` even when
    ``Gen_Rating[t] > 0``.

    Mirrors the PLEXOS CEN PCP pattern where ~31 thermal diesels
    (TOCOPILLA-TG1, COLMITO_DIE, …) ship a full Gen_Rating profile
    but Gen_UnitsOut marks them offline for the entire horizon.
    Without this override the gtopt MIP would commit these gens
    where PLEXOS does not.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    # thermal_a: rating non-zero everywhere; UnitsOut = 1 at hour 2
    # (forced outage), UnitsOut = 0 at hour 1.
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,100\n"
        "thermal_a,2026,1,1,2,1,100\n",
    )
    _write_csv(
        tmp_path,
        "Gen_UnitsOut.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,0\n"
        "thermal_a,2026,1,1,2,1,1\n",
    )
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    by_name = {g.name: g for g in gens}
    # Hour 1 (UnitsOut=0): full nameplate.
    assert by_name["thermal_a"].pmax_profile[0] == 100.0
    # Hour 2 (UnitsOut=1, single-unit): forced offline.
    assert by_name["thermal_a"].pmax_profile[1] == 0.0


def test_extract_generators_units_out_no_csv(tmp_path: Path) -> None:
    """Absence of ``Gen_UnitsOut.csv`` leaves ``pmax_profile`` untouched."""
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,50\n"
        "thermal_a,2026,1,1,2,1,75\n",
    )
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    by_name = {g.name: g for g in gens}
    assert by_name["thermal_a"].pmax_profile[0] == 50.0
    assert by_name["thermal_a"].pmax_profile[1] == 75.0


def test_extract_generators_units_out_multi_unit_partial(tmp_path: Path) -> None:
    """Two-unit gen with ``UnitsOut[t] = 1`` derates pmax to 50%.

    Validates the general formula
    ``factor = max(0, 1 - units_out / max_units)`` and ensures it
    keeps working when PLEXOS bundles ship multi-unit data (not seen
    in CEN PCP weekly, but documented for fleet plants like RUCUE).
    """
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,200\n"
        "thermal_a,2026,1,1,2,1,200\n"
        "thermal_a,2026,1,1,3,1,200\n",
    )
    _write_csv(
        tmp_path,
        "Gen_UnitsOut.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,0\n"
        "thermal_a,2026,1,1,2,1,1\n"
        "thermal_a,2026,1,1,3,1,2\n",
    )
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    by_name = {g.name: g for g in gens}
    # max_units = 2 (observed peak of UnitsOut series).
    # Hour 1: 0 out -> full 200; Hour 2: 1/2 derate -> 100; Hour 3: full out -> 0.
    assert by_name["thermal_a"].pmax_profile[0] == 200.0
    assert by_name["thermal_a"].pmax_profile[1] == 100.0
    assert by_name["thermal_a"].pmax_profile[2] == 0.0


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
    # Mini bundle has no Region collection → fcost defaults to 0
    # (per-Region VoLL routing only fires when memberships exist).
    assert by_bus["bus_north"].fcost == 0.0


# ---------------------------------------------------------------------------
# Per-Region VoLL → per-Demand fcost routing (literature audit #3)
# ---------------------------------------------------------------------------


_REGION_VOLL_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>26</class_id><name>Region</name></t_class>
  <t_class><class_id>33</class_id><name>System</name></t_class>
  <t_object>
    <object_id>1</object_id><class_id>22</class_id><name>bus_n</name>
  </t_object>
  <t_object>
    <object_id>2</object_id><class_id>22</class_id><name>bus_s</name>
  </t_object>
  <t_object>
    <object_id>3</object_id><class_id>22</class_id><name>bus_orphan</name>
  </t_object>
  <t_object>
    <object_id>10</object_id><class_id>26</class_id><name>RegionNorth</name>
  </t_object>
  <t_object>
    <object_id>11</object_id><class_id>26</class_id><name>RegionSouth</name>
  </t_object>
  <t_object>
    <object_id>99</object_id><class_id>33</class_id><name>System</name>
  </t_object>
  <t_collection>
    <collection_id>200</collection_id>
    <parent_class_id>22</parent_class_id>
    <child_class_id>26</child_class_id>
    <name>Region</name>
  </t_collection>
  <t_collection>
    <collection_id>300</collection_id>
    <parent_class_id>33</parent_class_id>
    <child_class_id>26</child_class_id>
    <name>Regions</name>
  </t_collection>
  <t_property>
    <property_id>400</property_id>
    <collection_id>300</collection_id>
    <name>VoLL</name>
  </t_property>
  <!-- bus_n → RegionNorth (VoLL=1000), bus_s → RegionSouth (VoLL=500) -->
  <t_membership>
    <membership_id>500</membership_id>
    <collection_id>200</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>501</membership_id>
    <collection_id>200</collection_id>
    <parent_object_id>2</parent_object_id>
    <child_object_id>11</child_object_id>
  </t_membership>
  <!-- System → RegionNorth memberships carry the VoLL property -->
  <t_membership>
    <membership_id>600</membership_id>
    <collection_id>300</collection_id>
    <parent_object_id>99</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>601</membership_id>
    <collection_id>300</collection_id>
    <parent_object_id>99</parent_object_id>
    <child_object_id>11</child_object_id>
  </t_membership>
  <t_data>
    <data_id>700</data_id><membership_id>600</membership_id>
    <property_id>400</property_id><value>1000.0</value>
  </t_data>
  <t_data>
    <data_id>701</data_id><membership_id>601</membership_id>
    <property_id>400</property_id><value>500.0</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_demands_routes_per_region_voll(tmp_path: Path) -> None:
    """Each Demand picks up its serving Region's VoLL as `fcost`.

    Literature audit #3 (2026-05-20): the old converter collapsed
    multi-Region VoLLs to ``max(VoLLs)`` and stamped it onto the
    global ``demand_fail_cost``, overpricing curtailment in cheap
    regions.  The fix routes each Region's VoLL onto the
    corresponding Demand's ``fcost`` field and leaves the global
    default as the conservative ``min(VoLLs)``.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_REGION_VOLL_XML)
    _write_csv(
        tmp_path,
        "Nod_Load.csv",
        "YEAR,MONTH,DAY,PERIOD,bus_n,bus_s,bus_orphan\n2026,1,1,1,100,200,50\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    demands = extract_demands(db, bundle)
    by_bus = {d.bus_name: d for d in demands}
    assert by_bus["bus_n"].fcost == 1000.0
    assert by_bus["bus_s"].fcost == 500.0
    # Orphan bus has no Region → fcost falls through to 0 (gtopt
    # global ``model_options.demand_fail_cost`` then applies).
    assert by_bus["bus_orphan"].fcost == 0.0


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


# ---------------------------------------------------------------------------
# T1: Lin_MaxRating sentinel filter (>8× Max Flow ⇒ sentinel, drop uplift)
# ---------------------------------------------------------------------------


_LINES_SENTINEL_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>24</class_id><name>Line</name></t_class>
  <t_object>
    <object_id>1</object_id><class_id>1</class_id><name>SEN</name>
  </t_object>
  <t_object>
    <object_id>10</object_id><class_id>22</class_id><name>bus_n</name>
  </t_object>
  <t_object>
    <object_id>11</object_id><class_id>22</class_id><name>bus_s</name>
  </t_object>
  <t_object>
    <object_id>20</object_id><class_id>24</class_id><name>A</name>
  </t_object>
  <t_object>
    <object_id>21</object_id><class_id>24</class_id><name>B</name>
  </t_object>
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
  <!-- System → Lines collection for static properties -->
  <t_collection>
    <collection_id>400</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>24</child_class_id>
    <name>Lines</name>
  </t_collection>
  <t_property>
    <property_id>1882</property_id><collection_id>400</collection_id>
    <name>Max Rating</name>
  </t_property>
  <t_property>
    <property_id>1881</property_id><collection_id>400</collection_id>
    <name>Enforce Limits</name>
  </t_property>
  <!-- Endpoints -->
  <t_membership>
    <membership_id>900</membership_id>
    <collection_id>306</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>901</membership_id>
    <collection_id>307</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>11</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>902</membership_id>
    <collection_id>306</collection_id>
    <parent_object_id>21</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>903</membership_id>
    <collection_id>307</collection_id>
    <parent_object_id>21</parent_object_id>
    <child_object_id>11</child_object_id>
  </t_membership>
  <!-- System↔Line memberships carry static Max Rating + Enforce Limits -->
  <t_membership>
    <membership_id>950</membership_id>
    <collection_id>400</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>20</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>951</membership_id>
    <collection_id>400</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>21</child_object_id>
  </t_membership>
  <!-- Line A: Max Rating 1700 = 8.5× Max Flow 200 → sentinel, drop. -->
  <t_data>
    <data_id>10001</data_id><membership_id>950</membership_id>
    <property_id>1882</property_id><value>1700</value>
  </t_data>
  <t_data>
    <data_id>10002</data_id><membership_id>950</membership_id>
    <property_id>1881</property_id><value>2</value>
  </t_data>
  <!-- Line B: Max Rating 1400 = 7× Max Flow 200 → realistic, keep. -->
  <t_data>
    <data_id>10003</data_id><membership_id>951</membership_id>
    <property_id>1882</property_id><value>1400</value>
  </t_data>
  <t_data>
    <data_id>10004</data_id><membership_id>951</membership_id>
    <property_id>1881</property_id><value>2</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_lines_max_rating_sentinel_filter(tmp_path: Path) -> None:
    """``Max Rating > 8 × Max Flow`` is treated as a PLEXOS-CEN sentinel
    and zeroed (paired ``Min Rating`` zeroed alongside).

    Audit context: PLEXOS-CEN ships sentinel ratings (e.g.
    Antofag110→Desalant110 at 17.5×) that would otherwise feed into
    gtopt's soft/hard limit pair and produce unphysical thermal
    uplifts.  Realistic emergency-rating uplifts stay below ~7-8×
    even on oil-cooled HV transformers, so anything above that band
    is data noise.  Line A here is 8.5× — must be dropped; Line B
    is 7× — must be kept verbatim.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_LINES_SENTINEL_XML)
    _write_csv(
        tmp_path,
        "Lin_MaxRating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "A,2026,1,1,1,1,200\n"
        "B,2026,1,1,1,1,200\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    lines = extract_lines(db, bundle)
    by_name = {ln.name: ln for ln in lines}
    assert set(by_name) == {"A", "B"}
    # A: 8.5× uplift → sentinel, dropped.
    assert by_name["A"].max_rating == 0.0
    # Paired min_rating also zeroed (the sentinel block clears both).
    assert by_name["A"].min_rating == 0.0
    # B: 7× uplift → realistic, kept verbatim.
    assert by_name["B"].max_rating == 1400.0


# ---------------------------------------------------------------------------
# T2: Battery extractor drops infeasible Min Charge / Min Discharge Level
# ---------------------------------------------------------------------------


_BATT_PMIN_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>7</class_id><name>Battery</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_object>
    <object_id>1</object_id><class_id>1</class_id><name>SEN</name>
  </t_object>
  <t_object>
    <object_id>10</object_id><class_id>22</class_id><name>bus_a</name>
  </t_object>
  <t_object>
    <object_id>40</object_id><class_id>7</class_id><name>bess_a</name>
  </t_object>
  <t_object>
    <object_id>41</object_id><class_id>7</class_id><name>valid</name>
  </t_object>
  <!-- Battery→Node attachment so the bus_map resolves. -->
  <t_collection>
    <collection_id>83</collection_id>
    <parent_class_id>7</parent_class_id>
    <child_class_id>22</child_class_id>
    <name>Nodes</name>
  </t_collection>
  <!-- System→Batteries carries the static Max Power / Min Charge / Min Discharge properties. -->
  <t_collection>
    <collection_id>80</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>7</child_class_id>
    <name>Batteries</name>
  </t_collection>
  <t_property>
    <property_id>500</property_id><collection_id>80</collection_id>
    <name>Max Power</name>
  </t_property>
  <t_property>
    <property_id>501</property_id><collection_id>80</collection_id>
    <name>Min Charge Level</name>
  </t_property>
  <t_property>
    <property_id>502</property_id><collection_id>80</collection_id>
    <name>Min Discharge Level</name>
  </t_property>
  <!-- Battery attachments to bus_a. -->
  <t_membership>
    <membership_id>900</membership_id>
    <collection_id>83</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>901</membership_id>
    <collection_id>83</collection_id>
    <parent_object_id>41</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <!-- System↔Battery memberships for the static-property lookups. -->
  <t_membership>
    <membership_id>950</membership_id>
    <collection_id>80</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>40</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>951</membership_id>
    <collection_id>80</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>41</child_object_id>
  </t_membership>
  <!-- bess_a: pmin_charge=2.5 > pmax=2.0, pmin_discharge=2.8 > 2.0 → both dropped. -->
  <t_data>
    <data_id>1</data_id><membership_id>950</membership_id>
    <property_id>500</property_id><value>2.0</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>950</membership_id>
    <property_id>501</property_id><value>2.5</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>950</membership_id>
    <property_id>502</property_id><value>2.8</value>
  </t_data>
  <!-- valid: pmin_charge=1.0 < pmax=5.0 → kept. -->
  <t_data>
    <data_id>4</data_id><membership_id>951</membership_id>
    <property_id>500</property_id><value>5.0</value>
  </t_data>
  <t_data>
    <data_id>5</data_id><membership_id>951</membership_id>
    <property_id>501</property_id><value>1.0</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_batteries_drops_infeasible_pmin_charge(tmp_path: Path) -> None:
    """PLEXOS-CEN sometimes ships ``Min Charge Level > Max Power`` on
    placeholder batteries.  The extractor drops the bad pmin → 0 so
    the downstream synthetic ``<bat>_dem`` doesn't become infeasible.

    Audit: BAT_DEL_DESIERTO (max=2, min=2.32) and BAT_TOCOPILLA
    (max=2, min=2.5) hit this on the CEN PCP daily bundle; leaving
    the lmin>lmax pair in place produced ~7.6 GWh of phantom unserved
    demand across the week.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_PMIN_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    batts = {b.name: b for b in extract_batteries(db, bundle)}
    assert set(batts) == {"bess_a", "valid"}
    # Infeasible pmins on bess_a → both cleared to 0.
    assert batts["bess_a"].pmin_charge == 0.0
    assert batts["bess_a"].pmin_discharge == 0.0
    # Feasible pmin on `valid` is preserved.
    assert batts["valid"].pmin_charge == 1.0


def test_extract_batteries_keeps_valid_pmin_end_to_end(tmp_path: Path) -> None:
    """Companion to the drop-path test: when ``Min Charge Level``
    fits inside ``Max Power``, the extractor preserves the value AND
    the downstream writer flips ``commitment: true`` on the JSON
    entry — verifying the full plexos2gtopt → JSON wiring for the
    commitment-conditional pmin.

    Uses the same fixture as the drop-path test; the ``valid``
    battery (pmin_charge = 1.0, pmax = 5.0) exercises the keep path.
    """
    # pylint: disable=import-outside-toplevel
    from plexos2gtopt.gtopt_writer import build_battery_array

    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_PMIN_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    batts = {b.name: b for b in extract_batteries(db, bundle)}
    spec = batts["valid"]
    # Parser keeps pmin_charge unchanged on the keep path.
    assert spec.pmin_charge == 1.0
    # Discharge side stays at the parser default of 0 (the fixture
    # ships no Min Discharge Level for ``valid``).
    assert spec.pmin_discharge == 0.0
    # And the writer translates a one-sided pmin into commitment=true
    # plus the matching key on the JSON entry.
    out = build_battery_array((spec,))
    entry = out[0]
    assert entry["pmin_charge"] == 1.0
    assert "pmin_discharge" not in entry
    assert entry["commitment"] is True


# ---------------------------------------------------------------------------
# Battery extractor reads ``Max Cycles Day`` → BatterySpec.max_cycles_day,
# and the writer emits ``capacity`` + ``max_cycles_day`` (the daily energy-
# throughput limit).  CEN PCP ships ``Max Cycles Day = 1`` on every battery.
# ---------------------------------------------------------------------------


_BATT_CYCLES_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>7</class_id><name>Battery</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_object>
    <object_id>1</object_id><class_id>1</class_id><name>SEN</name>
  </t_object>
  <t_object>
    <object_id>10</object_id><class_id>22</class_id><name>bus_a</name>
  </t_object>
  <t_object>
    <object_id>40</object_id><class_id>7</class_id><name>bess_cyc</name>
  </t_object>
  <t_collection>
    <collection_id>83</collection_id>
    <parent_class_id>7</parent_class_id>
    <child_class_id>22</child_class_id>
    <name>Nodes</name>
  </t_collection>
  <t_collection>
    <collection_id>80</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>7</child_class_id>
    <name>Batteries</name>
  </t_collection>
  <t_property>
    <property_id>500</property_id><collection_id>80</collection_id>
    <name>Max Power</name>
  </t_property>
  <t_property>
    <property_id>503</property_id><collection_id>80</collection_id>
    <name>Capacity</name>
  </t_property>
  <t_property>
    <property_id>504</property_id><collection_id>80</collection_id>
    <name>Max Cycles Day</name>
  </t_property>
  <t_membership>
    <membership_id>900</membership_id>
    <collection_id>83</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>950</membership_id>
    <collection_id>80</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>40</child_object_id>
  </t_membership>
  <t_data>
    <data_id>1</data_id><membership_id>950</membership_id>
    <property_id>500</property_id><value>50.0</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>950</membership_id>
    <property_id>503</property_id><value>100.0</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>950</membership_id>
    <property_id>504</property_id><value>1.0</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_batteries_reads_max_cycles_day(tmp_path: Path) -> None:
    """``Max Cycles Day`` on the System→Batteries collection populates
    ``BatterySpec.max_cycles_day`` (the daily energy-throughput limit N).
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_CYCLES_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    batts = {b.name: b for b in extract_batteries(db, bundle)}
    assert "bess_cyc" in batts
    assert batts["bess_cyc"].max_cycles_day == 1.0


def test_writer_emits_capacity_and_max_cycles_day(tmp_path: Path) -> None:
    """End-to-end: parser reads ``Max Cycles Day`` and the writer emits
    both ``capacity`` (usable energy = emax) and ``max_cycles_day`` on
    the JSON entry so gtopt can build the HARD ``Σ discharge·Δt ≤
    N·capacity`` daily-cycle row."""
    # pylint: disable=import-outside-toplevel
    from plexos2gtopt.gtopt_writer import build_battery_array

    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_CYCLES_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    spec = {b.name: b for b in extract_batteries(db, bundle)}["bess_cyc"]
    entry = build_battery_array((spec,))[0]
    assert entry["max_cycles_day"] == 1.0
    # capacity = usable energy = emax (Max SoC% × Capacity = 100% × 100).
    assert entry["capacity"] == 100.0


# ---------------------------------------------------------------------------
# T10: ReservoirSpec.water_value sentinel (1e+30) → never_drain hard floor
# ---------------------------------------------------------------------------


_RESERVOIR_WV_XML_TMPL = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>8</class_id><name>Storage</name></t_class>
  <t_object>
    <object_id>1</object_id><class_id>1</class_id><name>SEN</name>
  </t_object>
  <t_object>
    <object_id>30</object_id><class_id>8</class_id><name>RES_X</name>
  </t_object>
  <t_collection>
    <collection_id>93</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Storages</name>
  </t_collection>
  <t_property>
    <property_id>200</property_id><collection_id>93</collection_id>
    <name>Water Value</name>
  </t_property>
  <t_membership>
    <membership_id>520</membership_id>
    <collection_id>93</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_data>
    <data_id>1</data_id>
    <membership_id>520</membership_id>
    <property_id>200</property_id>
    <value>{{wv}}</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_reservoirs_sentinel_water_value_sets_never_drain(
    tmp_path: Path,
) -> None:
    """PLEXOS ``Water Value = 1e+30`` is the "never drain" sentinel:
    the reservoir must keep at least its initial volume.  The
    extractor must NOT clamp it to a finite price (a finite cost
    would let the LP buy out of the sentinel) — instead drop
    water_value to 0 and set ``never_drain=True`` so the writer
    emits a HARD ``vol_end >= eini`` constraint with no
    ``efin_cost`` slack.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_RESERVOIR_WV_XML_TMPL.format(wv="1e30"))
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    out = extract_reservoirs(db, bundle)
    assert len(out) == 1
    res = out[0]
    assert res.never_drain is True
    # Sentinel is NOT clamped to a finite price — water_value stays 0.
    assert res.water_value == 0.0


def test_extract_reservoirs_finite_water_value_is_passthrough(tmp_path: Path) -> None:
    """A finite ``Water Value`` is forwarded verbatim with
    ``never_drain=False`` — only the 1e+30 sentinel triggers the
    hard-floor mode.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_RESERVOIR_WV_XML_TMPL.format(wv="10000"))
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    out = extract_reservoirs(db, bundle)
    assert len(out) == 1
    res = out[0]
    assert res.never_drain is False
    assert res.water_value == 10000.0


def test_build_plant_cap_ucs_uniq_mutex_group_config_exclusivity() -> None:
    """F1: a PLEXOS ``*_Uniq`` mutex group caps Σ over ALL config × band
    variants of one physical plant at the largest config's pmax (config
    exclusivity), and the fuel-band fallback does NOT re-cap covered gens.
    """
    gens = (
        # Plant P: config -TG (pmax 100) and -TG+TV (pmax 250), 2 bands each.
        GeneratorSpec(object_id=1, name="P-TG_GN_A", bus_name="b", pmax=100.0),
        GeneratorSpec(object_id=2, name="P-TG_GN_B", bus_name="b", pmax=100.0),
        GeneratorSpec(object_id=3, name="P-TG+TV_GN_A", bus_name="b", pmax=250.0),
        GeneratorSpec(object_id=4, name="P-TG+TV_GN_B", bus_name="b", pmax=250.0),
        # Plant Q: single config, 2 fuel bands, NO _Uniq → fallback cap only.
        GeneratorSpec(object_id=5, name="Q_GN_A", bus_name="b", pmax=50.0),
        GeneratorSpec(object_id=6, name="Q_GN_B", bus_name="b", pmax=50.0),
    )
    case = PlexosCase(bundle=None, generators=gens)  # type: ignore[arg-type]
    mutex = (
        (
            "P_Uniq",
            frozenset({"P-TG_GN_A", "P-TG_GN_B", "P-TG+TV_GN_A", "P-TG+TV_GN_B"}),
        ),
    )
    ucs = _build_plant_cap_ucs(case, mutex)
    by_name = {u.name: u for u in ucs}

    # Config-exclusivity cap over ALL 4 P variants, capped at max pmax 250.
    assert "PlantCap_P" in by_name
    expr = by_name["PlantCap_P"].expression
    for n in ("P-TG_GN_A", "P-TG_GN_B", "P-TG+TV_GN_A", "P-TG+TV_GN_B"):
        assert f'generator("{n}").generation' in expr
    assert expr.endswith("<= 250.000000")

    # The P variants are capped by exactly ONE UC (no fuel-band double-cap).
    assert sum(1 for u in ucs if "P-TG_GN_A" in u.expression) == 1

    # F5: description states it's a config-exclusivity cap with the
    # source mutex group, the [MW] unit, and the source file.
    desc = by_name["PlantCap_P"].description
    assert "config exclusivity" in desc
    assert "[MW]" in desc
    assert "P_Uniq" in desc
    assert "(File: DBSEN_PRGDIARIO.xml)" in desc

    # Plant Q (no _Uniq) still gets a fuel-band fallback cap at its pmax 50.
    assert "PlantCap_Q" in by_name
    assert by_name["PlantCap_Q"].expression.endswith("<= 50.000000")


def test_build_plant_cap_ucs_no_mutex_falls_back_to_fuel_bands() -> None:
    """Without any ``_Uniq`` group, the legacy fuel-band family cap still
    fires (one cap per multi-band single-config family)."""
    gens = (
        GeneratorSpec(object_id=1, name="R_GN_A", bus_name="b", pmax=80.0),
        GeneratorSpec(object_id=2, name="R_GN_B", bus_name="b", pmax=80.0),
    )
    case = PlexosCase(bundle=None, generators=gens)  # type: ignore[arg-type]
    ucs = _build_plant_cap_ucs(case, ())
    assert any(u.name == "PlantCap_R" for u in ucs)


_UNIQ_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>10</object_id><class_id>2</class_id><name>GEN_A</name></t_object>
  <t_object><object_id>11</object_id><class_id>2</class_id><name>GEN_B</name></t_object>
  <t_object>
    <object_id>100</object_id><class_id>70</class_id><name>PLANT_Uniq</name>
  </t_object>
  <t_object>
    <object_id>101</object_id><class_id>70</class_id><name>OTHER_max</name>
  </t_object>
  <t_collection>
    <collection_id>32</collection_id><parent_class_id>2</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_membership>
    <membership_id>1</membership_id><collection_id>32</collection_id>
    <parent_object_id>10</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>2</membership_id><collection_id>32</collection_id>
    <parent_object_id>11</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>3</membership_id><collection_id>32</collection_id>
    <parent_object_id>10</parent_object_id><child_object_id>101</child_object_id>
  </t_membership>
</MasterDataSet>
"""


def test_extract_config_mutex_groups_db_walk(tmp_path: Path) -> None:
    """F1: ``_extract_config_mutex_groups`` reads ``*_Uniq`` Constraint
    memberships and returns the exact set of member generators; non-Uniq
    constraints are ignored."""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_UNIQ_XML)
    db = load_xml(xml_path)
    groups = _extract_config_mutex_groups(db)
    assert len(groups) == 1
    name, members = groups[0]
    assert name == "PLANT_Uniq"
    assert members == frozenset({"GEN_A", "GEN_B"})


def test_extract_config_mutex_groups_no_uniq_returns_empty(tmp_path: Path) -> None:
    """No ``*_Uniq`` Constraint objects → empty list (no crash)."""
    xml = _UNIQ_XML.replace("PLANT_Uniq", "PLANT_max")
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(xml)
    db = load_xml(xml_path)
    assert _extract_config_mutex_groups(db) == []
