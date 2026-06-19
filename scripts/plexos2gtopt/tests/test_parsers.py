"""Unit tests for :mod:`plexos2gtopt.parsers`.

Each P1 extractor is exercised against a hand-rolled synthetic
``DBSEN_PRGDIARIO.xml`` + minimal CSVs under ``tmp_path``. The fixtures
are deliberately tiny (4 buses / 2 gens / 1 line / 1 battery / 2 loads)
so failures point at one extractor at a time.
"""

from __future__ import annotations

import os as _os
from pathlib import Path

import pytest

from plexos2gtopt.entities import BundleSpec
from plexos2gtopt.entities import GeneratorSpec, LineSpec, PlexosCase
from plexos2gtopt.parsers import (
    FUEL_FAMILY_BIOGAS,
    FUEL_FAMILY_BIOMASA,
    FUEL_FAMILY_CARBON,
    FUEL_FAMILY_DIESEL,
    FUEL_FAMILY_FUEL_OIL,
    FUEL_FAMILY_GAS,
    FUEL_FAMILY_GEOTHERMAL,
    FUEL_FAMILY_GLP,
    FUEL_FAMILY_HYDRO,
    FUEL_FAMILY_OTHER,
    FUEL_FAMILY_OTROS,
    FUEL_FAMILY_RENEWABLE,
    FUEL_FAMILY_SOLAR,
    FUEL_FAMILY_THERMAL,
    FUEL_FAMILY_WIND,
    _apply_adaptive_loss_segments,
    _apply_loss_sos2_policy,
    _build_plant_cap_ucs,
    _extract_config_mutex_groups,
    extract_batteries,
    extract_demands,
    extract_fuels,
    extract_generators,
    extract_lines,
    extract_nodes,
    extract_reservoirs,
    family_from_plexos_category,
    fuel_family_of_fuel,
    fuel_family_of_generator,
    primary_energy_of_generator,
    renewable_tech_of_generator,
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
    """One FuelSpec per Fuel object; price=0 when Fuel_Price.csv absent.

    Also confirms the canonical fuel-family tag falls back to
    ``"other"`` when the bundle uses a non-CEN-prefix name (the
    synthetic bundle ships a lowercase ``"diesel"`` fuel rather than
    the CEN-PCP ``Diesel_*`` prefix convention).
    """
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert [f.name for f in fuels] == ["diesel"]
    assert fuels[0].price == 0.0
    # No canonical CEN prefix on the synthetic name → default fallback.
    assert fuels[0].type_tag == FUEL_FAMILY_OTHER


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


def test_extract_fuels_heat_content_from_plexos(tmp_path: Path) -> None:
    """PLEXOS ``Heat Content`` (System→Fuel property, GJ/fuel-unit) is
    extracted into ``FuelSpec.heat_content`` (issue #5).  Synthetic
    bundle ships one Fuel ``diesel`` with Heat Content = 35.8 GJ/ton
    (typical for diesel).  The System collection has parent_class=1
    (System) and child_class=4 (Fuel); the property carries collection_id
    = the System→Fuel collection id, and the membership_id is the
    auto-generated System-pseudo membership for that object."""
    xml = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>4</class_id><name>Fuel</name></t_class>
  <t_object>
    <object_id>1</object_id><class_id>1</class_id><name>System</name>
  </t_object>
  <t_object>
    <object_id>30</object_id><class_id>4</class_id><name>diesel</name>
  </t_object>
  <t_collection>
    <collection_id>5</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>4</child_class_id>
    <name>Fuels</name>
  </t_collection>
  <t_membership>
    <membership_id>5000</membership_id>
    <collection_id>5</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_property>
    <property_id>500</property_id>
    <collection_id>5</collection_id>
    <name>Heat Content</name>
  </t_property>
  <t_data>
    <data_id>1</data_id>
    <membership_id>5000</membership_id>
    <property_id>500</property_id>
    <value>35.8</value>
  </t_data>
</MasterDataSet>
"""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(xml)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert [f.name for f in fuels] == ["diesel"]
    assert fuels[0].heat_content == pytest.approx(35.8, rel=1e-9)


def test_extract_fuels_heat_content_default_zero(tmp_path: Path) -> None:
    """When the PLEXOS bundle ships no Heat Content data,
    ``FuelSpec.heat_content`` falls back to 0.0 — the downstream
    IPCC-fallback in ``apply_emission_defaults_from_file`` then fills
    it (unless the user runs ``--no-emissions``)."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert fuels[0].heat_content == 0.0


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


def test_extract_fuels_max_offtake_week_no_horizon_fallback(
    tmp_path: Path,
) -> None:
    """No Horizon in XML → first CSV row wins (legacy fallback).

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
        "diesel,2026,4,16,1,5.0\n"
        "diesel,2026,4,23,1,1.0\n",
    )
    db = load_xml(xml_path)
    assert db.horizon_start is None  # fixture has no Horizon
    fuels = extract_fuels(db, bundle)
    # 5.0 TJ × 1000 = 5000 GJ (first CSV row wins when horizon unknown).
    assert fuels[0].max_offtake == 5000.0


def test_extract_fuels_max_offtake_week_time_weighted_horizon(
    tmp_path: Path,
) -> None:
    """7-day horizon straddling 2 weeks → time-weighted PLEXOS budget.

    Reproduces the CEN PCP 2026-04-22 daily case for
    ``Gas_Colbun_GN_B``: ``Fuel_MaxOfftakeWeek.csv`` ships
    2026-04-16 → 2.1 TJ and 2026-04-23 → 4.6 TJ.  The 7-day horizon
    Apr 22-28 has 1 day in week 04-16 (Apr 22) and 6 days in week
    04-23 (Apr 23-28).  Time-weighted budget = 2100 × 1/7 + 4600 ×
    6/7 = 4242.857 GJ — matches PLEXOS's ``Σ_period duration ×
    RHS_period`` from the solution .accdb to the cent.

    Regression: legacy ``n_days=1`` reader returned 2100 GJ (too
    tight by 50 %) and starved NEHUENCO_1-TG+TV / NUEVA_RENCA in the
    MIP.  An earlier "binding-week" fix that picked a single week's
    cap landed at 4600 GJ (too loose by 8 %).  The time-weighted
    formula here is the only one that matches PLEXOS exactly.
    """
    from datetime import datetime as _dt

    bundle, xml_path = _build_bundle(tmp_path)
    object.__setattr__(bundle, "n_days", 7)
    _write_csv(
        tmp_path,
        "Fuel_MaxOfftakeWeek.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\n"
        "diesel,2026,4,16,1,2.1\n"
        "diesel,2026,4,23,1,4.6\n",
    )
    db = load_xml(xml_path)
    db.horizon_start = _dt(2026, 4, 22)
    fuels = extract_fuels(db, bundle)
    assert fuels[0].max_offtake is not None
    assert abs(fuels[0].max_offtake - (2100 * 1 / 7 + 4600 * 6 / 7)) < 1e-6


def test_extract_fuels_max_offtake_week_horizon_inside_one_week(
    tmp_path: Path,
) -> None:
    """horizon entirely inside one week → full cap (overlap = horizon_days/7)."""
    from datetime import datetime as _dt

    bundle, xml_path = _build_bundle(tmp_path)
    object.__setattr__(bundle, "n_days", 3)  # Apr 18-20 inside week 04-16
    _write_csv(
        tmp_path,
        "Fuel_MaxOfftakeWeek.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\ndiesel,2026,4,16,1,5.0\n",
    )
    db = load_xml(xml_path)
    db.horizon_start = _dt(2026, 4, 18)
    fuels = extract_fuels(db, bundle)
    # 3 horizon days inside week 04-16 → 5000 × 3/7 GJ
    assert fuels[0].max_offtake is not None
    assert abs(fuels[0].max_offtake - 5000 * 3 / 7) < 1e-6


def test_extract_fuels_max_offtake_week_single_week_full_horizon(
    tmp_path: Path,
) -> None:
    """Single CSV row covering the full 7-day horizon → cap × 1 (= CSV cap)."""
    from datetime import datetime as _dt

    bundle, xml_path = _build_bundle(tmp_path)
    object.__setattr__(bundle, "n_days", 7)
    _write_csv(
        tmp_path,
        "Fuel_MaxOfftakeWeek.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\ndiesel,2026,4,22,1,9.8\n",
    )
    db = load_xml(xml_path)
    db.horizon_start = _dt(2026, 4, 22)
    fuels = extract_fuels(db, bundle)
    # Apr 22-28 is fully inside week 04-22 (Apr 22-28) → 7/7 × 9800
    assert fuels[0].max_offtake == 9800.0


def test_extract_fuels_max_offtake_week_absent(tmp_path: Path) -> None:
    """No CSV → ``max_offtake = None`` (no cap binds)."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert fuels[0].max_offtake is None


# ---------------------------------------------------------------------------
# Fuel.min_offtake (PLEXOS Min Offtake / take-or-pay reproduction)
# ---------------------------------------------------------------------------


def test_extract_fuels_min_offtake_unset_default(tmp_path: Path) -> None:
    """Bundle without any Min Offtake property → ``min_offtake = None``.

    This is the state of every fuel across the 14 cached CEN PCP
    bundles (2025-10 → 2026-05).  Confirms the parser stays silent /
    no JSON field emitted when the entire family is unpopulated.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert fuels[0].min_offtake is None
    assert fuels[0].min_offtake_cost is None


def _build_min_offtake_xml(
    weekly_value: float | None = None,
    explicit_penalty: float | None = None,
) -> str:
    """Synthetic PLEXOS XML with the System→Fuels collection and a
    ``Min Offtake Week`` (pid 598) value optionally set on the fuel.
    """
    extras = []
    if weekly_value is not None:
        extras.append(
            f"  <t_data><data_id>5001</data_id>"
            f"<membership_id>9501</membership_id>"
            f"<property_id>598</property_id>"
            f"<value>{weekly_value}</value></t_data>"
        )
    if explicit_penalty is not None:
        extras.append(
            f"  <t_data><data_id>5002</data_id>"
            f"<membership_id>9501</membership_id>"
            f"<property_id>602</property_id>"
            f"<value>{explicit_penalty}</value></t_data>"
        )
    extras_str = "\n".join(extras)
    return f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>4</class_id><name>Fuel</name></t_class>
  <t_object>
    <object_id>1</object_id><class_id>1</class_id><name>SEN</name>
  </t_object>
  <t_object>
    <object_id>31</object_id><class_id>4</class_id><name>diesel</name>
  </t_object>
  <t_collection>
    <collection_id>40</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>4</child_class_id>
    <name>Fuels</name>
  </t_collection>
  <t_property>
    <property_id>598</property_id><collection_id>40</collection_id>
    <name>Min Offtake Week</name>
  </t_property>
  <t_property>
    <property_id>602</property_id><collection_id>40</collection_id>
    <name>Min Offtake Penalty</name>
  </t_property>
  <t_membership>
    <membership_id>9501</membership_id>
    <collection_id>40</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
{extras_str}
</MasterDataSet>
"""


def test_extract_fuels_min_offtake_week_default_penalty(
    tmp_path: Path,
    caplog: pytest.LogCaptureFixture,
) -> None:
    """``Min Offtake Week = 2000`` without an explicit penalty →
    ``min_offtake`` folded to horizon-wide budget and
    ``min_offtake_cost = 1000`` (PLEXOS soft-by-default translation).
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_build_min_offtake_xml(weekly_value=2000.0))
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    with caplog.at_level("WARNING", logger="plexos2gtopt.parsers"):
        fuels = extract_fuels(db, bundle)
    # n_days defaults to 7 → horizon_hours = 168 → windows_in_horizon
    # for the weekly bucket = 168 / 168 = 1, so contribution = 2000.0
    assert fuels[0].min_offtake == 2000.0
    # PLEXOS soft-by-default → 1000 $/fuel-unit injected by the parser.
    assert fuels[0].min_offtake_cost == 1000.0
    # WARNING is emitted so the first real bundle to ship a non-zero
    # Min Offtake surfaces loudly.
    assert any(
        "Min Offtake" in rec.message and "diesel" in rec.message
        for rec in caplog.records
    )


def test_extract_fuels_min_offtake_week_explicit_penalty(
    tmp_path: Path,
) -> None:
    """Explicit ``Min Offtake Penalty = 250`` overrides the
    PLEXOS-default $1000 — the parser passes the literal value through.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(
        _build_min_offtake_xml(weekly_value=2000.0, explicit_penalty=250.0)
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    fuels = extract_fuels(db, bundle)
    assert fuels[0].min_offtake == 2000.0
    assert fuels[0].min_offtake_cost == 250.0


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


@pytest.fixture
def _opt_in_aux_use(monkeypatch: pytest.MonkeyPatch) -> None:
    """Opt in to the ``Gen_AuxUse.csv`` parsing path.

    Upstream commit ``bf87282a0`` made the parser ignore Gen_AuxUse.csv
    by default (PLEXOS itself doesn't apply it); opt-in via the
    ``GTOPT_APPLY_GENERATION_AUX_USE`` env var (or the
    ``--apply-generation-aux-use`` CLI flag).  These three tests
    exercise the opt-in branch.
    """
    monkeypatch.setenv("GTOPT_APPLY_GENERATION_AUX_USE", "1")


def test_extract_generators_aux_use_percent_to_pu(
    tmp_path: Path,
    caplog: pytest.LogCaptureFixture,
    _opt_in_aux_use: None,
) -> None:
    """``Gen_AuxUse.csv`` ships PLEXOS ``Auxiliary Use`` in PERCENT
    (0-100), NOT p.u. fraction.  The parser must divide by 100 and
    clamp to the physical aux-use envelope (≤ 50%).

    Pin all four regimes from the empirical CEN data:
      * 0.068  → 0.00068 p.u.  (tiny aux-use, ARICA_GM)
      * 0.9995 → 0.009995 p.u. (CCGT, NEHUENCO_9B — was the
                                catastrophic 99.95% loss case)
      * 30.06  → 0.3006 p.u.   (high but valid, old diesel)
      * 50.01  → DROPPED + warned (above physical envelope)
      * -1.0   → DROPPED silently (invalid)
    """
    import csv as _csv

    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_AuxUse.csv",
        "Name,Value\n"
        "thermal_a,0.9995\n"  # CCGT case
        "solar_b,30.06\n",  # high but valid diesel
    )
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,100\n"
        "solar_b,2026,1,1,1,1,100\n",
    )

    # Quick sanity-check on the parser's per-row behaviour by
    # exercising the inner CSV-loop semantics it relies on.
    rows = []
    with (tmp_path / "Gen_AuxUse.csv").open(encoding="utf-8", newline="") as f:
        for r in _csv.DictReader(f):
            rows.append((r["Name"], float(r["Value"])))
    assert rows == [("thermal_a", 0.9995), ("solar_b", 30.06)]

    # End-to-end: extract_generators must store the values as p.u.
    # fractions divided by 100, never as the raw CSV values.
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    by_name = {g.name: g for g in gens}
    assert by_name["thermal_a"].aux_use == pytest.approx(0.009995, rel=1e-9)
    assert by_name["solar_b"].aux_use == pytest.approx(0.3006, rel=1e-9)


