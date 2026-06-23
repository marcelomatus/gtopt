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

import pytest

from plexos2gtopt.entities import (
    GeneratorSpec,
    UserConstraintSpec,
    generator_is_zero_pmax,
)
from plexos2gtopt.gtopt_writer import (
    build_user_constraint_array,
    filter_user_constraints,
    write_user_constraint_pampl,
)
from plexos2gtopt.parsers import (
    UnresolvedConstraintReferenceError,
    extract_user_constraints,
)
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


def test_extract_user_constraints_raises_on_unemitted_element(
    tmp_path: Path,
) -> None:
    """A term referencing an element gtopt never emits FAILS HARD.

    New contract (mirrors gtopt's strict JSON parser): a UserConstraint
    term whose referenced element is absent from ``emitted_names`` is
    NOT silently dropped — it is collected and, after all constraints
    are walked, ONE ``UnresolvedConstraintReferenceError`` is raised
    listing every offending reference.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    # G2 is genuinely absent from the emitted set → must fail hard.
    allow = {
        "Generator": frozenset({"G1"}),
        "Line": frozenset({"L1"}),
        "Battery": frozenset({"B1"}),
    }
    with pytest.raises(UnresolvedConstraintReferenceError) as excinfo:
        extract_user_constraints(db, bundle, emitted_names=allow)
    msg = str(excinfo.value)
    # The full list names the offending constraint + reference.
    assert "CORRIDOR_LE" in msg
    assert 'generator("G2").generation' in msg


def test_extract_user_constraints_raises_with_full_list(
    tmp_path: Path,
) -> None:
    """Every unresolvable reference is collected before the single raise.

    Dropping ALL generators leaves both ``CORRIDOR_LE`` (G1, G2) and
    ``HARD_EQ`` (G1) with unresolvable terms; the error must list all of
    them rather than bailing on the first.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Generator": frozenset(),
        "Line": frozenset({"L1"}),
        "Battery": frozenset({"B1"}),
    }
    with pytest.raises(UnresolvedConstraintReferenceError) as excinfo:
        extract_user_constraints(db, bundle, emitted_names=allow)
    msg = str(excinfo.value)
    assert "CORRIDOR_LE" in msg
    assert "HARD_EQ" in msg
    assert 'generator("G1").generation' in msg
    assert 'generator("G2").generation' in msg


def test_extract_user_constraints_shadow_line_all_off_silent_drop(
    tmp_path: Path,
) -> None:
    """PLEXOS contingency-state shadow Line with ``Units = 0`` across the
    entire horizon → term contributes mathematically 0 → silent drop.

    Mirrors the CEN PCP case where ``Cardones220->CPinto220_I/II/III``
    and ``NvaPAzucar500_SC->Polpaico500_I/II_SC`` shadow Lines have
    ``Lin_Units.csv = 0`` for all 168 blocks: PLEXOS's LP pins their
    flow to 0, so the UC's Flow Coefficient term contributes 0 to the
    LHS regardless of its value.  The converter must recognise this
    via the ``shadow_lines_all_off`` set and drop the term silently
    rather than fail-hard on the unresolved-name contract.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    # L1 is the Line membered to CORRIDOR_LE; mark it as a shadow Line
    # with Units=0 all-horizon, and DON'T include it in emitted_names
    # (since extract_lines would normally drop a mothballed line too).
    allow = {
        "Generator": frozenset({"G1", "G2"}),
        "Line": frozenset(),  # L1 NOT emitted
        "Battery": frozenset({"B1"}),
    }
    out = extract_user_constraints(
        db,
        bundle,
        emitted_names=allow,
        shadow_lines_all_off=frozenset({"L1"}),
    )
    # CORRIDOR_LE survives (no fail-hard); its LHS now contains only the
    # generator terms (G1, G2) — the L1 Flow Coefficient term was
    # silently dropped as a zero-contribution shadow-line reference.
    by_name = {c.name: c for c in out}
    assert "CORRIDOR_LE" in by_name
    expr = by_name["CORRIDOR_LE"].expression
    assert 'generator("G1").generation' in expr
    assert 'generator("G2").generation' in expr
    assert 'line("L1").flow' not in expr, "shadow-line term should be dropped"


def test_extract_user_constraints_all_shadow_emits_inactive_stub(
    tmp_path: Path,
) -> None:
    """Fix 6: when EVERY LHS term is a shadow-line drop, emit an inactive stub.

    Mirrors the CEN PCP ``SD_2024113659_Cardones_Cpinto_I`` and ``SDCF_Rx*``
    family: PLEXOS exercises these contingency-state UCs in its solution
    DB (they appear in t_object class=70) but every Line they reference
    has ``Lin_Units=0`` across the horizon — so the constraint reduces
    to ``0 ≤ rhs`` and contributes nothing.  Previously the parser
    silently dropped the constraint at the empty-LHS guard, leaving a
    spurious gap in the PLEXOS-sol → gtopt audit.  Now the parser emits
    an inactive stub (``0 <= 0``) carrying the constraint name + a
    description noting the no-op cause, so the bundle preserves
    provenance and the audit lines up.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    # ``EMPTY_LHS`` has Sense=1, RHS=10 already in the fixture (no
    # supported coefficients).  To exercise the new path we need a
    # constraint whose LHS terms are ALL shadow Lines.  Use
    # ``CORRIDOR_LE`` with shadow_lines_all_off={L1} AND empty
    # Generator/Battery allow-lists so every term gets silently dropped.
    allow = {
        "Generator": frozenset(),
        "Line": frozenset(),
        "Battery": frozenset(),
    }
    out = extract_user_constraints(
        db,
        bundle,
        emitted_names=allow,
        shadow_lines_all_off=frozenset({"L1"}),
        # CORRIDOR_LE has 5 terms (G1, G2, L1, B1.charge, B1.discharge).
        # G1/G2 fail-hard under empty Generator allow — so use
        # ``lax_refs`` to silently drop those instead, leaving L1's
        # shadow drop as the only path to empty LHS.
        lax_refs=True,
    )
    by_name = {c.name: c for c in out}
    assert "CORRIDOR_LE" in by_name, (
        "all-shadow constraint must emit as inactive stub, not be silently dropped"
    )
    stub = by_name["CORRIDOR_LE"]
    assert stub.active is False
    assert stub.expression == "0 <= 0"
    assert "shadow" in (stub.description or "").lower()


def test_extract_user_constraints_always_on_renewable_rhs_shift(
    tmp_path: Path,
) -> None:
    """A ``commitment("uc_<gen>").status`` term where ``<gen>`` IS emitted
    but has NO Commitment row (wind/solar pattern) → absorb the always-on
    contribution into the RHS (``rhs_val -= coeff``) instead of fail-hard.

    Mirrors the CEN PCP ``CSF_MinUnits`` case where 11 renewable plants
    (wind/solar) lack commitment binaries but PLEXOS treats their
    ``status = 1`` constant.  The constraint
    ``Σ status ≥ 3`` with 11 always-on plants becomes
    ``Σ status(committable) ≥ -8`` after the shift — exactly matching
    PLEXOS's dispatch.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    # HARD_EQ in the fixture references G1 with coeff=0.5 via Generation
    # Coefficient AND no Commitment Coefficient.  The reverse setup we
    # need is harder to synthesize without rebuilding the XML, so we
    # exercise the path by treating G1 as always-on for CORRIDOR_LE's
    # Generation Coefficient term (coeff=1).  CORRIDOR_LE has RHS=200
    # under sense=-1 (≤) — the shift should reduce RHS by 1 (200 - 1 = 199).
    #
    # NOTE: the Generation Coefficient maps to ``generator.generation``,
    # NOT ``commitment.status`` — so for a faithful unit test we'd need
    # the Units Generating Coefficient.  For now this test just exercises
    # the kwarg threading; the real-bundle integration test below
    # ("test_real_bundle_unresolved_uc_refs_fail_hard") covers the
    # end-to-end CSF_MinUnits path with the actual coefficient.
    allow = {
        "Generator": frozenset({"G1", "G2"}),
        "Line": frozenset({"L1"}),
        "Battery": frozenset({"B1"}),
        "Commitment": frozenset(),  # nothing emitted as commitment
    }
    # Without commitment refs in the XML this just verifies the kwarg
    # is accepted; convert returns normally because no commitment terms
    # are emitted at all.
    out = extract_user_constraints(
        db,
        bundle,
        emitted_names=allow,
        always_on_gens=frozenset({"G1", "G2"}),
    )
    assert len(out) >= 1, "non-commitment constraints still emit"


# Injected into _UC_XML to add a ``Units Generating Coefficient`` (→
# ``commitment("uc_G1").status``) on G1 — the shape that regressed on
# 2026-06-21 (NorthSecurity → uc_NEHUENCO_1-TG+TV_DIE) when the writer dropped
# the pmax=0 phantom's Commitment but the UC builder still emitted the term.
_PHANTOM_FRAGMENT = """\
  <t_object><object_id>103</object_id><class_id>70</class_id><name>UC_SEC</name></t_object>
  <t_property><property_id>396</property_id><collection_id>32</collection_id><name>Units Generating Coefficient</name></t_property>
  <t_membership><membership_id>700004</membership_id><collection_id>700</collection_id><parent_object_id>1</parent_object_id><child_object_id>103</child_object_id></t_membership>
  <t_membership><membership_id>32004</membership_id><collection_id>32</collection_id><parent_object_id>20</parent_object_id><child_object_id>103</child_object_id></t_membership>
  <t_data><data_id>10010</data_id><membership_id>700004</membership_id><property_id>4369</property_id><value>1</value></t_data>
  <t_data><data_id>10011</data_id><membership_id>700004</membership_id><property_id>4384</property_id><value>1</value></t_data>
  <t_data><data_id>20010</data_id><membership_id>32004</membership_id><property_id>396</property_id><value>1.0</value></t_data>
