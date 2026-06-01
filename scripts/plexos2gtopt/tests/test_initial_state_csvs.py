"""Unit tests for the three PLEXOS initial-state CSV files.

Verifies that ``Gen_IniUnits.csv``, ``Gen_IniHoursUp.csv``, and
``Gen_IniHoursDown.csv`` are not only LOADED by the parser, but actually
EMITTED on the gtopt JSON via ``Generator.uini`` (from IniUnits) and
``Commitment.ini_hours_up`` / ``ini_hours_down`` (from the raw PLEXOS pair).

Regression guard for the v0407 finding: 0/1728 generators had ``uini``
and 0/1567 commitments had ``ini_hours_up``/``ini_hours_down`` despite
the CSVs being read at ``parsers.py:4431/4436/4441``.
"""

from __future__ import annotations

from pathlib import Path

from plexos2gtopt.entities import CommitmentSpec, GeneratorSpec
from plexos2gtopt.gtopt_writer import (
    build_commitment_array,
    build_generator_array,
)
from plexos2gtopt.parsers import extract_commitments, extract_generators
from plexos2gtopt.plexos_loader import PlexosBundle
from plexos2gtopt.plexos_xml import NS, load_xml


# Minimal XML: one thermal-style Generator at one Bus with a Fuel
# membership so extract_commitments emits a CommitmentSpec.
_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>22</class_id><name>Node</name></t_class>
  <t_class><class_id>33</class_id><name>Fuel</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>2</class_id><name>GEN_A</name></t_object>
  <t_object><object_id>20</object_id><class_id>22</class_id><name>BUS_1</name></t_object>
  <t_object><object_id>30</object_id><class_id>33</class_id><name>DIESEL</name></t_object>
  <t_collection>
    <collection_id>12</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>22</child_class_id>
    <name>Nodes</name>
  </t_collection>
  <t_collection>
    <collection_id>13</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>33</child_class_id>
    <name>Fuels</name>
  </t_collection>
  <t_membership>
    <membership_id>100</membership_id>
    <collection_id>12</collection_id>
    <parent_object_id>10</parent_object_id>
    <child_object_id>20</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>101</membership_id>
    <collection_id>13</collection_id>
    <parent_object_id>10</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
