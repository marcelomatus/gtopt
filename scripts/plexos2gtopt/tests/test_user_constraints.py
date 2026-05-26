"""Unit + integration tests for the PLEXOS Constraint → UserConstraint wiring.

A self-contained synthetic XML carries:
* one System object, two Generators on the same bus, one Line, one Battery
* one Constraint ``CORRIDOR_LE`` with Sense=-1, RHS=200, with a Generation
  Coefficient of 1 on each generator and a Flow Coefficient of 1 on the line
* one Constraint ``HARD_EQ`` with Sense=0, RHS=50, a Generation Coefficient of
  0.5 on G1 and a Penalty Price of 100 (turns it soft)
* one Constraint ``RESERVE_RULE`` whose only coefficient is a (non-supported)
  Units Generating slot — should be dropped because the LHS is empty
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import UserConstraintSpec
from plexos2gtopt.gtopt_writer import build_user_constraint_array
from plexos2gtopt.parsers import extract_user_constraints
from plexos2gtopt.plexos_loader import PlexosBundle
from plexos2gtopt.plexos_xml import NS, load_xml


_UC_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>7</class_id><name>Battery</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>24</class_id><name>Line</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>22</class_id><name>b_a</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object><object_id>21</object_id><class_id>2</class_id><name>G2</name></t_object>
  <t_object><object_id>30</object_id><class_id>24</class_id><name>L1</name></t_object>
  <t_object><object_id>40</object_id><class_id>7</class_id><name>B1</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id><name>CORRIDOR_LE</name></t_object>
  <t_object><object_id>101</object_id><class_id>70</class_id><name>HARD_EQ</name></t_object>
  <t_object><object_id>102</object_id><class_id>70</class_id><name>EMPTY_LHS</name></t_object>

  <!-- System → Constraints (RHS / Sense / Penalty Price live here) -->
  <t_collection>
    <collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <!-- Generator → Constraints (Generation Coefficient) -->
  <t_collection>
    <collection_id>32</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <!-- Line → Constraints (Flow Coefficient) -->
  <t_collection>
    <collection_id>310</collection_id>
    <parent_class_id>24</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <!-- Battery → Constraints (Load / Generation Coefficients) -->
  <t_collection>
    <collection_id>90</collection_id>
    <parent_class_id>7</parent_class_id>
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
    <property_id>4393</property_id><collection_id>700</collection_id>
    <name>Penalty Price</name>
  </t_property>
  <!-- Properties on Generator→Constraints -->
  <t_property>
    <property_id>393</property_id><collection_id>32</collection_id>
    <name>Generation Coefficient</name>
  </t_property>
  <!-- Properties on Line→Constraints -->
  <t_property>
    <property_id>1963</property_id><collection_id>310</collection_id>
    <name>Flow Coefficient</name>
  </t_property>
  <!-- Properties on Battery→Constraints -->
  <t_property>
    <property_id>967</property_id><collection_id>90</collection_id>
    <name>Generation Coefficient</name>
  </t_property>
  <t_property>
    <property_id>968</property_id><collection_id>90</collection_id>
    <name>Load Coefficient</name>
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
  <!-- Line↔Constraint membership -->
  <t_membership>
    <membership_id>310001</membership_id>
    <collection_id>310</collection_id>
    <parent_object_id>30</parent_object_id>
    <child_object_id>100</child_object_id>
  </t_membership>
  <!-- Battery↔Constraint memberships (load + generation) -->
  <t_membership>
    <membership_id>90001</membership_id>
    <collection_id>90</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>100</child_object_id>
  </t_membership>

  <!-- System→Constraint t_data: Sense + RHS + (optional) Penalty Price -->
  <t_data>
    <data_id>10001</data_id><membership_id>700001</membership_id>
    <property_id>4369</property_id><value>-1</value>
  </t_data>
  <t_data>
    <data_id>10002</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>200</value>
  </t_data>
  <t_data>
    <data_id>10003</data_id><membership_id>700002</membership_id>
    <property_id>4369</property_id><value>0</value>
  </t_data>
  <t_data>
    <data_id>10004</data_id><membership_id>700002</membership_id>
    <property_id>4384</property_id><value>50</value>
  </t_data>
  <t_data>
    <data_id>10005</data_id><membership_id>700002</membership_id>
    <property_id>4393</property_id><value>100</value>
  </t_data>
  <t_data>
    <data_id>10006</data_id><membership_id>700003</membership_id>
    <property_id>4369</property_id><value>1</value>
  </t_data>
  <t_data>
    <data_id>10007</data_id><membership_id>700003</membership_id>
    <property_id>4384</property_id><value>10</value>
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
    <property_id>393</property_id><value>0.5</value>
  </t_data>
  <t_data>
    <data_id>20004</data_id><membership_id>310001</membership_id>
    <property_id>1963</property_id><value>1.0</value>
  </t_data>
  <t_data>
    <data_id>20005</data_id><membership_id>90001</membership_id>
    <property_id>968</property_id><value>1.0</value>
  </t_data>
  <t_data>
    <data_id>20006</data_id><membership_id>90001</membership_id>
    <property_id>967</property_id><value>-0.5</value>
  </t_data>
</MasterDataSet>
"""