def test_extract_generators_aux_use_above_max_is_dropped(
    tmp_path: Path,
    caplog: pytest.LogCaptureFixture,
    _opt_in_aux_use: None,
) -> None:
    """Values above the 50% physical aux-use envelope are dropped and
    surface as a WARNING (operator can audit)."""
    import logging

    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_AuxUse.csv",
        "Name,Value\n"
        "thermal_a,0.5\n"  # 0.5% — kept
        "solar_b,50.01\n",  # 50.01% — dropped (just above cap)
    )
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,100\n"
        "solar_b,2026,1,1,1,1,100\n",
    )
    caplog.clear()
    with caplog.at_level(logging.WARNING, logger="plexos2gtopt.parsers"):
        db = load_xml(xml_path)
        gens = extract_generators(db, bundle)
    by_name = {g.name: g for g in gens}
    assert by_name["thermal_a"].aux_use == pytest.approx(0.005, rel=1e-9)
    assert by_name["solar_b"].aux_use == 0.0  # dropped
    # Warning fired with the sample.
    assert any(
        "Gen_AuxUse.csv" in r.message and "solar_b" in r.message for r in caplog.records
    ), f"Expected a Gen_AuxUse warning naming solar_b; got: {caplog.records}"


def test_extract_generators_aux_use_negative_dropped_silently(
    tmp_path: Path,
    _opt_in_aux_use: None,
) -> None:
    """Non-positive aux-use is invalid data (no station service can
    consume <0% of gross).  Drop silently — no warning needed."""
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_AuxUse.csv",
        "Name,Value\nthermal_a,-1.0\nsolar_b,0\n",
    )
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,100\n"
        "solar_b,2026,1,1,1,1,100\n",
    )
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    by_name = {g.name: g for g in gens}
    assert by_name["thermal_a"].aux_use == 0.0
    assert by_name["solar_b"].aux_use == 0.0