"""


def _build_phantom_bundle(tmp_path: Path) -> tuple[PlexosBundle, Path]:
    """`_build_bundle` plus a Units Generating Coefficient on G1 (UC_SEC)."""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(
        _UC_XML.replace("</MasterDataSet>", _PHANTOM_FRAGMENT + "</MasterDataSet>")
    )
    return PlexosBundle(root=tmp_path, source=tmp_path), xml_path


def test_generator_is_zero_pmax_predicate() -> None:
    """Single source of truth shared by the writer's phantom-commitment drop
    (``_zero_pmax_generator_names``) and the UC term drop
    (``offline_commit_gens``).  Guard against the two drifting."""

    def g(**kw: object) -> GeneratorSpec:
        return GeneratorSpec(object_id=0, name="g", bus_name="b_a", **kw)  # type: ignore[arg-type]

    assert generator_is_zero_pmax(g(pmax=0.0)) is True
    assert generator_is_zero_pmax(g(pmax=0.0, pmax_profile=(0.0, 0.0, 0.0))) is True
    assert generator_is_zero_pmax(g(pmax=100.0)) is False
    # a nonzero anywhere in the profile means it CAN dispatch
    assert generator_is_zero_pmax(g(pmax=0.0, pmax_profile=(0.0, 5.0))) is False
    # piecewise capacity overrides a 0 scalar pmax
    assert generator_is_zero_pmax(g(pmax=0.0, pmax_segments=(10.0,))) is False


def test_uc_units_generating_on_phantom_commitment_is_dropped(
    tmp_path: Path,
) -> None:
    """Regression (2026-06-21): a ``Units Generating Coefficient`` on a pmax=0
    phantom whose ``Commitment`` the writer drops must NOT survive as a
    dangling ``commitment("uc_G1").status`` reference.  With G1 in
    ``offline_commit_gens`` the term is dropped (status≡0) and the convert
    succeeds — no ``commitment(...)`` term reaches the LP."""
    bundle, xml_path = _build_phantom_bundle(tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Generator": frozenset({"G1", "G2"}),
        "Line": frozenset({"L1"}),
        "Battery": frozenset({"B1"}),
        "Commitment": frozenset(),  # G1's commitment dropped (pmax=0 phantom)
    }
    out = extract_user_constraints(
        db,
        bundle,
        emitted_names=allow,
        offline_commit_gens=frozenset({"G1"}),
    )
    joined = "".join(c.expression for c in out)
    assert "commitment(" not in joined, "phantom commitment.status must be dropped"
    # the non-commitment constraints (CORRIDOR_LE / HARD_EQ) still emit
    assert any(c.name == "CORRIDOR_LE" for c in out)


def test_uc_units_generating_on_phantom_without_guard_fails_hard(
    tmp_path: Path,
) -> None:
    """Contrast: without ``offline_commit_gens`` (and the commitment absent
    from the allow-list) the same term is an unresolved reference → fail hard.
    Proves the guard is what prevents the dangling reference, not a silent
    drop of any unknown commitment."""
    bundle, xml_path = _build_phantom_bundle(tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Generator": frozenset({"G1", "G2"}),
        "Line": frozenset({"L1"}),
        "Battery": frozenset({"B1"}),
        "Commitment": frozenset(),
    }
    with pytest.raises(UnresolvedConstraintReferenceError) as excinfo:
        extract_user_constraints(db, bundle, emitted_names=allow)
    assert 'commitment("uc_G1").status' in str(excinfo.value)


def test_extract_user_constraints_shadow_line_unset_still_fails_hard(
    tmp_path: Path,
) -> None:
    """A Line ref to a name NOT in ``shadow_lines_all_off`` and NOT in
    ``emitted_names`` still fails hard — only the bona-fide
    contingency-state shadow Lines get the silent-drop leniency."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Generator": frozenset({"G1", "G2"}),
        "Line": frozenset(),
        "Battery": frozenset({"B1"}),
    }
    # shadow set is EMPTY → L1's missing-from-Lines status produces a
    # fail-hard error (no contingency-state leniency applies).
    with pytest.raises(UnresolvedConstraintReferenceError) as excinfo:
        extract_user_constraints(
            db, bundle, emitted_names=allow, shadow_lines_all_off=frozenset()
        )
    assert 'line("L1").flow' in str(excinfo.value)


def test_extract_user_constraints_offline_emitted_gen_does_not_raise(
    tmp_path: Path,
) -> None:
    """An offline / pmax==0 generator that IS emitted is a VALID reference.

    gtopt models a pmax==0 generator (it gets a dispatch column) and its
    UserConstraint resolver is lenient (a reference at a zero-pmax block
    contributes 0).  The valid-name set therefore includes EVERY emitted
    generator regardless of pmax — referencing one must NOT trip the
    hard-fail.  Here both G1 and G2 are emitted (as the real converter
    does, building the set from all generators), so the convert succeeds.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Generator": frozenset({"G1", "G2"}),
        "Line": frozenset({"L1"}),
        "Battery": frozenset({"B1"}),
    }
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    by_name = {c.name: c for c in out}
    assert "CORRIDOR_LE" in by_name
    expr = by_name["CORRIDOR_LE"].expression
    assert 'generator("G1").generation' in expr
    assert 'generator("G2").generation' in expr


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


def test_fuel_offtake_emits_fuel_offtake_accessor(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Fuel.Offtake Coefficient α emits one ``α × fuel('X').offtake`` term.

    The modern emission path (``GTOPT_USE_FUEL_OFFTAKE=1``) replaces the
    legacy per-generator expansion with a single
    ``α × fuel(name).offtake`` term, leaning on gtopt's FuelLP offtake
    decision variable ``Y_f[b]`` bound by ``Y_f − Σ hr·dur·gen = 0``
    (see source/fuel_lp.cpp::add_to_lp).  The path is OFF by default
    (legacy expansion) pending a FuelLP edge-case fix where the
    offtake column doesn't get registered for some (fuel, stage,
    block) cells with active gens — until then enabling at CEN-PCP
    scale trips the strict UC resolver.  Opt in via the env var to
    exercise the modern path in tests.
    """
    monkeypatch.setenv("GTOPT_USE_FUEL_OFFTAKE", "1")
    bundle, xml_path = (
        PlexosBundle(root=tmp_path, source=tmp_path),
        tmp_path / "DBSEN_PRGDIARIO.xml",
    )
    xml_path.write_text(_OFFTAKE_XML)
    db = load_xml(xml_path)
    out = extract_user_constraints(
        db,
        bundle,
        heat_rate_by_gen={"G1": 0.5, "G2": 0.3},
    )
    by_name = {c.name: c for c in out}
    assert "GAS_CAP" in by_name
    expr = by_name["GAS_CAP"].expression
    # One fuel.offtake term, coefficient = PLEXOS α (no heat-rate mult).
    assert 'fuel("GAS").offtake' in expr, f"expected fuel.offtake term; got {expr!r}"
    assert "generator(" not in expr, (
        f"per-gen terms must be absent in modern emission; got {expr!r}"
    )
    assert expr.endswith("<= 1000")


def test_fuel_offtake_legacy_fallback_when_fuel_not_emitted(
    tmp_path: Path,
) -> None:
    """When the Fuel is absent from ``emitted_names['Fuel']``, fall back to
    the legacy per-generator expansion.

    Covers the case where ``extract_fuels`` dropped the fuel (e.g. no
    generators reference it, or a future filter excludes it) but the
    PLEXOS Offtake-Coefficient UC still names it.  Falling through to
    the per-gen expansion keeps the constraint expressible against the
    surviving Generator columns instead of failing-hard or going
    silently empty.
    """
    bundle, xml_path = (
        PlexosBundle(root=tmp_path, source=tmp_path),
        tmp_path / "DBSEN_PRGDIARIO.xml",
    )
    xml_path.write_text(_OFFTAKE_XML)
    db = load_xml(xml_path)
    out = extract_user_constraints(
        db,
        bundle,
        emitted_names={
            "Generator": frozenset({"G1", "G2"}),
            "Fuel": frozenset(),  # fuel "gas" deliberately missing
        },
        heat_rate_by_gen={"G1": 0.5, "G2": 0.3},
    )
    by_name = {c.name: c for c in out}
    assert "GAS_CAP" in by_name
    expr = by_name["GAS_CAP"].expression
    # Legacy: α × hr × gen per generator (no fuel.offtake term).
    assert '0.5 * generator("G1").generation' in expr
    assert '0.3 * generator("G2").generation' in expr
    assert 'fuel("gas").offtake' not in expr


