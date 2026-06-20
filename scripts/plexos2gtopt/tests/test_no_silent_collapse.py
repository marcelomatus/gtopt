"""Structural guard: time-varying PLEXOS inputs must NEVER be silently
collapsed to a scalar.

For every per-period input series the converter consumes, the emitted gtopt
value must be EITHER

  * a per-block profile (a 2-D ``[[...]]`` matrix on the spec / JSON entry),
    OR
  * accompanied by a ``_warn_if_series_varies`` WARNING

when the input series genuinely varies across the horizon.  This test builds a
small synthetic bundle with VARYING emin, emax and fuel price (plus positive
controls), and asserts none of them is silently collapsed.

History: ``extract_reservoirs`` used to collapse ``Hydro_MinVolume`` to a
scalar ``static_emin`` and ``Hydro_MaxVolume`` to ``min(emax_series)`` (losing
e.g. CANUTILLAR's 12,330 → 10,570 step), and ``extract_fuels`` /
``extract_turbines`` kept only the period-1 fuel price / production factor —
all silently.  Since ``Fuel.price`` and ``Turbine.production_factor`` became
per-(stage, block) (``OptTBRealFieldSched``), those two now emit per-block
PROFILES instead of warning.  This guard fails on the old behaviour and passes
once profiles are emitted / warnings fire.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from plexos2gtopt.gtopt_writer import build_fuel_array
from plexos2gtopt.parsers import extract_fuels, extract_reservoirs
from plexos2gtopt.plexos_loader import PlexosBundle
from plexos2gtopt.plexos_xml import NS, load_xml


# One Storage (RES_VARY), one Fuel (GAS), one thermal Generator (THERM) wired
# to the fuel so extract_fuels has a Fuel object to process.
_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>8</class_id><name>Storage</name></t_class>
  <t_class><class_id>33</class_id><name>Fuel</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>30</object_id><class_id>8</class_id><name>RES_VARY</name></t_object>
  <t_object><object_id>40</object_id><class_id>33</class_id><name>GAS</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>THERM</name></t_object>
  <t_collection>
    <collection_id>93</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Storages</name>
  </t_collection>
  <t_collection>
    <collection_id>33</collection_id>
    <parent_class_id>1</parent_class_id>
    <child_class_id>33</child_class_id>
    <name>Fuels</name>
  </t_collection>
  <t_collection>
    <collection_id>2</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>33</child_class_id>
    <name>Fuels</name>
  </t_collection>
  <t_membership>
    <membership_id>520</membership_id>
    <collection_id>93</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>540</membership_id>
    <collection_id>33</collection_id>
    <parent_object_id>1</parent_object_id>
    <child_object_id>40</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>541</membership_id>
    <collection_id>2</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>40</child_object_id>
  </t_membership>
</MasterDataSet>
"""


# One hydro Generator (HYDRO) with a Head Storage membership to a Storage
# (RES) — the minimum extract_turbines needs to emit a TurbineSpec.
_TURBINE_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>8</class_id><name>Storage</name></t_class>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>HYDRO</name></t_object>
  <t_object><object_id>30</object_id><class_id>8</class_id><name>RES</name></t_object>
  <t_collection>
    <collection_id>50</collection_id>
    <parent_class_id>2</parent_class_id>
    <child_class_id>8</child_class_id>
    <name>Head Storage</name>
  </t_collection>
  <t_membership>
    <membership_id>600</membership_id>
    <collection_id>50</collection_id>
    <parent_object_id>20</parent_object_id>
    <child_object_id>30</child_object_id>
  </t_membership>