def _build_bundle(tmp_path: Path) -> tuple[PlexosBundle, Path]:
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_UC_XML)
    return PlexosBundle(root=tmp_path, source=tmp_path), xml_path


# ---------------------------------------------------------------------------
# extract_user_constraints
# ---------------------------------------------------------------------------


def test_extract_user_constraints_corridor_le(tmp_path: Path) -> None:
    """Sense=-1 → <=; LHS aggregates Generation + Flow + Battery coefficients."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    out = extract_user_constraints(db, bundle)
    by_name = {c.name: c for c in out}
    assert "CORRIDOR_LE" in by_name
    expr = by_name["CORRIDOR_LE"].expression
    # Operator + RHS sit at the tail.
    assert expr.endswith("<= 200")
    # LHS includes both generators, the line, and both battery accessors.
    assert 'generator("G1").generation' in expr
    assert 'generator("G2").generation' in expr
    assert 'line("L1").flow' in expr
    assert 'battery("B1").charge' in expr  # from Load Coefficient
    assert 'battery("B1").discharge' in expr  # from Generation Coefficient
    # No explicit Penalty Price in XML; the code's soft-default
    # ($10/MWh) applies because the constraint references generator
    # and battery terms (not pure line flow or commitment refs).
    assert by_name["CORRIDOR_LE"].penalty == 10.0


def test_extract_user_constraints_eq_with_penalty(tmp_path: Path) -> None:
    """Sense=0 with Penalty Price > 0 → soft equality."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    out = extract_user_constraints(db, bundle)
    by_name = {c.name: c for c in out}
    assert "HARD_EQ" in by_name
    expr = by_name["HARD_EQ"].expression
    assert expr.endswith("= 50")
    assert by_name["HARD_EQ"].penalty == 100.0


def test_extract_user_constraints_drops_empty_lhs(tmp_path: Path) -> None:
    """Constraints with no supported coefficients are silently dropped."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    out = extract_user_constraints(db, bundle)
    names = {c.name for c in out}
    assert "EMPTY_LHS" not in names


def test_extract_user_constraints_filters_inactive(tmp_path: Path) -> None:
    """``emitted_names`` filters out terms referencing missing elements."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    # Pretend G2 was dropped (e.g. zero pmax) — its term should vanish.
    allow = {
        "Generator": frozenset({"G1"}),
        "Line": frozenset({"L1"}),
        "Battery": frozenset({"B1"}),
    }
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    by_name = {c.name: c for c in out}
    assert "CORRIDOR_LE" in by_name
    expr = by_name["CORRIDOR_LE"].expression
    assert 'generator("G1").generation' in expr
    assert 'generator("G2").generation' not in expr


