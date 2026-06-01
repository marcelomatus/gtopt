"""Unit tests for ``Hydro_AntucoBounds.csv`` → discharge UC extraction.

Validates the fix for bug C2 from the deep PLEXOS audit (task #97):
``Hydro_AntucoBounds.csv`` ships RHS for the multi-unit hydro discharge
constraints ``ANTUCOmin`` / ``ANTUCOmax`` / ``ELTOROmax``, and the
Generator → Constraint memberships in the PLEXOS XML list every member
unit (e.g. ANTUCO_U1 + ANTUCO_U2 for ANTUCOmin).  The extractor must:

  * Read every membership row — even when the parent generator is
    out-of-service day-of (UnitsOut=1, pmax_profile all-zero) — because
    the gtopt UC resolver treats element-known-but-offline references
    as silent-zero contributions, so dropping such terms only obscures
    the audit trail without changing dispatch.
  * Pick the operator from the trailing ``min`` / ``max`` suffix and
    apply the soft / hard penalty consistently.
  * Convert MW → m³/s via ``1 / production_factor`` on every term so
    the LHS units match the CSV RHS (m³/s).
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import GeneratorSpec, TurbineSpec
from plexos2gtopt.parsers import extract_hydro_discharge_user_constraints
from plexos2gtopt.plexos_loader import PlexosBundle
from plexos2gtopt.plexos_xml import NS, load_xml


_ANTUCO_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>2</class_id><name>ANTUCO_U1</name></t_object>
  <t_object><object_id>11</object_id><class_id>2</class_id><name>ANTUCO_U2</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>EL_TORO_U1</name></t_object>
  <t_object><object_id>21</object_id><class_id>2</class_id><name>EL_TORO_U2</name></t_object>
  <t_object><object_id>22</object_id><class_id>2</class_id><name>EL_TORO_U3</name></t_object>
  <t_object><object_id>23</object_id><class_id>2</class_id><name>EL_TORO_U4</name></t_object>
  <t_object><object_id>30</object_id><class_id>70</class_id><name>ANTUCOmin</name></t_object>
  <t_object><object_id>31</object_id><class_id>70</class_id><name>ANTUCOmax</name></t_object>
  <t_object><object_id>32</object_id><class_id>70</class_id><name>ELTOROmax</name></t_object>
  <t_collection>
    <collection_id>32</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>70</child_class_id>
    <name>Constraints</name>
  </t_collection>
  <t_membership>
    <membership_id>100</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>10</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>101</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>11</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>102</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>10</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>103</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>11</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>110</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>32</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>111</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>21</parent_object_id>
    <child_object_id>32</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>112</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>22</parent_object_id>
    <child_object_id>32</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>113</membership_id>
    <collection_id>32</collection_id>
    <parent_object_id>23</parent_object_id>
    <child_object_id>32</child_object_id>
  </t_membership>
</MasterDataSet>
"""


_BOUNDS_CSV = (
    "NAME,YEAR,MONTH,DAY,PERIOD,Value\n"
    "ANTUCOmin,2026,4,7,1,83.3046\n"
    "ANTUCOmax,2026,4,7,1,85.3046\n"
    "ELTOROmax,2026,4,7,1,36.96608\n"
    # Second-day rows that must be collapsed to the first-seen value:
    "ANTUCOmin,2026,4,8,1,82.7531\n"
    "ANTUCOmax,2026,4,8,1,84.7531\n"
)


def _setup_bundle(tmp_path: Path, *, with_csv: bool = True) -> PlexosBundle:
    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text(_ANTUCO_XML)
    if with_csv:
        (tmp_path / "Hydro_AntucoBounds.csv").write_text(_BOUNDS_CSV)
    return PlexosBundle(root=tmp_path, source=tmp_path)


def _turbines() -> tuple[TurbineSpec, ...]:
    return (
        TurbineSpec(
            generator_name="ANTUCO_U1", reservoir_name="POLCURA", production_factor=1.6
        ),
        TurbineSpec(
            generator_name="ANTUCO_U2", reservoir_name="POLCURA", production_factor=1.6
        ),
        TurbineSpec(
            generator_name="EL_TORO_U1",
            reservoir_name="ELTORO",
            production_factor=4.62076,
        ),
        TurbineSpec(
            generator_name="EL_TORO_U2",
            reservoir_name="ELTORO",
            production_factor=4.62076,
        ),
        TurbineSpec(
            generator_name="EL_TORO_U3",
            reservoir_name="ELTORO",
            production_factor=4.62076,
        ),
        TurbineSpec(
            generator_name="EL_TORO_U4",
            reservoir_name="ELTORO",
            production_factor=4.62076,
        ),
    )


