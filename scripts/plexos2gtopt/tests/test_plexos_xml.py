"""Unit tests for :mod:`plexos2gtopt.plexos_xml`.

The full ``DBSEN_PRGDIARIO.xml`` is too large (36 MB) to vendor with
the test suite. These tests instead build a minimal MasterDataSet XML
in a tmp file and exercise the reader against it; the bigger bundle
is covered by the smoke + integration suites once they materialise.
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.plexos_csv import read_long, read_wide
from plexos2gtopt.plexos_xml import NS, load_xml


_MINI_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class>
    <class_id>1</class_id>
    <name>System</name>
  </t_class>
  <t_class>
    <class_id>2</class_id>
    <name>Generator</name>
  </t_class>
  <t_class>
    <class_id>22</class_id>
    <name>Node</name>
  </t_class>
  <t_object>
    <object_id>100</object_id>
    <class_id>22</class_id>
    <name>bus_north</name>
  </t_object>
  <t_object>
    <object_id>101</object_id>
    <class_id>22</class_id>
    <name>bus_south</name>
  </t_object>
  <t_object>
    <object_id>200</object_id>
    <class_id>2</class_id>
    <name>thermal_a</name>
  </t_object>
  <t_collection>
    <collection_id>50</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>22</child_class_id>
    <name>Nodes</name>
  </t_collection>
  <t_membership>
    <membership_id>900</membership_id>
    <collection_id>50</collection_id>
    <parent_object_id>200</parent_object_id>
    <child_object_id>100</child_object_id>
  </t_membership>
  <t_property>
    <property_id>10</property_id>
    <collection_id>1</collection_id>
    <name>Max Capacity</name>
  </t_property>
</MasterDataSet>
"""


def test_load_xml_classes(tmp_path: Path) -> None:
    """The class index is keyed by both id and name."""
    path = tmp_path / "mini.xml"
    path.write_text(_MINI_XML)
    db = load_xml(path)
    assert db.class_id("Generator") == 2
    assert db.class_id("Node") == 22
    assert db.class_id("Unknown") is None
    assert db.classes_by_id[2] == "Generator"


def test_load_xml_objects(tmp_path: Path) -> None:
    """Objects-of-class returns the right subset."""
    path = tmp_path / "mini.xml"
    path.write_text(_MINI_XML)
    db = load_xml(path)
    nodes = db.objects_of_class("Node")
    assert {n.name for n in nodes} == {"bus_north", "bus_south"}
    gens = db.objects_of_class("Generator")
    assert [g.name for g in gens] == ["thermal_a"]


def test_collection_lookup(tmp_path: Path) -> None:
    """Find a collection by (parent_class, child_class)."""
    path = tmp_path / "mini.xml"
    path.write_text(_MINI_XML)
    db = load_xml(path)
    col = db.collection_for("Generator", "Node")
    assert col is not None
    assert col.collection_id == 50
    edges = db.memberships_of(col.collection_id)
    assert len(edges) == 1
    assert edges[0].parent_object_id == 200
    assert edges[0].child_object_id == 100


def test_read_long_csv(tmp_path: Path) -> None:
    """Long-format reader projects per-name per-period."""
    csv_path = tmp_path / "Gen_Rating.csv"
    csv_path.write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "GEN_A,2026,1,1,1,1,100\n"
        "GEN_A,2026,1,1,2,1,90\n"
        "GEN_B,2026,1,1,1,1,50\n"
    )
    out = read_long(csv_path)
    assert set(out.keys()) == {"GEN_A", "GEN_B"}
    assert out["GEN_A"][0] == 100.0
    assert out["GEN_A"][1] == 90.0
    assert out["GEN_B"][0] == 50.0
    # Unset periods remain zero-padded to the default horizon length.
    assert out["GEN_A"][5] == 0.0
    assert len(out["GEN_A"]) == 24


def test_read_long_filters_band(tmp_path: Path) -> None:
    """Only the requested BAND segment makes it through."""
    csv_path = tmp_path / "Gen_HeatRate.csv"
    csv_path.write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n"
        "GEN_A,2026,1,1,1,1,9000\n"
        "GEN_A,2026,1,1,1,2,11000\n"
    )
    band1 = read_long(csv_path, band=1)
    assert band1["GEN_A"][0] == 9000.0
    band2 = read_long(csv_path, band=2)
    assert band2["GEN_A"][0] == 11000.0


def test_read_wide_csv(tmp_path: Path) -> None:
    """Wide-format reader returns one list per data column."""
    csv_path = tmp_path / "Nod_Load.csv"
    csv_path.write_text(
        "YEAR,MONTH,DAY,PERIOD,bus_a,bus_b,zero_bus\n"
        "2026,1,1,1,10,20,0\n"
        "2026,1,1,2,11,21,0\n"
    )
    out = read_wide(csv_path)
    # zero-only column is dropped.
    assert set(out.keys()) == {"bus_a", "bus_b"}
    assert out["bus_a"][0] == 10.0
    assert out["bus_a"][1] == 11.0
    assert out["bus_b"][0] == 20.0
    assert len(out["bus_a"]) == 24


def test_read_wide_with_band(tmp_path: Path) -> None:
    """Wide reader honours BAND meta-column and ``drop_zero_cols=False``."""
    csv_path = tmp_path / "Lin_Units.csv"
    # Two BANDs: band 1 has units online for line_a / line_b; band 2 is
    # an alternate schedule the writer shouldn't pick up by default.
    csv_path.write_text(
        "YEAR,MONTH,DAY,PERIOD,BAND,line_a,line_b,line_off\n"
        "2026,1,1,1,1,1,1,0\n"
        "2026,1,1,2,1,1,1,0\n"
        "2026,1,1,1,2,9,9,9\n"
    )
    out = read_wide(csv_path, drop_zero_cols=False)
    # All columns present (drop_zero_cols=False keeps line_off as 0).
    assert set(out.keys()) == {"line_a", "line_b", "line_off"}
    assert out["line_a"][0] == 1.0
    assert out["line_off"][0] == 0.0
    # Band-2 row should not have leaked into the band-1 output.
    assert all(v in (0.0, 1.0) for v in out["line_a"])
    # Now explicit band=2 should expose the alternate values.
    band2 = read_wide(csv_path, band=2, drop_zero_cols=False)
    assert band2["line_off"][0] == 9.0


def test_collection_for_named(tmp_path: Path) -> None:
    """`collection_for_named` disambiguates by collection name."""
    xml = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>24</class_id><name>Line</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
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
</MasterDataSet>
"""
    path = tmp_path / "two_node_colls.xml"
    path.write_text(xml)
    db = load_xml(path)
    from_coll = db.collection_for_named("Line", "Node", "Node From")
    to_coll = db.collection_for_named("Line", "Node", "Node To")
    assert from_coll is not None and from_coll.collection_id == 306
    assert to_coll is not None and to_coll.collection_id == 307
    # Unknown collection name returns None.
    assert db.collection_for_named("Line", "Node", "Bogus") is None