def test_extract_user_constraints_drops_when_all_terms_filtered(
    tmp_path: Path,
) -> None:
    """A constraint whose every coefficient references a dropped element."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    # HARD_EQ references G1 alone — drop G1 from the allow-list.
    allow = {
        "Generator": frozenset(),
        "Line": frozenset({"L1"}),
        "Battery": frozenset({"B1"}),
    }
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    names = {c.name for c in out}
    assert "HARD_EQ" not in names


# ---------------------------------------------------------------------------
# build_user_constraint_array
# ---------------------------------------------------------------------------


def test_build_user_constraint_array_basic_fields() -> None:
    """Writer emits uid + name + expression; penalty + description optional."""
    specs = (
        UserConstraintSpec(name="A", expression='1 * generator("G").generation <= 10'),
        UserConstraintSpec(
            name="B",
            expression='2 * line("L").flow >= 0',
            penalty=42.0,
            description="soft cap",
        ),
    )
    out = build_user_constraint_array(specs)
    assert out[0] == {
        "uid": 1,
        "name": "A",
        "expression": '1 * generator("G").generation <= 10',
    }
    assert out[1]["penalty"] == 42.0
    assert out[1]["description"] == "soft cap"


def test_build_user_constraint_array_omits_zero_penalty() -> None:
    """penalty = 0 → no key emitted (hard constraint)."""
    out = build_user_constraint_array(
        (UserConstraintSpec(name="X", expression="a <= b", penalty=0.0),)
    )
    assert "penalty" not in out[0]


# ---------------------------------------------------------------------------
# Contingency / N-1 security classification
# ---------------------------------------------------------------------------


def test_is_contingency_constraint_ignores_name() -> None:
    """The helper is structural-only — PRIMARY source for "skip in
    monolithic LP" is PLEXOS's ``Include in ST Schedule`` property
    (read upstream in ``extract_user_constraints`` and routed into
    ``include_st_excluded``).  Name-pattern recognisers were
    removed after audit showed they mis-classified operational
    rows (``ANTUCOmin``, ``ANGOSTURAmaxramp``) as contingency rows.
    Healthy coefficient structures stay active regardless of name.
    """
    from plexos2gtopt.parsers import _is_contingency_constraint

    # PLEXOS-looking names with satisfiable coefficients pass through.
    assert not _is_contingency_constraint(
        "SD_2024091389_Charrua_Conce", [1.0], "<=", 100.0
    )
    assert not _is_contingency_constraint("RALCO_U1_CTF_LW", [1.0], "<=", 100.0)
    assert not _is_contingency_constraint("RALCO_U2_CSF_RS", [1.0], ">=", -100.0)


def test_is_contingency_constraint_matches_structural_pattern() -> None:
    """All-negative coefficients + ``>= positive RHS`` ⇒ contingency."""
    from plexos2gtopt.parsers import _is_contingency_constraint

    # Σ -provision ≥ 90 is infeasible-as-hard (LHS ≤ 0, RHS > 0).
    assert _is_contingency_constraint("BENIGN_NAME", [-1.0, -1.0, -1.0], ">=", 90.0)


def test_is_contingency_constraint_rejects_satisfiable_rows() -> None:
    """Mixed signs / non-GE / negative RHS / no coefficients → not contingency."""
    from plexos2gtopt.parsers import _is_contingency_constraint

    # A single positive coefficient → LHS can grow → satisfiable.
    assert not _is_contingency_constraint("BENIGN_NAME", [-1.0, +1.0], ">=", 90.0)
    # Sense LE flips the feasibility argument.
    assert not _is_contingency_constraint("BENIGN_NAME", [-1.0, -1.0], "<=", 90.0)
    # Non-positive RHS makes ``Σ ≤ 0 ≥ 0`` satisfiable.
    assert not _is_contingency_constraint("BENIGN_NAME", [-1.0, -1.0], ">=", 0.0)
    # Empty coefficients → can't classify; default ⇒ not contingency.
    assert not _is_contingency_constraint("BENIGN_NAME", [], ">=", 90.0)


def test_build_user_constraint_array_emits_active_false() -> None:
    """When ``UserConstraintSpec.active is False`` the writer emits
    ``active: false`` so gtopt's UserConstraintLP skips the row in
    the monolithic LP (gating on ``is_active``)."""
    out = build_user_constraint_array(
        (
            UserConstraintSpec(name="X", expression="a <= b", active=False),
            UserConstraintSpec(name="Y", expression="a <= b", active=True),
            UserConstraintSpec(name="Z", expression="a <= b"),  # default None
        )
    )
    assert out[0]["active"] is False
    assert out[1]["active"] is True
    assert "active" not in out[2]


# ---------------------------------------------------------------------------
# Fuel.Offtake expansion + Reserve Provision wiring
# ---------------------------------------------------------------------------


_OFFTAKE_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>4</class_id><name>Fuel</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object><object_id>11</object_id><class_id>2</class_id><name>G2</name></t_object>
  <t_object><object_id>20</object_id><class_id>4</class_id><name>GAS</name></t_object>
  <t_object><object_id>30</object_id><class_id>70</class_id><name>GAS_CAP</name></t_object>

  <t_collection>
    <collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>54</collection_id>
    <parent_class_id>4</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>7</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>4</child_class_id>
    <name>Fuels</name>
  </t_collection>

  <t_property>
    <property_id>4369</property_id><collection_id>700</collection_id>
    <name>Sense</name>
  </t_property>
  <t_property>
    <property_id>4384</property_id><collection_id>700</collection_id>
    <name>RHS</name>
  </t_property>
  <t_property>
    <property_id>620</property_id><collection_id>54</collection_id>
    <name>Offtake Coefficient</name>
  </t_property>

  <!-- System→Constraint membership -->
  <t_membership>
    <membership_id>900</membership_id>
    <collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <!-- Fuel→Constraint membership -->
  <t_membership>
    <membership_id>910</membership_id>
    <collection_id>54</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <!-- Both generators consume GAS -->
  <t_membership>
    <membership_id>920</membership_id>
    <collection_id>7</collection_id>
    <parent_object_id>10</parent_object_id>
    <child_object_id>20</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>921</membership_id>
    <collection_id>7</collection_id>
    <parent_object_id>11</parent_object_id>
    <child_object_id>20</child_object_id>
  </t_membership>

  <!-- Sense + RHS for GAS_CAP -->
  <t_data>
    <data_id>1</data_id><membership_id>900</membership_id>
    <property_id>4369</property_id><value>-1</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>900</membership_id>
    <property_id>4384</property_id><value>1000</value>
  </t_data>
  <!-- Fuel.Offtake Coefficient = 1 (the burn coefficient) -->
  <t_data>
    <data_id>3</data_id><membership_id>910</membership_id>
    <property_id>620</property_id><value>1.0</value>
  </t_data>
</MasterDataSet>
"""