def _generators(
    *,
    antuco_u2_offline: bool = False,
    eltoro_u1_offline: bool = False,
) -> tuple[GeneratorSpec, ...]:
    """Build the matching GeneratorSpec list.

    When ``antuco_u2_offline=True`` the U2 generator carries a pmax_profile
    of all zeros (UnitsOut=1 day-of) — the membership row must STILL show
    up in the UC LHS because gtopt resolves the term as a silent zero.
    """
    on_profile = (160.0,) * 24
    off_profile = (0.0,) * 24
    return (
        GeneratorSpec(
            object_id=10,
            name="ANTUCO_U1",
            bus_name="b",
            pmax=160.0,
            pmax_profile=on_profile,
        ),
        GeneratorSpec(
            object_id=11,
            name="ANTUCO_U2",
            bus_name="b",
            pmax=0.0 if antuco_u2_offline else 160.0,
            pmax_profile=off_profile if antuco_u2_offline else on_profile,
        ),
        GeneratorSpec(
            object_id=20,
            name="EL_TORO_U1",
            bus_name="b",
            pmax=0.0 if eltoro_u1_offline else 112.0,
            pmax_profile=off_profile if eltoro_u1_offline else (112.0,) * 24,
        ),
        GeneratorSpec(
            object_id=21,
            name="EL_TORO_U2",
            bus_name="b",
            pmax=112.0,
            pmax_profile=(112.0,) * 24,
        ),
        GeneratorSpec(
            object_id=22,
            name="EL_TORO_U3",
            bus_name="b",
            pmax=113.0,
            pmax_profile=(113.0,) * 24,
        ),
        GeneratorSpec(
            object_id=23,
            name="EL_TORO_U4",
            bus_name="b",
            pmax=113.0,
            pmax_profile=(113.0,) * 24,
        ),
    )


# ── Behaviour tests ────────────────────────────────────────────────────────


def test_no_csv_emits_no_ucs(tmp_path: Path) -> None:
    """No ``Hydro_AntucoBounds.csv`` → empty tuple, no warnings."""
    bundle = _setup_bundle(tmp_path, with_csv=False)
    db = load_xml(bundle.xml_path)
    out = extract_hydro_discharge_user_constraints(
        db,
        bundle,
        _turbines(),
        _generators(),
    )
    assert not out


def test_emits_three_ucs_with_all_members(tmp_path: Path) -> None:
    """All three CSV-named constraints emit, each carrying every membership."""
    bundle = _setup_bundle(tmp_path)
    db = load_xml(bundle.xml_path)
    out = extract_hydro_discharge_user_constraints(
        db,
        bundle,
        _turbines(),
        _generators(),
    )
    names = {uc.name for uc in out}
    assert names == {
        "discharge_ANTUCOmin",
        "discharge_ANTUCOmax",
        "discharge_ELTOROmax",
    }
    by_name = {uc.name: uc for uc in out}

    # ANTUCOmin: both U1 and U2 in LHS, op = ">=", soft penalty 10.
    antuco_min = by_name["discharge_ANTUCOmin"]
    assert 'generator("ANTUCO_U1").generation' in antuco_min.expression
    assert 'generator("ANTUCO_U2").generation' in antuco_min.expression
    assert antuco_min.expression.endswith(">= 83.3046")
    assert antuco_min.penalty == 10.0

    # ANTUCOmax: both U1 and U2 in LHS, op = "<=", HARD (penalty 0).
    antuco_max = by_name["discharge_ANTUCOmax"]
    assert 'generator("ANTUCO_U1").generation' in antuco_max.expression
    assert 'generator("ANTUCO_U2").generation' in antuco_max.expression
    assert antuco_max.expression.endswith("<= 85.3046")
    assert antuco_max.penalty == 0.0

    # ELTOROmax: all four units in LHS, op = "<=", HARD.
    eltoro_max = by_name["discharge_ELTOROmax"]
    for unit in ("EL_TORO_U1", "EL_TORO_U2", "EL_TORO_U3", "EL_TORO_U4"):
        assert f'generator("{unit}").generation' in eltoro_max.expression
    assert eltoro_max.expression.endswith("<= 36.9661")
    assert eltoro_max.penalty == 0.0