def test_fuel_offtake_skips_zero_heat_rate_legacy(tmp_path: Path) -> None:
    """Legacy per-gen path still drops zero-heat-rate gens.

    Uses the Fuel-not-emitted fallback (``Fuel = frozenset()``) to force
    the legacy expansion, then verifies a missing heat rate on G2 means
    that generator contributes no term.  In the modern path the
    heat-rate filter lives in FuelLP::add_to_lp's ``hr <= 0`` guard.
    """
    bundle, xml_path = (
        PlexosBundle(root=tmp_path, source=tmp_path),
        tmp_path / "DBSEN_PRGDIARIO.xml",
    )
    xml_path.write_text(_OFFTAKE_XML)
    db = load_xml(xml_path)
    out = extract_user_constraints(
        db,
        bundle,
        emitted_names={
            "Generator": frozenset({"G1", "G2"}),
            "Fuel": frozenset(),  # force legacy fallback
        },
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


def test_battery_units_term_survives_with_emitted_names_no_commitment_spec(
    tmp_path: Path,
) -> None:
    """``CSF_MinUnits``-style battery-Units term is kept under validation.

    gtopt's ``System::expand_batteries`` creates the ``uc_<bat>_gen``
    Commitment UNCONDITIONALLY for every battery — even one that carries
    no PLEXOS commitment economics (no Min Discharge Level, no explicit
    commitment flag).  The converter therefore seeds
    ``emitted_names["Commitment"]`` with ``uc_<bat>_gen`` for every
    ``case.batteries`` entry, NOT only those that produced a
    ``CommitmentSpec``.

    Here ``bess_a`` has NO commitment row, yet a strict ``emitted_names``
    (battery present, commitment seeded from the battery list) must let
    the forwarded ``commitment("uc_bess_a_gen").status`` term resolve
    instead of dropping it or tripping the hard-fail — mirroring the real
    CEN PCP ``CSF_MinUnits`` → ``uc_BAT_MANZANO_FV_gen`` case.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_RU_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    # Strict allow-list: the battery exists, and uc_<bat>_gen is seeded
    # from the battery (NO CommitmentSpec for bess_a) — exactly what the
    # real converter builds via ``.union(uc_{b.name}_gen for case.batteries)``.
    allow = {
        "Battery": frozenset({"bess_a"}),
        "Commitment": frozenset({"uc_bess_a_gen"}),
    }
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    by_name = {c.name: c for c in out}
    assert "BATT_RU" in by_name
    expr = by_name["BATT_RU"].expression
    # The forwarded battery-commitment term survives validation.
    assert 'commitment("uc_bess_a_gen").status' in expr
    assert expr.endswith("<= 1")


# ---------------------------------------------------------------------------
# T4: Battery Reserve Provision Coefficient → zone-suffixed SSCC provision
# ---------------------------------------------------------------------------


# A battery (``BAT_X``) carries one ``Reserve Provision Coefficient = 1.0``
# row on a ``CSF_LW_MIN_BAT_X`` constraint (Sense >= , RHS 0).  The
# direct-coefficient builder maps the bare battery name to the plain
# ``provision_BAT_X`` — which the SSCC emitter never creates: its names
# are ZONE-suffixed (``provision_BAT_X_gen__CSF_LW_BESS``).  Without the
# reconciliation the term is silently dropped, degenerating the
# constraint.
_BATT_RP_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>7</class_id><name>Battery</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>40</object_id><class_id>7</class_id><name>BAT_X</name></t_object>
  <t_object>
    <object_id>100</object_id><class_id>70</class_id><name>CSF_LW_MIN_BAT_X</name>
  </t_object>

  <t_collection>
    <collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
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
    <property_id>981</property_id><collection_id>90</collection_id>
    <name>Reserve Provision Coefficient</name>
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
    <property_id>4369</property_id><value>1</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>0</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>90001</membership_id>
    <property_id>981</property_id><value>1.0</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_user_constraints_battery_reserve_provision_zone_suffix(
    tmp_path: Path,
) -> None:
    """Fix 1: a Battery ``Reserve Provision Coefficient`` reconciles to the
    ZONE-suffixed SSCC provision the BESS emitter actually produced.

    The bare ``provision_BAT_X`` name is NOT in ``emitted_names`` (the
    SSCC emitter only ever emits ``provision_BAT_X_gen__<ZONE>``).  The
    converter must route the term to the matching zone-suffixed name
    using the ``CSF_LW_`` constraint-name prefix (lower = down-reserve
    → ``.dn``) — NOT silently drop it into the degenerate form.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_RP_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    # Mirror the real converter: the bare ``provision_BAT_X`` is absent,
    # only the zone-suffixed SSCC provision exists in emitted_names.
    allow = {
        "Battery": frozenset({"BAT_X"}),
        "ReserveProvision": frozenset({"provision_BAT_X_gen__CSF_LW_BESS"}),
    }
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    by_name = {c.name: c for c in out}
    assert "CSF_LW_MIN_BAT_X" in by_name
    expr = by_name["CSF_LW_MIN_BAT_X"].expression
    # The term resolves to the zone-suffixed provision with the down
    # accessor (LW = lower = down-reserve).
    assert 'reserve_provision("provision_BAT_X_gen__CSF_LW_BESS").dn' in expr
    # NOT dropped: the plain (never-emitted) name must not appear, and
    # the constraint is not the degenerate empty / commitment-only form.
    assert 'reserve_provision("provision_BAT_X")' not in expr


def test_extract_user_constraints_unmappable_ref_fails_hard(
    tmp_path: Path,
) -> None:
    """Fix 2: an unreconcilable synthesized-name reference FAILS HARD.

    Same battery ``Reserve Provision Coefficient`` row, but the
    constraint name carries NO recognised ``C[SP]F_(LW|RS)_`` prefix, so
    no zone reconciliation applies, and the zone-suffixed provision is
    absent from ``emitted_names``.  The term cannot resolve — the
    converter must raise ``UnresolvedConstraintReferenceError`` listing
    the bad reference (with the closest emitted name as a hint), NEVER
    silently drop the term or the whole constraint.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    # Rename the constraint to a non-CSF/CPF name so no prefix matches.
    xml_path.write_text(_BATT_RP_XML.replace("CSF_LW_MIN_BAT_X", "MISC_MIN_BAT_X"))
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Battery": frozenset({"BAT_X"}),
        # A near-miss name is emitted so the hint can suggest it.
        "ReserveProvision": frozenset({"provision_BAT_X_gen__CSF_LW_BESS"}),
    }
    with pytest.raises(UnresolvedConstraintReferenceError) as excinfo:
        extract_user_constraints(db, bundle, emitted_names=allow)
    msg = str(excinfo.value)
    assert "MISC_MIN_BAT_X" in msg
    assert 'reserve_provision("provision_BAT_X")' in msg
    # Closest-emitted-name hint surfaces the zone-suffixed candidate.
    assert "provision_BAT_X_gen__CSF_LW_BESS" in msg


# ---------------------------------------------------------------------------
# T5: AGGREGATE battery reserve constraints (CPF_*MinProvision /
# UP/DOWNStorageBound_*) → zone-suffixed SSCC provision SUM.
#
# Unlike the per-battery ``CSF_LW_MIN_BAT_<bat>`` row (T4), these carry the
# DIRECTION in the coefficient KIND (``Regulation Raise`` / ``Lower``) — NOT
# a name prefix — and may reference SEVERAL batteries on one constraint.
# ``CPF_DownMinProvision`` (type CPF, Lower) routes each battery to its
# ``CPF_LW_BESS.dn`` provision; ``UP/DOWNStorageBound_BAT_X`` carry NO
# CPF/CSF type token, so a single battery term fans out to a SUM over BOTH
# reserve types (``CPF_<dir>_BESS`` + ``CSF_<dir>_BESS``).
# ---------------------------------------------------------------------------


_BATT_AGG_CPF_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>7</class_id><name>Battery</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>40</object_id><class_id>7</class_id><name>BAT_A</name></t_object>
  <t_object><object_id>41</object_id><class_id>7</class_id><name>BAT_B</name></t_object>
  <t_object><object_id>42</object_id><class_id>7</class_id><name>BAT_C</name></t_object>
  <t_object>
    <object_id>100</object_id><class_id>70</class_id><name>CPF_DownMinProvision</name>
  </t_object>

  <t_collection>
    <collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
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
    <property_id>982</property_id><collection_id>90</collection_id>
    <name>Regulation Lower Reserve Provision Coefficient</name>
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
  <t_membership>
    <membership_id>90002</membership_id>
    <collection_id>90</collection_id>
    <parent_object_id>41</parent_object_id>
    <child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>90003</membership_id>
    <collection_id>90</collection_id>
    <parent_object_id>42</parent_object_id>
    <child_object_id>100</child_object_id>
  </t_membership>

  <t_data>
    <data_id>1</data_id><membership_id>700001</membership_id>
    <property_id>4369</property_id><value>1</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>0</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>90001</membership_id>
    <property_id>982</property_id><value>0.25</value>
  </t_data>
  <t_data>
    <data_id>4</data_id><membership_id>90002</membership_id>
    <property_id>982</property_id><value>0.30</value>
  </t_data>
  <t_data>
    <data_id>5</data_id><membership_id>90003</membership_id>
    <property_id>982</property_id><value>0.35</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_user_constraints_aggregate_cpf_down_min_provision(
    tmp_path: Path,
) -> None:
    """``CPF_DownMinProvision`` (Regulation Lower, CPF type) routes each
    battery's coefficient to its ``CPF_LW_BESS.dn`` SSCC provision.

    Direction comes from the coefficient KIND (Lower → ``.dn``); the CPF
    type token in the name restricts to ``CPF_*`` zones (so NO CSF term
    appears).  Two batteries on one constraint → two resolved terms.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_AGG_CPF_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Battery": frozenset({"BAT_A", "BAT_B", "BAT_C"}),
        "ReserveProvision": frozenset(
            {
                "provision_BAT_A_gen__CPF_LW_BESS",
                "provision_BAT_A_gen__CPF_RS_BESS",
                "provision_BAT_A_gen__CSF_LW_BESS",
                "provision_BAT_A_gen__CSF_RS_BESS",
                "provision_BAT_B_gen__CPF_LW_BESS",
                "provision_BAT_B_gen__CPF_RS_BESS",
                "provision_BAT_B_gen__CSF_LW_BESS",
                "provision_BAT_B_gen__CSF_RS_BESS",
                "provision_BAT_C_gen__CPF_LW_BESS",
                "provision_BAT_C_gen__CPF_RS_BESS",
                "provision_BAT_C_gen__CSF_LW_BESS",
                "provision_BAT_C_gen__CSF_RS_BESS",
            }
        ),
    }
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    by_name = {c.name: c for c in out}
    assert "CPF_DownMinProvision" in by_name
    spec = by_name["CPF_DownMinProvision"]
    expr = spec.expression
    # All three batteries resolve to the CPF lower (down) zone provision.
    assert 'reserve_provision("provision_BAT_A_gen__CPF_LW_BESS").dn' in expr
    assert 'reserve_provision("provision_BAT_B_gen__CPF_LW_BESS").dn' in expr
    assert 'reserve_provision("provision_BAT_C_gen__CPF_LW_BESS").dn' in expr
    # CPF type token in the name ⇒ NO CSF zone term, and the bare
    # never-emitted name is gone.
    assert "CSF_LW_BESS" not in expr
    assert "_RS_BESS" not in expr  # Lower kind ⇒ down only
    assert 'reserve_provision("provision_BAT_A").' not in expr

    # As of the data-driven HARD-promote work, ``CPF_DownMinProvision``
    # is one of the 90 names listed in
    # ``data/cen_pcp_hard_ucs.txt`` (PLEXOS-HARD per RES20260422 audit:
    # Slack=0, HrsBind=30, Shadow=$232).  The HARD-promote branch fires
    # BEFORE ``is_reserve_provision_sum`` so the spec ends up with
    # penalty=0 and no directive stamped.  This matches PLEXOS's
    # solving behaviour (binds tight at zero slack).  When the
    # constraint is REMOVED from the hard-list (or the list cannot be
    # loaded), it falls through to the ``reserve_prov_sum`` $1000 soft
    # default — covered by other tests in this file.
    assert spec.penalty == 0.0
    assert spec.directive is None


