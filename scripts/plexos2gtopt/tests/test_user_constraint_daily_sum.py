"""Unit tests for the PLEXOS ``RHS Day`` daily-ENERGY UserConstraint wiring.

These pin the converter side of gtopt's ``daily_sum`` feature
(``RALCOramp_max_e1/e2``, ``CANUTILLARreserve``):

* A Constraint that ships a ``RHS Day`` (GWh) value AND a
  ``generator(...).generation`` LHS becomes a ``daily_sum`` +
  ``constraint_type="energy"`` spec with the RHS scaled GWh→MWh (×1000).
* A Constraint that ships a ``RHS Day`` value but a NON-generation LHS
  (here a commitment-startup count) is DEFERRED — not marked ``daily_sum``
  (different units / semantics).

The synthetic XML carries one System object, two Generators on the same
bus and three Constraints:

* ``DAILY_ENERGY`` — Sense=-1, ``RHS Day``=4.12, Generation Coefficient 1
  on each generator → ``Σ_day (G1+G2)·Δt ≤ 4120`` MWh (CANUTILLAR shape).
* ``RHS_DAY_COUNT`` — Sense=-1, ``RHS Day``=2, ``Units Started`` Coefficient
  1 on G1 → a commitment-count daily cap → deferred (not ``daily_sum``).
* ``PLAIN_LE`` — Sense=-1, plain ``RHS``=200, Generation Coefficient 1 on
  G1 → a normal (non-daily) UC → not ``daily_sum``.
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import UserConstraintSpec
from plexos2gtopt.gtopt_writer import (
    build_user_constraint_array,
    write_user_constraint_pampl,
)
from plexos2gtopt.parsers import extract_user_constraints
from plexos2gtopt.plexos_loader import PlexosBundle
from plexos2gtopt.plexos_xml import NS, load_xml


_DAILY_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>22</class_id><name>b_a</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object><object_id>21</object_id><class_id>2</class_id><name>G2</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id><name>DAILY_ENERGY</name></t_object>
  <t_object><object_id>101</object_id><class_id>70</class_id><name>RHS_DAY_COUNT</name></t_object>
  <t_object><object_id>102</object_id><class_id>70</class_id><name>PLAIN_LE</name></t_object>

  <!-- System → Constraints (RHS / RHS Day / Sense live here) -->
  <t_collection>
    <collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <!-- Generator → Constraints (Generation / Units Started Coefficient) -->
  <t_collection>
    <collection_id>32</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>

  <!-- Properties on System→Constraints -->
  <t_property>
    <property_id>4369</property_id><collection_id>700</collection_id>
    <name>Sense</name>
  </t_property>
  <t_property>
    <property_id>4384</property_id><collection_id>700</collection_id>
    <name>RHS</name>
  </t_property>
  <t_property>
    <property_id>4390</property_id><collection_id>700</collection_id>
    <name>RHS Day</name>
  </t_property>
  <!-- Properties on Generator→Constraints -->
  <t_property>
    <property_id>393</property_id><collection_id>32</collection_id>
    <name>Generation Coefficient</name>
  </t_property>
  <t_property>
    <property_id>400</property_id><collection_id>32</collection_id>
    <name>Units Started Coefficient</name>
  </t_property>

  <!-- System↔Constraints memberships -->
  <t_membership>
    <membership_id>700001</membership_id>
    <collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>700002</membership_id>
    <collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>101</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>700003</membership_id>
    <collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>102</child_object_id>
  </t_membership>

  <!-- Generator↔Constraint memberships -->
  <t_membership>
    <membership_id>32001</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32002</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>21</parent_object_id>
    <child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32003</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>101</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32004</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>102</child_object_id>
  </t_membership>

  <!-- DAILY_ENERGY: Sense=-1, RHS Day=4.12 (GWh), Generation Coeff 1 each -->
  <t_data>
    <data_id>10001</data_id><membership_id>700001</membership_id>
    <property_id>4369</property_id><value>-1</value>
  </t_data>
  <t_data>
    <data_id>10002</data_id><membership_id>700001</membership_id>
    <property_id>4390</property_id><value>4.12</value>
  </t_data>
  <!-- RHS_DAY_COUNT: Sense=-1, RHS Day=2, Units Started Coeff 1 on G1 -->
  <t_data>
    <data_id>10003</data_id><membership_id>700002</membership_id>
    <property_id>4369</property_id><value>-1</value>
  </t_data>
  <t_data>
    <data_id>10004</data_id><membership_id>700002</membership_id>
    <property_id>4390</property_id><value>2</value>
  </t_data>
  <!-- PLAIN_LE: Sense=-1, plain RHS=200, Generation Coeff 1 on G1 -->
  <t_data>
    <data_id>10005</data_id><membership_id>700003</membership_id>
    <property_id>4369</property_id><value>-1</value>
  </t_data>
  <t_data>
    <data_id>10006</data_id><membership_id>700003</membership_id>
    <property_id>4384</property_id><value>200</value>
  </t_data>

  <!-- Coefficient t_data -->
  <t_data>
    <data_id>20001</data_id><membership_id>32001</membership_id>
    <property_id>393</property_id><value>1.0</value>
  </t_data>
  <t_data>
    <data_id>20002</data_id><membership_id>32002</membership_id>
    <property_id>393</property_id><value>1.0</value>
  </t_data>
  <t_data>
    <data_id>20003</data_id><membership_id>32003</membership_id>
    <property_id>400</property_id><value>1.0</value>
  </t_data>
  <t_data>
    <data_id>20004</data_id><membership_id>32004</membership_id>
    <property_id>393</property_id><value>1.0</value>
  </t_data>
</MasterDataSet>
"""


