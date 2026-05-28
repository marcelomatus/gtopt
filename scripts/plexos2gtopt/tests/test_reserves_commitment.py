"""Unit tests for P5 (reserves) + P6 (commitment) extractors + writers."""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import (
    CommitmentSpec,
    GeneratorSpec,
    ReserveProvisionSpec,
    ReserveSpec,
)
from plexos2gtopt.gtopt_writer import (
    build_commitment_array,
    build_reserve_provision_array,
    build_reserve_zone_array,
)
from plexos2gtopt.parsers import (
    _parse_res_requirement_csv,
    extract_commitments,
    extract_reserve_provisions,
    extract_reserves,
)
from plexos2gtopt.plexos_loader import PlexosBundle
from plexos2gtopt.plexos_xml import NS, load_xml


_RES_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>14</class_id><name>Reserve</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>2</class_id><name>GEN_A</name></t_object>
  <t_object><object_id>11</object_id><class_id>2</class_id><name>GEN_B</name></t_object>
  <t_object><object_id>20</object_id><class_id>14</class_id><name>CSF_RS</name></t_object>
  <t_object><object_id>21</object_id><class_id>14</class_id><name>CSF_LW</name></t_object>
  <t_collection>
    <collection_id>159</collection_id>
    <parent_class_id>14</parent_class_id>
    <child_class_id>2</child_class_id>
    <name>Generators</name>
  </t_collection>
  <t_membership>
    <membership_id>500</membership_id>
    <collection_id>159</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>501</membership_id>
    <collection_id>159</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>11</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>502</membership_id>
    <collection_id>159</collection_id>
    <parent_object_id>21</parent_object_id>
    <child_object_id>10</child_object_id>
  </t_membership>