_BATT_STORAGEBOUND_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>7</class_id><name>Battery</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>40</object_id><class_id>7</class_id><name>BAT_X</name></t_object>
  <t_object>
    <object_id>100</object_id><class_id>70</class_id>
    <name>UPStorageBound_BAT_X</name>
  </t_object>

  <t_collection>
    <collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
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
    <property_id>983</property_id><collection_id>90</collection_id>
    <name>Regulation Raise Reserve Provision Coefficient</name>
  </t_property>
  <t_property>
    <property_id>984</property_id><collection_id>90</collection_id>
    <name>Energy Coefficient</name>
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
    <property_id>4384</property_id><value>0</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>90001</membership_id>
    <property_id>983</property_id><value>1.0</value>
  </t_data>
  <t_data>
    <data_id>4</data_id><membership_id>90001</membership_id>
    <property_id>984</property_id><value>-0.98</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_user_constraints_storagebound_fans_out_both_zones(
    tmp_path: Path,
) -> None:
    """``UPStorageBound_BAT_X`` (Regulation Raise + Energy, NO type token).

    Direction = Raise → ``.up``; no CPF/CSF token ⇒ the single battery
    reserve term fans out to a SUM over BOTH reserve types
    (``CPF_RS_BESS.up`` + ``CSF_RS_BESS.up``).  The ``Energy Coefficient``
    resolves to ``battery(BAT_X).energy`` (the battery itself IS emitted).
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_STORAGEBOUND_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Battery": frozenset({"BAT_X"}),
        "ReserveProvision": frozenset(
            {
                "provision_BAT_X_gen__CPF_LW_BESS",
                "provision_BAT_X_gen__CPF_RS_BESS",
                "provision_BAT_X_gen__CSF_LW_BESS",
                "provision_BAT_X_gen__CSF_RS_BESS",
            }
        ),
    }
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    by_name = {c.name: c for c in out}
    assert "UPStorageBound_BAT_X" in by_name
    expr = by_name["UPStorageBound_BAT_X"].expression
    # Raise (up) fans out to BOTH reserve-type zones.
    assert 'reserve_provision("provision_BAT_X_gen__CPF_RS_BESS").up' in expr
    assert 'reserve_provision("provision_BAT_X_gen__CSF_RS_BESS").up' in expr
    # No down-direction term (Raise kind only).
    assert "_LW_BESS" not in expr
    # The Energy Coefficient resolves to the battery's own energy column.
    assert 'battery("BAT_X").energy' in expr


def test_extract_user_constraints_storagebound_not_sscc_eligible_fails_hard(
    tmp_path: Path,
) -> None:
    """A StorageBound battery with NO emitted matching-direction provision
    is genuinely unmappable → FAIL HARD (no silent drop).

    Same UP StorageBound, but ``emitted_names['ReserveProvision']`` is
    empty (battery not SSCC-eligible).  ``_bess_matching_provisions``
    returns nothing, so the reserve term cannot resolve and the converter
    must raise ``UnresolvedConstraintReferenceError``.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_STORAGEBOUND_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Battery": frozenset({"BAT_X"}),
        "ReserveProvision": frozenset(),  # not SSCC-eligible
    }
    with pytest.raises(UnresolvedConstraintReferenceError) as excinfo:
        extract_user_constraints(db, bundle, emitted_names=allow)
    assert "UPStorageBound_BAT_X" in str(excinfo.value)


# ---------------------------------------------------------------------------
# T6: Battery commitment recognition — ``uc_<bat>_gen`` is a valid
# Commitment reference (gtopt's ``expand_batteries`` creates it
# unconditionally for EVERY battery).  A battery ``Reserve Units
# Coefficient`` forwards to ``commitment("uc_<bat>_gen").status``.
# ---------------------------------------------------------------------------


def test_extract_user_constraints_battery_commitment_recognized_with_allow(
    tmp_path: Path,
) -> None:
    """``Battery.Reserve Units`` resolves against an ``emitted_names`` set
    that lists the synthetic ``uc_<bat>_gen`` commitment.

    Reuses the T3 fixture but supplies the real allow-list shape: the
    battery commitment ``uc_bess_a_gen`` is present in
    ``emitted_names['Commitment']`` (mirroring the converter, which adds
    ``uc_<bat>_gen`` for every battery), so the forwarded term resolves
    rather than tripping the fail-hard.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BATT_RU_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Battery": frozenset({"bess_a"}),
        "Commitment": frozenset({"uc_bess_a_gen"}),
    }
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    by_name = {c.name: c for c in out}
    assert "BATT_RU" in by_name
    expr = by_name["BATT_RU"].expression
    assert 'commitment("uc_bess_a_gen").status' in expr


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


def test_extract_user_constraints_rhs_custom_transform(tmp_path: Path) -> None:
    """F2: a constraint with NO plain ``RHS`` but a ``RHS Custom`` value
    (the daily gas-offtake ``Gas_MaxOpDay*`` pattern) is emitted with
    ``RHS = RHS_Custom × 1000 / horizon_hours`` (PLEXOS's custom-time-
    period evaluation, verified ratio 1000/168 on CEN PCP) instead of
    being dropped for a missing RHS.
    """
    xml = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object>
    <object_id>100</object_id><class_id>70</class_id><name>Gas_MaxOpDay0_X</name>
  </t_object>
  <t_collection>
    <collection_id>700</collection_id><parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>32</collection_id><parent_class_id>2</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_property>
    <property_id>4369</property_id><collection_id>700</collection_id><name>Sense</name>
  </t_property>
  <t_property>
    <property_id>4384</property_id><collection_id>700</collection_id><name>RHS</name>
  </t_property>
  <t_property>
    <property_id>4390</property_id><collection_id>700</collection_id>
    <name>RHS Custom</name>
  </t_property>
  <t_property>
    <property_id>393</property_id><collection_id>32</collection_id>
    <name>Generation Coefficient</name>
  </t_property>
  <t_membership>
    <membership_id>700001</membership_id><collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32001</membership_id><collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_data>
    <data_id>1</data_id><membership_id>700001</membership_id>
    <property_id>4369</property_id><value>-1</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>700001</membership_id>
    <property_id>4390</property_id><value>2.0</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>32001</membership_id>
    <property_id>393</property_id><value>1.0</value>
  </t_data>
</MasterDataSet>
"""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(xml)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)  # n_days=1 → 24 h
    db = load_xml(xml_path)
    out = extract_user_constraints(db, bundle)
    by_name = {c.name: c for c in out}
    # Post-2026-05-29: ``Gas_MaxOpDay**X**_<group>`` per-block specs are
    # consolidated by ``_consolidate_gas_maxopday_groups`` into ONE
    # ``Gas_MaxOpDay_<group>`` daily_sum+energy UC per (fuel,owner) group
    # — matches PLEXOS's per-day cumulative scope evaluation.  See the
    # docstring on ``_consolidate_gas_maxopday_groups`` for the rationale.
    # For a single-day test bundle (items_count=1, horizon_days=1) the
    # consolidator's offset is 0, so PLEXOS Day0 maps directly to gtopt
    # day 0 with RHS = 2.0 × 1000 = 2000 GJ (the daily-total cap; not
    # divided by horizon_hours since daily_sum aggregates per day).
    assert "Gas_MaxOpDay_X" in by_name, by_name.keys()
    spec = by_name["Gas_MaxOpDay_X"]
    assert 'generator("G1").generation' in spec.expression
    assert spec.daily_sum is True
    assert spec.constraint_type == "energy"
    # Per-block RHS profile broadcasts the per-day total to every block in
    # the day (gtopt's daily_sum picks the cap from the day-ending block).
    # For 24 blocks of day 0, RHS at each = 2000 (the consolidated total).
    assert spec.rhs_profile, "consolidator must emit per-block RHS profile"
    # Recovery of the per-day-total RHS from the per-block expression
    # tail goes through string-formatted truncation (`<= 83.3333`),
    # so allow ~1e-3 relative tolerance.
    assert all(abs(v - 2000.0) < 1e-2 for v in spec.rhs_profile), (
        f"expected per-block RHS ≈ 2000 GJ everywhere on day 0, "
        f"got distinct values: {sorted(set(spec.rhs_profile))}"
    )
    # Description carries the consolidator's audit trail.
    desc = spec.description
    assert "consolidated" in desc.lower()
    assert "(File: DBSEN_PRGDIARIO.xml)" in desc or "PLEXOS" in desc
    # Soft at $1000/MWh — same tier as reserve_provision_sum.  PLEXOS
    # itself takes slack on these caps (Day1/Day4/Day6 in RES20260422)
    # so going genuinely hard would risk presolve infeasibility from the
    # internal-Big-M PLEXOS uses but we cannot reproduce.
    assert spec.penalty == 1000.0
    # Step 4b (#54): typed ``daily_budget`` directive carries the
    # constraint-family classification + the PLEXOS fuel-owner scope.
    # The directive is what any downstream JSON consumer / gtopt-side
    # ``UserConstraint::directive`` reads — name-regex detection on
    # ``Gas_MaxOpDay*`` is no longer required to identify this family.
    assert spec.directive is not None
    assert spec.directive.kind == "daily_budget"
    assert spec.directive.scope == "gas_maxopday:X"


# ---------------------------------------------------------------------------
# write_user_constraint_pampl — hard / soft / off modes (--pampl-uc-mode)
# ---------------------------------------------------------------------------


def _sample_ucs() -> list[dict[str, object]]:
    """Two scalar rows (one hard, one already-soft) + one per-block-RHS row."""
    return [
        {"name": "FORCE_ON_A", "expression": 'generator("G1").commitment == 1'},
        {
            "name": "RESERVE_B",
            "expression": 'generator("G2").generation >= 5',
            "penalty": 250.0,
        },
        {
            "name": "Gas_MaxOpDay_C",
            "expression": 'generator("G3").generation',
            "rhs": [10.0, 20.0],
        },
    ]


def test_pampl_uc_mode_off_drops_everything(tmp_path: Path) -> None:
    files, remaining = write_user_constraint_pampl(_sample_ucs(), tmp_path, mode="off")
    assert not files
    assert not remaining
    assert not list(tmp_path.glob("*.pampl"))


def test_pampl_uc_mode_soft_forces_penalty_on_every_row(tmp_path: Path) -> None:
    ucs = _sample_ucs()
    files, remaining = write_user_constraint_pampl(
        ucs, tmp_path, mode="soft", force_penalty=9999.0
    )
    assert files  # scalar rows emitted to per-family files
    # the hard row got the forced penalty; the already-soft row kept its own
    by_name = {u["name"]: u for u in ucs}
    assert by_name["FORCE_ON_A"]["penalty"] == 9999.0
    assert by_name["RESERVE_B"]["penalty"] == 250.0
    # the per-block-RHS row stays inline but is also softened
    assert by_name["Gas_MaxOpDay_C"]["penalty"] == 9999.0
    assert len(remaining) == 1
    # every emitted scalar row references a penalty tier
    text = "\n".join(p.read_text() for p in tmp_path.glob("*.pampl"))
    assert "penalty" in text


def test_pampl_uc_mode_hard_keeps_rows_unpenalised(tmp_path: Path) -> None:
    ucs = _sample_ucs()
    files, remaining = write_user_constraint_pampl(ucs, tmp_path, mode="hard")
    assert files
    by_name = {u["name"]: u for u in ucs}
    # hard row keeps NO penalty; pre-soft row keeps its own
    assert "penalty" not in by_name["FORCE_ON_A"]
    assert by_name["RESERVE_B"]["penalty"] == 250.0
    assert len(remaining) == 1


