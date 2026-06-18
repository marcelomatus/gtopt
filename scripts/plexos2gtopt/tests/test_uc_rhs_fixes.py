# SPDX-License-Identifier: BSD-3-Clause
"""Unit tests for the gtopt-UC vs PLEXOS RHS-parity converter fixes.

Each test pins one fix found by cross-validating the emitted UserConstraints
against the PLEXOS solution DB (see ``project_uc_plexos_parity``):

* ``_has_active_rhs_row`` — PANGUE: an expired-only RHS must not mask an
  active ``RHS Day``.
* Gas ``RHS Custom`` divisor — uses the XML ``Horizon`` day count (168 h),
  not ``bundle.n_days × 24`` (which was 24 h → a spurious ×7 inflation).
* Fuel-offtake daily cap (Diesel) — must NOT get the ×1000 GWh→MWh scale.
* Crew / commitment-count daily cap (Guacolda) — gets ``daily_sum`` with
  ``constraint_type=""`` (a per-day COUNT, not Δt-weighted energy).
"""

from __future__ import annotations

from datetime import datetime
from pathlib import Path
from types import SimpleNamespace

from plexos2gtopt.parsers import (
    _block_in_date_window,
    _build_rhs_date_overlay_profile,
    _build_rhs_timeslice_profile,
    _expand_timeslice,
    _has_active_rhs_row,
    _horizon_value,
    extract_user_constraints,
)
from plexos2gtopt.plexos_loader import PlexosBundle
from plexos2gtopt.plexos_xml import NS, PlexosDataRow, load_xml


# --------------------------------------------------------------------------- #
# PANGUE: _has_active_rhs_row distinguishes active from expired-only RHS rows
# --------------------------------------------------------------------------- #
def test_has_active_rhs_row() -> None:
    hs = datetime(2026, 4, 22)
    undated = SimpleNamespace(date_from=None, date_to=None)
    covering = SimpleNamespace(
        date_from=datetime(2026, 1, 1), date_to=datetime(2026, 12, 31)
    )
    expired = SimpleNamespace(
        date_from=datetime(2025, 11, 6), date_to=datetime(2025, 12, 31)
    )
    assert _has_active_rhs_row([undated], hs) is True
    assert _has_active_rhs_row([covering], hs) is True
    assert _has_active_rhs_row([expired], hs) is False
    # an expired window does not mask the undated base
    assert _has_active_rhs_row([expired, undated], hs) is True
    assert _has_active_rhs_row([], hs) is False
    # horizon unknown: undated still active, a dated row is not
    assert _has_active_rhs_row([undated], None) is True
    assert _has_active_rhs_row([expired], None) is False


# --------------------------------------------------------------------------- #
# Gas: RHS Custom divisor = XML Horizon hours (168), not n_days×24 (24)
# --------------------------------------------------------------------------- #
_GAS_HORIZON_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>9</class_id><name>Horizon</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>5</object_id><class_id>9</class_id>
    <name>Coordinador_diario_1H_7d</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id>
    <name>Gas_MaxOpDay0_X</name></t_object>
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


def test_rhs_custom_uses_xml_horizon_days(tmp_path: Path) -> None:
    """With a ``Horizon`` named ``..._7d`` the per-block RHS Custom divisor
    is 168 h (``2.0 × 1000 / 168 = 11.9048``), NOT ``n_days × 24 = 24 h``
    (which gave the spurious 83.3333 = ×7 inflation).

    Post-2026-05-29 ``_consolidate_gas_maxopday_groups`` merges the
    per-block specs into one ``Gas_MaxOpDay_<group>`` daily_sum+energy
    spec.  This test still has to verify the per-block factor 1/168 (the
    pre-consolidation per-block-rate scaling) — recovered from the
    consolidator's per-block RHS profile, which broadcasts the per-day
    total ``11.9048 × 168 = 2000`` GJ at each block in the day.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_GAS_HORIZON_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)  # n_days defaults 1
    out = extract_user_constraints(load_xml(xml_path), bundle)
    by_name = {c.name: c for c in out}
    assert "Gas_MaxOpDay_X" in by_name, by_name.keys()
    spec = by_name["Gas_MaxOpDay_X"]
    # Per-day total = 2.0 × 1000 = 2000 GJ.  With the XML's ``_7d``
    # horizon the consolidator builds a 168-block profile spanning 7
    # days — only blocks of PLEXOS Day_0 (the only one in this fixture)
    # carry the 2000 GJ cap; the other 6 gtopt days have no matching
    # PLEXOS Day_X so they get the BIG sentinel (effectively
    # unconstrained for those days).  This is correct: a Day_0-only
    # input means "cap only the first day".
    assert spec.rhs_profile, "consolidator should emit per-block RHS"
    BIG = 1e9
    capped = [v for v in spec.rhs_profile if v < BIG]
    sentineled = [v for v in spec.rhs_profile if v >= BIG]
    assert capped, "expected at least one block carrying the day-0 cap"
    assert all(abs(v - 2000.0) < 1e-2 for v in capped), (
        f"recovered per-day cap expected ≈ 2000 GJ, "
        f"got capped values: {sorted(set(capped))}"
    )
    # ``_GAS_HORIZON_XML`` has only Day_0 input → only one gtopt day
    # populated; the remaining 6 gtopt days are sentineled.
    assert sentineled, "Day_0-only input should leave 6 gtopt days sentineled"


# --------------------------------------------------------------------------- #
# Guacolda: a commitment-count daily cap (RHS Day, startup LHS) → daily_sum
#           with constraint_type="" (per-day count, not energy, no ×1000)
# --------------------------------------------------------------------------- #
_CREW_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id>
    <name>G1_Crew</name></t_object>
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
    <property_id>4391</property_id><collection_id>700</collection_id><name>RHS Day</name>
  </t_property>
  <t_property>
    <property_id>395</property_id><collection_id>32</collection_id>
    <name>Units Started Coefficient</name>
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
    <property_id>4391</property_id><value>2.0</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>32001</membership_id>
    <property_id>395</property_id><value>1.0</value>
  </t_data>
</MasterDataSet>
"""


def test_crew_count_daily_sum_without_energy(tmp_path: Path) -> None:
    """A daily commitment-count cap (RHS Day, ``commitment.startup`` LHS,
    no generation) → ``daily_sum=True`` + ``constraint_type=""`` (per-day
    count) with the RHS at face value (2), NOT ×1000."""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_CREW_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    out = extract_user_constraints(load_xml(xml_path), bundle)
    uc = {c.name: c for c in out}["G1_Crew"]
    assert 'commitment("uc_G1").startup' in uc.expression
    assert uc.daily_sum is True
    assert uc.constraint_type == ""  # count, NOT "energy"
    assert uc.expression.endswith("<= 2")  # face value, not 2000