def test_coefficients_match_production_factor(tmp_path: Path) -> None:
    """LHS coefficient is ``1 / production_factor`` (MW → m³/s conversion)."""
    bundle = _setup_bundle(tmp_path)
    db = load_xml(bundle.xml_path)
    out = extract_hydro_discharge_user_constraints(
        db,
        bundle,
        _turbines(),
        _generators(),
    )
    by_name = {uc.name: uc for uc in out}

    # ANTUCO pf = 1.6  →  coeff = 0.625
    assert (
        '0.625 * generator("ANTUCO_U1").generation'
        in by_name["discharge_ANTUCOmin"].expression
    )
    assert (
        '0.625 * generator("ANTUCO_U2").generation'
        in by_name["discharge_ANTUCOmin"].expression
    )

    # EL_TORO pf = 4.62076  →  coeff = 0.216415... (printed to 6 sig figs)
    expr_eltoro = by_name["discharge_ELTOROmax"].expression
    assert "0.216415" in expr_eltoro


def test_rhs_is_first_seen_value(tmp_path: Path) -> None:
    """Day-2 RHS rows are collapsed to the day-1 first-seen value."""
    bundle = _setup_bundle(tmp_path)
    db = load_xml(bundle.xml_path)
    out = extract_hydro_discharge_user_constraints(
        db,
        bundle,
        _turbines(),
        _generators(),
    )
    by_name = {uc.name: uc for uc in out}
    # CSV ships ANTUCOmin = 83.3046 on day 1 (kept) and 82.7531 on day 2
    # (collapsed by the first-seen rule).
    assert by_name["discharge_ANTUCOmin"].expression.endswith(">= 83.3046")
    assert by_name["discharge_ANTUCOmax"].expression.endswith("<= 85.3046")


def test_inactive_member_still_in_lhs(tmp_path: Path) -> None:
    """ANTUCO_U2 with all-zero pmax_profile MUST stay in the LHS.

    The pre-fix ``dispatchable`` filter dropped any membership generator
    whose pmax_profile contained a zero block — silently corrupting the
    discharge sum.  gtopt's resolver treats element-known-but-offline
    references as a silent zero (see ``element_column_resolver.hpp``
    ``element_known = true`` branch and ``user_constraint_lp.cpp``
    ``no LP column for this block`` branch), so keeping the term is
    safe and audit-faithful.
    """
    bundle = _setup_bundle(tmp_path)
    db = load_xml(bundle.xml_path)
    out = extract_hydro_discharge_user_constraints(
        db,
        bundle,
        _turbines(),
        _generators(antuco_u2_offline=True, eltoro_u1_offline=True),
    )
    by_name = {uc.name: uc for uc in out}
    # ANTUCO_U2 stays in LHS even though its pmax_profile is all-zero.
    assert (
        'generator("ANTUCO_U2").generation' in by_name["discharge_ANTUCOmin"].expression
    )
    assert (
        'generator("ANTUCO_U2").generation' in by_name["discharge_ANTUCOmax"].expression
    )
    # EL_TORO_U1 stays in LHS too.
    assert (
        'generator("EL_TORO_U1").generation'
        in by_name["discharge_ELTOROmax"].expression
    )


def test_unknown_generator_in_membership_is_dropped(tmp_path: Path) -> None:
    """Membership rows whose parent gen is missing from ``generators`` are
    dropped — those references would trip gtopt's strict resolver.

    Empty ``generators`` ⇒ no eligibility filter ⇒ every PLEXOS membership
    is trusted (back-compat for older callers / test fixtures).
    """
    bundle = _setup_bundle(tmp_path)
    db = load_xml(bundle.xml_path)
    # generators list excludes ANTUCO_U2 — its membership row is now stale.
    pruned_gens = tuple(g for g in _generators() if g.name != "ANTUCO_U2")
    out = extract_hydro_discharge_user_constraints(
        db,
        bundle,
        _turbines(),
        pruned_gens,
    )
    by_name = {uc.name: uc for uc in out}
    assert (
        'generator("ANTUCO_U1").generation' in by_name["discharge_ANTUCOmin"].expression
    )
    assert (
        'generator("ANTUCO_U2").generation'
        not in by_name["discharge_ANTUCOmin"].expression
    )


def test_no_generators_trusts_every_membership(tmp_path: Path) -> None:
    """Empty ``generators`` ⇒ keep every PLEXOS membership row.

    Back-compat for older callers that didn't pass GeneratorSpec; the
    eligibility filter is skipped and the LHS mirrors PLEXOS verbatim.
    """
    bundle = _setup_bundle(tmp_path)
    db = load_xml(bundle.xml_path)
    out = extract_hydro_discharge_user_constraints(
        db,
        bundle,
        _turbines(),
        generators=(),
    )
    by_name = {uc.name: uc for uc in out}
    for unit in ("ANTUCO_U1", "ANTUCO_U2"):
        assert (
            f'generator("{unit}").generation'
            in by_name["discharge_ANTUCOmin"].expression
        )