</MasterDataSet>
"""


def _build_bundle(tmp_path: Path) -> tuple[PlexosBundle, Path]:
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_RES_XML)
    return PlexosBundle(root=tmp_path, source=tmp_path), xml_path


def test_parse_res_requirement_csv(tmp_path: Path) -> None:
    """PATTERN parser pulls H<n> hourly values and filters unknown names."""
    csv_path = tmp_path / "Res_Requirement.csv"
    csv_path.write_text(
        "NAME,PATTERN,VALUE\n"
        'CSF_LW,"DO_1,H1",100\n'
        'CSF_LW,"DO_1,H2",110\n'
        'CSF_LW,"DO_1,H24",120\n'
        'CSF_DownMinProvision,"DO_1,H1",999\n'  # should be filtered out
        'UNKNOWN_RES,"DO_1,H1",50\n'  # not in known set → filtered
    )
    out = _parse_res_requirement_csv(csv_path, frozenset({"CSF_LW"}))
    assert set(out) == {"CSF_LW"}
    assert out["CSF_LW"][0] == 100.0
    assert out["CSF_LW"][1] == 110.0
    assert out["CSF_LW"][23] == 120.0
    # Hours 3..23 unset → zero.
    assert out["CSF_LW"][5] == 0.0


def test_res_timeslice_selects_per_day_pattern(tmp_path: Path) -> None:
    """Res_Timeslice maps each day to the active day-type slice, so the
    per-day requirement varies instead of replicating one 24h pattern."""
    from plexos2gtopt.parsers import _parse_res_timeslice_csv

    ts_path = tmp_path / "Res_Timeslice.csv"
    ts_path.write_text(
        "YEAR,MONTH,DAY,TR_2,SA_2\n"
        "2026,4,22,-1,0\n"  # day 0 → TR_2 (weekday)
        "2026,4,23,0,-1\n"  # day 1 → SA_2 (weekend)
    )
    slices = _parse_res_timeslice_csv(ts_path, n_days=2)
    assert slices == ["TR_2", "SA_2"]

    req_path = tmp_path / "Res_Requirement.csv"
    req_path.write_text(
        "NAME,PATTERN,VALUE\n"
        'CSF_RS,"TR_2,H1",154\n'
        'CSF_RS,"SA_2,H1",135\n'  # weekend value differs
    )
    names = frozenset({"CSF_RS"})
    flat = _parse_res_requirement_csv(req_path, names, n_days=2)
    # Without timeslice: last-wins (135) replicated across both days.
    assert flat["CSF_RS"][0] == flat["CSF_RS"][24]
    # With timeslice: day 0 = TR_2 (154), day 1 = SA_2 (135).
    sliced = _parse_res_requirement_csv(req_path, names, n_days=2, day_slices=slices)
    assert sliced["CSF_RS"][0] == 154.0
    assert sliced["CSF_RS"][24] == 135.0


def test_hydro_maxrampday_per_day_rhs(tmp_path: Path) -> None:
    """Hydro_MaxRampDay supplies the per-day RHS for hydro ramp UCs."""
    from plexos2gtopt.parsers import _parse_hydro_maxrampday_csv

    path = tmp_path / "Hydro_MaxRampDay.csv"
    path.write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\n"
        "RALCOramp_max_e1,2026,4,22,1,4.20\n"
        "RALCOramp_max_e1,2026,4,23,1,3.63\n"
        "RALCOramp_max_e2,2026,4,22,1,9.99\n"
    )
    out = _parse_hydro_maxrampday_csv(path)
    assert out["RALCOramp_max_e1"] == [4.20, 3.63]
    assert out["RALCOramp_max_e2"] == [9.99]


def test_sscc_activation_bess_parses_to_fractions(tmp_path: Path) -> None:
    """SSCC % per 2-hour band expands to per-hour fractions per zone."""
    from plexos2gtopt.parsers import _parse_sscc_activation_bess_csv

    path = tmp_path / "SSCC_Activation_BESS.csv"
    path.write_text(
        "Year,Pattern,CPF_RS_BESS,CPF_LW_BESS\n2026,H1-2,50,67\n2026,H3-4,40,0\n"
    )
    out = _parse_sscc_activation_bess_csv(path, n_days=1)
    assert out["CPF_RS_BESS"][0] == 0.50  # H1
    assert out["CPF_RS_BESS"][1] == 0.50  # H2
    assert out["CPF_RS_BESS"][2] == 0.40  # H3
    # CPF_LW_BESS: H1-2 = 0.67, H3-4 = 0.0 (still emitted — column non-zero).
    assert out["CPF_LW_BESS"][0] == 0.67
    assert out["CPF_LW_BESS"][2] == 0.0
    # n_days replication.
    out2 = _parse_sscc_activation_bess_csv(path, n_days=2)
    assert len(out2["CPF_RS_BESS"]) == 48
    assert out2["CPF_RS_BESS"][24] == 0.50


def test_extract_reserves_splits_up_down(tmp_path: Path) -> None:
    """`_LW` suffix → drreq; `_RS` (or other) → urreq."""
    bundle, xml_path = _build_bundle(tmp_path)
    csv_path = tmp_path / "Res_Requirement.csv"
    csv_path.write_text(
        'NAME,PATTERN,VALUE\nCSF_RS,"DO_1,H1",150\nCSF_LW,"DO_1,H1",200\n'
    )
    db = load_xml(xml_path)
    reserves = extract_reserves(db, bundle)
    by_name = {r.name: r for r in reserves}
    # CSF_RS gets up-reserve, CSF_LW down-reserve.
    assert by_name["CSF_RS"].ur_requirement[0] == 150.0
    assert by_name["CSF_RS"].dr_requirement == ()
    assert by_name["CSF_LW"].dr_requirement[0] == 200.0
    assert by_name["CSF_LW"].ur_requirement == ()


def test_extract_reserves_eligibility(tmp_path: Path) -> None:
    """Each Reserve carries the set of eligible Generators."""
    bundle, xml_path = _build_bundle(tmp_path)
    db = load_xml(xml_path)
    reserves = extract_reserves(db, bundle)
    by_name = {r.name: r for r in reserves}
    assert set(by_name["CSF_RS"].eligible_generators) == {"GEN_A", "GEN_B"}
    assert set(by_name["CSF_LW"].eligible_generators) == {"GEN_A"}


def test_extract_reserve_provisions_inverts_eligibility() -> None:
    """One ReserveProvision per Generator with aggregated zone list."""
    reserves = (
        ReserveSpec(object_id=20, name="ZONE_A", eligible_generators=("g1", "g2")),
        ReserveSpec(object_id=21, name="ZONE_B", eligible_generators=("g1",)),
    )
    # Must pass generators with positive pmax; extract_reserve_provisions
    # filters out gens with pmax <= 0 from eligibility.
    gens = (
        GeneratorSpec(object_id=1, name="g1", bus_name="b", pmax=100.0),
        GeneratorSpec(object_id=2, name="g2", bus_name="b", pmax=100.0),
    )
    provisions = extract_reserve_provisions(reserves, generators=gens)
    by_gen = {p.generator_name: p for p in provisions}
    assert set(by_gen["g1"].reserve_zones) == {"ZONE_A", "ZONE_B"}
    assert set(by_gen["g2"].reserve_zones) == {"ZONE_A"}


def test_extract_reserve_provisions_zero_pmax_config_variants() -> None:
    """Zero-capacity reserve-eligible gens get a STRICTLY ``[0, 0]``-bounded
    provision (no urmin/drmin floor) so PLEXOS reserve user_constraints
    referencing ``reserve_provision("provision_<config>")`` resolve.

    PLEXOS CEN PCP emits one Generator per combined-cycle config variant
    (e.g. ``TOCOPILLA-TG3_GN_A``, ``…_GNL_INF``); only the operating
    config carries pmax > 0, the rest are scalar pmax == 0.  Without the
    provision row, ~2095 ``CPF/CSF/CTF*MinProvision`` references would
    dangle.  The provision must be ``urmax = drmax = 0`` with NO floor
    so it contributes exactly 0 to any reserve sum and cannot recreate
    the primal-infeasibility the old ``pmax > 0`` filter prevented.
    """
    reserves = (
        ReserveSpec(
            object_id=20,
            name="CPF_RS",
            eligible_generators=("g_on", "g_off1", "g_off2"),
        ),
    )
    gens = (
        GeneratorSpec(object_id=1, name="g_on", bus_name="b", pmax=50.0),
        GeneratorSpec(object_id=2, name="g_off1", bus_name="b", pmax=0.0),
        GeneratorSpec(object_id=3, name="g_off2", bus_name="b", pmax=0.0),
    )
    provisions = extract_reserve_provisions(reserves, generators=gens)
    by_gen = {p.generator_name: p for p in provisions}
    # Every reserve-eligible gen — including the zero-pmax configs —
    # gets a provision so the reference resolves.
    assert set(by_gen) == {"g_on", "g_off1", "g_off2"}
    # Capacity gen keeps real urmax/drmax = pmax.
    assert by_gen["g_on"].urmax == 50.0
    assert by_gen["g_on"].drmax == 50.0
    # Zero-capacity configs are strictly [0, 0] AND have NO floor —
    # this is the "safe shape" that cannot force any dispatch.
    for nm in ("g_off1", "g_off2"):
        p = by_gen[nm]
        assert p.urmax == 0.0, f"{nm}: urmax must be 0 (got {p.urmax})"
        assert p.drmax == 0.0, f"{nm}: drmax must be 0 (got {p.drmax})"
        assert p.urmin == 0.0, f"{nm}: urmin must be 0 (got {p.urmin})"
        assert p.drmin == 0.0, f"{nm}: drmin must be 0 (got {p.drmin})"


def test_extract_reserve_provisions_extra_provision_gens() -> None:
    """A generator referenced by a UserConstraint
    ``reserve_provision("provision_<gen>")`` coefficient but NOT a member
    of any Reserve→Generator eligibility table still gets a provision row
    (via the ``extra_provision_gens`` set), so the UC reference resolves.

    A capacity-bearing gen gets its real ``urmax = drmax = pmax``; a
    zero-pmax gen stays the safe ``[0, 0]`` column.  Neither carries a
    floor.
    """
    reserves: tuple[ReserveSpec, ...] = ()  # not a reserve member
    gens = (
        GeneratorSpec(object_id=1, name="cap_gen", bus_name="b", pmax=40.0),
        GeneratorSpec(object_id=2, name="zero_gen", bus_name="b", pmax=0.0),
    )
    provisions = extract_reserve_provisions(
        reserves,
        generators=gens,
        extra_provision_gens=frozenset({"cap_gen", "zero_gen"}),
    )
    by_gen = {p.generator_name: p for p in provisions}
    assert set(by_gen) == {"cap_gen", "zero_gen"}
    # Capacity-bearing → real cap from pmax.
    assert by_gen["cap_gen"].urmax == 40.0
    assert by_gen["cap_gen"].drmax == 40.0
    # Both are zone-less (no reserve membership) and floor-less.
    for nm in ("cap_gen", "zero_gen"):
        assert by_gen[nm].reserve_zones == ()
        assert by_gen[nm].urmin == 0.0
        assert by_gen[nm].drmin == 0.0
    # Zero-pmax stays [0, 0].
    assert by_gen["zero_gen"].urmax == 0.0
    assert by_gen["zero_gen"].drmax == 0.0


def test_build_reserve_zone_array_emits_matrix() -> None:
    """urreq / drreq emit as [[24-block]] matrices when present."""
    reserves = (
        ReserveSpec(
            object_id=20,
            name="Z",
            ur_requirement=tuple(range(24)),
        ),
    )
    out = build_reserve_zone_array(reserves)
    assert out[0]["name"] == "Z"
    # 1 stage × 24 blocks shape.
    assert len(out[0]["urreq"]) == 1
    assert len(out[0]["urreq"][0]) == 24
    # ``drreq`` is always emitted (zero-vector when the spec carries
    # nothing) so gtopt's reserve_provision LP unconditionally
    # materialises the ``up``/``dn`` columns — required for PLEXOS
    # Constraint coefficients that force reserve provision even on
    # zero-requirement zones.
    assert "drreq" in out[0]
    assert all(v == 0.0 for v in out[0]["drreq"][0])


def test_extract_reserves_reads_violation_cost(tmp_path: Path) -> None:
    """`Violation Cost` on a Reserve maps to urcost / drcost by Type.

    Raise/spinning/regulation-raise/replacement/tertiary (Type 1/2/3/5/6)
    → ``urcost``; Regulation Lower (Type 4) → ``drcost``.  The writer
    surfaces the value on the emitted ``ReserveZone`` JSON.
    """
    xml = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>14</class_id><name>Reserve</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>30</object_id><class_id>14</class_id><name>UP_ZONE</name></t_object>
  <t_object><object_id>31</object_id><class_id>14</class_id><name>DOWN_ZONE</name></t_object>
  <t_collection>
    <collection_id>156</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>14</child_class_id>
    <name>Reserves</name>
  </t_collection>
  <t_membership>
    <membership_id>600</membership_id>
    <collection_id>156</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>601</membership_id>
    <collection_id>156</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>31</child_object_id>
  </t_membership>
  <t_property>
    <property_id>1370</property_id>
    <collection_id>156</collection_id>
    <name>Type</name>
  </t_property>
  <t_property>
    <property_id>1400</property_id>
    <collection_id>156</collection_id>
    <name>Violation Cost</name>
  </t_property>
  <t_data>
    <data_id>9001</data_id>
    <membership_id>600</membership_id>
    <property_id>1370</property_id>
    <value>1</value>
  </t_data>
  <t_data>
    <data_id>9002</data_id>
    <membership_id>600</membership_id>
    <property_id>1400</property_id>
    <value>1500</value>
  </t_data>
  <t_data>
    <data_id>9003</data_id>
    <membership_id>601</membership_id>
    <property_id>1370</property_id>
    <value>4</value>
  </t_data>
  <t_data>
    <data_id>9004</data_id>
    <membership_id>601</membership_id>
    <property_id>1400</property_id>
    <value>2500</value>
  </t_data>
</MasterDataSet>
"""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(xml)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    reserves = extract_reserves(db, bundle)
    by_name = {r.name: r for r in reserves}
    # Raise reserve (Type 1) → urcost only.
    assert by_name["UP_ZONE"].urcost == 1500.0
    assert by_name["UP_ZONE"].drcost == 0.0
    # Lower reserve (Type 4) → drcost only.
    assert by_name["DOWN_ZONE"].urcost == 0.0
    assert by_name["DOWN_ZONE"].drcost == 2500.0
    # Writer surfaces both on the ReserveZone JSON.
    out = build_reserve_zone_array(reserves)
    by_json = {e["name"]: e for e in out}
    assert by_json["UP_ZONE"]["urcost"] == 1500.0
    assert "drcost" not in by_json["UP_ZONE"]
    assert by_json["DOWN_ZONE"]["drcost"] == 2500.0
    assert "urcost" not in by_json["DOWN_ZONE"]