def test_pampl_per_block_rhs_matrix_emitted_as_pampl_clause(
    tmp_path: Path,
) -> None:
    """A per-block RHS in TB-matrix form (``[[v0, v1, ...]]``) now round-trips
    through ``.pampl`` via the ``rhs [...]`` header clause instead of being
    pushed to the JSON remainder."""
    ucs: list[dict[str, object]] = [
        {
            "name": "RALCOramp_max_e1",
            "expression": 'generator("RALCO").generation <= 0',
            "rhs": [[40.0, 40.0, 60.0, 60.0]],
        },
    ]
    files, remaining = write_user_constraint_pampl(ucs, tmp_path, mode="hard")
    # It is NOT left inline in the JSON any more.
    assert not remaining
    assert files
    text = "\n".join(p.read_text() for p in tmp_path.glob("*.pampl"))
    assert "RALCOramp_max_e1" in text
    # The header carries the scheduled-RHS clause with each block value.
    assert "rhs [40, 40, 60, 60]" in text
    # The inline scalar fallback survives in the expression body.
    assert "<= 0" in text


def test_pampl_scalar_or_string_rhs_stays_inline(tmp_path: Path) -> None:
    """RHS shapes with no ``.pampl`` encoding (flat list, scalar, string,
    multi-stage matrix) keep being pushed to the JSON remainder."""
    ucs: list[dict[str, object]] = [
        {"name": "A", "expression": "x <= 0", "rhs": [10.0, 20.0]},  # flat
        {"name": "B", "expression": "y <= 0", "rhs": 5.0},  # scalar
        {"name": "C", "expression": "z <= 0", "rhs": "rhs_field"},  # string
        {
            "name": "D",
            "expression": "w <= 0",
            "rhs": [[1.0], [2.0]],  # multi-stage matrix
        },
    ]
    _, remaining = write_user_constraint_pampl(ucs, tmp_path, mode="hard")
    assert {u["name"] for u in remaining} == {"A", "B", "C", "D"}


def _family_ucs() -> list[dict[str, object]]:
    """Rows mapping to three distinct families (config / security / reserve)."""
    return [
        {"name": "TG1_Uniq", "expression": "a <= 1"},  # config_exclusivity
        {"name": "SD_L1", "expression": "b <= 2"},  # security
        {"name": "CTF_North", "expression": "c >= 3"},  # reserve
    ]


def test_pampl_uc_only_keeps_single_family(tmp_path: Path) -> None:
    files, _ = write_user_constraint_pampl(
        _family_ucs(), tmp_path, only=frozenset({"reserve"})
    )
    assert files == ["uc_reserve.pampl"]
    assert [p.name for p in tmp_path.glob("*.pampl")] == ["uc_reserve.pampl"]


def test_pampl_uc_off_drops_named_family(tmp_path: Path) -> None:
    _files, _ = write_user_constraint_pampl(
        _family_ucs(), tmp_path, off=frozenset({"security"})
    )
    names = sorted(p.name for p in tmp_path.glob("*.pampl"))
    assert "uc_security.pampl" not in names
    assert "uc_config_exclusivity.pampl" in names
    assert "uc_reserve.pampl" in names


def test_filter_user_constraints_shared_helper() -> None:
    ucs = _family_ucs()  # config_exclusivity, security, reserve
    assert len(filter_user_constraints(ucs)) == 3  # hard keeps all
    assert not filter_user_constraints(ucs, mode="off")
    assert [
        u["name"] for u in filter_user_constraints(ucs, only=frozenset({"reserve"}))
    ] == ["CTF_North"]
    assert {
        u["name"] for u in filter_user_constraints(ucs, off=frozenset({"security"}))
    } == {
        "TG1_Uniq",
        "CTF_North",
    }
    soft = filter_user_constraints(
        [{"name": "X_Uniq", "expression": "a <= 1"}], mode="soft", force_penalty=42.0
    )
    assert soft[0]["penalty"] == 42.0


# ── No-limit line-security sentinel drop (SD_* 100000) ──────────────────────


def test_is_nolimit_line_sentinel() -> None:
    """MW-aggregate LHS at/above either sentinel tier → drop.

    PLEXOS uses two no-limit magnitudes on CEN PCP:
      * ``100000`` MW — "hard sentinel" (~half of contingency rows)
      * ``10000`` MW — "soft sentinel": 259 pure-line-flow
        (``SDCF_Rx*``, ``SD_*`` contingency rows) + ~30 pure-generator
        corridor-flow-proxy rows (``Gx_Colbun_Ancoa``, ``ANGmax`` …) +
        20 MIXED gen+reserve-provision aggregates (``KELAR_Max_Operativo``,
        ``ATA_Max_Operativo`` — pattern ``Σ gen + 2 Σ provision ≤ 10000``)

    The detector accepts ANY combination of MW-magnitude kinds
    (``line``, ``generator``, ``reserve_provision``, ``battery``) and
    rejects any LHS that mentions commitment binaries, decision
    variables, or fuel.offtake — those are not MW-aggregates and may
    encode real structural semantics.
    """
    from plexos2gtopt.parsers import _is_nolimit_line_sentinel

    pure_line = 'line("L1").flow + line("L2").flow'
    pure_gen = 'generator("G1").generation + generator("G2").generation'
    pure_provision = (
        '2 * reserve_provision("p_G1").up + 2 * reserve_provision("p_G2").up'
    )
    mixed_gen_provision = (
        'generator("G1").generation + 2 * reserve_provision("p_G1").up'
    )
    line_plus_battery = 'line("L1").flow + battery("B1").discharge'

    # All MW-aggregate LHS at either sentinel tier → dropped.
    for expr in (
        pure_line,
        pure_gen,
        pure_provision,
        mixed_gen_provision,
        line_plus_battery,
    ):
        assert _is_nolimit_line_sentinel(expr, 100000.0) is True, expr
        assert _is_nolimit_line_sentinel(expr, 10000.0) is True, expr
        assert _is_nolimit_line_sentinel(expr, -10000.0) is True, expr  # abs()
    # Below the soft sentinel → kept (real cap).
    for expr in (pure_line, pure_gen, pure_provision, mixed_gen_provision):
        assert _is_nolimit_line_sentinel(expr, 2000.0) is False, expr
        assert _is_nolimit_line_sentinel(expr, 9999.99) is False, expr
    # ANY of (commitment / decision_variable / fuel) bypasses the check.
    for other in (
        'commitment("uc_G").status',
        'decision_variable("DV1").value',
        'fuel("GAS").offtake',
    ):
        assert _is_nolimit_line_sentinel(other, 100000.0) is False, (
            f"sentinel must bypass for {other!r}"
        )
        # MW-mixed with one of these still bypasses
        assert (
            _is_nolimit_line_sentinel(f'line("L1").flow + {other}', 100000.0) is False
        )


# ---------------------------------------------------------------------------
# Fix A — PLEXOS default Sense=`=` when only RHS is set
# ---------------------------------------------------------------------------

_UC_NO_SENSE_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>22</class_id><name>b_a</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object><object_id>21</object_id><class_id>2</class_id><name>G2</name></t_object>
  <!-- ``_ConfTGA``-style equality linker: ``+G1 -G2 = 0`` (Sense
       intentionally omitted; PLEXOS defaults it to equality). -->
  <t_object><object_id>100</object_id><class_id>70</class_id><name>CONF_LINK_NO_SENSE</name></t_object>
  <!-- Control: same structure but with an explicit Sense=0. -->
  <t_object><object_id>101</object_id><class_id>70</class_id><name>CONF_LINK_EXPLICIT</name></t_object>

  <t_collection><collection_id>700</collection_id><parent_class_id>1</parent_class_id><child_class_id>70</child_class_id><name>Constraints</name></t_collection>
  <t_collection><collection_id>32</collection_id><parent_class_id>2</parent_class_id><child_class_id>70</child_class_id><name>Constraints</name></t_collection>

  <t_property><property_id>4369</property_id><collection_id>700</collection_id><name>Sense</name></t_property>
  <t_property><property_id>4384</property_id><collection_id>700</collection_id><name>RHS</name></t_property>
  <t_property><property_id>4393</property_id><collection_id>700</collection_id><name>Penalty Price</name></t_property>
  <t_property><property_id>393</property_id><collection_id>32</collection_id><name>Generation Coefficient</name></t_property>

  <t_membership><membership_id>700001</membership_id><collection_id>700</collection_id><parent_object_id>1</parent_object_id><child_object_id>100</child_object_id></t_membership>
  <t_membership><membership_id>700002</membership_id><collection_id>700</collection_id><parent_object_id>1</parent_object_id><child_object_id>101</child_object_id></t_membership>
  <t_membership><membership_id>32001</membership_id><collection_id>32</collection_id><parent_object_id>20</parent_object_id><child_object_id>100</child_object_id></t_membership>
  <t_membership><membership_id>32002</membership_id><collection_id>32</collection_id><parent_object_id>21</parent_object_id><child_object_id>100</child_object_id></t_membership>
  <t_membership><membership_id>32003</membership_id><collection_id>32</collection_id><parent_object_id>20</parent_object_id><child_object_id>101</child_object_id></t_membership>
  <t_membership><membership_id>32004</membership_id><collection_id>32</collection_id><parent_object_id>21</parent_object_id><child_object_id>101</child_object_id></t_membership>

  <!-- CONF_LINK_NO_SENSE: ONLY RHS set, no Sense (the bug-reproducer). -->
  <t_data><data_id>10001</data_id><membership_id>700001</membership_id><property_id>4384</property_id><value>0</value></t_data>
  <!-- CONF_LINK_EXPLICIT: RHS + Sense=0 (control). -->
  <t_data><data_id>10002</data_id><membership_id>700002</membership_id><property_id>4384</property_id><value>0</value></t_data>
  <t_data><data_id>10003</data_id><membership_id>700002</membership_id><property_id>4369</property_id><value>0</value></t_data>
  <!-- Generation coefficients +1, -1 on each constraint. -->
  <t_data><data_id>20001</data_id><membership_id>32001</membership_id><property_id>393</property_id><value>1.0</value></t_data>
  <t_data><data_id>20002</data_id><membership_id>32002</membership_id><property_id>393</property_id><value>-1.0</value></t_data>
  <t_data><data_id>20003</data_id><membership_id>32003</membership_id><property_id>393</property_id><value>1.0</value></t_data>
  <t_data><data_id>20004</data_id><membership_id>32004</membership_id><property_id>393</property_id><value>-1.0</value></t_data>