def test_fuel_offtake_expands_to_generator_terms(tmp_path: Path) -> None:
    """Fuel.Offtake Coefficient α expands to ``α × heat_rate × generator.generation``."""
    bundle, xml_path = (
        PlexosBundle(root=tmp_path, source=tmp_path),
        tmp_path / "DBSEN_PRGDIARIO.xml",
    )
    xml_path.write_text(_OFFTAKE_XML)
    db = load_xml(xml_path)
    # Heat rates supplied externally (mirrors the CSV → GeneratorSpec path).
    out = extract_user_constraints(
        db,
        bundle,
        heat_rate_by_gen={"G1": 0.5, "G2": 0.3},
    )
    by_name = {c.name: c for c in out}
    assert "GAS_CAP" in by_name
    expr = by_name["GAS_CAP"].expression
    # α × heat_rate per generator:  1 × 0.5 × G1.generation + 1 × 0.3 × G2.generation
    assert '0.5 * generator("G1").generation' in expr
    assert '0.3 * generator("G2").generation' in expr
    assert expr.endswith("<= 1000")


def test_fuel_offtake_skips_zero_heat_rate(tmp_path: Path) -> None:
    """A generator with no heat rate doesn't contribute to the expansion."""
    bundle, xml_path = (
        PlexosBundle(root=tmp_path, source=tmp_path),
        tmp_path / "DBSEN_PRGDIARIO.xml",
    )
    xml_path.write_text(_OFFTAKE_XML)
    db = load_xml(xml_path)
    out = extract_user_constraints(
        db,
        bundle,
        heat_rate_by_gen={"G1": 0.5},  # G2 omitted
    )
    by_name = {c.name: c for c in out}
    assert "GAS_CAP" in by_name
    expr = by_name["GAS_CAP"].expression
    assert 'generator("G1").generation' in expr
    assert 'generator("G2").generation' not in expr