def test_build_reserve_provision_array() -> None:
    """Provisions stash the reserve_zones array in name order."""
    provs = (ReserveProvisionSpec(generator_name="g1", reserve_zones=("Z1", "Z2")),)
    out = build_reserve_provision_array(provs)
    assert out[0]["generator"] == "g1"
    assert out[0]["reserve_zones"] == ["Z1", "Z2"]


# ----- P6 commitment tests ---------------------------------------------------


_UC_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>2</class_id><name>GEN_A</name></t_object>
</MasterDataSet>
"""


def test_extract_commitments_skips_no_op(tmp_path: Path) -> None:
    """Gens with all-zero UC parameters get no Commitment entry."""
    xml = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml.write_text(_UC_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml)
    gens = (GeneratorSpec(object_id=10, name="GEN_A", bus_name="b"),)
    # No CSVs, no t_data → all zero → drop GEN_A.
    out = extract_commitments(db, bundle, gens)
    assert not out


def test_extract_commitments_reads_csvs(tmp_path: Path) -> None:
    """Per-unit Gen_StartCost.csv + Gen_IniHoursUp.csv populate the spec."""
    xml = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml.write_text(_UC_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    (tmp_path / "Gen_StartCost.csv").write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\nGEN_A,2026,1,1,1,500\n"
    )
    (tmp_path / "Gen_IniHoursUp.csv").write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\nGEN_A,2026,1,1,1,24\n"
    )
    (tmp_path / "Gen_IniUnits.csv").write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\nGEN_A,2026,1,1,1,1\n"
    )
    db = load_xml(xml)
    gens = (GeneratorSpec(object_id=10, name="GEN_A", bus_name="b"),)
    out = extract_commitments(db, bundle, gens)
    assert len(out) == 1
    c = out[0]
    assert c.startup_cost == 500.0
    assert c.initial_status == 1.0
    assert c.initial_hours == 24.0


def test_extract_commitments_signs_initial_hours_down(tmp_path: Path) -> None:
    """Gen_IniHoursDown → negative initial_hours."""
    xml = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml.write_text(_UC_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    (tmp_path / "Gen_IniHoursDown.csv").write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\nGEN_A,2026,1,1,1,12\n"
    )
    (tmp_path / "Gen_StartCost.csv").write_text(
        "NAME,YEAR,MONTH,DAY,PERIOD,VALUE\nGEN_A,2026,1,1,1,1\n"
    )
    db = load_xml(xml)
    gens = (GeneratorSpec(object_id=10, name="GEN_A", bus_name="b"),)
    out = extract_commitments(db, bundle, gens)
    assert out[0].initial_hours == -12.0
    assert out[0].initial_status == 0.0


def test_build_commitment_array_default_mip() -> None:
    """build_commitment_array default = MIP — ``relax`` field omitted.

    Changed 2026-05-23: previously emitted ``relax: True`` on every
    commitment, which collapsed gtopt's ``<plant>_Uniq`` constraints
    (``Σ status ≤ 1`` over band variants) to fractional commitments
    and undercut PLEXOS dispatch by ~31 % on the CEN PCP bundle.
    MIP enforcement closed ~7 pp of that gap.  Pass ``lp_relax=True``
    for the legacy LP-only behaviour.
    """
    commits = (
        CommitmentSpec(
            generator_name="GEN_A",
            startup_cost=500.0,
            initial_status=1.0,
            initial_hours=24.0,
        ),
    )
    out = build_commitment_array(commits)
    assert out[0]["generator"] == "GEN_A"
    assert out[0]["startup_cost"] == 500.0
    assert out[0]["initial_status"] == 1.0
    assert out[0]["initial_hours"] == 24.0
    # MIP by default — no ``relax`` field.
    assert "relax" not in out[0]
    # No min_up_time key when the spec has min_up_time=0.
    assert "min_up_time" not in out[0]
    # No noload_cost key when the spec has noload_cost=0.
    assert "noload_cost" not in out[0]


def test_build_commitment_array_lp_relax_opt_in() -> None:
    """``lp_relax=True`` (CLI ``--lp-relax``) re-enables LP relaxation.

    The flag is the explicit opt-in for solvers without MIP support
    (CLP, OSI without CBC) or for fast LP-only diagnostics.
    """
    commits = (
        CommitmentSpec(
            generator_name="GEN_C",
            startup_cost=100.0,
            initial_status=1.0,
        ),
    )
    out = build_commitment_array(commits, lp_relax=True)
    assert out[0]["relax"] is True


def test_build_commitment_array_with_noload() -> None:
    """noload_cost > 0 emits as a top-level scalar field."""
    commits = (
        CommitmentSpec(
            generator_name="GEN_B",
            startup_cost=100.0,
            noload_cost=42.0,
        ),
    )
    out = build_commitment_array(commits)
    assert out[0]["noload_cost"] == 42.0


def test_build_generator_array_per_block_pmax_when_varying() -> None:
    """build_generator_array emits a per-block matrix when pmax_profile varies."""
    from plexos2gtopt.gtopt_writer import build_generator_array

    gens = (
        GeneratorSpec(
            object_id=10,
            name="solar",
            bus_name="b",
            pmax=80.0,
            pmax_profile=tuple([0.0] * 6 + [40.0] * 6 + [80.0] * 6 + [0.0] * 6),
        ),
    )
    out = build_generator_array(gens, fuels=())
    # Profile varies → pmax is a [[24-block]] matrix.
    assert isinstance(out[0]["pmax"], list)
    assert len(out[0]["pmax"]) == 1
    assert len(out[0]["pmax"][0]) == 24
    assert out[0]["pmax"][0][12] == 80.0


def test_build_line_array_emits_wheeling_charge() -> None:
    """P3.6: Line.tcost honours wheeling_charge."""
    from plexos2gtopt.entities import LineSpec
    from plexos2gtopt.gtopt_writer import build_line_array

    lines = (
        LineSpec(
            object_id=1,
            name="l",
            bus_from="a",
            bus_to="b",
            tmax_ab=100.0,
            tmin_ab=-100.0,
            wheeling_charge=2.5,
        ),
    )
    out = build_line_array(lines)
    assert out[0]["tcost"] == 2.5