# --------------------------------------------------------------------------- #
# Diesel: a fuel-offtake daily cap (RHS Day + heat_rate·generation) must NOT
#         get the ×1000 GWh→MWh scale (it is a fuel-unit budget).
# --------------------------------------------------------------------------- #
_DIESEL_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>4</class_id><name>Fuel</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>10</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object><object_id>20</object_id><class_id>4</class_id><name>DIE</name></t_object>
  <t_object><object_id>30</object_id><class_id>70</class_id>
    <name>Diesel_OffTakeDay</name></t_object>
  <t_collection>
    <collection_id>700</collection_id><parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>54</collection_id><parent_class_id>4</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>7</collection_id><parent_class_id>2</parent_class_id>
    <child_class_id>4</child_class_id><name>Fuels</name>
  </t_collection>
  <t_property>
    <property_id>4369</property_id><collection_id>700</collection_id><name>Sense</name>
  </t_property>
  <t_property>
    <property_id>4384</property_id><collection_id>700</collection_id><name>RHS</name>
  </t_property>
  <t_property>
    <property_id>4391</property_id><collection_id>700</collection_id><name>RHS Day</name>
  </t_property>
  <t_property>
    <property_id>620</property_id><collection_id>54</collection_id>
    <name>Offtake Coefficient</name>
  </t_property>
  <t_membership>
    <membership_id>900</membership_id><collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id><child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>910</membership_id><collection_id>54</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>700</membership_id><collection_id>7</collection_id>
    <parent_object_id>10</parent_object_id><child_object_id>20</child_object_id>
  </t_membership>
  <t_data>
    <data_id>1</data_id><membership_id>900</membership_id>
    <property_id>4369</property_id><value>-1</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>900</membership_id>
    <property_id>4391</property_id><value>2.0</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>910</membership_id>
    <property_id>620</property_id><value>1.0</value>
  </t_data>
</MasterDataSet>
"""


def test_fuel_offtake_daily_cap_gwh_scale(tmp_path: Path) -> None:
    """A fuel-offtake daily cap (RHS Day + ``heat_rate·generation`` terms)
    DOES get the ×1000 GWh→MWh scale applied inside the
    ``is_fuel_offtake and rhs_from_day`` branch — it just doesn't go via
    the ``is_daily_energy`` path (which is excluded to avoid double-
    scaling).  Verified against PLEXOS sol for ``Diesel_OffTakeDay`` on
    CEN PCP RES20260422 (raw RHS Day = 9.277 → per-block = 386.54).

    The constraint is NOT classified as a daily-ENERGY (Δt-weighted)
    type — it stays a per-block fuel-cap (``constraint_type=""``).
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_DIESEL_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    out = extract_user_constraints(
        load_xml(xml_path), bundle, heat_rate_by_gen={"G1": 1.0}
    )
    uc = {c.name: c for c in out}["Diesel_OffTakeDay"]
    assert 'generator("G1").generation' in uc.expression
    # ×1000 GWh→MWh applied: raw RHS Day=2.0 → expression scalar=2000.
    assert "2000" in uc.expression
    rhs_vals = (
        [
            v
            for r in (uc.rhs_profile or ())
            for v in ((r,) if isinstance(r, float) else r)
        ]
        if uc.rhs_profile
        else []
    )
    # per-block split: 2000 / 24 = 83.33 in each block (×1000 of the
    # pre-fix 0.0833).  All values are in MWh-equivalent fuel units.
    assert all(80.0 < abs(v) < 90.0 for v in rhs_vals), rhs_vals
    # fuel-unit budget, not Δt-weighted energy
    assert uc.constraint_type != "energy"


# --------------------------------------------------------------------------- #
# Residual RHS issues (CHARACTERIZATION — pin current behavior; the LHS
# coefficients of all three are verified correct, only the RHS differs):
#   * ANTUCOmax/min, CTF_DownMinProvision — input is a single SCALAR; PLEXOS
#     runtime-derives a time-varying value NOT present in the input.
#   * Reg_SouthZone — several UNDATED, untagged RHS values; prefer_min picks
#     the tightest. See memory `project_uc_plexos_parity`.
# --------------------------------------------------------------------------- #
def _rhs_row(value: float, data_id: int) -> SimpleNamespace:
    """A minimal undated PlexosDataRow stand-in for `_horizon_value`."""
    return SimpleNamespace(value=value, date_from=None, date_to=None, data_id=data_id)


def test_horizon_value_multivalue_untagged_picks_min() -> None:
    """`Reg_SouthZone` residual: several UNDATED, untagged RHS rows →
    `_horizon_value(prefer_min=True)` returns the MINIMUM.  Known-suboptimal
    for `<=` caps (input `[187,196,232,320]`; PLEXOS uses ≥232 but gtopt picks
    187) — the per-block mapping isn't in the input, so this pins the current
    pick so any future change to it is intentional."""
    rows = [
        _rhs_row(232.0, 1),
        _rhs_row(187.42, 2),
        _rhs_row(320.0, 3),
        _rhs_row(196.6, 4),
    ]
    assert _horizon_value(rows, None, prefer_min=True) == 187.42
    # default (lowest-data_id) for contrast — NOT used for RHS
    assert _horizon_value(rows, None) == 232.0


def test_horizon_value_single_scalar_faithful() -> None:
    """`ANTUCOmax/min` (137), `CTF_DownMinProvision` (293) residual: the input
    ships a single SCALAR RHS and `_horizon_value` returns it verbatim — gtopt
    is faithful to the input.  PLEXOS runtime-derives a different time-varying
    value (60-70 / 141-603) that is NOT in the input, so gtopt neither can nor
    should fabricate it."""
    rows = [_rhs_row(137.0, 1)]
    assert _horizon_value(rows, None, prefer_min=True) == 137.0
    assert _horizon_value(rows, None) == 137.0


# --------------------------------------------------------------------------- #
# ST-Schedule honoured directly: `Include in ST Schedule == 0` → active=False,
# even for a former _FORCE_ACTIVE_PATTERNS family (Commit_/CTFOFF/_OFF/...).
# The old force-active override (kept them active despite flag==0) was removed
# — it diverged from PLEXOS by enforcing rows PLEXOS itself disabled.
# --------------------------------------------------------------------------- #
_ST_EXCLUDE_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id>
    <name>Commit_TEST</name></t_object>
  <t_object><object_id>101</object_id><class_id>70</class_id>
    <name>Commit_KEEP</name></t_object>
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
    <property_id>4370</property_id><collection_id>700</collection_id>
    <name>Include in ST Schedule</name>
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
    <membership_id>700002</membership_id><collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id><child_object_id>101</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32001</membership_id><collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32002</membership_id><collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>101</child_object_id>
  </t_membership>
  <t_data><data_id>1</data_id><membership_id>700001</membership_id>
    <property_id>4369</property_id><value>-1</value></t_data>
  <t_data><data_id>2</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>10</value></t_data>
  <t_data><data_id>3</data_id><membership_id>700001</membership_id>
    <property_id>4370</property_id><value>0</value></t_data>
  <t_data><data_id>4</data_id><membership_id>32001</membership_id>
    <property_id>393</property_id><value>1.0</value></t_data>
  <t_data><data_id>5</data_id><membership_id>700002</membership_id>
    <property_id>4369</property_id><value>-1</value></t_data>
  <t_data><data_id>6</data_id><membership_id>700002</membership_id>
    <property_id>4384</property_id><value>10</value></t_data>
  <t_data><data_id>7</data_id><membership_id>700002</membership_id>
    <property_id>4370</property_id><value>-1</value></t_data>
  <t_data><data_id>8</data_id><membership_id>32002</membership_id>
    <property_id>393</property_id><value>1.0</value></t_data>