# ---------------------------------------------------------------------------
# T3: Battery Reserve Units Coefficient → commitment("uc_<bat>_gen").status
# ---------------------------------------------------------------------------


_BATT_RU_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>7</class_id><name>Battery</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>40</object_id><class_id>7</class_id><name>bess_a</name></t_object>
  <t_object>
    <object_id>100</object_id><class_id>70</class_id><name>BATT_RU</name>
  </t_object>

  <!-- System → Constraints (Sense / RHS) -->
  <t_collection>
    <collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <!-- Battery → Constraints (Reserve Units Coefficient lives here) -->
  <t_collection>
    <collection_id>90</collection_id>
    <parent_class_id>7</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>

  <t_property>
    <property_id>4369</property_id><collection_id>700</collection_id>
    <name>Sense</name>
  </t_property>
  <t_property>
    <property_id>4384</property_id><collection_id>700</collection_id>
    <name>RHS</name>
  </t_property>
  <t_property>
    <property_id>980</property_id><collection_id>90</collection_id>
    <name>Reserve Units Coefficient</name>
  </t_property>

  <t_membership>
    <membership_id>700001</membership_id>
    <collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>90001</membership_id>
    <collection_id>90</collection_id>
    <parent_object_id>40</parent_object_id>
    <child_object_id>100</child_object_id>
  </t_membership>

  <t_data>
    <data_id>1</data_id><membership_id>700001</membership_id>
    <property_id>4369</property_id><value>-1</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>1</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>90001</membership_id>
    <property_id>980</property_id><value>1.0</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_user_constraints_battery_reserve_units_forwards_to_gen_commit(
    tmp_path: Path,
) -> None:
    """``Battery.Reserve Units Coefficient`` has no direct LP column.

    gtopt's Battery LP manages its u_charge / u_discharge binaries
    internally — there's no ``battery_commitment("X").status`` AMPL
    accessor.  The parser forwards the coefficient onto the
    synthetic ``<battery>_gen`` Generator's Commitment column
    (auto-created by gtopt's C++ ``expand_batteries``), which DOES
    expose ``.status`` via ``commitment("uc_<battery>_gen").status``.

    Locks the forward path so the wiring doesn't silently revert to
    a ``battery("X").commitment`` form gtopt can't resolve.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_RU_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    out = extract_user_constraints(db, bundle)
    by_name = {c.name: c for c in out}
    assert "BATT_RU" in by_name
    expr = by_name["BATT_RU"].expression
    # Forwarded onto the synthetic generator's Commitment status.
    assert 'commitment("uc_bess_a_gen").status' in expr
    # No direct battery("…") term lands on the LHS.
    assert 'battery("bess_a")' not in expr
    # Sense=-1 → "<="; RHS=1.
    assert expr.endswith("<= 1")


def test_reserve_provision_writer_emits_provision_factor_and_max() -> None:
    """build_reserve_provision_array emits provision_factor + urmax/drmax."""
    from plexos2gtopt.entities import ReserveProvisionSpec
    from plexos2gtopt.gtopt_writer import build_reserve_provision_array

    out = build_reserve_provision_array(
        (
            ReserveProvisionSpec(
                generator_name="G1",
                reserve_zones=("Z1",),
                urmax=100.0,
                drmax=80.0,
            ),
        )
    )
    assert out[0]["ur_provision_factor"] == 1.0
    assert out[0]["dr_provision_factor"] == 1.0
    assert out[0]["urmax"] == 100.0
    assert out[0]["drmax"] == 80.0
