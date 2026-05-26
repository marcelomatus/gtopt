"""Unit tests for the PLEXOS Decision Variable wiring.

Covers the end-to-end path:
* :func:`parsers.extract_decision_variables` — pulls Lower/Upper Bound +
  Objective Function Coefficient from the System→Decision Variables
  collection.
* :func:`gtopt_writer.build_decision_variable_array` — emits the
  ``decision_variable_array`` JSON node with optional bounds + cost.
* :func:`parsers.extract_user_constraints` — wires the
  ``Value Coefficient`` into ``decision_variable("X").value`` LHS terms
  when the DV is in ``emitted_names["DecisionVariable"]``.
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import DecisionVariableSpec
from plexos2gtopt.gtopt_writer import build_decision_variable_array
from plexos2gtopt.parsers import (
    extract_decision_variables,
    extract_user_constraints,
)
from plexos2gtopt.plexos_loader import PlexosBundle
from plexos2gtopt.plexos_xml import NS, load_xml


_DV_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_class><class_id>72</class_id><name>Decision Variable</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>72</class_id><name>DV_A</name></t_object>
  <t_object><object_id>11</object_id><class_id>72</class_id><name>DV_B</name></t_object>
  <t_object><object_id>20</object_id><class_id>70</class_id><name>DV_BALANCE</name></t_object>

  <t_collection>
    <collection_id>707</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>72</child_class_id>
    <name>Decision Variables</name>
  </t_collection>
  <t_collection>
    <collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>710</collection_id>
    <parent_class_id>72</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>

  <t_property>
    <property_id>4431</property_id><collection_id>707</collection_id>
    <name>Lower Bound</name>
  </t_property>
  <t_property>
    <property_id>4432</property_id><collection_id>707</collection_id>
    <name>Upper Bound</name>
  </t_property>
  <t_property>
    <property_id>4425</property_id><collection_id>707</collection_id>
    <name>Objective Function Coefficient</name>
  </t_property>
  <t_property>
    <property_id>4369</property_id><collection_id>700</collection_id>
    <name>Sense</name>
  </t_property>
  <t_property>
    <property_id>4384</property_id><collection_id>700</collection_id>
    <name>RHS</name>
  </t_property>
  <t_property>
    <property_id>4438</property_id><collection_id>710</collection_id>
    <name>Value Coefficient</name>
  </t_property>

  <!-- System→DV memberships -->
  <t_membership>
    <membership_id>500</membership_id>
    <collection_id>707</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>501</membership_id>
    <collection_id>707</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>11</child_object_id>
  </t_membership>
  <!-- System→Constraint membership for DV_BALANCE -->
  <t_membership>
    <membership_id>600</membership_id>
    <collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>20</child_object_id>
  </t_membership>
  <!-- DV→Constraint membership (DV_A in DV_BALANCE) -->
  <t_membership>
    <membership_id>700</membership_id>
    <collection_id>710</collection_id>
    <parent_object_id>10</parent_object_id>
    <child_object_id>20</child_object_id>
  </t_membership>

  <!-- DV bounds + cost -->
  <t_data>
    <data_id>1</data_id><membership_id>500</membership_id>
    <property_id>4431</property_id><value>-50</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>500</membership_id>
    <property_id>4432</property_id><value>100</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>500</membership_id>
    <property_id>4425</property_id><value>2.5</value>
  </t_data>
  <!-- Constraint sense + RHS -->
  <t_data>
    <data_id>10</data_id><membership_id>600</membership_id>
    <property_id>4369</property_id><value>0</value>
  </t_data>
  <t_data>
    <data_id>11</data_id><membership_id>600</membership_id>
    <property_id>4384</property_id><value>5</value>
  </t_data>
  <!-- Value Coefficient = 1 on DV_A inside DV_BALANCE -->
  <t_data>
    <data_id>20</data_id><membership_id>700</membership_id>
    <property_id>4438</property_id><value>1</value>
  </t_data>
</MasterDataSet>
"""


def _build_bundle(tmp_path: Path) -> tuple[PlexosBundle, Path]:
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_DV_XML)
    return PlexosBundle(root=tmp_path, source=tmp_path), xml_path


# ---------------------------------------------------------------------------
# extract_decision_variables
# ---------------------------------------------------------------------------


def test_extract_decision_variables_picks_up_bounds_and_cost(
    tmp_path: Path,
) -> None:
    """Lower / Upper / Objective Coefficient populate the spec."""
    _, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    out = extract_decision_variables(db)
    by_name = {d.name: d for d in out}
    assert set(by_name) == {"DV_A", "DV_B"}
    assert by_name["DV_A"].lower_bound == -50.0
    assert by_name["DV_A"].upper_bound == 100.0
    assert by_name["DV_A"].cost == 2.5
    # DV_B has no t_data → all unset.
    assert by_name["DV_B"].lower_bound is None
    assert by_name["DV_B"].upper_bound is None
    assert by_name["DV_B"].cost == 0.0


# ---------------------------------------------------------------------------
# build_decision_variable_array
# ---------------------------------------------------------------------------


def test_build_decision_variable_array_omits_unset_fields() -> None:
    """Unset bounds + zero cost → no key emitted; non-zero values land verbatim."""
    out = build_decision_variable_array(
        (
            DecisionVariableSpec(name="DV1", lower_bound=-1.0, upper_bound=1.0),
            DecisionVariableSpec(name="DV2", cost=3.0),
            DecisionVariableSpec(name="DV3"),
        )
    )
    assert out[0]["lower_bound"] == -1.0
    assert out[0]["upper_bound"] == 1.0
    assert "cost" not in out[0]
    assert out[1]["cost"] == 3.0
    assert "lower_bound" not in out[1]
    assert "upper_bound" not in out[1]
    # Fully unset DV emits just uid + name.
    assert set(out[2].keys()) == {"uid", "name"}


# ---------------------------------------------------------------------------
# extract_user_constraints with DV references
# ---------------------------------------------------------------------------


def test_user_constraint_routes_dv_value_coefficient(tmp_path: Path) -> None:
    """``Value Coefficient`` on the DV→Constraint edge → decision_variable(...).value."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    allow = {"DecisionVariable": frozenset({"DV_A"})}
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    by_name = {c.name: c for c in out}
    assert "DV_BALANCE" in by_name
    expr = by_name["DV_BALANCE"].expression
    assert 'decision_variable("DV_A").value' in expr
    # Sense=0 (EQ); RHS=5 → "= 5"
    assert expr.endswith("= 5")


def test_user_constraint_drops_unknown_dv(tmp_path: Path) -> None:
    """A DV not in ``emitted_names`` causes the term to be filtered out."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    allow = {"DecisionVariable": frozenset()}  # nothing allowed
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    # DV_BALANCE had only one term (DV_A.value); after filter → empty LHS → drop.
    assert all(c.name != "DV_BALANCE" for c in out)