</MasterDataSet>
"""


def test_extract_user_constraints_no_sense_defaults_to_equality(
    tmp_path: Path,
) -> None:
    """Fix A: PLEXOS Constraint with RHS set but no Sense → emit as ``=``.

    On CEN PCP 2026-04-22 the input XML carries 62 PLEXOS-solved
    Constraints with ``RHS = 0`` but no explicit ``Sense`` property
    (families ``ATA_CC_*_ConfTGA*`` / ``KELAR_ConfTG*`` /
    ``NEHUENCO_*_ConfTG*`` / ``SANISIDRO_*_ConfCA*`` / ``BAT_*_CF_GEN_COMP``
    / ``*_CPF_Simmetry`` / ``Inertia_Calculation_e*`` …).  PLEXOS evaluates
    these as equality.  The parser used to drop them silently at the
    ``sense_val is None`` guard; the result was the ATA / KELAR / NEHUENCO
    / SANISIDRO commitment-mutex being broken — 14 plant configs
    simultaneously committed on every block of the MIP solve.

    This test asserts that ``CONF_LINK_NO_SENSE`` (Sense intentionally
    omitted, RHS=0) is rescued and emitted with ``=``, matching the
    ``CONF_LINK_EXPLICIT`` control which carries an explicit Sense=0.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_UC_NO_SENSE_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    out = extract_user_constraints(db, bundle)
    by_name = {c.name: c for c in out}
    assert "CONF_LINK_NO_SENSE" in by_name, (
        "Sense=None + RHS=0 constraint must NOT be silently dropped"
    )
    assert "CONF_LINK_EXPLICIT" in by_name
    expr_rescued = by_name["CONF_LINK_NO_SENSE"].expression
    expr_control = by_name["CONF_LINK_EXPLICIT"].expression
    # Both must emit with `= 0` operator + RHS at the tail.
    assert expr_rescued.endswith("= 0"), (
        f"Rescued constraint must use equality; got {expr_rescued!r}"
    )
    assert expr_control.endswith("= 0")
    # And carry the LHS terms with their coefficients.
    assert 'generator("G1").generation' in expr_rescued
    assert 'generator("G2").generation' in expr_rescued


def test_extract_user_constraints_lax_refs_downgrades_to_warning(
    tmp_path: Path,
    caplog: pytest.LogCaptureFixture,
) -> None:
    """Fix 2A: ``lax_refs=True`` turns unresolved-ref FAIL-HARD into a warning.

    Strict mode (default) raises ``UnresolvedConstraintReferenceError``
    on a single unresolved term.  Lax mode (``--lax-uc-refs`` from the
    CLI, ``lax_refs=True`` from the API) logs the count and continues —
    the offending TERMS are already excluded from the emitted
    expression (``continue`` before the LHS append in the resolver
    loop), so the bundle that lands is internally consistent but the
    rescued constraint may be weaker than PLEXOS's version.
    """
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    allow = {
        "Generator": frozenset({"G1"}),  # G2 missing
        "Line": frozenset({"L1"}),
        "Battery": frozenset({"B1"}),
    }
    stats: dict[str, int] = {}
    # Strict mode raises:
    with pytest.raises(UnresolvedConstraintReferenceError):
        extract_user_constraints(db, bundle, emitted_names=allow)
    # Lax mode returns instead:
    out = extract_user_constraints(
        db, bundle, emitted_names=allow, lax_refs=True, stats_out=stats
    )
    names = {c.name for c in out}
    assert "CORRIDOR_LE" in names
    # G2-term is silently dropped; G1-term survives.
    expr = next(c for c in out if c.name == "CORRIDOR_LE").expression
    assert 'generator("G1").generation' in expr
    assert 'generator("G2").generation' not in expr
    # Counter populated for telemetry.
    assert stats.get("lax_unresolved_dropped", 0) >= 1


def test_extract_user_constraints_no_sense_no_rhs_still_dropped(
    tmp_path: Path,
) -> None:
    """Fix A is bounded: constraints with neither Sense nor RHS stay dropped.

    PLEXOS uses unset-Sense as a default to equality ONLY when an RHS
    field is present.  A truly empty constraint (no Sense, no RHS in any
    field — RHS, RHS Custom, RHS Day) must remain dropped, not promoted
    to ``= None``.
    """
    # Reuse the XML but delete the lone RHS row for CONF_LINK_NO_SENSE
    # (data_id 10001) → constraint now has neither Sense nor RHS.
    xml = _UC_NO_SENSE_XML.replace(
        "<t_data><data_id>10001</data_id><membership_id>700001</membership_id>"
        "<property_id>4384</property_id><value>0</value></t_data>",
        "",
    )
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(xml)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    out = extract_user_constraints(db, bundle)
    names = {c.name for c in out}
    assert "CONF_LINK_NO_SENSE" not in names, (
        "no-Sense-no-RHS constraint must still be dropped"
    )
    assert "CONF_LINK_EXPLICIT" in names


# ---------------------------------------------------------------------------
# Fix 5 — PLEXOS BAT_*_AUX redirect (dropped auxiliary battery → main)
# ---------------------------------------------------------------------------

_UC_AUX_REDIRECT_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>7</class_id><name>Battery</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>22</class_id><name>b_a</name></t_object>
  <t_object><object_id>40</object_id><class_id>7</class_id><name>BAT_TEST</name></t_object>
  <t_object><object_id>41</object_id><class_id>7</class_id><name>BAT_TEST_AUX</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id><name>BAT_TEST_CF_GEN_COMP</name></t_object>

  <t_collection><collection_id>700</collection_id><parent_class_id>1</parent_class_id><child_class_id>70</child_class_id><name>Constraints</name></t_collection>
  <t_collection><collection_id>90</collection_id><parent_class_id>7</parent_class_id><child_class_id>70</child_class_id><name>Constraints</name></t_collection>

  <t_property><property_id>4369</property_id><collection_id>700</collection_id><name>Sense</name></t_property>
  <t_property><property_id>4384</property_id><collection_id>700</collection_id><name>RHS</name></t_property>
  <t_property><property_id>968</property_id><collection_id>90</collection_id><name>Generation Coefficient</name></t_property>

  <t_membership><membership_id>700001</membership_id><collection_id>700</collection_id><parent_object_id>1</parent_object_id><child_object_id>100</child_object_id></t_membership>
  <!-- LHS term references the AUX battery (which we tell the parser is dropped). -->
  <t_membership><membership_id>90001</membership_id><collection_id>90</collection_id><parent_object_id>41</parent_object_id><child_object_id>100</child_object_id></t_membership>

  <t_data><data_id>10001</data_id><membership_id>700001</membership_id><property_id>4369</property_id><value>0</value></t_data>
  <t_data><data_id>10002</data_id><membership_id>700001</membership_id><property_id>4384</property_id><value>0</value></t_data>
  <t_data><data_id>20001</data_id><membership_id>90001</membership_id><property_id>968</property_id><value>-1.0</value></t_data>
</MasterDataSet>
"""


def test_extract_user_constraints_aux_battery_redirect(tmp_path: Path) -> None:
    """Fix 5: ``BAT_<name>_AUX`` LHS refs redirect to the main battery.

    PLEXOS pairs each real BESS with a virtual ``_AUX`` buffer for its
    reserve-flow composition (``BAT_<name>_CF_GEN_COMP`` /
    ``CF_LOAD_COMP``).  The converter drops these AUX batteries
    (extract_batteries:1870) since gtopt models a single battery
    covering both energy + reserves.  With the AUX dropped from
    ``emitted_names``, the composition constraint's LHS term
    ``battery("BAT_<name>_AUX").discharge`` would trip the strict-
    reference fail-hard.  The parser must redirect ``_AUX`` → main
    (when the main is emitted) so the constraint preserves its
    semantics against the surviving battery's dispatch column.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_UC_AUX_REDIRECT_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    # Simulate the post-drop emitted set: main BAT_TEST is emitted, AUX is not.
    allow = {"Battery": frozenset({"BAT_TEST"})}
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    by_name = {c.name: c for c in out}
    assert "BAT_TEST_CF_GEN_COMP" in by_name, (
        "AUX-only LHS should NOT fail-hard; the term must redirect to the main"
    )
    expr = by_name["BAT_TEST_CF_GEN_COMP"].expression
    assert 'battery("BAT_TEST").discharge' in expr, (
        f"redirect must rewrite AUX → main; got {expr!r}"
    )
    # Activity-flow expansion: BAT_*_CF_GEN_COMP / CF_LOAD_COMP must
    # include BOTH .discharge AND .charge so the constraint binds in
    # either battery operating mode.  Without the .charge leg, the
    # constraint is trivially zero at solar peak when the battery is
    # charging (discharge=0), which broke the PLEXOS-LMP reproduction
    # at the northern BESS buses (Andes220 / MariaElena220) — see
    # the parsers.py docstring near the AUX redirect for the
    # 2026-05-31 audit + PLEXOS sol cross-check.
    assert 'battery("BAT_TEST").charge' in expr, (
        f"BAT_*_CF_GEN_COMP must also emit the complementary "
        f".charge leg (L1 of net dispatch); got {expr!r}"
    )
    assert "_AUX" not in expr, "no AUX ref should survive the redirect"


def test_extract_user_constraints_aux_redirect_only_when_main_emitted(
    tmp_path: Path,
) -> None:
    """Fix 5 is bounded: redirect only fires when the MAIN battery exists.

    If both ``BAT_X`` and ``BAT_X_AUX`` were dropped (atypical, but
    possible), the redirect must NOT fire — there's no main to redirect
    to.  The strict-reference fail-hard fires as before.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_UC_AUX_REDIRECT_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    # No main battery either → redirect must fail-hard.
    allow = {"Battery": frozenset()}
    with pytest.raises(UnresolvedConstraintReferenceError) as excinfo:
        extract_user_constraints(db, bundle, emitted_names=allow)
    msg = str(excinfo.value)
    assert "BAT_TEST_CF_GEN_COMP" in msg
    assert "BAT_TEST_AUX" in msg


def test_extract_user_constraints_cf_gen_comp_emits_activity_flow(
    tmp_path: Path,
) -> None:
    """``BAT_*_CF_GEN_COMP`` must emit BOTH ``.charge`` AND ``.discharge``
    legs (the L1 of net dispatch / activity-flow).

    Empirical PLEXOS sol audit (2026-05-31): at solar peak in northern
    Chile, the 4 BESS plants (DON_HUMBERTO_FV, MANZANO_FV, TOCOPILLA,
    LA_CABANA_EO) are CHARGING — ``main.discharge = 0``, ``main.load >
    0``.  A constraint emitted as ``-1 * battery.discharge + ... >= 0``
    is then trivially zero on the LHS and never binds, letting the LP
    free-ride on the battery's downward reserve provision without any
    operational coupling.

    PLEXOS's actual model uses a synthetic ``BAT_*_AUX`` mirror battery
    whose Generation tracks the main's Load when charging.  The
    activity-flow expansion ``charge + discharge`` is the simplest
    AUX-free reproduction that binds correctly in both operating modes.

    Default sense is ``<=`` (physical UPPER bound on reserve from
    activity).  See the ``--plexos-legacy`` test below for the
    PLEXOS-faithful ``=`` alternative.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_UC_AUX_REDIRECT_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    allow = {"Battery": frozenset({"BAT_TEST"})}
    out = extract_user_constraints(db, bundle, emitted_names=allow)
    by_name = {c.name: c for c in out}

    expr = by_name["BAT_TEST_CF_GEN_COMP"].expression
    # Both legs are present with the same (negative) coefficient.
    assert 'battery("BAT_TEST").discharge' in expr
    assert 'battery("BAT_TEST").charge' in expr
    # Default sense is ``<=`` — physical UPPER bound on reserve from
    # activity (no negative-dual artifact).
    assert "<=" in expr, (
        f"BAT_*_CF_GEN_COMP must emit ``<=`` sense by default "
        f"(physical upper bound); got {expr!r}"
    )
    assert " = " not in expr, (
        f"default mode must NOT emit equality (that's --plexos-legacy "
        f"only — produces negative-dual artifacts); got {expr!r}"
    )
    # No AUX leaks through.
    assert "_AUX" not in expr