def _build_bundle(tmp_path: Path) -> Path:
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_DAILY_XML)
    return xml_path


def test_rhs_day_generation_marked_daily_sum_energy(tmp_path: Path) -> None:
    """A ``RHS Day`` + ``generation`` LHS → daily_sum energy, RHS ×1000."""
    xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    out = extract_user_constraints(db, bundle)
    by_name = {c.name: c for c in out}

    assert "DAILY_ENERGY" in by_name
    spec = by_name["DAILY_ENERGY"]
    assert spec.daily_sum is True
    assert spec.constraint_type == "energy"
    # Both generation terms survive; the inline RHS tail is 4.12 GWh × 1000.
    assert 'generator("G1").generation' in spec.expression
    assert 'generator("G2").generation' in spec.expression
    assert spec.expression.endswith("<= 4120")


def test_rhs_day_commitment_count_daily_sum_count(tmp_path: Path) -> None:
    """A ``RHS Day`` constraint with a commitment-COUNT LHS (a crew limit,
    e.g. ``Guacolda_Crew``) is a per-DAY count cap: emitted with
    ``daily_sum=True`` and ``constraint_type=""`` (an UNWEIGHTED per-day
    count, NOT a Δt-weighted energy budget) with the RHS at face value (NOT
    ×1000 GWh→MWh scaled).  PLEXOS spreads the daily 2 across 24 h (solution
    RHS 2/24 = 0.083); gtopt sums the per-block counts per day instead."""
    xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    out = extract_user_constraints(db, bundle)
    by_name = {c.name: c for c in out}

    assert "RHS_DAY_COUNT" in by_name
    spec = by_name["RHS_DAY_COUNT"]
    assert spec.daily_sum is True
    assert spec.constraint_type == ""  # count, NOT "energy"
    # commitment startup count — no .generation accessor, RHS NOT ×1000-scaled.
    assert 'commitment("uc_G1").startup' in spec.expression
    assert spec.expression.endswith("<= 2")


def test_plain_rhs_generation_not_daily_sum(tmp_path: Path) -> None:
    """A plain ``RHS`` (not ``RHS Day``) generation constraint stays a normal
    per-block UC — daily_sum is not triggered."""
    xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    out = extract_user_constraints(db, bundle)
    by_name = {c.name: c for c in out}

    assert "PLAIN_LE" in by_name
    spec = by_name["PLAIN_LE"]
    assert spec.daily_sum is False
    assert spec.constraint_type == ""
    assert spec.expression.endswith("<= 200")


def test_build_user_constraint_array_emits_daily_sum_fields() -> None:
    """The writer mirrors ``daily_sum`` / ``constraint_type`` onto the JSON
    entry; plain specs omit both keys."""
    specs = (
        UserConstraintSpec(
            name="ENERGY",
            expression='generator("G1").generation <= 4120',
            daily_sum=True,
            constraint_type="energy",
        ),
        UserConstraintSpec(name="PLAIN", expression='line("L").flow <= 10'),
    )
    out = build_user_constraint_array(specs)
    assert out[0]["daily_sum"] is True
    assert out[0]["constraint_type"] == "energy"
    assert "daily_sum" not in out[1]
    assert "constraint_type" not in out[1]


def test_daily_sum_uc_routed_inline_not_pampl(tmp_path: Path) -> None:
    """``daily_sum`` UCs must stay inline in the JSON ``user_constraint_array``
    — the ``.pampl`` grammar has no ``daily_sum`` clause."""
    specs = (
        UserConstraintSpec(
            name="ENERGY",
            expression='generator("G1").generation <= 4120',
            daily_sum=True,
            constraint_type="energy",
        ),
    )
    uc_array = build_user_constraint_array(specs)
    pampl_files, json_remaining = write_user_constraint_pampl(uc_array, tmp_path)
    assert not pampl_files
    assert len(json_remaining) == 1
    assert json_remaining[0]["daily_sum"] is True