def test_extract_generators_recovers_csv_only_thermals(tmp_path: Path) -> None:
    """CSV-only thermals (no XML Generator object) are recovered as
    zero-capacity audit entries by inheriting bus + fuel from the
    longest-common-prefix XML sibling.

    Mirrors CEN-PCP CCGT mode-variant pattern: ``ATA-TG1A_GNL_X``
    has no XML object but a real heat-rate in ``Gen_HeatRate.csv`` and
    a fuel-bearing XML sibling ``ATA-TG1A_GNL_A``.  Phantoms with no
    sibling sharing ≥6 chars of prefix are skipped.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nthermal_a,2026,1,1,1,1,100\n",
    )
    # thermal_a is the XML sibling; thermal_aX is the CSV-only phantom
    # (sibling share = "thermal_a", 9 chars, well above the 6-char floor).
    # orphan_unknown has no sibling, must be skipped.
    _write_csv(
        tmp_path,
        "Gen_HeatRate.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,0.31\n"
        "thermal_aX,2026,1,1,1,1,0.33\n"
        "orphan_unknown,2026,1,1,1,1,0.40\n",
    )
    _write_csv(
        tmp_path,
        "Gen_VOMCharge.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "thermal_a,2026,1,1,1,1,2.0\n"
        "thermal_aX,2026,1,1,1,1,4.5\n"
        "orphan_unknown,2026,1,1,1,1,8.0\n",
    )
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    by_name = {g.name: g for g in gens}
    # XML sibling intact.
    assert "thermal_a" in by_name
    # CSV-only phantom recovered with bus + fuel inherited from sibling.
    phantom = by_name.get("thermal_aX")
    assert phantom is not None
    assert phantom.pmax == 0.0
    assert phantom.heat_rate == 0.33
    assert phantom.vom_charge == 4.5
    assert phantom.bus_name == by_name["thermal_a"].bus_name
    assert phantom.fuel_names == by_name["thermal_a"].fuel_names
    # orphan_unknown has no sibling (prefix shared with "thermal_a" is
    # zero chars) → skipped, no GeneratorSpec emitted.
    assert "orphan_unknown" not in by_name


def test_fuel_family_of_generator_canonical_mapping() -> None:
    """Generator-name suffix → canonical fuel family.

    Covers every entry in ``_GEN_SUFFIX_TO_FUEL_FAMILY`` plus the two
    "tag is the second-to-last segment" cases (``_GNL_X`` /
    ``_GNL_B``).  Unrecognised suffixes (``_U2``, ``_TG``) return
    ``None`` so callers fall back cleanly.
    """
    # Last-segment fuel tags.
    assert fuel_family_of_generator("UJINA_U5_DIE") == FUEL_FAMILY_DIESEL
    assert fuel_family_of_generator("UJINA_U5_HFO") == FUEL_FAMILY_FUEL_OIL
    assert fuel_family_of_generator("CMPC_X_IFO") == FUEL_FAMILY_FUEL_OIL
    assert fuel_family_of_generator("ANY_FO6") == FUEL_FAMILY_FUEL_OIL
    assert fuel_family_of_generator("ATA_GN") == FUEL_FAMILY_GAS
    assert fuel_family_of_generator("KELAR_TG1_GNL") == FUEL_FAMILY_GAS
    assert fuel_family_of_generator("COLBUN_INF") == FUEL_FAMILY_GAS
    assert fuel_family_of_generator("RENCA_GLP") == FUEL_FAMILY_GLP
    # Second-to-last segment fuel tags (dispatch-mode marker last).
    assert fuel_family_of_generator("ATA-TG1A_GNL_X") == FUEL_FAMILY_GAS
    assert fuel_family_of_generator("COLMITO_GNL_B") == FUEL_FAMILY_GAS
    assert fuel_family_of_generator("KELAR-TG1+0.5TV_GNL_X") == FUEL_FAMILY_GAS
    # Non-fuel suffixes (unit indexes, technology markers) → None.
    assert fuel_family_of_generator("EL_TOTORAL_U2") is None
    assert fuel_family_of_generator("LAGUNA_VERDE_TG") is None
    assert fuel_family_of_generator("LAGUNA_VERDE_TV") is None
    assert fuel_family_of_generator("PEHUENCHE_U1") is None


def test_fuel_family_of_fuel_canonical_mapping() -> None:
    """Fuel-object name prefix → canonical fuel family.

    The eight CEN-PCP prefixes round-trip to the eight canonical tags;
    a name that doesn't begin with any known prefix returns ``None``
    (callers default to ``FUEL_FAMILY_OTHER``).
    """
    assert fuel_family_of_fuel("Diesel_Collahuasi") == FUEL_FAMILY_DIESEL
    assert fuel_family_of_fuel("FuelOil_Norgener") == FUEL_FAMILY_FUEL_OIL
    assert fuel_family_of_fuel("Gas_Kelar_A") == FUEL_FAMILY_GAS
    assert fuel_family_of_fuel("Gas_Colbun_GN_A") == FUEL_FAMILY_GAS
    assert fuel_family_of_fuel("GLP_TenoGas") == FUEL_FAMILY_GLP
    assert fuel_family_of_fuel("Biomasa_CMPCLaja_B1") == FUEL_FAMILY_BIOMASA
    assert fuel_family_of_fuel("Biogas_SantaMarta") == FUEL_FAMILY_BIOGAS
    assert fuel_family_of_fuel("Carbon_Angamos1") == FUEL_FAMILY_CARBON
    assert fuel_family_of_fuel("Otros_Noracid") == FUEL_FAMILY_OTROS
    # Unknown / hand-rolled name → None; caller defaults to "other".
    assert fuel_family_of_fuel("synthetic_pseudo_fuel") is None
    assert fuel_family_of_fuel("VIRTUAL_FUEL") is None


def test_renewable_tech_of_generator_canonical_mapping() -> None:
    """CEN-PCP renewable-suffix → canonical tech tag.

    Verified against the bundle's perfect 1:1 correspondence between
    name suffix and PLEXOS category: 740 ``*_FV`` units all in
    ``"Solar Farms"`` and 66 ``*_EO`` all in ``"Wind Farms"``.
    """
    assert renewable_tech_of_generator("AILLIN_FV") == FUEL_FAMILY_SOLAR
    assert renewable_tech_of_generator("ALENA_EO") == FUEL_FAMILY_WIND
    assert renewable_tech_of_generator("plain_name") is None
    # The fuel-tag suffixes are NOT renewable tech tags — keep the
    # two namespaces separate.
    assert renewable_tech_of_generator("UJINA_U5_DIE") is None
    assert renewable_tech_of_generator("ATA_GNL") is None


def test_family_from_plexos_category_substring_match() -> None:
    """PLEXOS category names → canonical primary-energy tag.

    Case-insensitive substring match — handles CEN-PCP's group
    variants (``"Hydro Gen Group A/B/C"``, ``"Thermal Gen N. Zone"``,
    ``"Termicas Ficticias"``) without enumerating every suffix.
    """
    assert family_from_plexos_category("Solar Farms") == FUEL_FAMILY_SOLAR
    assert family_from_plexos_category("Wind Farms") == FUEL_FAMILY_WIND
    assert family_from_plexos_category("Hydro Gen Group A") == FUEL_FAMILY_HYDRO
    assert family_from_plexos_category("Hydro Gen Group B") == FUEL_FAMILY_HYDRO
    assert family_from_plexos_category("Hydro Gen Group C") == FUEL_FAMILY_HYDRO
    assert family_from_plexos_category("Thermal Gen N. Zone") == FUEL_FAMILY_THERMAL
    assert family_from_plexos_category("Thermal Gen S. Zone") == FUEL_FAMILY_THERMAL
    # Spanish accent-stripped variant.
    assert family_from_plexos_category("Termicas Ficticias") == FUEL_FAMILY_THERMAL
    assert family_from_plexos_category("Hidroeléctrica") == FUEL_FAMILY_HYDRO
    assert family_from_plexos_category("Geothermal") == FUEL_FAMILY_GEOTHERMAL
    # Unknown category → None.
    assert family_from_plexos_category("Some Other Category") is None
    assert family_from_plexos_category("") is None


def test_primary_energy_of_generator_priority_order() -> None:
    """Detection priority: name-suffix → renewable-suffix → category
    → fuel attachment → generic fallback.
    """
    # 1. Name-suffix family wins even when category disagrees.
    assert (
        primary_energy_of_generator(
            "UJINA_U5_DIE", category_name="Solar Farms", fuel_names=("Gas_X",)
        )
        == FUEL_FAMILY_DIESEL
    )
    # 2. Renewable-suffix wins when no thermal-suffix.
    assert (
        primary_energy_of_generator("ALENA_EO", category_name=None, fuel_names=())
        == FUEL_FAMILY_WIND
    )
    # 3. PLEXOS category catches hydro (no name suffix).
    assert (
        primary_energy_of_generator(
            "ABANICO", category_name="Hydro Gen Group A", fuel_names=()
        )
        == FUEL_FAMILY_HYDRO
    )
    # 4. Fuel attachment classifies when name + category are silent.
    assert (
        primary_energy_of_generator(
            "PLAIN_NAME", category_name=None, fuel_names=("Diesel_X",)
        )
        == FUEL_FAMILY_DIESEL
    )
    # 5. Bare-thermal fallback when fuel signal present but unclassified.
    assert (
        primary_energy_of_generator(
            "PLAIN_NAME",
            category_name=None,
            fuel_names=("synthetic_pseudo_fuel",),
            has_heat_rate=True,
        )
        == FUEL_FAMILY_THERMAL
    )
    # 6. Renewable fallback when no fuel signal at all.
    assert (
        primary_energy_of_generator("MYSTERY_PLANT", category_name=None, fuel_names=())
        == FUEL_FAMILY_RENEWABLE
    )


def test_fuel_family_generator_and_fuel_agree_on_canonical_tag() -> None:
    """The two helpers return the SAME canonical tag for matching
    Generator/Fuel name pairs — the property the orphan-recovery
    sibling search and the published ``FuelSpec.type_tag`` rely on.
    """
    pairs = [
        ("UJINA_U5_DIE", "Diesel_Collahuasi"),
        ("UJINA_U5_HFO", "FuelOil_Collahuasi"),
        ("ATA-TG1A_GNL_X", "Gas_EnelMejillones_A"),
        ("COLMITO_GNL_B", "Gas_Colmito_A"),
        ("KELAR-TG1+0.5TV_GNL_X", "Gas_Kelar_A"),
        ("NUEVA_RENCA_GLP", "GLP_NuevaRencaFA"),
    ]
    for gen_name, fuel_name in pairs:
        gen_family = fuel_family_of_generator(gen_name)
        fuel_family = fuel_family_of_fuel(fuel_name)
        assert gen_family is not None
        assert fuel_family is not None
        assert gen_family == fuel_family, (
            f"family mismatch for {gen_name}↔{fuel_name}: "
            f"gen={gen_family} fuel={fuel_family}"
        )


def test_recover_csv_thermals_fuel_tag_picks_correct_family() -> None:
    """``_recover_csv_only_thermals`` honours the PLEXOS fuel-family
    tag at the end of the name: ``UJINA_U5_DIE`` must inherit a
    ``Diesel_*`` fuel from the 7-char-prefix sibling ``UJINA_U1_DIE``
    even when the 9-char-prefix neighbour ``UJINA_U5_HFO`` is
    available (whose ``FuelOil_*`` fuel would be wrong).
    """
    # pylint: disable=import-outside-toplevel
    from plexos2gtopt.parsers import _recover_csv_only_thermals

    siblings = [
        GeneratorSpec(
            object_id=10,
            name="UJINA_U1_DIE",
            bus_name="Collahuasi220",
            fuel_names=("Diesel_Collahuasi",),
        ),
        GeneratorSpec(
            object_id=11,
            name="UJINA_U5_HFO",
            bus_name="Collahuasi220",
            fuel_names=("FuelOil_Collahuasi",),
        ),
    ]
    recovered = _recover_csv_only_thermals(
        siblings,
        heat_rate_csv={"UJINA_U5_DIE": [0.246]},
        vom_csv={"UJINA_U5_DIE": [18.98]},
    )
    assert len(recovered) == 1
    phantom = recovered[0]
    assert phantom.name == "UJINA_U5_DIE"
    # Fuel-tag bias rejects the 9-char-prefix UJINA_U5_HFO winner and
    # picks the fuel-family-matching UJINA_U1_DIE instead.
    assert phantom.fuel_names == ("Diesel_Collahuasi",)
    assert phantom.bus_name == "Collahuasi220"
    assert phantom.heat_rate == 0.246
    assert phantom.vom_charge == 18.98
    assert phantom.pmax == 0.0


def test_recover_csv_thermals_no_tag_falls_back_to_longest_prefix() -> None:
    """Orphans whose suffix is NOT a PLEXOS fuel tag (e.g. ``_U2`` is a
    unit index, not a fuel family) fall back to the plain longest-prefix
    sibling — preserves the bus-only inheritance for non-thermal-tag
    name patterns like ``EL_TOTORAL_U2``.
    """
    # pylint: disable=import-outside-toplevel
    from plexos2gtopt.parsers import _recover_csv_only_thermals

    siblings = [
        GeneratorSpec(
            object_id=20,
            name="EL_TOTORAL",
            bus_name="ASanta110",
            fuel_names=("Diesel_ElTotoral",),
        ),
    ]
    recovered = _recover_csv_only_thermals(
        siblings,
        heat_rate_csv={"EL_TOTORAL_U2": [0.200]},
        vom_csv={"EL_TOTORAL_U2": [28.06]},
    )
    assert len(recovered) == 1
    assert recovered[0].name == "EL_TOTORAL_U2"
    assert recovered[0].fuel_names == ("Diesel_ElTotoral",)
    assert recovered[0].bus_name == "ASanta110"


def test_recover_csv_thermals_tag_with_no_match_falls_back() -> None:
    """When the orphan's fuel tag is recognised but NO sibling carries a
    matching fuel family, the helper falls back to the longest-prefix
    sibling regardless of fuel — better than skipping a real audit
    entry.
    """
    # pylint: disable=import-outside-toplevel
    from plexos2gtopt.parsers import _recover_csv_only_thermals

    # FAKE_PLANT_GNL_X has tag GNL → expects Gas_*, but the only sibling
    # carries FuelOil_*.  Fallback path keeps the orphan rather than
    # dropping it.
    siblings = [
        GeneratorSpec(
            object_id=30,
            name="FAKE_PLANT_HFO",
            bus_name="bus_X",
            fuel_names=("FuelOil_Norgener",),
        ),
    ]
    recovered = _recover_csv_only_thermals(
        siblings,
        heat_rate_csv={"FAKE_PLANT_GNL_X": [0.30]},
        vom_csv={"FAKE_PLANT_GNL_X": [5.0]},
    )
    assert len(recovered) == 1
    # No GNL-tagged sibling existed → fell back to the FuelOil neighbour.
    assert recovered[0].fuel_names == ("FuelOil_Norgener",)


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


def _build_nameplate_xml(max_capacity: float) -> str:
    """Synthetic PLEXOS XML with a System→Generators collection carrying
    a ``Max Capacity`` value on an offline hydro unit (object_id=10).

    The generator is attached to a node (so it is not dropped as a
    phantom) but ships no ``Gen_Rating`` row by itself; the test supplies
    an explicit-zero rating CSV so the dispatch ``pmax`` collapses to 0
    (PLEXOS holds the unit offline for the week) while ``Max Capacity``
    preserves the physical nameplate.
    """
    return f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_object>
    <object_id>1</object_id><class_id>1</class_id><name>SEN</name>
  </t_object>
  <t_object>
    <object_id>5</object_id><class_id>22</class_id><name>bus_north</name>
  </t_object>
  <t_object>
    <object_id>10</object_id><class_id>2</class_id><name>COLBUN_U2</name>
  </t_object>
  <t_collection>
    <collection_id>12</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>22</child_class_id>
    <name>Nodes</name>
  </t_collection>
  <t_collection>
    <collection_id>2</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>2</child_class_id>
    <name>Generators</name>
  </t_collection>
  <t_property>
    <property_id>11</property_id><collection_id>2</collection_id>
    <name>Max Capacity</name>
  </t_property>
  <t_membership>
    <membership_id>900</membership_id>
    <collection_id>12</collection_id>
    <parent_object_id>10</parent_object_id>
    <child_object_id>5</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>200</membership_id>
    <collection_id>2</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <t_data>
    <data_id>3001</data_id><membership_id>200</membership_id>
    <property_id>11</property_id><value>{max_capacity}</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_generators_nameplate_capture_offline_unit(tmp_path: Path) -> None:
    """``Max Capacity`` is captured into ``nameplates_out`` UNCONDITIONALLY
    for an offline unit whose dispatch ``pmax`` collapses to 0.

    Mirrors the CEN-PCP case where PLEXOS zeroes ``Gen_Rating`` for units
    it holds offline (COLBUN_U2, PEHUENCHE_U1, …): the dispatch ``pmax``
    must stay 0 (PLEXOS-faithful) while the physical nameplate flows to
    the reservoir extraction-flow estimator.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_build_nameplate_xml(240.0))
    # Explicit-zero Gen_Rating → the unit is offline all horizon, so
    # pmax = max(profile) = 0.0 (the explicit_zero_profile guard).
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nCOLBUN_U2,2026,1,1,1,1,0\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    nameplates: dict[str, float] = {}
    gens = extract_generators(db, bundle, nameplates_out=nameplates)
    by_name = {g.name: g for g in gens}
    # Dispatch pmax stays 0 — the offline unit is PLEXOS-faithful.
    assert by_name["COLBUN_U2"].pmax == 0.0
    # The physical nameplate is captured for the estimator regardless.
    assert nameplates["COLBUN_U2"] == 240.0


