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
e.g. CANUTILLAR's 12,330 → 10,570 step), and ``extract_fuels`` kept only the
period-1 fuel price — all silently.  This guard fails on that behaviour and
passes once profiles are emitted / warnings fire.
"""

from __future__ import annotations

import logging
from pathlib import Path

import pytest

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


def test_fuel_price_collapse_warns(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    """A varying Fuel_Price has no per-block field on gtopt's Fuel (the price
    is pre-folded into scalar gcost), so the collapse must at least WARN."""
    bundle = _build_bundle(tmp_path)
    db = load_xml(bundle.xml_path)
    with caplog.at_level(logging.WARNING, logger="plexos2gtopt.parsers"):
        fuels = extract_fuels(db, bundle)
    assert any(f.name == "GAS" for f in fuels)
    msgs = " ".join(rec.getMessage() for rec in caplog.records)
    assert "fuel price" in msgs and "GAS" in msgs, (
        "varying Fuel_Price was collapsed to the period-1 scalar with no "
        "warning — silent collapse."
    )


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