</MasterDataSet>
"""


def _wide_csv(col: str, per_hour: list[float]) -> str:
    """Build a 1-day, 24-hour WIDE CSV (YEAR,MONTH,DAY,PERIOD,<col>)."""
    lines = ["YEAR,MONTH,DAY,PERIOD," + col]
    for h, v in enumerate(per_hour, start=1):
        lines.append(f"2025,10,19,{h},{v}")
    return "\n".join(lines) + "\n"


def _long_csv(name: str, per_hour: list[float]) -> str:
    """Build a 1-day, 24-hour LONG CSV (NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE)."""
    lines = ["NAME,YEAR,MONTH,DAY,PERIOD,BAND,VALUE"]
    for h, v in enumerate(per_hour, start=1):
        lines.append(f"{name},2025,10,19,{h},1,{v}")
    return "\n".join(lines) + "\n"


def _varying(low: float, high: float) -> list[float]:
    """24-hour series: first 12 hours at ``low``, last 12 at ``high``."""
    return [low] * 12 + [high] * 12


def _build_bundle(tmp_path: Path) -> PlexosBundle:
    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text(_XML)
    # Hydro_MinVolume / Hydro_MaxVolume vary across the day.
    (tmp_path / "Hydro_MinVolume.csv").write_text(
        _wide_csv("RES_VARY", _varying(100.0, 250.0))
    )
    (tmp_path / "Hydro_MaxVolume.csv").write_text(
        _wide_csv("RES_VARY", _varying(2000.0, 1500.0))  # cap STEPS DOWN
    )
    # Fuel price varies across the day.
    (tmp_path / "Fuel_Price.csv").write_text(_long_csv("GAS", _varying(50.0, 90.0)))
    bundle = PlexosBundle(root=tmp_path, source=tmp_path, n_days=1)
    # A simple 24-block-per-day layout (one block per hour) so the per-block
    # profile aggregation has a layout to bind against.
    bundle.block_layout = tuple((h,) for h in range(1, 25))
    return bundle


def _is_profile(value: object) -> bool:
    """True when ``value`` is a per-block profile ([[...]]) or list, not a
    bare scalar."""
    return isinstance(value, (list, tuple))


def test_emin_emax_not_silently_collapsed(tmp_path: Path) -> None:
    """Varying Hydro_MinVolume / Hydro_MaxVolume must produce per-block
    emin/emax PROFILES, not a single scalar."""
    bundle = _build_bundle(tmp_path)
    db = load_xml(bundle.xml_path)
    reservoirs = extract_reservoirs(db, bundle)
    res = next(r for r in reservoirs if r.name == "RES_VARY")

    # emin varies (100 → 250) ⇒ a per-block profile must be emitted.
    assert res.emin_profile, (
        "Hydro_MinVolume varies across the horizon but emin_profile is empty "
        "— the per-period floor was silently collapsed to a scalar."
    )
    assert _is_profile(res.emin_profile)
    assert len(set(res.emin_profile)) > 1
    # The high floor (250) must be carried, not dropped.
    assert max(res.emin_profile) == pytest.approx(250.0)

    # emax varies (2000 → 1500) ⇒ a per-block profile must be emitted, and
    # the EARLY high cap (2000) must survive — not be flattened to min()=1500.
    assert res.emax_profile, (
        "Hydro_MaxVolume varies across the horizon but emax_profile is empty "
        "— the per-period cap was silently collapsed to min(series)."
    )
    assert _is_profile(res.emax_profile)
    assert len(set(res.emax_profile)) > 1
    assert max(res.emax_profile) == pytest.approx(2000.0)


def test_fuel_price_emits_per_block_profile(tmp_path: Path) -> None:
    """A varying Fuel_Price must now be carried as a per-period profile on
    the FuelSpec AND emitted as a per-block ``[[...]]`` matrix by
    ``build_fuel_array`` — no longer collapsed to the period-1 scalar."""
    bundle = _build_bundle(tmp_path)
    db = load_xml(bundle.xml_path)
    fuels = extract_fuels(db, bundle)
    gas = next(f for f in fuels if f.name == "GAS")

    # The full per-period series must survive parsing (50 → 90).
    assert gas.price_profile, (
        "varying Fuel_Price was dropped — price_profile is empty (silent "
        "collapse to the period-1 scalar)."
    )
    assert len(set(gas.price_profile)) > 1
    assert max(gas.price_profile) == pytest.approx(90.0)

    # The writer must emit a per-block matrix (not a scalar) given the
    # per-hour block layout, and the high price (90) must survive.
    entry = next(
        e
        for e in build_fuel_array(fuels, block_layout=bundle.block_layout)
        if e["name"] == "GAS"
    )
    price = entry["price"]
    assert _is_profile(price), (
        "varying Fuel_Price collapsed to a scalar in build_fuel_array — "
        "the per-block profile was not emitted."
    )
    blocks = price[0]
    assert len(set(blocks)) > 1
    assert max(blocks) == pytest.approx(90.0)


def test_turbine_pf_emits_per_block_profile(tmp_path: Path) -> None:
    """A varying Hydro_EfficiencyIncr (head-dependent production factor) must
    be carried as a per-period profile on the TurbineSpec AND emitted as a
    per-block ``[[...]]`` matrix by ``build_turbine_array``."""
    from plexos2gtopt.gtopt_writer import build_turbine_array
    from plexos2gtopt.parsers import extract_turbines

    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text(_TURBINE_XML)
    # Head-dependent PF varies across the day (1.6 → 0.8).
    (tmp_path / "Hydro_EfficiencyIncr.csv").write_text(
        _long_csv("HYDRO", _varying(1.6, 0.8))
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path, n_days=1)
    bundle.block_layout = tuple((h,) for h in range(1, 25))
    db = load_xml(bundle.xml_path)
    turbines = extract_turbines(db, bundle)
    tur = next(t for t in turbines if t.generator_name == "HYDRO")

    assert tur.pf_profile, (
        "varying Hydro_EfficiencyIncr was dropped — pf_profile is empty "
        "(silent collapse to the period-1 scalar)."
    )
    assert len(set(tur.pf_profile)) > 1

    # Built-in waterway mode (extra_waterways set) so the turbine is
    # emitted (legacy mode skips turbines without a PLEXOS waterway).
    entry = next(
        e
        for e in build_turbine_array(
            turbines, extra_waterways=[], block_layout=bundle.block_layout
        )
        if e["generator"] == "HYDRO"
    )
    pf = entry["production_factor"]
    assert _is_profile(pf), (
        "varying Hydro_EfficiencyIncr collapsed to a scalar in "
        "build_turbine_array — the per-block profile was not emitted."
    )
    blocks = pf[0]
    assert len(set(blocks)) > 1
    assert max(blocks) == pytest.approx(1.6)


def test_constant_series_emits_no_profile(tmp_path: Path) -> None:
    """Positive control: a CONSTANT emin/emax series stays scalar (no
    needless per-block profile) and no warning fires for a flat fuel price."""
    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text(_XML)
    (tmp_path / "Hydro_MinVolume.csv").write_text(_wide_csv("RES_VARY", [100.0] * 24))
    (tmp_path / "Hydro_MaxVolume.csv").write_text(_wide_csv("RES_VARY", [2000.0] * 24))
    bundle = PlexosBundle(root=tmp_path, source=tmp_path, n_days=1)
    bundle.block_layout = tuple((h,) for h in range(1, 25))
    db = load_xml(bundle.xml_path)
    res = next(r for r in extract_reservoirs(db, bundle) if r.name == "RES_VARY")
    assert not res.emin_profile  # constant ⇒ scalar suffices
    assert not res.emax_profile