def test_extract_generators_nameplate_opt_in(tmp_path: Path) -> None:
    """Without ``nameplates_out`` the extractor does not read Max Capacity
    (back-compat: the map is purely opt-in and never pollutes the spec)."""
    bundle, xml_path = _build_bundle(tmp_path)
    _write_csv(
        tmp_path,
        "Gen_Rating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nthermal_a,2026,1,1,1,1,100\n",
    )
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    # GeneratorSpec carries no nameplate field — the map is external.
    assert not hasattr(gens[0], "nameplate")
    assert not hasattr(gens[0], "max_capacity")


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


_EL0_LINE_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>24</class_id><name>Line</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>22</class_id><name>bus_n</name></t_object>
  <t_object><object_id>11</object_id><class_id>22</class_id><name>bus_s</name></t_object>
  <t_object><object_id>20</object_id><class_id>24</class_id><name>CABLE</name></t_object>
  <t_object><object_id>21</object_id><class_id>24</class_id><name>HARDLINE</name></t_object>
  <t_collection>
    <collection_id>306</collection_id><parent_class_id>24</parent_class_id>
    <child_class_id>22</child_class_id><name>Node From</name>
  </t_collection>
  <t_collection>
    <collection_id>307</collection_id><parent_class_id>24</parent_class_id>
    <child_class_id>22</child_class_id><name>Node To</name>
  </t_collection>
  <t_collection>
    <collection_id>400</collection_id><parent_class_id>1</parent_class_id>
    <child_class_id>24</child_class_id><name>Lines</name>
  </t_collection>
  <t_property>
    <property_id>1881</property_id><collection_id>400</collection_id>
    <name>Enforce Limits</name>
  </t_property>
  <t_membership>
    <membership_id>900</membership_id><collection_id>306</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>901</membership_id><collection_id>307</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>11</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>902</membership_id><collection_id>306</collection_id>
    <parent_object_id>21</parent_object_id><child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>903</membership_id><collection_id>307</collection_id>
    <parent_object_id>21</parent_object_id><child_object_id>11</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>950</membership_id><collection_id>400</collection_id>
    <parent_object_id>1</parent_object_id><child_object_id>20</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>951</membership_id><collection_id>400</collection_id>
    <parent_object_id>1</parent_object_id><child_object_id>21</child_object_id>
  </t_membership>
  <!-- CABLE: Enforce Limits = 0 ("Never enforce") → EL=0 soft-cap candidate. -->
  <t_data>
    <data_id>10001</data_id><membership_id>950</membership_id>
    <property_id>1881</property_id><value>0</value>
  </t_data>
  <!-- HARDLINE: Enforce Limits = 2 ("Always") → already a plain hard cap. -->
  <t_data>
    <data_id>10002</data_id><membership_id>951</membership_id>
    <property_id>1881</property_id><value>2</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_lines_no_lift_pins_hard_cap(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """``--no-lift-lines`` pins an EL=0 line to a plain HARD cap.

    The inverse of ``--lift-line-caps``: without the pin an EL=0
    ("Never enforce") line is soft-capped (``enforce_limits=1``,
    ``soft_cap=True`` → free over-rating band); listing it in
    ``GTOPT_NO_LIFT_LINES`` flips it to a plain hard cap
    (``enforce_limits=2``, ``soft_cap=False``) at the directional rating
    (forward ``Lin_MaxRating``, reverse ``|Lin_MinRating|``).

    The pin acts ONLY on EL=0 lines: a line already hard-capped in PLEXOS
    (EL=1/EL=2) named in the list is a no-op and keeps its enforce level.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_EL0_LINE_XML)
    _write_csv(
        tmp_path,
        "Lin_MaxRating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "CABLE,2026,1,1,1,1,90\n"
        "HARDLINE,2026,1,1,1,1,200\n",
    )
    _write_csv(
        tmp_path,
        "Lin_MinRating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "CABLE,2026,1,1,1,1,-90\n"
        "HARDLINE,2026,1,1,1,1,-200\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)

    monkeypatch.delenv("GTOPT_LIFT_LINE_CAPS", raising=False)
    monkeypatch.setenv("GTOPT_EL0_LINES", "extended")

    # No pin → EL=0 extended soft cap.
    monkeypatch.delenv("GTOPT_NO_LIFT_LINES", raising=False)
    soft = {ln.name: ln for ln in extract_lines(db, bundle)}["CABLE"]
    assert soft.enforce_limits == 1
    assert soft.soft_cap is True

    # Pin BOTH the EL=0 cable and the EL=2 line.
    monkeypatch.setenv("GTOPT_NO_LIFT_LINES", "CABLE,HARDLINE")
    pinned = {ln.name: ln for ln in extract_lines(db, bundle)}

    # EL=0 cable → flipped to a plain hard cap, no free band.
    hard = pinned["CABLE"]
    assert hard.enforce_limits == 2
    assert hard.soft_cap is False
    assert hard.soft_cap_lifted is False
    assert hard.tmax_ab == 90.0
    assert hard.tmin_ab == -90.0

    # EL=2 line → pin is a no-op; stays EL=2, never soft-capped.
    noop = pinned["HARDLINE"]
    assert noop.enforce_limits == 2
    assert noop.soft_cap is False


def test_extract_lines_el0_rating_suppresses_auto_lift(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The DEFAULT (auto) ``--lift-line-caps`` list does NOT lift an EL=0 line
    that ships a real Max Rating, but an EXPLICIT list does (precedence).

    An EL=0 line with a PLEXOS-defined rating (``Lin_MaxRating``) is the
    operator's stated intent, so the automatic strict-mode lift is suppressed
    and the line is hard-capped at its original rating.  A user-supplied
    ``--lift-line-caps`` value (differing from the shipped default) takes
    precedence and lifts it to the soft-cap band regardless.
    """
    from gtopt_shared.cli_flags import DEFAULT_LIFT_LINE_CAPS_PLEXOS

    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_EL0_LINE_XML)
    _write_csv(
        tmp_path,
        "Lin_MaxRating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\nCABLE,2026,1,1,1,1,90\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    monkeypatch.setenv("GTOPT_EL0_LINES", "strict")
    monkeypatch.delenv("GTOPT_NO_LIFT_LINES", raising=False)

    # Default (auto) lift list active → rated EL=0 line is hard-capped, NOT
    # lifted (automatic lift suppressed).
    monkeypatch.setenv("GTOPT_LIFT_LINE_CAPS", DEFAULT_LIFT_LINE_CAPS_PLEXOS)
    auto = {ln.name: ln for ln in extract_lines(db, bundle)}["CABLE"]
    assert auto.enforce_limits == 2
    assert auto.soft_cap is False
    assert auto.soft_cap_lifted is False
    assert auto.tmax_ab == 90.0  # original rating, not 6× inflated

    # Explicit list (differs from default) naming the line → lifted (precedence).
    monkeypatch.setenv("GTOPT_LIFT_LINE_CAPS", "CABLE")
    explicit = {ln.name: ln for ln in extract_lines(db, bundle)}["CABLE"]
    assert explicit.enforce_limits == 1
    assert explicit.soft_cap is True
    assert explicit.soft_cap_lifted is True


def test_warn_if_series_varies(caplog: pytest.LogCaptureFixture) -> None:
    """Structural guard: warns only on genuine variation among DEFINED periods.

    Inputs read with ``read_long(fill_forward=False)`` zero-pad undefined
    periods, so a sparse period-1-only series ``[v, 0, 0, …]`` must NOT warn;
    a real intra-horizon change among non-zero values MUST warn.
    """
    import logging

    from plexos2gtopt.parsers import _warn_if_series_varies

    # Sparse period-1 only (rest zero-padding) → no warning.
    with caplog.at_level(logging.WARNING):
        _warn_if_series_varies("heat rate", "G", [0.23, 0.0, 0.0, 0.0])
    assert "time-varying" not in caplog.text

    # Constant non-zero → no warning.
    caplog.clear()
    with caplog.at_level(logging.WARNING):
        _warn_if_series_varies("heat rate", "G", [0.23, 0.23, 0.23])
    assert "time-varying" not in caplog.text

    # Genuine variation among DEFINED (non-zero) periods → warning.
    caplog.clear()
    with caplog.at_level(logging.WARNING):
        _warn_if_series_varies("heat rate", "G", [0.23, 0.0, 0.30, 0.0])
    assert "time-varying" in caplog.text
    assert "G" in caplog.text


def test_extract_lines_el0_default_strict(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The DEFAULT EL=0 mode is ``strict`` — a plain hard cap.

    With no ``GTOPT_EL0_LINES`` / list env set, an EL=0 line is hard-capped
    (``enforce_limits=2``, ``soft_cap=False``) at its rating; only lines named
    in ``--lift-line-caps`` get the soft over-rating band.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_EL0_LINE_XML)
    _write_csv(
        tmp_path,
        "Lin_MaxRating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "CABLE,2026,1,1,1,1,90\n"
        "HARDLINE,2026,1,1,1,1,200\n",
    )
    _write_csv(
        tmp_path,
        "Lin_MinRating.csv",
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "CABLE,2026,1,1,1,1,-90\n"
        "HARDLINE,2026,1,1,1,1,-200\n",
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)

    # Clean slate: no mode, no lists → default strict.
    monkeypatch.delenv("GTOPT_EL0_LINES", raising=False)
    monkeypatch.delenv("GTOPT_LIFT_LINE_CAPS", raising=False)
    monkeypatch.delenv("GTOPT_NO_LIFT_LINES", raising=False)
    strict = {ln.name: ln for ln in extract_lines(db, bundle)}["CABLE"]
    assert strict.enforce_limits == 2  # EL=0 → hard cap by default
    assert strict.soft_cap is False

    # A line in the lift list IS lifted to a soft cap, overriding strict.
    monkeypatch.setenv("GTOPT_LIFT_LINE_CAPS", "CABLE")
    lifted = {ln.name: ln for ln in extract_lines(db, bundle)}["CABLE"]
    assert lifted.enforce_limits == 1
    assert lifted.soft_cap is True
    assert lifted.soft_cap_lifted is True


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
# T10: ReservoirSpec.water_value — PLEXOS scalar is silently dropped to 0
# (the per-reservoir boundary-cut slopes from Hydro_StoWaterValues.csv now
# price terminal storage uniformly; the legacy 1e+30 ``never_drain`` clamp
# is retired).
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


def test_extract_reservoirs_sentinel_water_value_silently_drops_to_zero(
    tmp_path: Path,
) -> None:
    """PLEXOS ``Water Value = 1e+30`` is the legacy "never drain"
    sentinel.  The boundary-cut FCF + per-reservoir water-value
    slopes (``Hydro_StoWaterValues.csv``) now price terminal storage
    correctly, so the extractor silently drops the sentinel to 0
    with NO warning and NO ``never_drain`` hard clamp.  The
    ``never_drain`` field on ``ReservoirSpec`` is retired but kept
    as a no-op for backward compatibility.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_RESERVOIR_WV_XML_TMPL.format(wv="1e30"))
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    out = extract_reservoirs(db, bundle)
    assert len(out) == 1
    res = out[0]
    # 1e+30 sentinel collapses to 0 (keep_sentinel defaults False on the
    # static_property reader); no warning emitted, no never_drain set.
    assert res.water_value == 0.0
    assert res.never_drain is False


def test_extract_reservoirs_finite_water_value_is_passthrough(tmp_path: Path) -> None:
    """A finite ``Water Value`` is forwarded verbatim with
    ``never_drain=False`` (the retired sentinel branch never fires).
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
    assert not _extract_config_mutex_groups(db)


# --------------------------------------------------------------------------- #
# Adaptive per-line loss-segment count (cube-root rule)
# --------------------------------------------------------------------------- #
def _reset_loss_env() -> None:
    """Clear every env var the adaptive helper consults so each test starts
    from a known state (defaults: err_pct=0.01, ceiling=6, extend=off,
    layout=midpoint)."""
    for k in (
        "GTOPT_LOSS_ERROR_PCT",
        "GTOPT_NSEG_LOSSES",
        "GTOPT_LOSS_EXTEND_OVERLOAD",
        # Tests at line 1890+ / 2012+ set ``GTOPT_LOSS_PWL_LAYOUT =
        # "dynamic"`` to exercise the dynamic-layout path.  Without
        # clearing it here the test_writer.py
        # ``test_build_line_array_dlr_emits_matrix_and_loss_mode``
        # asserts ``loss_pwl_layout == "midpoint"`` and fails when a
        # previous test on the same xdist worker leaked ``dynamic``.
        "GTOPT_LOSS_PWL_LAYOUT",
    ):
        _os.environ.pop(k, None)


@pytest.fixture(autouse=True)
def _scrub_loss_env_after_test():
    """Autouse fixture — clear the adaptive-loss env vars AFTER every test
    in this module so a test that leaves ``GTOPT_NSEG_LOSSES=10`` set
    can't leak into other test modules running on the same xdist worker
    (the failure mode was ``test_writer.py::
    test_build_line_array_dlr_emits_matrix_and_loss_mode`` reading
    ``loss_segments=10`` instead of the expected default of 4 when it
    happened to run AFTER ``test_adaptive_k_ceiling_from_env_var`` in
    the same worker process).
    """
    yield
    _reset_loss_env()


def _ls(name: str, R: float, tmax: float, **kw) -> LineSpec:
    """Compact LineSpec factory for adaptive-K test fixtures."""
    return LineSpec(
        object_id=hash(name) & 0xFFFF,
        name=name,
        bus_from="a",
        bus_to="b",
        resistance=R,
        tmax_ab=tmax,
        **kw,
    )


def test_adaptive_k_cube_root_allocates_more_to_bigger_lines() -> None:
    """Per the L^(1/3) KKT rule, a line carrying 100× the peak loss of
    another gets at most ⌈100^(1/3)⌉ ≈ 5× the segments — and both stay
    clamped to [floor=2, ceiling=6] by default."""
    _reset_loss_env()
    # Peak losses: L_big=R·fmax² = 0.01·2000² = 40,000
    #              L_mid                       = 0.05·400²  = 8,000
    #              L_sml                       = 0.10·40²   = 160
    out = _apply_adaptive_loss_segments(
        (
            _ls("big", 0.01, 2000.0),
            _ls("mid", 0.05, 400.0),
            _ls("sml", 0.10, 40.0),
        )
    )
    by_name = {ln.name: ln.loss_segments for ln in out}
    # Cube-root ordering must hold strictly: bigger L → at least as many K.
    assert by_name["big"] >= by_name["mid"] >= by_name["sml"]
    # Default ceiling caps the biggest at 6.
    assert by_name["big"] == 6
    # Floor (2) prevents degenerate single-secant on tiny lines.
    assert by_name["sml"] == 2


def test_adaptive_k_floor_clamps_tiny_lines() -> None:
    """The floor=2 hardcoded constant is non-negotiable: a tiny line
    paired with a much bigger one (so the cube-root rule would
    naturally allocate K<2 to it) gets bumped up to K=2.  Lossless
    lines (R=0 or tmax=0) are excluded entirely (loss_segments=0).

    Note: an isolated lossy line never hits the floor — with one line
    in the system, K = 1/(2√err_pct) ≈ 5 at the default 1 % budget,
    well above the floor.  The floor matters when other lines dominate
    the error budget."""
    _reset_loss_env()
    out = _apply_adaptive_loss_segments(
        (
            _ls("dominator", 0.05, 2000.0),  # L_max = 200,000 — dominates
            _ls("tiny", 0.10, 0.1),  # L_max = 0.001 — cube-root would give K<2
            _ls("zero_R", 0.0, 1000.0),
            _ls("zero_tmax", 0.10, 0.0),
        )
    )
    by_name = {ln.name: ln.loss_segments for ln in out}
    assert by_name["tiny"] == 2  # floor kicks in
    assert by_name["zero_R"] == 0  # lossless skipped
    assert by_name["zero_tmax"] == 0  # zero envelope skipped


def test_adaptive_k_ceiling_from_env_var() -> None:
    """``GTOPT_NSEG_LOSSES`` (``--nseg-losses``) overrides the default
    adaptive ceiling of 6.  With ceiling=10 AND a tight error budget
    (0.1 %) the cube-root rule wants K_big ≈ 1/(2·√0.001) ≈ 16, which
    is clamped to the new ceiling of 10."""
    _reset_loss_env()
    _os.environ["GTOPT_NSEG_LOSSES"] = "10"
    _os.environ["GTOPT_LOSS_ERROR_PCT"] = "0.001"  # 0.1 % — tight
    out = _apply_adaptive_loss_segments(
        (
            _ls("big", 0.05, 2000.0),  # massive peak loss
            _ls("sml", 0.001, 10.0),
        )
    )
    by_name = {ln.name: ln.loss_segments for ln in out}
    assert by_name["big"] == 10  # hits the new ceiling
    assert by_name["sml"] == 2  # floor


def test_adaptive_k_uniform_mode_when_error_pct_zero() -> None:
    """``--loss-error-pct 0`` disables adaptation: every lossy line gets
    the same K (the historical uniform behaviour).  Default uniform-K
    when env var unset is 4 (the pre-2026-05-29 default)."""
    _reset_loss_env()
    _os.environ["GTOPT_LOSS_ERROR_PCT"] = "0"
    out = _apply_adaptive_loss_segments(
        (
            _ls("big", 0.05, 2000.0),
            _ls("mid", 0.05, 200.0),
            _ls("sml", 0.05, 20.0),
            _ls("zero", 0.0, 1000.0),
        )
    )
    by_name = {ln.name: ln.loss_segments for ln in out}
    # All lossy lines collapse to the same K=4 default.
    assert by_name["big"] == 4
    assert by_name["mid"] == 4
    assert by_name["sml"] == 4
    # Lossless still skipped.
    assert by_name["zero"] == 0


def test_adaptive_k_uniform_mode_honours_nseg_losses() -> None:
    """When adaptation is disabled (``GTOPT_LOSS_ERROR_PCT=0``), the
    uniform K still respects ``GTOPT_NSEG_LOSSES`` if the user passed
    ``--nseg-losses`` explicitly.  Lets users reproduce the historic
    K=8 envfix sweep results."""
    _reset_loss_env()
    _os.environ["GTOPT_LOSS_ERROR_PCT"] = "0"
    _os.environ["GTOPT_NSEG_LOSSES"] = "8"
    out = _apply_adaptive_loss_segments((_ls("any", 0.05, 500.0),))
    assert out[0].loss_segments == 8


def test_adaptive_k_dlr_profile_used_for_envelope() -> None:
    """DLR (Dynamic Line Rating) lines ship a per-hour profile peak that
    exceeds the static tmax_ab (e.g. LoAguirre500->Polpaico500: 900
    overnight, 2078 daytime).  The cube-root rule must size K for the
    DLR peak — using the 900 floor would under-resolve the daytime band
    where losses scale ~5× higher."""
    _reset_loss_env()
    out = _apply_adaptive_loss_segments(
        (
            # DLR peak 2078 dominates the L_max,i calculation.
            _ls(
                "dlr",
                0.05,
                900.0,
                tmax_ab_profile=tuple([900.0] * 6 + [2078.0] * 12 + [900.0] * 6),
            ),
            # Same resistance, static tmax = DLR floor (900).
            _ls("static_low", 0.05, 900.0),
            # Same R, tmax at DLR peak — should match dlr's K.
            _ls("static_peak", 0.05, 2078.0),
        )
    )
    by_name = {ln.name: ln.loss_segments for ln in out}
    # The DLR-aware envelope makes 'dlr' equivalent to 'static_peak' for
    # K-sizing, both bigger than 'static_low'.
    assert by_name["dlr"] == by_name["static_peak"]
    assert by_name["dlr"] >= by_name["static_low"]


def test_adaptive_k_extend_overload_bumps_soft_cap_lines() -> None:
    """``--loss-extend-overload`` widens the PWL envelope for soft-cap
    lines by the writer's headroom factor (2× for regular soft_cap,
    4× for soft_cap_lifted), so the cube-root rule allocates more K to
    them.  EL=1/EL=2 (``soft_cap=False``) is unaffected — LP cannot
    flow past tmax there, no benefit from a wider envelope."""
    _reset_loss_env()
    # Same resistance, same tmax — only soft-cap status differs.
    lines = (
        _ls("hard_only", 0.05, 500.0),  # EL=2
        _ls("soft_reg", 0.05, 500.0, soft_cap=True),  # 2× envelope
        _ls("soft_lifted", 0.05, 500.0, soft_cap=True, soft_cap_lifted=True),  # 4×
    )
    # Without the flag: all three see the same K (same R, same tmax).
    off = {ln.name: ln.loss_segments for ln in _apply_adaptive_loss_segments(lines)}
    assert off["hard_only"] == off["soft_reg"] == off["soft_lifted"]
    # With the flag: soft-cap lines get the wider envelope, K shifts.
    _os.environ["GTOPT_LOSS_EXTEND_OVERLOAD"] = "1"
    on = {ln.name: ln.loss_segments for ln in _apply_adaptive_loss_segments(lines)}
    # Hard-only EL=2 is unaffected (LP can't exceed tmax anyway).
    # Soft-cap and lifted get the wider envelope: K ranks them higher.
    assert on["hard_only"] <= on["soft_reg"] <= on["soft_lifted"]
    # And at least one of soft_reg / soft_lifted moves up vs the flag-off
    # case (the wider envelope means more peak loss → bigger K).
    assert max(on.values()) >= max(off.values())


def test_adaptive_k_total_error_budget_bounded() -> None:
    """Sanity: the cube-root allocation respects the absolute MW error
    budget B = err_pct · Σ L_max,i.  Worst-case Σ_i L_max,i/(4 K_i²)
    after clamping must stay ≤ B (or at worst within a small margin
    when the ceiling kicks in)."""
    _reset_loss_env()
    _os.environ["GTOPT_LOSS_ERROR_PCT"] = "0.02"  # 2 % budget
    lines = (
        _ls("L1", 0.01, 2000.0),  # L_max = 40,000
        _ls("L2", 0.05, 500.0),  # L_max = 12,500
        _ls("L3", 0.10, 100.0),  # L_max = 1,000
        _ls("L4", 0.20, 30.0),  # L_max = 180
    )
    out = _apply_adaptive_loss_segments(lines)
    total_L = sum(ln.resistance * ln.tmax_ab**2 for ln in lines)
    realized_error = sum(
        (ln.resistance * ln.tmax_ab**2) / (4.0 * ln.loss_segments**2)
        for ln in out
        if ln.loss_segments > 0
    )
    budget = 0.02 * total_L
    # Allow 25% margin: the ceiling-clamp can keep total error above the
    # raw KKT value, but the rule still concentrates K where it matters
    # so the realized error stays bounded for any reasonable input.
    assert realized_error <= budget * 1.25, (
        f"realized error {realized_error:.1f} > budget {budget:.1f}"
    )


# --------------------------------------------------------------------------- #
# Adaptive K — full error/tolerance sweep (Python-side companion to the C++
# test_line_losses_decoupled_envelope.cpp K∈{1,2,4,8,16,128} sweep)
# --------------------------------------------------------------------------- #
# pytest is imported at the top of the file; the redundant mid-file import
# was removed.


def _realistic_line_mix() -> tuple[LineSpec, ...]:
    """Mock a CEN-PCP-shaped line mix: 4 tiers of (R, fmax) covering
    3 orders of magnitude in peak loss L_max,i = R·fmax².

    Per-line peak loss [MW]:
      * trunk_500kV  L =  900     (R=0.0001, fmax=3000)   ← biggest
      * backbone_220 L =  72      (R=0.0008, fmax=300)
      * regional_154 L =   5      (R=0.005,  fmax=100)
      * stub_66kV    L =   0.0250 (R=0.025,  fmax=10)    ← smallest

    Three orders of magnitude → exercises the cube-root rule across
    its full range.  Total Σ L_max = 977.025 MW.
    """
    return (
        _ls("trunk_500kV", 0.0001, 3000.0),
        _ls("backbone_220", 0.0008, 300.0),
        _ls("regional_154", 0.005, 100.0),
        _ls("stub_66kV", 0.025, 10.0),
    )


@pytest.mark.parametrize(
    # Empirical bounds: at err_pct ≥ 0.01 the raw KKT solution lands
    # inside [floor=2, ceiling=6] for every line, so realized stays at
    # or below budget.  At tighter err_pct the ceiling clamps the
    # heaviest line(s); realized exceeds budget by the squared ratio
    # (K_raw/ceiling)² for the clamped lines.  Tolerances measured
    # against the fixture line mix:
    #   err=0.001 → ratio≈7.0 (ceiling binds on trunk+backbone+regional)
    #   err=0.005 → ratio≈1.6 (ceiling binds on trunk only)
    #   err=0.010 → ratio≈1.0 (KKT lands inside clamps)
    #   err≥0.020 → ratio<1.0 (rule has slack to spare)
    "err_pct,tol_mult",
    [
        (0.001, 8.0),  # 0.1 % — ceiling binds hard; budget ⨯ 7 realized
        (0.005, 2.0),  # 0.5 % — ceiling binds on trunk only
        (0.010, 1.10),  # 1 % — default; KKT inside clamps
        (0.020, 1.00),  # 2 %  — slack available
        (0.050, 1.00),  # 5 % — loose; raw KKT clears budget easily
    ],
)
def test_adaptive_k_budget_holds_across_err_pct_sweep(
    err_pct: float, tol_mult: float
) -> None:
    """For every err_pct in the typical range (0.1 % to 5 %), the
    cube-root rule allocates per-line K such that the worst-case total
    PWL error ``Σ L_max,i / (4 K_i²)`` stays within ``tol_mult × budget``.

    The ``tol_mult`` slack accounts for:
      * Ceiling=6 clamp on lines where raw KKT wants K>6 — at the
        tightest err_pct, the ceiling caps achievable accuracy.
      * Integer rounding (K is an int, raw is float).

    Companion to C++ test_line_losses_decoupled_envelope.cpp which pins
    the per-K, per-mode LP-reported loss against analytic R·f²; this
    test pins the SYSTEM-level allocation rule that picks K per line."""
    _reset_loss_env()
    _os.environ["GTOPT_LOSS_ERROR_PCT"] = str(err_pct)
    lines = _realistic_line_mix()
    out = _apply_adaptive_loss_segments(lines)
    # Sum of per-line worst-case errors at the realized K_i.
    realized_error = sum(
        (ln.resistance * ln.tmax_ab**2) / (4.0 * ln.loss_segments**2)
        for ln in out
        if ln.loss_segments > 0
    )
    total_L = sum(ln.resistance * ln.tmax_ab**2 for ln in lines)
    budget = err_pct * total_L
    assert realized_error <= budget * tol_mult, (
        f"err_pct={err_pct}: realized {realized_error:.4f} MW > "
        f"budget {budget:.4f} × {tol_mult} tolerance"
    )


def test_adaptive_k_total_segments_decreases_with_looser_budget() -> None:
    """The cube-root rule's main payoff: looser err_pct → fewer total
    LP segments.  Σ K should drop monotonically as err_pct grows."""
    _reset_loss_env()
    lines = _realistic_line_mix()
    sums: list[tuple[float, int]] = []
    for err_pct in (0.001, 0.005, 0.010, 0.020, 0.050):
        _os.environ["GTOPT_LOSS_ERROR_PCT"] = str(err_pct)
        out = _apply_adaptive_loss_segments(lines)
        sums.append((err_pct, sum(ln.loss_segments for ln in out)))
    # Strictly non-increasing — looser budget never costs more segments
    # (allowing equality on the floor/ceiling clamps).
    for (eps_a, k_a), (eps_b, k_b) in zip(sums, sums[1:]):
        assert k_b <= k_a, (
            f"Σ K went UP from {k_a} (err={eps_a}) to {k_b} (err={eps_b})"
        )


def test_adaptive_k_matches_kkt_prediction_for_unbounded_case() -> None:
    """When no clamps fire (floor and ceiling don't bind), the raw KKT
    solution K_i = c · L_i^(1/3) must hold EXACTLY (up to int ceil).

    Setup: pick a 3-line system where the cube-root rule lands every K
    inside [3, 5] so neither floor=2 nor ceiling=6 clamps."""
    _reset_loss_env()
    _os.environ["GTOPT_LOSS_ERROR_PCT"] = "0.04"  # 4 % budget — loose
    # L = (R·fmax²)
    lines = (
        _ls("a", 0.01, 500.0),  # L = 2500
        _ls("b", 0.01, 250.0),  # L = 625
        _ls("c", 0.01, 125.0),  # L = 156.25
    )
    out = _apply_adaptive_loss_segments(lines)
    by_name = {ln.name: ln.loss_segments for ln in out}
    # Compute the raw KKT prediction directly.
    Ls = [ln.resistance * ln.tmax_ab**2 for ln in lines]
    total_L = sum(Ls)
    S = sum(L ** (1.0 / 3.0) for L in Ls)
    B = 0.04 * total_L
    import math as _m

    c = _m.sqrt(S / (4.0 * B))
    expected = {
        ln.name: max(2, min(6, _m.ceil(c * (ln.resistance * ln.tmax_ab**2) ** (1 / 3))))
        for ln in lines
    }
    assert by_name == expected, f"KKT mismatch: got {by_name}, expected {expected}"
    # And the cube-root ordering must hold: bigger L → bigger K.
    assert by_name["a"] >= by_name["b"] >= by_name["c"]


def test_adaptive_k_compares_to_uniform_at_equivalent_total_K() -> None:
    """Sanity: at the same total segment count Σ K, the cube-root
    allocation produces a SMALLER worst-case total error than uniform.

    This is the rule's whole pitch — better K placement at fixed LP
    cost.  Compute the realized error for adaptive (default 1 %) and
    compare to uniform K = ⌈Σ K / N⌉ across the same lines."""
    _reset_loss_env()
    lines = _realistic_line_mix()
    out_adaptive = _apply_adaptive_loss_segments(lines)
    sum_k_adaptive = sum(ln.loss_segments for ln in out_adaptive)
    err_adaptive = sum(
        (ln.resistance * ln.tmax_ab**2) / (4.0 * ln.loss_segments**2)
        for ln in out_adaptive
    )
    # Now compare against uniform-K at the same total segment count.
    n_lossy = sum(1 for ln in lines if ln.resistance > 0 and ln.tmax_ab > 0)
    k_uniform = max(2, round(sum_k_adaptive / n_lossy))
    err_uniform = sum(
        (ln.resistance * ln.tmax_ab**2) / (4.0 * k_uniform**2)
        for ln in lines
        if ln.resistance > 0 and ln.tmax_ab > 0
    )
    # Adaptive must beat uniform at the same total LP cost — the L^(1/3)
    # allocation is the KKT-optimum, uniform is sub-optimal.
    assert err_adaptive < err_uniform, (
        f"Adaptive error {err_adaptive:.3f} not better than uniform "
        f"({err_uniform:.3f}) at Σ K={sum_k_adaptive}"
    )


# --------------------------------------------------------------------------- #
# Dynamic loss layout — per-line midpoint/uniform selection under err_pct
# (companion to adaptive K tests; same fixtures, same error-budget concept)
# --------------------------------------------------------------------------- #


def _dynamic_run(R: list[float], fmax: list[float], err_pct: float):
    """Convenience: run adaptive K + dynamic layout in one shot and return
    a list of (name, K, layout) tuples for the lossy lines."""
    _reset_loss_env()
    _os.environ["GTOPT_LOSS_ERROR_PCT"] = str(err_pct)
    _os.environ["GTOPT_LOSS_PWL_LAYOUT"] = "dynamic"
    lines = tuple(_ls(f"L{i}", r, f) for i, (r, f) in enumerate(zip(R, fmax)))
    out = _apply_adaptive_loss_segments(lines)
    return [
        (ln.name, ln.loss_segments, ln.loss_pwl_layout)
        for ln in out
        if ln.loss_segments > 0
    ]


def test_dynamic_all_uniform_when_budget_loose() -> None:
    """When err_pct is large enough that uniform alone meets the mean
    budget, every line stays uniform (cheapest LP — presolve eliminates
    the loss column)."""
    R = [0.01, 0.05, 0.10]
    fmax = [200.0, 100.0, 50.0]
    rows = _dynamic_run(R, fmax, err_pct=0.10)  # very loose
    assert all(layout == "uniform" for _, _, layout in rows), (
        f"Loose budget should keep all uniform, got: {rows}"
    )


def test_dynamic_promotes_heaviest_when_budget_tight() -> None:
    """When err_pct is small enough that uniform overshoots the mean
    budget, at least one heavy L_max contributor flips to midpoint.
    The promoted line must be the heaviest by mean-error contribution."""
    R = [0.001, 0.005, 0.01, 0.05, 0.10]
    fmax = [3000.0, 500.0, 100.0, 50.0, 10.0]
    rows = _dynamic_run(R, fmax, err_pct=0.001)
    midpoint_count = sum(1 for _, _, lay in rows if lay == "midpoint")
    assert midpoint_count >= 1, (
        f"Tight budget should force at least one midpoint promotion: {rows}"
    )
    # Heaviest line (L0 has L_max = 9000, ~7× the next) MUST be among the
    # promoted ones.
    layout_by_name = {n: lay for n, _, lay in rows}
    assert layout_by_name["L0"] == "midpoint", (
        f"Heaviest line L0 should be promoted first, got: {rows}"
    )


def _signed_mean(R, fmax, decisions):
    """Helper: compute Σ_uniform L/(6K²) − Σ_midpoint L/(12K²)."""
    total = 0.0
    for (r, f), (_, k, layout) in zip(zip(R, fmax), decisions):
        if r <= 0 or f <= 0 or k == 0:
            continue
        L = r * f * f
        if layout == "midpoint":
            total -= L / (12.0 * k * k)
        else:
            total += L / (6.0 * k * k)
    return total


def test_dynamic_reaches_local_optimum() -> None:
    """The greedy invariant: after the rule decides, no remaining
    uniform→midpoint single flip improves abs(running).

    The algorithm cannot always achieve abs(running) ≤ budget when a
    single heavy line's L/(4K²) contribution dominates the budget — in
    that regime the greedy is locked at its local minimum.  But it MUST
    have explored every improving flip; this invariant pins exactly
    that local-optimality guarantee."""
    R = [0.001, 0.005, 0.01, 0.05, 0.1]
    fmax = [3000.0, 500.0, 100.0, 50.0, 10.0]
    rows = _dynamic_run(R, fmax, err_pct=0.001)
    final = _signed_mean(R, fmax, rows)

    # For each line still uniform, flipping it should NOT reduce abs.
    for j, (_, k, layout) in enumerate(rows):
        if layout != "uniform":
            continue
        L = R[j] * fmax[j] * fmax[j]
        if L <= 0 or k == 0:
            continue
        # Flip would change running by -L/(4 k²).
        delta = L / (4.0 * k * k)
        after_flip = final - delta
        assert abs(after_flip) >= abs(final) - 1e-9, (
            f"Greedy missed an improving flip on line L{j}: "
            f"final={final:.4f}, after_flip={after_flip:.4f}"
        )


def test_dynamic_meets_budget_when_achievable() -> None:
    """Happy path: when no single line's L/(4K²) dominates the budget,
    the greedy DOES land abs(running) ≤ budget (within ~5 %% slack for
    the discrete last flip)."""
    R = [0.01] * 10
    fmax = [100.0] * 10  # uniform-size lines, none individually decisive
    err_pct = 0.02
    rows = _dynamic_run(R, fmax, err_pct=err_pct)
    L_total = sum(r * f * f for r, f in zip(R, fmax))
    budget = err_pct * L_total
    realised = _signed_mean(R, fmax, rows)
    assert abs(realised) <= budget * 1.05, (
        f"Achievable budget should be met within 5 %%: "
        f"realised={realised:.3f}, budget={budget:.3f}"
    )


def test_dynamic_falls_through_to_uniform_when_no_promotion_needed() -> None:
    """When all lines have very small L_max·K^(-2), uniform mean error is
    already tiny and well below budget → no promotions, all uniform."""
    R = [0.0001, 0.0001, 0.0001]
    fmax = [100.0, 100.0, 100.0]  # tiny L=1 per line
    rows = _dynamic_run(R, fmax, err_pct=0.10)
    assert all(layout == "uniform" for _, _, layout in rows), (
        f"Tiny lines should stay uniform regardless: {rows}"
    )


def test_dynamic_respects_err_pct_zero_uniform_fallback() -> None:
    """When err_pct ≤ 0 (uniform K mode), dynamic should still behave
    sanely.  The adaptive rule returns ceiling K with no layout
    annotation; dynamic doesn't trigger because there's no mean-error
    overshoot to flip.  Verify no layout field is stamped."""
    R = [0.01, 0.05]
    fmax = [200.0, 100.0]
    _reset_loss_env()
    _os.environ["GTOPT_LOSS_ERROR_PCT"] = "0"  # disable adaptive
    _os.environ["GTOPT_LOSS_PWL_LAYOUT"] = "dynamic"
    lines = tuple(_ls(f"L{i}", r, f) for i, (r, f) in enumerate(zip(R, fmax)))
    out = _apply_adaptive_loss_segments(lines)
    for ln in out:
        # Uniform-K fallback: layout field should be empty (writer
        # picks the base "dynamic" → falls back to uniform per
        # _resolve_loss_layout's safety net).
        assert ln.loss_pwl_layout == "", (
            f"Dynamic should not annotate uniform-K-fallback lines: {ln}"
        )


# --------------------------------------------------------------------------- #
# Issue #504 task #5 — SOS2 fill-order selector (loss_use_sos2 post-pass)
# --------------------------------------------------------------------------- #


def _reset_sos2_env() -> None:
    """Clear the two SOS2-policy env vars so each test starts fresh."""
    for k in ("GTOPT_LOSS_SOS2_LINES", "GTOPT_LOSS_SOS2_AUTO"):
        _os.environ.pop(k, None)


@pytest.fixture(autouse=True)
def _scrub_sos2_env_after_test():
    """Autouse fixture — clear the SOS2 env vars AFTER every test in
    this module so a test that leaves them set can't leak into siblings
    (mirrors ``_scrub_loss_env_after_test`` for the adaptive-K helper).
    """
    yield
    _reset_sos2_env()


def _ls_k(name: str, R: float, tmax: float, K: int = 4, **kw) -> LineSpec:
    """LineSpec factory that pre-stamps ``loss_segments`` (K).

    The SOS2 post-pass runs AFTER the adaptive-K pass, so production
    inputs always carry a non-zero K on lossy lines.  Tests mirror this
    by stamping K=4 by default; lossless lines pass K=0 to mimic the
    real flow.
    """
    return LineSpec(
        object_id=hash(name) & 0xFFFF,
        name=name,
        bus_from="a",
        bus_to="b",
        resistance=R,
        tmax_ab=tmax,
        loss_segments=K,
        **kw,
    )


def test_sos2_policy_off_by_default_no_stamping() -> None:
    """No env vars set ⇒ policy is off ⇒ every LineSpec returned
    unchanged.  This is the regression guard: existing bundle outputs
    must not flip on by accident."""
    _reset_sos2_env()
    lines = (
        _ls_k("heavy", 0.05, 2000.0, K=6),
        _ls_k("light", 0.01, 100.0, K=2),
        _ls_k("zero_R", 0.0, 1000.0, K=0),
    )
    out = _apply_loss_sos2_policy(lines)
    # Identity (same tuple object short-circuits on the empty fast path).
    assert out is lines
    for ln in out:
        assert ln.loss_secant_segments == 0
        assert ln.loss_use_sos2 is False


def test_sos2_policy_explicit_lines_honoured() -> None:
    """``GTOPT_LOSS_SOS2_LINES`` lists names by comma; flagged lines get
    ``loss_secant_segments = K`` + ``loss_use_sos2 = True``.  Unnamed
    lines are untouched.  Lossless (R=0) lines are skipped even when
    named — the L-secant chord is vacuous there."""
    _reset_sos2_env()
    _os.environ["GTOPT_LOSS_SOS2_LINES"] = "heavy, also_heavy ,zero_R"
    lines = (
        _ls_k("heavy", 0.05, 2000.0, K=6),
        _ls_k("also_heavy", 0.05, 1000.0, K=5),
        _ls_k("not_picked", 0.01, 500.0, K=3),
        _ls_k("zero_R", 0.0, 1000.0, K=0),
    )
    out = _apply_loss_sos2_policy(lines)
    by_name = {ln.name: ln for ln in out}
    assert by_name["heavy"].loss_use_sos2 is True
    assert by_name["heavy"].loss_secant_segments == 6
    assert by_name["also_heavy"].loss_use_sos2 is True
    assert by_name["also_heavy"].loss_secant_segments == 5
    assert by_name["not_picked"].loss_use_sos2 is False
    assert by_name["not_picked"].loss_secant_segments == 0
    # Lossless lines: never stamped even when explicitly named.
    assert by_name["zero_R"].loss_use_sos2 is False
    assert by_name["zero_R"].loss_secant_segments == 0


def test_sos2_policy_auto_all_lossy_stamps_every_lossy_line() -> None:
    """``GTOPT_LOSS_SOS2_AUTO=all-lossy`` ⇒ every line with R>0 and
    non-zero peak loss gets stamped.  Lossless lines stay untouched."""
    _reset_sos2_env()
    _os.environ["GTOPT_LOSS_SOS2_AUTO"] = "all-lossy"
    lines = (
        _ls_k("a", 0.01, 500.0, K=3),
        _ls_k("b", 0.05, 2000.0, K=6),
        _ls_k("zero_R", 0.0, 1000.0, K=0),
        _ls_k("zero_tmax", 0.10, 0.0, K=0),
    )
    out = _apply_loss_sos2_policy(lines)
    by_name = {ln.name: ln for ln in out}
    assert by_name["a"].loss_use_sos2 is True
    assert by_name["b"].loss_use_sos2 is True
    assert by_name["zero_R"].loss_use_sos2 is False
    assert by_name["zero_tmax"].loss_use_sos2 is False


def test_sos2_policy_auto_heavy_picks_top_quartile() -> None:
    """``GTOPT_LOSS_SOS2_AUTO=heavy`` picks the top quartile by peak
    loss ``R·envelope²``.  With 8 lossy lines, the top 2 (≥ 75th
    percentile) should be flagged.  Boundary case: ties at the
    threshold are inclusive (selecting more is safer than fewer)."""
    _reset_sos2_env()
    _os.environ["GTOPT_LOSS_SOS2_AUTO"] = "heavy"
    # Peak losses: R=0.01 × tmax² → L_i = 0.01 * (100·i)² for i=1..8.
    # Sorted L: 100, 400, 900, 1600, 2500, 3600, 4900, 6400.
    # 75th percentile (index 3·8//4 = 6) = sorted_L[6] = 4900.  So
    # lines with L >= 4900 (i.e. L7=4900 and L8=6400) are flagged.
    lines = tuple(_ls_k(f"L{i}", 0.01, 100.0 * i, K=3) for i in range(1, 9))
    out = _apply_loss_sos2_policy(lines)
    by_name = {ln.name: ln for ln in out}
    flagged = {n for n, ln in by_name.items() if ln.loss_use_sos2}
    assert flagged == {"L7", "L8"}, f"Expected top-quartile picks; got {flagged}"


def test_sos2_policy_explicit_and_auto_compose_union() -> None:
    """Manual ``--loss-sos2-lines`` + auto ``--loss-sos2-auto`` compose
    as a UNION — both sources can contribute.  An explicit pick that
    the auto-rule would have missed still gets stamped."""
    _reset_sos2_env()
    _os.environ["GTOPT_LOSS_SOS2_AUTO"] = "heavy"
    _os.environ["GTOPT_LOSS_SOS2_LINES"] = "L2"  # below-quartile pick
    lines = tuple(_ls_k(f"L{i}", 0.01, 100.0 * i, K=3) for i in range(1, 9))
    out = _apply_loss_sos2_policy(lines)
    by_name = {ln.name: ln for ln in out}
    flagged = {n for n, ln in by_name.items() if ln.loss_use_sos2}
    # Auto picks L7, L8 (top quartile); explicit adds L2.  Union = {L2, L7, L8}.
    assert flagged == {"L2", "L7", "L8"}, f"Union expected; got {flagged}"


def test_sos2_policy_skips_lines_with_k_le_1() -> None:
    """A line whose adaptive-K pass left ``loss_segments <= 1`` (degenerate
    single secant) must NOT be stamped — an L=1 SOS2 declaration is
    vacuous and ``make_config`` would drop it anyway.  The post-pass
    saves the round-trip by filtering up front."""
    _reset_sos2_env()
    _os.environ["GTOPT_LOSS_SOS2_LINES"] = "vacuous"
    lines = (
        _ls_k("vacuous", 0.01, 100.0, K=1),
        _ls_k("ok", 0.01, 100.0, K=4),
    )
    _os.environ["GTOPT_LOSS_SOS2_LINES"] = "vacuous,ok"
    out = _apply_loss_sos2_policy(lines)
    by_name = {ln.name: ln for ln in out}
    assert by_name["vacuous"].loss_use_sos2 is False
    assert by_name["vacuous"].loss_secant_segments == 0
    assert by_name["ok"].loss_use_sos2 is True
    assert by_name["ok"].loss_secant_segments == 4


def test_sos2_policy_auto_off_string_treated_as_off() -> None:
    """The auto-rule's ``off`` value (and any empty / unknown value)
    must be a true no-op even when paired with an empty manual list."""
    _reset_sos2_env()
    _os.environ["GTOPT_LOSS_SOS2_AUTO"] = "off"
    lines = (_ls_k("a", 0.05, 1000.0, K=5),)
    out = _apply_loss_sos2_policy(lines)
    assert out is lines  # identity → no-op fast path
    assert out[0].loss_use_sos2 is False


def test_sos2_policy_auto_empty_string_normalised_to_off() -> None:
    """Explicit ``GTOPT_LOSS_SOS2_AUTO=""`` is normalised to ``off`` by
    the ``if auto_mode == "":`` branch.  Without a manual list this
    must short-circuit through the no-op fast path."""
    _reset_sos2_env()
    _os.environ["GTOPT_LOSS_SOS2_AUTO"] = ""
    lines = (_ls_k("a", 0.05, 1000.0, K=5),)
    out = _apply_loss_sos2_policy(lines)
    assert out is lines  # identity → no-op fast path
    assert out[0].loss_use_sos2 is False


def test_sos2_policy_extend_overload_soft_cap_lifted_4x_envelope() -> None:
    """With ``GTOPT_LOSS_EXTEND_OVERLOAD=1`` the ``_peak_loss`` helper
    inflates the envelope of ``soft_cap_lifted`` lines by 4× (mirrors
    ``_apply_adaptive_loss_segments``).  Exercise the branch and pin
    the peak-loss ordering it produces under the ``heavy`` auto-rule:
    a lifted-cap line beats a regular line of the same nominal rating."""
    _reset_sos2_env()
    _os.environ["GTOPT_LOSS_SOS2_AUTO"] = "heavy"
    _os.environ["GTOPT_LOSS_EXTEND_OVERLOAD"] = "1"
    # Both lines: R = 0.01, tmax = 100 → nominal peak loss = 100.
    # Lifted line gets envelope × 4 → peak loss × 16 = 1600.
    # With 2 lines and top-quartile (index 3·2//4 = 1) → only the
    # max-L line is flagged: the lifted one.
    lines = (
        _ls_k("regular", 0.01, 100.0, K=4),
        _ls_k("lifted", 0.01, 100.0, K=4, soft_cap_lifted=True),
    )
    out = _apply_loss_sos2_policy(lines)
    by_name = {ln.name: ln for ln in out}
    assert by_name["lifted"].loss_use_sos2 is True
    assert by_name["regular"].loss_use_sos2 is False
    # Clean up the extra env var we set (autouse fixture covers the SOS2 ones).
    _os.environ.pop("GTOPT_LOSS_EXTEND_OVERLOAD", None)


def test_sos2_policy_extend_overload_soft_cap_2x_envelope() -> None:
    """Mirror of the previous test for the ``soft_cap`` (× 2 envelope)
    branch.  Without the ``soft_cap_lifted`` flag the line gets the
    regular soft-cap multiplier."""
    _reset_sos2_env()
    _os.environ["GTOPT_LOSS_SOS2_AUTO"] = "heavy"
    _os.environ["GTOPT_LOSS_EXTEND_OVERLOAD"] = "1"
    # Lines: regular (nominal peak 100), soft_cap (4×), soft_cap_lifted (16×).
    # Top quartile of 3 = index 3·3//4 = 2 → only the lifted line.
    # To exercise the 2× branch as a "picked" line, keep just two: one
    # regular and one soft_cap.  Soft-cap wins.
    lines = (
        _ls_k("regular", 0.01, 100.0, K=4),
        _ls_k("soft", 0.01, 100.0, K=4, soft_cap=True),
    )
    out = _apply_loss_sos2_policy(lines)
    by_name = {ln.name: ln for ln in out}
    assert by_name["soft"].loss_use_sos2 is True
    assert by_name["regular"].loss_use_sos2 is False
    _os.environ.pop("GTOPT_LOSS_EXTEND_OVERLOAD", None)


def test_sos2_policy_empty_stamp_set_returns_input() -> None:
    """When the auto-rule is enabled but every line is lossless, the
    stamp set is empty and the function returns the input unchanged.
    Covers the `if not stamp_idx: return lines` branch after the auto
    pass (vs the upfront no-op path)."""
    _reset_sos2_env()
    _os.environ["GTOPT_LOSS_SOS2_AUTO"] = "all-lossy"
    lines = (
        _ls_k("zero_R_1", 0.0, 1000.0, K=0),
        _ls_k("zero_R_2", 0.0, 500.0, K=0),
    )
    out = _apply_loss_sos2_policy(lines)
    # All lines are lossless → no stamps → input returned verbatim.
    assert out is lines
    for ln in out:
        assert ln.loss_use_sos2 is False
        assert ln.loss_secant_segments == 0