def test_extract_user_constraints_cf_gen_comp_plexos_legacy_uses_equality(
    tmp_path: Path,
) -> None:
    """With ``--plexos-legacy`` (i.e. ``plexos_legacy=True``), the
    ``BAT_*_CF_*_COMP`` family flips back to ``=`` (the PLEXOS-faithful
    equality).  Use for LMP-comparison testing where reproducing
    PLEXOS's negative duals at the northern BESS buses matters.

    Mirrors plp2gtopt's ``--plp-legacy`` pattern: a single global
    switch gating PLEXOS-reproduction quirks that diverge from the
    physics-correct default.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_UC_AUX_REDIRECT_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    allow = {"Battery": frozenset({"BAT_TEST"})}
    out = extract_user_constraints(db, bundle, emitted_names=allow, plexos_legacy=True)
    by_name = {c.name: c for c in out}

    expr = by_name["BAT_TEST_CF_GEN_COMP"].expression
    assert 'battery("BAT_TEST").discharge' in expr
    assert 'battery("BAT_TEST").charge' in expr
    # PLEXOS-faithful sense: equality.
    assert " = " in expr, (
        f"--plexos-legacy must emit equality (PLEXOS unset = "
        f"equality on this family); got {expr!r}"
    )
    assert "<=" not in expr, (
        f"--plexos-legacy must NOT keep the physical ``<=``; got {expr!r}"
    )
    assert "_AUX" not in expr


# ---------------------------------------------------------------------------
# Fix 6 — extra inactive-stub paths (sentinel, offline-gen, fuel-no-consumers)
# ---------------------------------------------------------------------------


_SENTINEL_LHS_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>24</class_id><name>Line</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>22</class_id><name>b_a</name></t_object>
  <t_object><object_id>30</object_id><class_id>24</class_id><name>L1</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id><name>SD_contingency_off</name></t_object>

  <t_collection><collection_id>700</collection_id><parent_class_id>1</parent_class_id><child_class_id>70</child_class_id><name>Constraints</name></t_collection>
  <t_collection><collection_id>310</collection_id><parent_class_id>24</parent_class_id><child_class_id>70</child_class_id><name>Constraints</name></t_collection>

  <t_property><property_id>4369</property_id><collection_id>700</collection_id><name>Sense</name></t_property>
  <t_property><property_id>4384</property_id><collection_id>700</collection_id><name>RHS</name></t_property>
  <t_property><property_id>1963</property_id><collection_id>310</collection_id><name>Flow Coefficient</name></t_property>

  <t_membership><membership_id>700001</membership_id><collection_id>700</collection_id><parent_object_id>1</parent_object_id><child_object_id>100</child_object_id></t_membership>
  <t_membership><membership_id>310001</membership_id><collection_id>310</collection_id><parent_object_id>30</parent_object_id><child_object_id>100</child_object_id></t_membership>

  <!-- RHS at the no-limit sentinel (100000) — pure line-flow constraint. -->
  <t_data><data_id>10001</data_id><membership_id>700001</membership_id><property_id>4369</property_id><value>-1</value></t_data>
  <t_data><data_id>10002</data_id><membership_id>700001</membership_id><property_id>4384</property_id><value>100000</value></t_data>
  <t_data><data_id>20001</data_id><membership_id>310001</membership_id><property_id>1963</property_id><value>1.0</value></t_data>
</MasterDataSet>
"""


def test_extract_user_constraints_sentinel_emits_inactive_stub(
    tmp_path: Path,
) -> None:
    """Fix 6: pure line-flow constraint at RHS=100000 sentinel → inactive stub.

    PLEXOS encodes "this SD_* line-security contingency is INACTIVE
    today" via the ``RHS = 100000`` sentinel.  Previously the parser
    silently dropped such constraints via ``_is_nolimit_line_sentinel``,
    leaving them missing from the bundle.  Fix 6 emits a trivial
    inactive stub (``0 <= 0``) carrying the PLEXOS name + a
    description so the constraint shows up in the audit + can be
    activated in a future run where the contingency engages.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_SENTINEL_LHS_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    out = extract_user_constraints(
        db,
        bundle,
        emitted_names={"Line": frozenset({"L1"})},
    )
    by_name = {c.name: c for c in out}
    assert "SD_contingency_off" in by_name, (
        "RHS=sentinel constraint must emit as inactive stub, not be dropped"
    )
    stub = by_name["SD_contingency_off"]
    assert stub.active is False, "sentinel stub must be inactive"
    assert stub.expression == "0 <= 0"
    assert "sentinel" in (stub.description or "").lower()


_OFFLINE_GEN_LHS_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>

  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>22</class_id><name>b_a</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>OFFLINE_GEN</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id><name>Gas_MaxOpDay0_Inf_Tier</name></t_object>

  <t_collection><collection_id>700</collection_id><parent_class_id>1</parent_class_id><child_class_id>70</child_class_id><name>Constraints</name></t_collection>
  <t_collection><collection_id>32</collection_id><parent_class_id>2</parent_class_id><child_class_id>70</child_class_id><name>Constraints</name></t_collection>

  <t_property><property_id>4369</property_id><collection_id>700</collection_id><name>Sense</name></t_property>
  <t_property><property_id>4384</property_id><collection_id>700</collection_id><name>RHS</name></t_property>
  <t_property><property_id>393</property_id><collection_id>32</collection_id><name>Generation Coefficient</name></t_property>

  <t_membership><membership_id>700001</membership_id><collection_id>700</collection_id><parent_object_id>1</parent_object_id><child_object_id>100</child_object_id></t_membership>
  <t_membership><membership_id>32001</membership_id><collection_id>32</collection_id><parent_object_id>20</parent_object_id><child_object_id>100</child_object_id></t_membership>

  <t_data><data_id>10001</data_id><membership_id>700001</membership_id><property_id>4369</property_id><value>-1</value></t_data>
  <t_data><data_id>10002</data_id><membership_id>700001</membership_id><property_id>4384</property_id><value>0</value></t_data>
  <t_data><data_id>20001</data_id><membership_id>32001</membership_id><property_id>393</property_id><value>1.0</value></t_data>
</MasterDataSet>
"""


def test_extract_user_constraints_all_offline_emits_inactive_stub(
    tmp_path: Path,
) -> None:
    """Fix 6: when every LHS term is a fully-offline gen, emit inactive stub.

    Mirrors the CEN PCP ``Gas_MaxOpDay0_Colbun_GNL_INF`` family: PLEXOS
    references the ``_INF`` "infinity tier" generators (CANDELARIA_*,
    NEHUENCO_*) which gtopt emits with ``pmax = 0`` and no
    ``pmax_profile``.  The parser's Fix 5 silently drops the
    individual terms; with all terms dropped the constraint reduces
    to a no-op, and Fix 6 emits a stub instead of silently dropping.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_OFFLINE_GEN_LHS_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    out = extract_user_constraints(
        db,
        bundle,
        emitted_names={"Generator": frozenset({"OFFLINE_GEN"})},
        # Triggers the offline-gen drop (Fix 5 in the direct-coefficient path).
        pmax_by_gen={"OFFLINE_GEN": 0.0},
        pmax_profiles_by_gen={},
    )
    by_name = {c.name: c for c in out}
    assert "Gas_MaxOpDay0_Inf_Tier" in by_name, (
        "all-offline-gen constraint must emit as inactive stub"
    )
    stub = by_name["Gas_MaxOpDay0_Inf_Tier"]
    assert stub.active is False
    assert stub.expression == "0 <= 0"
    assert "offline" in (stub.description or "").lower()


def test_fuel_offtake_no_consumers_emits_inactive_stub(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Fix 6b: ``fuel(X).offtake`` for a fuel with no emitted consumers → stub.

    Mirrors the CEN PCP regression where ``Gas_Colbun_GN_A`` has 8
    consuming gens in PLEXOS XML but the gtopt FuelLP::add_to_lp
    edge case leaves the offtake DV unregistered for some (stage,
    block) cells (see GTOPT_USE_FUEL_OFFTAKE gate).  When the emitted
    set has zero consumers for the fuel, the parser must emit the
    constraint as an inactive stub so the resolver never sees the
    unregistered ``fuel.offtake`` reference.  Requires the modern
    path to be enabled.
    """
    monkeypatch.setenv("GTOPT_USE_FUEL_OFFTAKE", "1")
    bundle, xml_path = (
        PlexosBundle(root=tmp_path, source=tmp_path),
        tmp_path / "DBSEN_PRGDIARIO.xml",
    )
    xml_path.write_text(_OFFTAKE_XML)
    db = load_xml(xml_path)
    # Empty Generator allow-list → fuel "GAS" has zero emitted consumers.
    out = extract_user_constraints(
        db,
        bundle,
        emitted_names={
            "Generator": frozenset(),
            "Fuel": frozenset({"GAS"}),
        },
        heat_rate_by_gen={"G1": 0.5, "G2": 0.3},
    )
    by_name = {c.name: c for c in out}
    assert "GAS_CAP" in by_name, (
        "fuel-with-no-emitted-consumers must emit as inactive stub"
    )
    stub = by_name["GAS_CAP"]
    assert stub.active is False
    # Stub form is ``0 <op> 0`` regardless of the original RHS — the
    # constraint is a no-op (LHS contributes 0 unconditionally), so a
    # zero-on-both-sides trivial form suffices.
    assert stub.expression == "0 <= 0", (
        f"stub uses 0 <op> 0 trivial form; got {stub.expression!r}"
    )
    assert "fuel.offtake" in (stub.description or "").lower()