</MasterDataSet>
"""


def _write_xml(tmp_path: Path) -> tuple[PlexosBundle, Path]:
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_XML)
    return PlexosBundle(root=tmp_path, source=tmp_path), xml_path


def _write_ini_units(path: Path, *rows: tuple[str, float]) -> None:
    """Long-format CSV: NAME, YEAR, MONTH, DAY, PERIOD, BAND, VALUE."""
    body = "NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE\n" + "\n".join(
        f"{nm},2026,4,22,1,1,{v}" for nm, v in rows
    )
    path.write_text(body + "\n")


# ─── GeneratorSpec.initial_units round-trip ──────────────────────────────


def test_generator_uini_emitted_when_csv_present(tmp_path: Path) -> None:
    """Gen_IniUnits.csv VALUE > 0 → Generator entry carries ``uini``."""
    bundle, xml_path = _write_xml(tmp_path)
    _write_ini_units(tmp_path / "Gen_IniUnits.csv", ("GEN_A", 1))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    assert len(gens) == 1
    assert gens[0].initial_units == 1.0
    entries = build_generator_array(gens)
    assert len(entries) == 1
    assert entries[0]["uini"] == 1.0


def test_generator_uini_emitted_zero_value(tmp_path: Path) -> None:
    """Explicit 0 in the CSV is still PUBLISHED (distinct from "no entry")."""
    bundle, xml_path = _write_xml(tmp_path)
    _write_ini_units(tmp_path / "Gen_IniUnits.csv", ("GEN_A", 0))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    # Explicit zero round-trips as 0.0 (NOT None) so the writer emits the field.
    assert gens[0].initial_units == 0.0
    entries = build_generator_array(gens)
    assert entries[0]["uini"] == 0.0


def test_generator_uini_omitted_when_csv_missing(tmp_path: Path) -> None:
    """No Gen_IniUnits.csv → no ``uini`` field on the generator entry."""
    bundle, xml_path = _write_xml(tmp_path)
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    assert gens[0].initial_units is None
    entries = build_generator_array(gens)
    assert "uini" not in entries[0]


def test_generator_uini_omitted_when_csv_skips_gen(tmp_path: Path) -> None:
    """CSV present but no row for this gen → no ``uini`` field emitted."""
    bundle, xml_path = _write_xml(tmp_path)
    _write_ini_units(tmp_path / "Gen_IniUnits.csv", ("OTHER_GEN", 1))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    assert gens[0].initial_units is None
    entries = build_generator_array(gens)
    assert "uini" not in entries[0]


# ─── CommitmentSpec.ini_hours_up / ini_hours_down round-trip ─────────────


def test_commitment_ini_hours_emitted_when_csvs_present(tmp_path: Path) -> None:
    """Gen_IniHoursUp/Down.csv → Commitment entry carries the raw pair."""
    bundle, xml_path = _write_xml(tmp_path)
    # Need IniUnits + a startup cost to trigger CommitmentSpec emission.
    _write_ini_units(tmp_path / "Gen_IniUnits.csv", ("GEN_A", 1))
    _write_ini_units(tmp_path / "Gen_IniHoursUp.csv", ("GEN_A", 24))
    _write_ini_units(tmp_path / "Gen_IniHoursDown.csv", ("GEN_A", 0))
    _write_ini_units(tmp_path / "Gen_StartCost.csv", ("GEN_A", 5000))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    commits = extract_commitments(db, bundle, gens)
    assert len(commits) == 1
    cmt = commits[0]
    assert cmt.ini_hours_up == 24.0
    assert cmt.ini_hours_down == 0.0
    # The LP-consumed signed view collapses to +24 (online; up wins).
    assert cmt.initial_status == 1.0
    assert cmt.initial_hours == 24.0
    entries = build_commitment_array(commits)
    assert entries[0]["ini_hours_up"] == 24.0
    assert entries[0]["ini_hours_down"] == 0.0


def test_commitment_ini_hours_omitted_when_csvs_missing(tmp_path: Path) -> None:
    """No Ini hours CSVs → no ``ini_hours_up`` / ``ini_hours_down`` fields."""
    bundle, xml_path = _write_xml(tmp_path)
    _write_ini_units(tmp_path / "Gen_IniUnits.csv", ("GEN_A", 1))
    _write_ini_units(tmp_path / "Gen_StartCost.csv", ("GEN_A", 5000))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    commits = extract_commitments(db, bundle, gens)
    assert commits[0].ini_hours_up is None
    assert commits[0].ini_hours_down is None
    entries = build_commitment_array(commits)
    assert "ini_hours_up" not in entries[0]
    assert "ini_hours_down" not in entries[0]


def test_commitment_ini_hours_explicit_zeroes_round_trip(tmp_path: Path) -> None:
    """Explicit 0 in BOTH CSVs round-trips as 0.0 (not None)."""
    bundle, xml_path = _write_xml(tmp_path)
    _write_ini_units(tmp_path / "Gen_IniUnits.csv", ("GEN_A", 0))
    _write_ini_units(tmp_path / "Gen_IniHoursUp.csv", ("GEN_A", 0))
    _write_ini_units(tmp_path / "Gen_IniHoursDown.csv", ("GEN_A", 0))
    _write_ini_units(tmp_path / "Gen_StartCost.csv", ("GEN_A", 5000))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    commits = extract_commitments(db, bundle, gens)
    assert commits[0].ini_hours_up == 0.0
    assert commits[0].ini_hours_down == 0.0
    entries = build_commitment_array(commits)
    assert entries[0]["ini_hours_up"] == 0.0
    assert entries[0]["ini_hours_down"] == 0.0


# ─── initial_hours sign collapse (bug C1, task #97) ──────────────────────


def test_initial_hours_off_unit_picks_negative_when_both_csvs_set(
    tmp_path: Path,
) -> None:
    """OFF unit + IniHoursUp=168 + IniHoursDown=168 → −168 (not +168).

    PLEXOS publishes BOTH IniHoursUp and IniHoursDown simultaneously for
    the same generator (verified for ANGOSTURA_U1..U3, COLBUN_U1..U2 on
    v0407).  The old heuristic favoured ``ih_up`` whenever it was > 0,
    so an OFF unit (``IniUnits = 0``) with both scalars set collapsed
    to a POSITIVE ``initial_hours`` — telling the LP the unit had been
    online for 168 h when it was actually offline for 168 h.  The sign
    must come from ``initial_status`` (= ``IniUnits``), not from a
    tie-break between the two raw scalars.
    """
    bundle, xml_path = _write_xml(tmp_path)
    _write_ini_units(tmp_path / "Gen_IniUnits.csv", ("GEN_A", 0))  # OFF
    _write_ini_units(tmp_path / "Gen_IniHoursUp.csv", ("GEN_A", 168))
    _write_ini_units(tmp_path / "Gen_IniHoursDown.csv", ("GEN_A", 168))
    _write_ini_units(tmp_path / "Gen_StartCost.csv", ("GEN_A", 5000))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    commits = extract_commitments(db, bundle, gens)
    assert len(commits) == 1
    cmt = commits[0]
    # Raw pair is preserved unchanged — only the collapsed sign is fixed.
    assert cmt.ini_hours_up == 168.0
    assert cmt.ini_hours_down == 168.0
    assert cmt.initial_status == 0.0
    assert cmt.initial_hours == -168.0


def test_initial_hours_on_unit_picks_positive_from_ih_up(tmp_path: Path) -> None:
    """ON unit (IniUnits=1) + IniHoursUp=24 + IniHoursDown=0 → +24."""
    bundle, xml_path = _write_xml(tmp_path)
    _write_ini_units(tmp_path / "Gen_IniUnits.csv", ("GEN_A", 1))
    _write_ini_units(tmp_path / "Gen_IniHoursUp.csv", ("GEN_A", 24))
    _write_ini_units(tmp_path / "Gen_IniHoursDown.csv", ("GEN_A", 0))
    _write_ini_units(tmp_path / "Gen_StartCost.csv", ("GEN_A", 5000))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    commits = extract_commitments(db, bundle, gens)
    cmt = commits[0]
    assert cmt.initial_status == 1.0
    assert cmt.initial_hours == 24.0


def test_initial_hours_on_unit_with_zero_hours_collapses_to_zero(
    tmp_path: Path,
) -> None:
    """ON unit (IniUnits=1) + IniHoursUp=0 + IniHoursDown=0 → 0."""
    bundle, xml_path = _write_xml(tmp_path)
    _write_ini_units(tmp_path / "Gen_IniUnits.csv", ("GEN_A", 1))
    _write_ini_units(tmp_path / "Gen_IniHoursUp.csv", ("GEN_A", 0))
    _write_ini_units(tmp_path / "Gen_IniHoursDown.csv", ("GEN_A", 0))
    _write_ini_units(tmp_path / "Gen_StartCost.csv", ("GEN_A", 5000))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    commits = extract_commitments(db, bundle, gens)
    cmt = commits[0]
    assert cmt.initial_status == 1.0
    assert cmt.initial_hours == 0.0


def test_initial_hours_off_unit_picks_negative_from_ih_down(
    tmp_path: Path,
) -> None:
    """OFF unit (IniUnits=0) + IniHoursUp=0 + IniHoursDown=72 → −72."""
    bundle, xml_path = _write_xml(tmp_path)
    _write_ini_units(tmp_path / "Gen_IniUnits.csv", ("GEN_A", 0))
    _write_ini_units(tmp_path / "Gen_IniHoursUp.csv", ("GEN_A", 0))
    _write_ini_units(tmp_path / "Gen_IniHoursDown.csv", ("GEN_A", 72))
    _write_ini_units(tmp_path / "Gen_StartCost.csv", ("GEN_A", 5000))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    commits = extract_commitments(db, bundle, gens)
    cmt = commits[0]
    assert cmt.initial_status == 0.0
    assert cmt.initial_hours == -72.0


def test_initial_hours_ambiguous_falls_back_to_max_magnitude(
    tmp_path: Path,
) -> None:
    """No Gen_IniUnits.csv + ih_up=24 + ih_down=72 → max-magnitude → −72.

    When ``initial_units`` is ambiguous (CSV missing or no row for this
    gen) we can't derive the sign from status, so the fallback picks
    the larger of the two raw scalars and signs it accordingly.
    """
    bundle, xml_path = _write_xml(tmp_path)
    # No Gen_IniUnits.csv → initial_status defaults to 0.0 AND units==[].
    _write_ini_units(tmp_path / "Gen_IniHoursUp.csv", ("GEN_A", 24))
    _write_ini_units(tmp_path / "Gen_IniHoursDown.csv", ("GEN_A", 72))
    _write_ini_units(tmp_path / "Gen_StartCost.csv", ("GEN_A", 5000))
    db = load_xml(xml_path)
    gens = extract_generators(db, bundle)
    commits = extract_commitments(db, bundle, gens)
    cmt = commits[0]
    # max-magnitude tie-break: |72| > |24| → −72.
    assert cmt.initial_hours == -72.0


# ─── Direct writer tests (no XML / DB round-trip) ────────────────────────


def test_writer_emits_uini_field_directly() -> None:
    """build_generator_array emits ``uini`` when GeneratorSpec sets it."""
    gens = (
        GeneratorSpec(
            object_id=1,
            name="G",
            bus_name="B",
            pmax=100.0,
            initial_units=1.0,
        ),
    )
    out = build_generator_array(gens)
    assert out[0]["uini"] == 1.0


def test_writer_emits_ini_hours_pair_directly() -> None:
    """build_commitment_array emits both fields when CommitmentSpec sets them."""
    commits = (
        CommitmentSpec(
            generator_name="G",
            startup_cost=100.0,
            ini_hours_up=48.0,
            ini_hours_down=12.0,
        ),
    )
    out = build_commitment_array(commits)
    assert out[0]["ini_hours_up"] == 48.0
    assert out[0]["ini_hours_down"] == 12.0