</MasterDataSet>
"""


def test_st_schedule_zero_disables_force_active_family(tmp_path: Path) -> None:
    """`Include in ST Schedule == 0` → `active=False`, EVEN for a former
    force-active family (`Commit_*`).  `flag == -1` (PLEXOS default = include)
    → active (not disabled).  Pins the removal of the _FORCE_ACTIVE_PATTERNS
    override (gtopt now honours PLEXOS's own ST-Schedule exclusion)."""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_ST_EXCLUDE_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    by_name = {c.name: c for c in extract_user_constraints(load_xml(xml_path), bundle)}
    # flag==0 → disabled, despite the Commit_ (former force-active) name
    assert by_name["Commit_TEST"].active is False
    # flag==-1 (PLEXOS default include) → NOT disabled
    assert by_name["Commit_KEEP"].active is not False


# --------------------------------------------------------------------------- #
# Scenario-tagged ST-Schedule resolution: dual-row constraints whose `-1`
# override is gated by a Scenario object only fire when that Scenario is in
# the active Model's `Scenarios` collection (PLEXOS run semantics).  Verified
# against CEN PCP 2026-04-22 where `InflexibilityRule` / `ManageableRule` /
# `ElToroOnlyCPF` are NOT activated by `PRGdia_Full_Definitivo`, so their
# `-1` overrides are inert and the default `0` (exclude) wins.
# --------------------------------------------------------------------------- #
_ST_SCENARIO_DUAL_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_class><class_id>78</class_id><name>Scenario</name></t_class>
  <t_class><class_id>80</class_id><name>Model</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id>
    <name>UC_KeepActive</name></t_object>
  <t_object><object_id>101</object_id><class_id>70</class_id>
    <name>UC_DropInactive</name></t_object>
  <t_object><object_id>200</object_id><class_id>78</class_id>
    <name>CSFUp_ON</name></t_object>
  <t_object><object_id>201</object_id><class_id>78</class_id>
    <name>InflexibilityRule</name></t_object>
  <t_object><object_id>300</object_id><class_id>80</class_id>
    <name>PRGdia_Full_Definitivo</name></t_object>
  <t_collection>
    <collection_id>700</collection_id><parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>32</collection_id><parent_class_id>2</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>738</collection_id><parent_class_id>80</parent_class_id>
    <child_class_id>78</child_class_id><name>Scenarios</name>
  </t_collection>
  <t_property>
    <property_id>4369</property_id><collection_id>700</collection_id><name>Sense</name>
  </t_property>
  <t_property>
    <property_id>4384</property_id><collection_id>700</collection_id><name>RHS</name>
  </t_property>
  <t_property>
    <property_id>4370</property_id><collection_id>700</collection_id>
    <name>Include in ST Schedule</name>
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
    <membership_id>700002</membership_id><collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id><child_object_id>101</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32001</membership_id><collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32002</membership_id><collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>101</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>738001</membership_id><collection_id>738</collection_id>
    <parent_object_id>300</parent_object_id><child_object_id>200</child_object_id>
  </t_membership>
  <t_data><data_id>1</data_id><membership_id>700001</membership_id>
    <property_id>4369</property_id><value>-1</value></t_data>
  <t_data><data_id>2</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>10</value></t_data>
  <t_data><data_id>3</data_id><membership_id>700001</membership_id>
    <property_id>4370</property_id><value>-1</value></t_data>
  <t_data><data_id>4</data_id><membership_id>700001</membership_id>
    <property_id>4370</property_id><value>0</value></t_data>
  <t_data><data_id>5</data_id><membership_id>32001</membership_id>
    <property_id>393</property_id><value>1.0</value></t_data>
  <t_data><data_id>6</data_id><membership_id>700002</membership_id>
    <property_id>4369</property_id><value>-1</value></t_data>
  <t_data><data_id>7</data_id><membership_id>700002</membership_id>
    <property_id>4384</property_id><value>10</value></t_data>
  <t_data><data_id>8</data_id><membership_id>700002</membership_id>
    <property_id>4370</property_id><value>-1</value></t_data>
  <t_data><data_id>9</data_id><membership_id>700002</membership_id>
    <property_id>4370</property_id><value>0</value></t_data>
  <t_data><data_id>10</data_id><membership_id>32002</membership_id>
    <property_id>393</property_id><value>1.0</value></t_data>
  <t_tag><data_id>3</data_id><object_id>200</object_id></t_tag>
  <t_tag><data_id>8</data_id><object_id>201</object_id></t_tag>
</MasterDataSet>
"""


def test_st_schedule_scenario_tag_active_overrides_default(tmp_path: Path) -> None:
    """Dual-row resolution: a tagged ``-1`` whose Scenario IS in the active
    Model's Scenarios collection overrides the untagged ``0`` default →
    constraint stays active.  Untagged ``-1`` whose Scenario is NOT active
    is ignored → default ``0`` wins → constraint excluded.

    Models the CEN PCP 2026-04-22 pattern: `MAXCSF_RALCO_U1_RALCO_U2`
    (`CSFUp_ON` active → included) vs `KELAR_GNL_INF_ON`
    (`InflexibilityRule` inactive → excluded).
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_ST_SCENARIO_DUAL_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    # PlexosDb.tag_for_data populated from t_tag rows
    assert db.tag_for_data == {3: [200], 8: [201]}
    # PlexosDb.active_scenario_ids only sees the Scenario activated by
    # the Model's Scenarios collection (membership_id=738001 → 200).
    assert db.active_scenario_ids() == {200}
    by_name = {c.name: c for c in extract_user_constraints(db, bundle)}
    # CSFUp_ON IS active → -1 wins → include
    assert by_name["UC_KeepActive"].active is not False
    # InflexibilityRule is NOT active → -1 ignored → default 0 wins → exclude
    assert by_name["UC_DropInactive"].active is False


# --------------------------------------------------------------------------- #
# Reg_SouthZone: an RHS *value* row tagged with an INACTIVE scenario is a
# losing override and must NOT be folded into the per-block RHS profile —
# but a constraint whose ONLY RHS row is inactive-tagged keeps that value
# (PLEXOS still applies it as the effective RHS).
#
# Verified on CEN PCP DATOS20251005 ``Reg_SouthZone``: base RHS=320 + an
# untagged H11-17→230 timeslice row are applied, while H9-10→196.6 and
# H18-20→187.42 carry an inactive Scenario tag → PLEXOS's solution RHS is
# {320, 230} only.  ``CTF_DownMinProvision`` (single inactive-tagged
# RHS=293) is still applied → never dropped.
# --------------------------------------------------------------------------- #
_RHS_SCENARIO_TAG_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_class><class_id>78</class_id><name>Scenario</name></t_class>
  <t_class><class_id>80</class_id><name>Model</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>G1</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id>
    <name>Reg_SouthZone</name></t_object>
  <t_object><object_id>101</object_id><class_id>70</class_id>
    <name>UC_OnlyInactiveTag</name></t_object>
  <t_object><object_id>200</object_id><class_id>78</class_id>
    <name>ActiveScen</name></t_object>
  <t_object><object_id>201</object_id><class_id>78</class_id>
    <name>InactiveScen</name></t_object>
  <t_object><object_id>300</object_id><class_id>80</class_id>
    <name>PRGdia_Full_Definitivo</name></t_object>
  <t_collection>
    <collection_id>700</collection_id><parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>32</collection_id><parent_class_id>2</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>738</collection_id><parent_class_id>80</parent_class_id>
    <child_class_id>78</child_class_id><name>Scenarios</name>
  </t_collection>
  <t_property>
    <property_id>4369</property_id><collection_id>700</collection_id><name>Sense</name>
  </t_property>
  <t_property>
    <property_id>4384</property_id><collection_id>700</collection_id><name>RHS</name>
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
    <membership_id>700002</membership_id><collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id><child_object_id>101</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32001</membership_id><collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32002</membership_id><collection_id>32</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>101</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>738001</membership_id><collection_id>738</collection_id>
    <parent_object_id>300</parent_object_id><child_object_id>200</child_object_id>
  </t_membership>
  <t_data><data_id>1</data_id><membership_id>700001</membership_id>
    <property_id>4369</property_id><value>-1</value></t_data>
  <t_data><data_id>2</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>320</value></t_data>
  <t_data><data_id>3</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>230</value></t_data>
  <t_data><data_id>4</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>196.6</value></t_data>
  <t_data><data_id>5</data_id><membership_id>32001</membership_id>
    <property_id>393</property_id><value>1.0</value></t_data>
  <t_data><data_id>6</data_id><membership_id>700002</membership_id>
    <property_id>4369</property_id><value>-1</value></t_data>
  <t_data><data_id>7</data_id><membership_id>700002</membership_id>
    <property_id>4384</property_id><value>293</value></t_data>
  <t_data><data_id>8</data_id><membership_id>32002</membership_id>
    <property_id>393</property_id><value>1.0</value></t_data>
  <t_text><class_id>76</class_id><data_id>3</data_id><value>H11-17</value></t_text>
  <t_text><class_id>76</class_id><data_id>4</data_id><value>H9-10</value></t_text>
  <t_tag><data_id>4</data_id><object_id>201</object_id></t_tag>
  <t_tag><data_id>7</data_id><object_id>201</object_id></t_tag>
</MasterDataSet>
"""


def test_rhs_inactive_scenario_tag_suppressed(tmp_path: Path) -> None:
    """An RHS timeslice row tagged with an INACTIVE Scenario is dropped, so
    the emitted per-block RHS keeps only the untagged base (320) + the
    untagged timeslice override (H11-17 → 230).  The inactive-tagged
    H9-10 → 196.6 row never reaches the profile.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_RHS_SCENARIO_TAG_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    assert db.active_scenario_ids() == {200}
    by_name = {c.name: c for c in extract_user_constraints(db, bundle)}
    reg = by_name["Reg_SouthZone"]
    # 24-block profile: base 320 with H11-17 (blocks 10..16) lowered to 230;
    # H9-10 (blocks 8..9) must stay at 320, NOT 196.6.
    profile = reg.rhs_profile
    assert profile, "expected a per-block RHS profile"
    assert set(round(v, 3) for v in profile) == {320.0, 230.0}
    assert profile[8] == 320.0  # H9 — inactive-tagged 196.6 suppressed
    assert profile[10] == 230.0  # H11 — untagged override applied


def test_rhs_lone_inactive_scenario_tag_kept(tmp_path: Path) -> None:
    """When the ONLY RHS row is inactive-scenario-tagged, PLEXOS still
    applies its value (the row IS the effective RHS) — the converter must
    keep it rather than drop the whole constraint.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_RHS_SCENARIO_TAG_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    db = load_xml(xml_path)
    by_name = {c.name: c for c in extract_user_constraints(db, bundle)}
    # UC_OnlyInactiveTag carries a single RHS=293 tagged with InactiveScen;
    # it must still be emitted with that RHS (active, not dropped).
    assert "UC_OnlyInactiveTag" in by_name
    uc = by_name["UC_OnlyInactiveTag"]
    assert uc.active is not False
    assert "293" in uc.expression


# --------------------------------------------------------------------------- #
# BAT_*_CF_*_COMP: Reserve×Battery Provision Coefficient cross-product.
# PLEXOS scopes the ``α × Σ Provision[bat, rsv]`` term to (bat, rsv) pairs
# where BOTH are members of the same constraint.  Without the cross-product
# expansion the term silently dropped, leaving a degenerate
# ``-α × battery.discharge = 0`` row that the parser marked inactive →
# 10 PLEXOS-binding constraints (~$48k of shadow cost) unenforced.
# --------------------------------------------------------------------------- #
_BAT_CF_COMP_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>14</class_id><name>Reserve</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_class><class_id>76</class_id><name>Battery</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>50</object_id><class_id>76</class_id><name>BAT_X</name></t_object>
  <t_object><object_id>51</object_id><class_id>76</class_id><name>BAT_X_AUX</name></t_object>
  <t_object><object_id>60</object_id><class_id>14</class_id><name>CSF_LW_BESS</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id>
    <name>BAT_X_CF_GEN_COMP</name></t_object>
  <t_collection>
    <collection_id>700</collection_id><parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>90</collection_id><parent_class_id>76</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>172</collection_id><parent_class_id>14</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>200</collection_id><parent_class_id>14</parent_class_id>
    <child_class_id>76</child_class_id><name>Batteries</name>
  </t_collection>
  <t_property><property_id>4369</property_id><collection_id>700</collection_id>
    <name>Sense</name></t_property>
  <t_property><property_id>4384</property_id><collection_id>700</collection_id>
    <name>RHS</name></t_property>
  <t_property><property_id>967</property_id><collection_id>90</collection_id>
    <name>Generation Coefficient</name></t_property>
  <t_property><property_id>1448</property_id><collection_id>172</collection_id>
    <name>Provision Coefficient</name></t_property>
  <t_membership>
    <membership_id>700100</membership_id><collection_id>700</collection_id>
    <parent_object_id>1</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>90100</membership_id><collection_id>90</collection_id>
    <parent_object_id>51</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>90101</membership_id><collection_id>90</collection_id>
    <parent_object_id>50</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>172100</membership_id><collection_id>172</collection_id>
    <parent_object_id>60</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>200100</membership_id><collection_id>200</collection_id>
    <parent_object_id>60</parent_object_id><child_object_id>51</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>200101</membership_id><collection_id>200</collection_id>
    <parent_object_id>60</parent_object_id><child_object_id>50</child_object_id>
  </t_membership>
  <t_data><data_id>1</data_id><membership_id>700100</membership_id>
    <property_id>4369</property_id><value>-1</value></t_data>
  <t_data><data_id>2</data_id><membership_id>700100</membership_id>
    <property_id>4384</property_id><value>0</value></t_data>
  <t_data><data_id>3</data_id><membership_id>90100</membership_id>
    <property_id>967</property_id><value>-1</value></t_data>
  <t_data><data_id>4</data_id><membership_id>172100</membership_id>
    <property_id>1448</property_id><value>0.3</value></t_data>
</MasterDataSet>
"""


def test_bat_cf_comp_emits_reserve_battery_cross_product(tmp_path: Path) -> None:
    """A ``BAT_*_CF_GEN_COMP`` constraint with a Reserve→Constraint
    Provision Coefficient must emit BOTH the battery generation term
    AND the (battery, reserve) provision cross-product term.  Pins the
    fix that closes 10 PLEXOS-binding gaps (~$48k of reserve cost).
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_BAT_CF_COMP_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    by_name = {c.name: c for c in extract_user_constraints(load_xml(xml_path), bundle)}
    expr = by_name["BAT_X_CF_GEN_COMP"].expression or ""
    # Both terms must be present
    assert "battery(" in expr
    assert "discharge" in expr
    assert 'reserve_provision("provision_BAT_X_gen__CSF_LW_BESS").dn' in expr
    # Coefficient on the reserve provision term
    assert "0.3" in expr
    # Constraint should NOT be marked inactive
    assert by_name["BAT_X_CF_GEN_COMP"].active is not False


def test_st_schedule_scenario_tag_explicit_model_name(tmp_path: Path) -> None:
    """``active_scenario_ids(model_name=...)`` selects a non-default Model."""
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_ST_SCENARIO_DUAL_XML)
    db = load_xml(xml_path)
    assert db.active_scenario_ids("PRGdia_Full_Definitivo") == {200}
    # Unknown Model name → empty set (NOT KeyError)
    assert db.active_scenario_ids("NoSuchModel") == set()


# --------------------------------------------------------------------------- #
# Timeslice tags: PLEXOS recurring hour-of-day / weekday RHS modulation
# --------------------------------------------------------------------------- #


def _ts_row(
    value: float,
    data_id: int,
    timeslice: str | None = None,
    date_from: datetime | None = None,
    date_to: datetime | None = None,
) -> PlexosDataRow:
    return PlexosDataRow(
        data_id=data_id,
        membership_id=2908,
        property_id=4384,
        value=value,
        date_from=date_from,
        date_to=date_to,
        timeslice=timeslice,
    )


def test_expand_timeslice_single_hour_range() -> None:
    """`H16-20` covers hours 16-20 (inclusive) of a 24-hour day."""
    mask = _expand_timeslice("H16-20", None, 24)
    assert mask == tuple(i in {15, 16, 17, 18, 19} for i in range(24))


def test_expand_timeslice_split_shift() -> None:
    """`H1-8,H20-24` (split shift) covers the UNION of hour ranges."""
    mask = _expand_timeslice("H1-8,H20-24", None, 24)
    expected = set(range(0, 8)) | set(range(19, 24))
    assert mask == tuple(i in expected for i in range(24))


def test_expand_timeslice_weekday_intersect_hour() -> None:
    """`W2-6,H8-21` intersects weekday (Mon-Fri) AND hours 8-21.

    2026-04-22 = Wednesday (PLEXOS W4).  Over 168 blocks:
    5 weekdays × 14 hours each = 70 active blocks.
    """
    hs = datetime(2026, 4, 22)
    mask = _expand_timeslice("W2-6,H8-21", hs, 168)
    assert sum(mask) == 5 * 14


def test_expand_timeslice_tiles_across_days() -> None:
    """`H16-20` tiles its 5-hour pattern across every day of the horizon."""
    hs = datetime(2026, 4, 22)
    mask = _expand_timeslice("H16-20", hs, 168)
    assert sum(mask) == 5 * 7  # 5 hours × 7 days


def test_expand_timeslice_rejects_bad_tag() -> None:
    """A non-`H`/`W` atom raises `ValueError` (caller falls back to scalar)."""
    import pytest

    with pytest.raises(ValueError, match="unrecognised timeslice atom"):
        _expand_timeslice("XYZ", None, 24)


def test_build_rhs_profile_campiche_shape() -> None:
    """Campiche_starting: base RHS=0 + tagged row RHS=1 with `H16-20`.

    Verified against the PLEXOS solution's pid-3073 RHS output for the
    week of 2026-04-22: blocks 16-20 carry RHS=1, every other block
    carries RHS=0.  This is THE shape that motivated timeslice support.
    """
    rows = [
        _ts_row(value=0.0, data_id=28308, timeslice=None),
        _ts_row(value=1.0, data_id=28309, timeslice="H16-20"),
    ]
    prof = _build_rhs_timeslice_profile(
        rows, base_value=0.0, horizon_start=datetime(2026, 4, 22), n_blocks=24
    )
    expected = tuple([0.0] * 15 + [1.0] * 5 + [0.0] * 4)
    assert prof == expected


def test_build_rhs_profile_skips_expired_tagged_rows() -> None:
    """A timeslice-tagged row whose date window is expired is IGNORED.

    Verified against Commit_AtaTV2C: PLEXOS XML has H1-8 + H20-24 rows
    both dated 2025-03-04 (expired by run date 2026-04-22); PLEXOS's
    own solver pid-3073 reports RHS=0 every block, matching the
    untouched base.  We must not overlay expired rows.
    """
    rows = [
        _ts_row(value=0.0, data_id=28406, timeslice=None),
        _ts_row(
            value=1.0,
            data_id=28413,
            timeslice="H1-8",
            date_from=datetime(2025, 3, 4),
            date_to=datetime(2025, 3, 4),
        ),
        _ts_row(
            value=1.0,
            data_id=28414,
            timeslice="H20-24",
            date_from=datetime(2025, 3, 4),
            date_to=datetime(2025, 3, 4),
        ),
    ]
    prof = _build_rhs_timeslice_profile(
        rows, base_value=0.0, horizon_start=datetime(2026, 4, 22), n_blocks=24
    )
    # No active tagged row → no profile → caller falls back to scalar.
    assert not prof


def test_build_rhs_profile_last_write_wins_on_overlap() -> None:
    """Two ACTIVE tagged rows overlap (`H16-20` ⊂ `H7-20`): higher
    `data_id` wins on overlapping blocks (PLEXOS MDB append semantics).
    """
    rows = [
        _ts_row(value=0.0, data_id=28308, timeslice=None),
        _ts_row(value=1.0, data_id=28309, timeslice="H16-20"),
        _ts_row(value=2.0, data_id=28310, timeslice="H7-20"),
    ]
    prof = _build_rhs_timeslice_profile(
        rows, base_value=0.0, horizon_start=datetime(2026, 4, 22), n_blocks=24
    )
    # H7-20 wins (higher data_id) → blocks 7-20 = 2.0; outside = 0.0
    expected = tuple([0.0] * 6 + [2.0] * 14 + [0.0] * 4)
    assert prof == expected


def test_build_rhs_profile_untagged_row_wins_outside_overlay() -> None:
    """N_to_Nogales_N1 shape: untagged RHS=430 + tagged RHS=402.3 (H9-18).

    `_horizon_value(prefer_min=True)` would pick min(402.3, 430) = 402.3 and
    pass it as ``base_value`` — that would set 402.3 EVERYWHERE, hiding the
    tagged overlay.  ``_build_rhs_timeslice_profile`` must use the UNTAGGED
    active row's value (430) as the base for blocks outside the tag mask,
    so blocks 9-18 carry 402.3 and the rest carry 430 — matching PLEXOS's
    solver-applied RHS (pid 3073).
    """
    rows = [
        _ts_row(value=402.3, data_id=30332, timeslice="H9-18"),
        _ts_row(value=430.0, data_id=30333, timeslice=None),
    ]
    prof = _build_rhs_timeslice_profile(
        rows, base_value=402.3, horizon_start=datetime(2026, 4, 22), n_blocks=24
    )
    # blocks 9-18 (1-indexed) = idx 8-17 carry 402.3; rest carry 430
    for i in range(24):
        expected = 402.3 if 8 <= i <= 17 else 430.0
        assert prof[i] == expected, (i, prof[i])


def test_build_rhs_profile_pangueramp_three_ranges() -> None:
    """PANGUEramp: H10-15 (RHS=0), H16-24 (RHS=60), H1-9 (RHS=60).

    Verified against PLEXOS pid-3073 output: blocks 10-15 = 0,
    blocks 1-9 and 16-24 = 60.  The active ramp window is 60 MW/h
    everywhere except the midday off-window 10:00-15:00.
    """
    rows = [
        _ts_row(value=180.0, data_id=30502, timeslice=None),  # undated base
        _ts_row(value=0.0, data_id=30503, timeslice="H10-15"),
        _ts_row(value=60.0, data_id=30504, timeslice="H16-24"),
        _ts_row(value=60.0, data_id=30505, timeslice="H1-9"),
    ]
    prof = _build_rhs_timeslice_profile(
        rows, base_value=180.0, horizon_start=datetime(2026, 4, 22), n_blocks=24
    )
    # blocks 1-9 (idx 0-8) = 60; blocks 10-15 (idx 9-14) = 0;
    # blocks 16-24 (idx 15-23) = 60
    expected = tuple([60.0] * 9 + [0.0] * 6 + [60.0] * 9)
    assert prof == expected


# --------------------------------------------------------------------------- #
# extract_user_constraints honours timeslice overlay end-to-end
# --------------------------------------------------------------------------- #
_TIMESLICE_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>9</class_id><name>Horizon</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_class><class_id>76</class_id><name>Timeslice</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>5</object_id><class_id>9</class_id>
    <name>Coordinador_diario_1H_7d</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>CAMPICHE</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id>
    <name>Campiche_starting</name></t_object>
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
    <property_id>400</property_id><collection_id>32</collection_id>
    <name>Units Started Coefficient</name>
  </t_property>
  <t_membership>
    <membership_id>700001</membership_id><collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id><child_class_id>70</child_class_id>
    <parent_object_id>1</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>32001</membership_id><collection_id>32</collection_id>
    <parent_class_id>2</parent_class_id><child_class_id>70</child_class_id>
    <parent_object_id>20</parent_object_id><child_object_id>100</child_object_id>
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
    <data_id>3</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>1</value>
  </t_data>
  <t_text>
    <data_id>3</data_id><class_id>76</class_id><value>H16-20</value>
  </t_text>
  <t_data>
    <data_id>4</data_id><membership_id>32001</membership_id>
    <property_id>400</property_id><value>1.0</value>
  </t_data>
</MasterDataSet>
"""


def test_extract_user_constraints_emits_timeslice_profile(tmp_path: Path) -> None:
    """End-to-end: PLEXOS XML with `<t_text class_id=76>H16-20</t_text>`
    on an RHS row produces a UserConstraintSpec carrying the per-block
    `rhs_profile` with blocks 16-20 of each day = 1, all others = 0.

    The block count derives from the XML Horizon name
    (``Coordinador_diario_1H_7d`` → 7 days = 168 blocks); the H16-20 mask
    tiles across all 7 days, yielding 5 × 7 = 35 active blocks.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_TIMESLICE_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    ucs = extract_user_constraints(load_xml(xml_path), bundle)
    by_name = {c.name: c for c in ucs}
    spec = by_name["Campiche_starting"]
    assert spec.rhs_profile, "expected timeslice profile, got empty rhs_profile"
    assert len(spec.rhs_profile) == 168, len(spec.rhs_profile)
    # 5 hours × 7 days = 35 active blocks at value 1.0
    assert sum(1 for v in spec.rhs_profile if v == 1.0) == 35
    assert sum(1 for v in spec.rhs_profile if v == 0.0) == 168 - 35
    # Day 0 (block indices 0-23): blocks 15-19 carry RHS=1
    for i in range(24):
        expected = 1.0 if 15 <= i <= 19 else 0.0
        assert spec.rhs_profile[i] == expected, (i, spec.rhs_profile[i])


# --------------------------------------------------------------------------- #
# Partial-horizon date-window overlay (SD_2026030813 / SD_2026036857 family)
# --------------------------------------------------------------------------- #


def _dated_row(
    value: float,
    data_id: int,
    date_from: datetime | None = None,
    date_to: datetime | None = None,
) -> PlexosDataRow:
    return PlexosDataRow(
        data_id=data_id,
        membership_id=2908,
        property_id=4384,
        value=value,
        date_from=date_from,
        date_to=date_to,
        timeslice=None,
    )


def test_block_in_date_window_basic_hours() -> None:
    """A block overlaps its date window when their hour ranges intersect.

    Run-horizon starts 2026-04-22T00:00, blocks are 1 h each, so block ``i``
    spans ``[hs + ih, hs + (i+1)h)``.  A dated row ``[06:00, 10:00]``
    overlaps blocks 6, 7, 8, 9 (and ONLY those).
    """
    hs = datetime(2026, 4, 22)
    df = datetime(2026, 4, 22, 6)
    dt = datetime(2026, 4, 22, 10)
    in_window = [i for i in range(24) if _block_in_date_window(i, hs, df, dt)]
    assert in_window == [6, 7, 8, 9], in_window


def test_block_in_date_window_open_ended() -> None:
    """An undated boundary (``None``) treats the window as ±∞."""
    hs = datetime(2026, 4, 22)
    # No upper bound: every block from 06:00 onwards is in
    in_from = [
        i
        for i in range(24)
        if _block_in_date_window(i, hs, datetime(2026, 4, 22, 6), None)
    ]
    assert in_from == list(range(6, 24)), in_from
    # No lower bound: every block before 10:00 is in
    in_to = [
        i
        for i in range(24)
        if _block_in_date_window(i, hs, None, datetime(2026, 4, 22, 10))
    ]
    assert in_to == list(range(0, 10)), in_to
    # Both unbounded: every block is in
    in_all = [i for i in range(24) if _block_in_date_window(i, hs, None, None)]
    assert in_all == list(range(24))


def test_build_rhs_date_overlay_sd_2026030813_shape() -> None:
    """SD_2026030813_NvaPAzucar_Polpaico500_neg: undated RHS=10000 base +
    dated RHS=1600 covering 2026-04-22T06:00 .. 10:00 (4 blocks).

    Verified against PLEXOS solution pid-3073 RHS output: blocks 6-9 of the
    week (run starts 2026-04-22) carry RHS=1600; all other 164 blocks
    carry RHS=10000.  Earlier gtopt collapsed the dated row through
    ``_horizon_value`` (horizon_start ∉ window → fallback to undated
    10000 sentinel) and stubbed the whole UC inactive, missing $1.0-1.1M
    of PLEXOS slack per such constraint.
    """
    rows = [
        _dated_row(10000.0, 76824),
        _dated_row(
            1600.0,
            76826,
            date_from=datetime(2026, 4, 22, 6),
            date_to=datetime(2026, 4, 22, 10),
        ),
    ]
    prof = _build_rhs_date_overlay_profile(
        rows, base_value=10000.0, horizon_start=datetime(2026, 4, 22), n_blocks=168
    )
    assert len(prof) == 168
    # Blocks 6, 7, 8, 9 carry the dated overlay 1600
    assert [i for i, v in enumerate(prof) if v == 1600.0] == [6, 7, 8, 9]
    # Every other block carries the undated 10000
    assert sum(1 for v in prof if v == 10000.0) == 164


def test_build_rhs_date_overlay_sd_2026036857_shape() -> None:
    """SD_2026036857_LVilos_*: dated window spans Apr 21 23:00 .. Apr 22
    06:00 — only the LAST 1 h of Apr 21 + the FIRST 5 h of Apr 22 are
    inside the horizon.  Verified against PLEXOS pid-3073: blocks 0-5 of
    the week carry RHS=896, all others carry the undated 10000.
    """
    rows = [
        _dated_row(10000.0, 76803),
        _dated_row(
            896.0,
            76842,
            date_from=datetime(2026, 4, 21, 23),
            date_to=datetime(2026, 4, 22, 6),
        ),
    ]
    prof = _build_rhs_date_overlay_profile(
        rows, base_value=10000.0, horizon_start=datetime(2026, 4, 22), n_blocks=168
    )
    assert [i for i, v in enumerate(prof) if v == 896.0] == [0, 1, 2, 3, 4, 5]


def test_build_rhs_date_overlay_skips_when_no_dated_row() -> None:
    """No dated rows ⇒ empty tuple ⇒ caller keeps the scalar path."""
    rows = [_dated_row(10000.0, 76824)]
    prof = _build_rhs_date_overlay_profile(
        rows, base_value=10000.0, horizon_start=datetime(2026, 4, 22), n_blocks=168
    )
    assert not prof


def test_build_rhs_date_overlay_skips_when_window_outside_horizon() -> None:
    """A dated row whose window is entirely outside the horizon contributes
    nothing; the function returns ``()`` so the scalar path takes over."""
    rows = [
        _dated_row(10000.0, 76824),
        _dated_row(
            1600.0,
            76826,
            date_from=datetime(2025, 1, 1),
            date_to=datetime(2025, 1, 2),
        ),
    ]
    prof = _build_rhs_date_overlay_profile(
        rows, base_value=10000.0, horizon_start=datetime(2026, 4, 22), n_blocks=168
    )
    assert not prof


def test_build_rhs_date_overlay_ignores_timeslice_tagged_rows() -> None:
    """Rows that ALSO carry a timeslice tag are routed through
    :func:`_build_rhs_timeslice_profile`, not this builder.  Mixing the
    two in one path would double-overlay.
    """
    tagged = PlexosDataRow(
        data_id=76826,
        membership_id=0,
        property_id=4384,
        value=1600.0,
        date_from=datetime(2026, 4, 22, 6),
        date_to=datetime(2026, 4, 22, 10),
        timeslice="H7-9",
    )
    rows = [_dated_row(10000.0, 76824), tagged]
    prof = _build_rhs_date_overlay_profile(
        rows, base_value=10000.0, horizon_start=datetime(2026, 4, 22), n_blocks=168
    )
    assert not prof


def test_build_rhs_date_overlay_returns_empty_without_horizon_start() -> None:
    """The overlay needs a calendar reference; ``horizon_start=None`` ⇒ ``()``."""
    rows = [
        _dated_row(10000.0, 76824),
        _dated_row(
            1600.0,
            76826,
            date_from=datetime(2026, 4, 22, 6),
            date_to=datetime(2026, 4, 22, 10),
        ),
    ]
    prof = _build_rhs_date_overlay_profile(
        rows, base_value=10000.0, horizon_start=None, n_blocks=168
    )
    assert not prof


# --------------------------------------------------------------------------- #
# _index_coefficient_rows: date-active coefficient row selection
# --------------------------------------------------------------------------- #
_COEF_AMENDMENT_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>9</class_id><name>Horizon</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_class><class_id>72</class_id><name>Decision Variable</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>5</object_id><class_id>9</class_id>
    <name>Coordinador_diario_1H_7d</name></t_object>
  <t_object><object_id>40</object_id><class_id>72</class_id>
    <name>Generation_SEN</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id>
    <name>CPF_Up5Calculation</name></t_object>
  <t_collection>
    <collection_id>700</collection_id><parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>710</collection_id><parent_class_id>72</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_property>
    <property_id>4369</property_id><collection_id>700</collection_id><name>Sense</name>
  </t_property>
  <t_property>
    <property_id>4384</property_id><collection_id>700</collection_id><name>RHS</name>
  </t_property>
  <t_property>
    <property_id>4438</property_id><collection_id>710</collection_id>
    <name>Value Coefficient</name>
  </t_property>
  <t_membership>
    <membership_id>700001</membership_id><collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id><child_class_id>70</child_class_id>
    <parent_object_id>1</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>710001</membership_id><collection_id>710</collection_id>
    <parent_class_id>72</parent_class_id><child_class_id>70</child_class_id>
    <parent_object_id>40</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_data>
    <data_id>1</data_id><membership_id>700001</membership_id>
    <property_id>4369</property_id><value>0</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>700001</membership_id>
    <property_id>4384</property_id><value>4.125</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>710001</membership_id>
    <property_id>4438</property_id><value>12.7111</value>
  </t_data>
  <t_date_from><data_id>3</data_id><date>2022-12-27T00:00:00</date></t_date_from>
  <t_date_to><data_id>3</data_id><date>2024-12-31T00:00:00</date></t_date_to>
  <t_data>
    <data_id>4</data_id><membership_id>710001</membership_id>
    <property_id>4438</property_id><value>13.37</value>
  </t_data>
  <t_date_from><data_id>4</data_id><date>2025-04-16T00:00:00</date></t_date_from>
  <t_date_to><data_id>4</data_id><date>2026-02-11T00:00:00</date></t_date_to>
  <t_data>
    <data_id>5</data_id><membership_id>710001</membership_id>
    <property_id>4438</property_id><value>10.75</value>
  </t_data>
</MasterDataSet>
"""


def test_coefficient_amendments_filter_expired_rows(tmp_path: Path) -> None:
    """CPF_Up5Calculation: three Value Coefficient rows on the same
    Decision Variable → Constraint membership — two with expired date
    windows (12.7111 active 2022-12-27..2024-12-31, 13.37 active
    2025-04-16..2026-02-11) and one live undated (10.75).

    Before the date filter, ``_index_coefficient_rows`` summed all three
    → 36.83 — verified to produce the spurious ``CPF5mUp_Requirement +
    36.83·Generation_SEN ... = 4.125`` row carrying $913K of gtopt soft
    slack.  After the fix only the active row's value (10.75) appears
    in the emitted UserConstraint, matching PLEXOS pid-3073 exactly.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_COEF_AMENDMENT_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    ucs = extract_user_constraints(load_xml(xml_path), bundle)
    by_name = {c.name: c for c in ucs}
    spec = by_name["CPF_Up5Calculation"]
    # The summed-coefficient bug would produce "12.7111 * ... + 13.37 *
    # ... + 10.75 * ..." (three terms).  Fixed version has ONE term per
    # decision_variable, with the value of the active undated row.
    assert spec.expression.count("Generation_SEN") == 1, spec.expression
    assert "10.75 * decision_variable" in spec.expression, spec.expression
    # And the dead historical values must NOT leak through:
    assert "12.7111" not in spec.expression, spec.expression
    assert "13.37" not in spec.expression, spec.expression


# --------------------------------------------------------------------------- #
# GEN_BAT_/LOAD_BAT_ "battery-shutoff" modeling-artifact detection
# --------------------------------------------------------------------------- #
_GEN_BAT_SHUTOFF_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>7</class_id><name>Battery</name></t_class>
  <t_class><class_id>9</class_id><name>Horizon</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>5</object_id><class_id>9</class_id>
    <name>Coordinador_diario_1H_7d</name></t_object>
  <t_object><object_id>30</object_id><class_id>7</class_id>
    <name>BAT_VICTOR_JARA_FV</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id>
    <name>GEN_BAT_VICTOR_JARA_FV</name></t_object>
  <t_object><object_id>101</object_id><class_id>70</class_id>
    <name>LOAD_BAT_VICTOR_JARA_FV</name></t_object>
  <t_collection>
    <collection_id>700</collection_id><parent_class_id>1</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_collection>
    <collection_id>90</collection_id><parent_class_id>7</parent_class_id>
    <child_class_id>70</child_class_id><name>Constraints</name>
  </t_collection>
  <t_property>
    <property_id>4369</property_id><collection_id>700</collection_id><name>Sense</name>
  </t_property>
  <t_property>
    <property_id>4384</property_id><collection_id>700</collection_id><name>RHS</name>
  </t_property>
  <t_property>
    <property_id>4376</property_id><collection_id>700</collection_id>
    <name>Include in ST Schedule</name>
  </t_property>
  <t_property>
    <property_id>967</property_id><collection_id>90</collection_id>
    <name>Generation Coefficient</name>
  </t_property>
  <t_property>
    <property_id>968</property_id><collection_id>90</collection_id>
    <name>Load Coefficient</name>
  </t_property>
  <t_membership>
    <membership_id>1001</membership_id><collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id><child_class_id>70</child_class_id>
    <parent_object_id>1</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>1002</membership_id><collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id><child_class_id>70</child_class_id>
    <parent_object_id>1</parent_object_id><child_object_id>101</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>1003</membership_id><collection_id>90</collection_id>
    <parent_class_id>7</parent_class_id><child_class_id>70</child_class_id>
    <parent_object_id>30</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>1004</membership_id><collection_id>90</collection_id>
    <parent_class_id>7</parent_class_id><child_class_id>70</child_class_id>
    <parent_object_id>30</parent_object_id><child_object_id>101</child_object_id>
  </t_membership>
  <t_data>
    <data_id>1</data_id><membership_id>1001</membership_id>
    <property_id>4376</property_id><value>-1</value>
  </t_data>
  <t_data>
    <data_id>2</data_id><membership_id>1001</membership_id>
    <property_id>4384</property_id><value>0</value>
  </t_data>
  <t_data>
    <data_id>3</data_id><membership_id>1002</membership_id>
    <property_id>4376</property_id><value>-1</value>
  </t_data>
  <t_data>
    <data_id>4</data_id><membership_id>1002</membership_id>
    <property_id>4384</property_id><value>0</value>
  </t_data>
  <t_data>
    <data_id>5</data_id><membership_id>1003</membership_id>
    <property_id>967</property_id><value>1</value>
  </t_data>
  <t_data>
    <data_id>6</data_id><membership_id>1004</membership_id>
    <property_id>968</property_id><value>1</value>
  </t_data>
</MasterDataSet>
"""


def test_gen_bat_load_bat_shutoff_artifact_emits_inactive(tmp_path: Path) -> None:
    """PLEXOS ships 35 GEN_BAT_<name> + 35 LOAD_BAT_<name> source UCs with
    Sense=None (→ equality default), RHS=0, and a single Battery LHS term
    (``Generation Coefficient = 1`` or ``Load Coefficient = 1``).
    Literally these force ``battery.discharge = 0`` / ``battery.charge =
    0`` — i.e. the battery off.  PLEXOS itself drops the whole family from
    the ST schedule (verified against the RES20260422 solution: none of
    the 70 appear in t_object, while the batteries do dispatch).  Before
    this fix gtopt emitted them as SOFT equalities at $10/MWh penalty,
    burning ~$182K of slack on BAT_VICTOR_JARA_FV alone and similar on
    the other 33 batteries with ``Include in ST Schedule = -1`` (the
    other 56 already had ``Include = 0`` and were excluded earlier).
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_GEN_BAT_SHUTOFF_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    emitted = {"Battery": frozenset({"BAT_VICTOR_JARA_FV"})}
    ucs = extract_user_constraints(load_xml(xml_path), bundle, emitted_names=emitted)
    by_name = {c.name: c for c in ucs}
    assert by_name["GEN_BAT_VICTOR_JARA_FV"].active is False, by_name[
        "GEN_BAT_VICTOR_JARA_FV"
    ]
    assert by_name["LOAD_BAT_VICTOR_JARA_FV"].active is False


# --------------------------------------------------------------------------- #
# Generator-shutoff modeling artifact (PEHUENCHE_GENT7def pattern)
# --------------------------------------------------------------------------- #
_GEN_SHUTOFF_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>9</class_id><name>Horizon</name></t_class>
  <t_class><class_id>70</class_id><name>Constraint</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>5</object_id><class_id>9</class_id>
    <name>Coordinador_diario_1H_7d</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>PEHUENCHE_U2</name></t_object>
  <t_object><object_id>100</object_id><class_id>70</class_id>
    <name>PEHUENCHE_GENT7def</name></t_object>
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
    <property_id>393</property_id><collection_id>32</collection_id>
    <name>Generation Coefficient</name>
  </t_property>
  <t_membership>
    <membership_id>1001</membership_id><collection_id>700</collection_id>
    <parent_class_id>1</parent_class_id><child_class_id>70</child_class_id>
    <parent_object_id>1</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>1003</membership_id><collection_id>32</collection_id>
    <parent_class_id>2</parent_class_id><child_class_id>70</child_class_id>
    <parent_object_id>20</parent_object_id><child_object_id>100</child_object_id>
  </t_membership>
  <t_data>
    <data_id>2</data_id><membership_id>1001</membership_id>
    <property_id>4384</property_id><value>0</value>
  </t_data>
  <t_data>
    <data_id>5</data_id><membership_id>1003</membership_id>
    <property_id>393</property_id><value>-1</value>
  </t_data>
</MasterDataSet>
"""


def test_generator_shutoff_artifact_emits_inactive(tmp_path: Path) -> None:
    """PEHUENCHE_GENT7def shape: PLEXOS XML carries TWO Generation Coefficient
    memberships (PEHUENCHE_U1 + U2, both coef=-1) with Sense=None (default
    equality) and RHS=0.  Taken literally these force both gens off.

    On the CEN PCP run date PEHUENCHE_U1 has pmax=0 (offline), so gtopt's
    silent-zero-drop pruned its term and emitted ``-PEHUENCHE_U2.generation
    = 0`` as a SOFT equality — pinning U2 to zero output.  That drove the
    −11,576 MWh / −68.8% PEHUENCHE_U2 under-dispatch vs PLEXOS (which
    drops the whole UC from the ST schedule and lets both units run).

    This fixture replicates the post-drop residual (single −1*gen=0 with
    Sense=None default equality) and asserts the new detector flips the
    constraint to ``active=False`` so the LP no longer pays soft slack on
    it and U2 dispatches naturally.
    """
    xml_path = tmp_path / "DBSEN_PRGDIARIO.xml"
    xml_path.write_text(_GEN_SHUTOFF_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path)
    emitted = {"Generator": frozenset({"PEHUENCHE_U2"})}
    ucs = extract_user_constraints(load_xml(xml_path), bundle, emitted_names=emitted)
    by_name = {c.name: c for c in ucs}
    assert by_name["PEHUENCHE_GENT7def"].active is False, by_name["PEHUENCHE_GENT7def"]