def test_fuel_offtake_legacy_is_default(tmp_path: Path) -> None:
    """The modern ``fuel(X).offtake`` emission is gated OFF by default.

    Without ``GTOPT_USE_FUEL_OFFTAKE=1`` the parser must use the legacy
    per-generator expansion even when Fuel is in ``emitted_names`` —
    pending the gtopt-side FuelLP registration fix.  Guards against an
    accidental flip back to the modern path that would re-introduce the
    CEN PCP regression.
    """
    bundle, xml_path = (
        PlexosBundle(root=tmp_path, source=tmp_path),
        tmp_path / "DBSEN_PRGDIARIO.xml",
    )
    xml_path.write_text(_OFFTAKE_XML)
    db = load_xml(xml_path)
    out = extract_user_constraints(
        db,
        bundle,
        emitted_names={
            "Generator": frozenset({"G1", "G2"}),
            "Fuel": frozenset({"GAS"}),  # fuel emitted; modern path would kick in
        },
        heat_rate_by_gen={"G1": 0.5, "G2": 0.3},
    )
    by_name = {c.name: c for c in out}
    expr = by_name["GAS_CAP"].expression
    # Default = legacy per-gen expansion (heat_rate baked into coefficient).
    assert 'fuel("GAS").offtake' not in expr, (
        f"modern path must NOT fire without env var; got {expr!r}"
    )
    assert '0.5 * generator("G1").generation' in expr
    assert '0.3 * generator("G2").generation' in expr


# ──────────────────────────────────────────────────────────────────────────
# Visible-slack + typed-directive annotations in the .pampl files (Task 3)
# ──────────────────────────────────────────────────────────────────────────
class TestPamplVisibleSlackAnnotations:
    """The PAMPL grammar has no ``var slack_<name>;`` syntax today, so the
    visibility win is delivered as structured comments next to each
    soft constraint (where the LP slack column LIVES) and a directive
    summary line for typed-directive rows.  These tests pin the
    annotations so a future tweak doesn't quietly drop them."""

    def test_soft_constraint_gets_slack_annotation(self, tmp_path):
        ucs = [
            {
                "uid": 1,
                "name": "HYDRO_FLOOR_X",
                "expression": '1 * generator("G").generation >= 50',
                "penalty": 10.0,
                "description": "Test hydro floor",
            },
        ]
        files, _ = write_user_constraint_pampl(ucs, tmp_path)
        assert files == ["uc_operational.pampl"]
        text = (tmp_path / "uc_operational.pampl").read_text()
        # AMPL-style ``var slack_<ident>;`` declaration for each soft UC.
        assert "var slack_HYDRO_FLOOR_X;" in text, (
            f"missing visible-slack declaration in: {text!r}"
        )
        # Per-UC annotation names the matching slack column.
        assert "#   soft: slack column 'slack_HYDRO_FLOOR_X' (per-block;" in text, (
            f"missing visible-slack annotation in: {text!r}"
        )
        assert 'UserConstraint/slack_sol.parquet["HYDRO_FLOOR_X"]' in text, (
            "annotation must include the per-UC slack parquet key"
        )
        # File-header soft/hard tally:
        assert "hard:" in text and "soft:" in text
        # Slack column documentation present in the header banner:
        assert "UserConstraint::slack_name" in text

    def test_hard_constraint_has_no_slack_annotation(self, tmp_path):
        ucs = [
            {
                "uid": 1,
                "name": "BAT_X_CF_GEN_COMP",
                "expression": '1 * battery("B").discharge <= 0',
                "penalty": 0.0,
                "description": "Test hard complementarity",
            },
        ]
        files, _ = write_user_constraint_pampl(ucs, tmp_path)
        assert files
        text = (tmp_path / files[0]).read_text()
        # No slack-column annotation on hard rows:
        assert "soft: slack column" not in text
        # And no ``var slack_*;`` STATEMENT either — only the header-banner
        # description should mention the syntax.  Strip comment lines
        # before checking so the comment example in the file header
        # doesn't trigger a false positive.
        statement_lines = [
            ln for ln in text.splitlines() if ln and not ln.lstrip().startswith("#")
        ]
        body = "\n".join(statement_lines)
        assert "var slack_" not in body, (
            f"hard-only file must not emit a var slack statement, got: {body!r}"
        )

    def test_directive_kind_surfaced_in_comment(self, tmp_path):
        """A ``directive`` payload is rendered as ``#   directive: kind=…``
        so a reader can attribute the soft tier to a family without
        grepping the policy module."""
        ucs = [
            {
                "uid": 1,
                "name": "Gas_MaxOpDayEnel",
                "expression": '1 * generator("G").generation <= 100',
                "penalty": 1000.0,
                "directive": {
                    "kind": "daily_budget",
                    "scope": "gas_maxopday:Enel",
                },
                "description": "gas cap",
                "daily_sum": True,  # ⇒ stays inline; PAMPL has no daily_sum
            },
        ]
        # daily_sum constraints stay inline (no .pampl emission), so add a
        # non-daily-sum copy to exercise the .pampl path.
        ucs.append(
            {
                "uid": 2,
                "name": "X_RegRange_e1",
                "expression": (
                    '1 * commitment("uc_g").status + 1 * generator("g").generation >= 0'
                ),
                "penalty": 1000.0,
                "directive": {"kind": "regrange", "penalty": 1000.0},
                "description": "RegRange test",
            }
        )
        files, _ = write_user_constraint_pampl(ucs, tmp_path)
        all_text = "\n".join((tmp_path / fname).read_text() for fname in files)
        # The non-daily-sum RegRange row writes a directive comment line:
        assert "#   directive: kind=regrange" in all_text, (
            f"expected typed directive annotation in: {all_text!r}"
        )

    def test_directive_with_scope_surfaces_scope(self, tmp_path):
        ucs = [
            {
                "uid": 1,
                "name": "X",
                "expression": '1 * generator("G").generation <= 0',
                "penalty": 100.0,
                "directive": {
                    "kind": "daily_budget",
                    "scope": "fuel:GAS",
                },
            },
        ]
        files, _ = write_user_constraint_pampl(ucs, tmp_path)
        text = (tmp_path / files[0]).read_text()
        assert "directive: kind=daily_budget scope=fuel:GAS" in text

    def test_var_slack_declared_before_constraint(self, tmp_path):
        """``var slack_<ident>;`` must precede every ``constraint <ident>``
        that references it — the PAMPL parser is single-pass and
        constraints look back at the declared-vars set."""
        ucs = [
            {
                "uid": 1,
                "name": "FLOOR_A",
                "expression": '1 * generator("G").generation >= 1',
                "penalty": 10.0,
            },
            {
                "uid": 2,
                "name": "FLOOR_B",
                "expression": '1 * generator("G").generation >= 2',
                "penalty": 10.0,
            },
        ]
        files, _ = write_user_constraint_pampl(ucs, tmp_path)
        text = (tmp_path / files[0]).read_text()
        var_a = text.find("var slack_FLOOR_A;")
        var_b = text.find("var slack_FLOOR_B;")
        con_a = text.find("constraint FLOOR_A ")
        con_b = text.find("constraint FLOOR_B ")
        assert -1 not in (var_a, var_b, con_a, con_b)
        assert var_a < con_a, "var slack_FLOOR_A; must precede constraint FLOOR_A:"
        assert var_b < con_b, "var slack_FLOOR_B; must precede constraint FLOOR_B:"

    def test_var_slack_uses_sanitised_ident(self, tmp_path):
        """When a PLEXOS name contains chars invalid in PAMPL idents
        (``+`` ``-`` space ``.``), the writer sanitises it.  The
        ``var slack_<ident>;`` declaration MUST use the SAME sanitised
        ident as the constraint header so the parser's name-convention
        binding still finds it.
        """
        ucs = [
            {
                "uid": 1,
                "name": "ATA-TG1A+0.5TV1C_DIE",
                "expression": '1 * generator("G").generation >= 1',
                "penalty": 10.0,
            },
        ]
        files, _ = write_user_constraint_pampl(ucs, tmp_path)
        text = (tmp_path / files[0]).read_text()
        # Find the constraint line — the sanitised ident is between
        # "constraint " and " penalty".
        import re

        m = re.search(r"^constraint\s+(\w+)", text, re.MULTILINE)
        assert m is not None, f"no constraint header in: {text!r}"
        ident = m.group(1)
        # The var declaration must use the SAME sanitised ident.
        assert f"var slack_{ident};" in text, (
            f"var declaration must match constraint ident {ident!r} in: {text!r}"
        )


# ──────────────────────────────────────────────────────────────────────────
# Inline JSON path: build_user_constraint_array sets slack_name on soft UCs
# ──────────────────────────────────────────────────────────────────────────
class TestBuildUserConstraintArraySlackName:
    """When a UC is emitted inline (penalty > 0 + daily_sum, per-block
    RHS, etc.), the JSON entry must carry ``slack_name`` so the gtopt
    LP-side picks up the per-UC label even though no PAMPL ``var`` was
    emitted (the .pampl grammar lacks ``daily_sum`` so those UCs stay
    inline by design).
    """

    def test_soft_inline_uc_gets_slack_name(self):
        specs = [
            UserConstraintSpec(
                name="DAILY_GAS_CAP",
                expression='1 * generator("G").generation <= 100',
                penalty=1000.0,
                daily_sum=True,
                constraint_type="energy",
            )
        ]
        arr = build_user_constraint_array(specs)
        assert arr and arr[0]["slack_name"] == "slack_DAILY_GAS_CAP"

    def test_hard_inline_uc_omits_slack_name(self):
        specs = [
            UserConstraintSpec(
                name="HARD_CAP",
                expression='1 * generator("G").generation <= 100',
                penalty=0.0,
            )
        ]
        arr = build_user_constraint_array(specs)
        assert arr and "slack_name" not in arr[0]

    def test_inline_slack_name_uses_sanitised_ident(self):
        """The inline-JSON slack_name must use the same sanitisation rule
        as the PAMPL path (``+`` ``-`` ``.`` → ``_``) so cross-emission-
        path consumers see a single naming convention."""
        specs = [
            UserConstraintSpec(
                name="ATA-TG1A+0.5TV1C_DIE",
                expression='1 * generator("G").generation <= 100',
                penalty=10.0,
            )
        ]
        arr = build_user_constraint_array(specs)
        slack = arr[0].get("slack_name")
        assert slack is not None
        # The sanitised ident replaces every invalid char with _ so
        # the resulting name is a valid PAMPL/AMPL identifier.
        assert slack.startswith("slack_")
        assert all(c.isalnum() or c == "_" for c in slack), (
            f"slack_name must be a valid identifier: {slack!r}"
        )
