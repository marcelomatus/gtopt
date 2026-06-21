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


def test_turbine_pf_is_scalar_not_per_block(tmp_path: Path) -> None:
    """``Turbine.production_factor`` is a SCALAR (MW per m³/s).  PLEXOS
    "Efficiency Incr" is not time-varying — its proper varying axis is
    generation Load Point or reservoir volume / head, the latter modelled by
    gtopt's volume-indexed ``ReservoirProductionFactor`` (PLP "rendimiento").
    So even a varying ``Hydro_EfficiencyIncr`` is carried as the scalar
    production factor, NOT a per-block turbine profile."""
    from plexos2gtopt.gtopt_writer import build_turbine_array
    from plexos2gtopt.parsers import extract_turbines

    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text(_TURBINE_XML)
    # Even if Hydro_EfficiencyIncr varies across the day (1.6 → 0.8), the
    # turbine takes the (first nonzero) scalar production factor.
    (tmp_path / "Hydro_EfficiencyIncr.csv").write_text(
        _long_csv("HYDRO", _varying(1.6, 0.8))
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path, n_days=1)
    bundle.block_layout = tuple((h,) for h in range(1, 25))
    db = load_xml(bundle.xml_path)
    turbines = extract_turbines(db, bundle)
    tur = next(t for t in turbines if t.generator_name == "HYDRO")

    # No per-period profile field on the spec — production_factor is scalar.
    assert not hasattr(tur, "pf_profile")
    assert tur.production_factor == pytest.approx(1.6)  # first nonzero value

    entry = next(
        e
        for e in build_turbine_array(
            turbines, extra_waterways=[], block_layout=bundle.block_layout
        )
        if e["generator"] == "HYDRO"
    )
    pf = entry["production_factor"]
    assert not _is_profile(pf), (
        "Turbine.production_factor must be a scalar, not a per-block profile "
        "— head/volume variation belongs to ReservoirProductionFactor."
    )
    assert pf == pytest.approx(1.6)


def test_start_shutdown_cost_emit_per_block_profile() -> None:
    """A varying Gen_StartCost / Gen_ShutDownCost must now be carried as a
    per-period profile on the CommitmentSpec AND emitted as a per-block
    ``[[...]]`` matrix by ``build_commitment_array`` — no longer collapsed to
    the period-1 scalar (the catalog showed Gen_StartCost varies for 210 units
    and Gen_ShutDownCost for 181 units across the 14 CEN cases)."""
    from plexos2gtopt.entities import CommitmentSpec
    from plexos2gtopt.gtopt_writer import build_commitment_array

    block_layout = tuple((h,) for h in range(1, 25))
    su_series = tuple(_varying(1000.0, 5000.0))
    sd_series = tuple(_varying(200.0, 800.0))
    cmt = CommitmentSpec(
        generator_name="THERM",
        startup_cost=su_series[0],
        shutdown_cost=sd_series[0],
        startup_cost_profile=su_series,
        shutdown_cost_profile=sd_series,
    )

    # The full per-period series must be carried on the spec (the parser's
    # job — verified here as a structural invariant of the spec contract).
    assert cmt.startup_cost_profile
    assert len(set(cmt.startup_cost_profile)) > 1
    assert max(cmt.startup_cost_profile) == pytest.approx(5000.0)
    assert cmt.shutdown_cost_profile
    assert len(set(cmt.shutdown_cost_profile)) > 1

    # The writer must emit per-block matrices (not scalars).
    entry = next(
        e
        for e in build_commitment_array((cmt,), block_layout=block_layout)
        if e["generator"] == "THERM"
    )
    su = entry["startup_cost"]
    assert _is_profile(su), (
        "varying Gen_StartCost collapsed to a scalar in build_commitment_array "
        "— the per-block profile was not emitted."
    )
    su_blocks = su[0]
    assert len(set(su_blocks)) > 1
    assert max(su_blocks) == pytest.approx(5000.0)

    sd = entry["shutdown_cost"]
    assert _is_profile(sd), (
        "varying Gen_ShutDownCost collapsed to a scalar in "
        "build_commitment_array — the per-block profile was not emitted."
    )
    sd_blocks = sd[0]
    assert len(set(sd_blocks)) > 1
    assert max(sd_blocks) == pytest.approx(800.0)


def test_constant_start_cost_emits_scalar() -> None:
    """Positive control: a CONSTANT Gen_StartCost stays a scalar (no needless
    per-block profile) — back-compat with the legacy per-stage cost."""
    from plexos2gtopt.entities import CommitmentSpec
    from plexos2gtopt.gtopt_writer import build_commitment_array

    block_layout = tuple((h,) for h in range(1, 25))
    cmt = CommitmentSpec(
        generator_name="THERM",
        startup_cost=3000.0,
        startup_cost_profile=(3000.0,) * 24,
    )
    entry = next(
        e
        for e in build_commitment_array((cmt,), block_layout=block_layout)
        if e["generator"] == "THERM"
    )
    assert not _is_profile(entry["startup_cost"])  # constant ⇒ scalar suffices
    assert entry["startup_cost"] == pytest.approx(3000.0)


def test_single_period_start_cost_broadcasts_not_zero_padded() -> None:
    """A monthly single-period start cost (``[v, 0, 0, …]`` after read with
    ``fill_forward=False``) must fill-forward to the scalar — NOT emit a
    ``[v, 0, 0, …]`` matrix that would make starting free in later blocks."""
    from plexos2gtopt.entities import CommitmentSpec
    from plexos2gtopt.gtopt_writer import build_commitment_array

    block_layout = tuple((h,) for h in range(1, 25))
    # Defined only at period 1, zero-padded thereafter.
    cmt = CommitmentSpec(
        generator_name="THERM",
        startup_cost=4200.0,
        startup_cost_profile=(4200.0,) + (0.0,) * 23,
    )
    entry = next(
        e
        for e in build_commitment_array((cmt,), block_layout=block_layout)
        if e["generator"] == "THERM"
    )
    # Fill-forward ⇒ constant ⇒ scalar (not a [v, 0, 0, …] matrix).
    assert not _is_profile(entry["startup_cost"])
    assert entry["startup_cost"] == pytest.approx(4200.0)


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


def _xml_with_static_min(static_min: float) -> str:
    """``_XML`` with a static System→Storages ``Min Volume`` property for
    RES_VARY (membership 520) set to ``static_min`` — the *stale default*
    that the per-period CSV must override."""
    inject = f"""
  <t_property>
    <property_id>7000</property_id>
    <collection_id>93</collection_id>
    <name>Min Volume</name>
  </t_property>
  <t_data>
    <data_id>9001</data_id>
    <membership_id>520</membership_id>
    <property_id>7000</property_id>
    <value>{static_min}</value>
  </t_data>
"""
    return _XML.replace("</MasterDataSet>", inject + "</MasterDataSet>")


@pytest.mark.parametrize(
    ("static_min", "csv_floor", "label"),
    [
        # POLCURA-like: stale static default ABOVE the per-period CSV floor.
        # PLEXOS draws down to 3.093; gtopt must NOT over-constrain to 3.998.
        (3.9983877, 3.0929944, "polcura_static_above_csv"),
        # CANUTILLAR-like: stale static default BELOW the CSV floor (1041 vs
        # 5741.7).  gtopt must raise the floor to the value PLEXOS enforces.
        (1041.0625, 5741.7384, "canutillar_static_below_csv"),
    ],
)
def test_constant_emin_csv_overrides_static_min_volume(
    tmp_path: Path, static_min: float, csv_floor: float, label: str
) -> None:
    """CLASS GUARD: a CONSTANT ``Hydro_MinVolume.csv`` series is PLEXOS's
    authoritative operational floor and OVERRIDES the static ``Min Volume``
    default — in BOTH directions.  The old converter used ``emin =
    static_emin`` (and ``max([static_emin] + hours)`` per block), so it
    over-constrained POLCURA (static 3.998 > CSV 3.093) and under-constrained
    CANUTILLAR (static 1041 < CSV 5741.7).  The emitted scalar ``emin`` must
    equal the CSV value, not the static default."""
    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text(_xml_with_static_min(static_min))
    (tmp_path / "Hydro_MinVolume.csv").write_text(
        _wide_csv("RES_VARY", [csv_floor] * 24)
    )
    # A non-zero cap so the reservoir is kept (not dropped as a zero-storage
    # topology node).
    (tmp_path / "Hydro_MaxVolume.csv").write_text(
        _wide_csv("RES_VARY", [max(static_min, csv_floor) * 2.0] * 24)
    )
    bundle = PlexosBundle(root=tmp_path, source=tmp_path, n_days=1)
    bundle.block_layout = tuple((h,) for h in range(1, 25))
    db = load_xml(bundle.xml_path)
    # Sanity: the static default really is what we injected (so the test is
    # exercising the override, not a no-op).
    assert db.static_property("Storage", 30, "Min Volume") == pytest.approx(static_min)
    res = next(r for r in extract_reservoirs(db, bundle) if r.name == "RES_VARY")
    assert not res.emin_profile  # constant ⇒ scalar, no profile
    assert res.emin == pytest.approx(csv_floor), (
        f"[{label}] constant Hydro_MinVolume ({csv_floor}) must override the "
        f"static Min Volume default ({static_min}); got emin={res.emin}"
    )


def test_zero_pf_turbine_warns(tmp_path: Path, caplog) -> None:  # type: ignore[no-untyped-def]
    """GUARD: a turbine whose production_factor resolves to 0 — no
    Hydro_EfficiencyIncr.csv value AND no static 'Efficiency Incr' (default
    0.0) — must emit a WARNING.  A PF=0 turbine converts flow to 0 MW, so any
    commitment/reserve/UC constraint that forces the generator on becomes
    infeasible; it must never be created silently and then dispatched."""
    import logging

    from plexos2gtopt.parsers import extract_turbines

    # _TURBINE_XML ships HYDRO -> RES (Head Storage) but NO 'Efficiency Incr'
    # property, and we write NO Hydro_EfficiencyIncr.csv ⇒ PF resolves to 0.
    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text(_TURBINE_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path, n_days=1)
    bundle.block_layout = tuple((h,) for h in range(1, 25))
    db = load_xml(bundle.xml_path)
    with caplog.at_level(logging.WARNING):
        turbines = extract_turbines(db, bundle)
    tur = next(t for t in turbines if t.generator_name == "HYDRO")
    assert (tur.production_factor or 0.0) <= 0.0  # PF really is 0
    assert any("production_factor=0" in r.getMessage() for r in caplog.records), (
        "a PF=0 turbine was emitted without a warning — it could be dispatched "
        "silently and cause an opaque LP infeasibility"
    )


# HYDRO (gen) -> RES (Head Storage) with a head-effect volume->efficiency curve:
# System->Generator collection (3) carrying Head Effects Method (enabled) plus
# paired multi-band Head Storage Volume/Efficiency Point properties.
_HEAD_EFFECT_XML = f"""<?xml version="1.0" standalone="yes"?>
<MasterDataSet xmlns="{NS[1:-1]}">
  <t_class><class_id>1</class_id><name>System</name></t_class>
  <t_class><class_id>2</class_id><name>Generator</name></t_class>
  <t_class><class_id>8</class_id><name>Storage</name></t_class>
  <t_object><object_id>1</object_id><class_id>1</class_id><name>SEN</name></t_object>
  <t_object><object_id>20</object_id><class_id>2</class_id><name>HYDRO</name></t_object>
  <t_object><object_id>30</object_id><class_id>8</class_id><name>RES</name></t_object>
  <t_collection>
    <collection_id>3</collection_id><parent_class_id>1</parent_class_id>
    <child_class_id>2</child_class_id><name>Generators</name>
  </t_collection>
  <t_collection>
    <collection_id>50</collection_id><parent_class_id>2</parent_class_id>
    <child_class_id>8</child_class_id><name>Head Storage</name>
  </t_collection>
  <t_membership>
    <membership_id>700</membership_id><collection_id>3</collection_id>
    <parent_object_id>1</parent_object_id><child_object_id>20</child_object_id>
  </t_membership>
  <t_membership>
    <membership_id>600</membership_id><collection_id>50</collection_id>
    <parent_object_id>20</parent_object_id><child_object_id>30</child_object_id>
  </t_membership>
  <t_property>
    <property_id>800</property_id><collection_id>3</collection_id>
    <name>Head Storage Volume Point</name>
  </t_property>
  <t_property>
    <property_id>801</property_id><collection_id>3</collection_id>
    <name>Head Storage Efficiency Point</name>
  </t_property>
  <t_property>
    <property_id>802</property_id><collection_id>3</collection_id>
    <name>Head Effects Method</name>
  </t_property>
  <t_data><data_id>10</data_id><membership_id>700</membership_id><property_id>802</property_id><value>1</value></t_data>
  <t_data><data_id>11</data_id><membership_id>700</membership_id><property_id>800</property_id><value>10.0</value></t_data>
  <t_data><data_id>12</data_id><membership_id>700</membership_id><property_id>800</property_id><value>50.0</value></t_data>
  <t_data><data_id>13</data_id><membership_id>700</membership_id><property_id>801</property_id><value>1.0</value></t_data>
  <t_data><data_id>14</data_id><membership_id>700</membership_id><property_id>801</property_id><value>2.0</value></t_data>
</MasterDataSet>
"""


def test_reservoir_production_factor_emitted_from_head_effect_data(
    tmp_path: Path,
) -> None:
    """(b) Head-effect path: when a bundle ships PLEXOS head-storage
    volume/efficiency points (with Head Effects Method enabled), emit a
    ReservoirProductionFactor with a concave volume→PF curve."""
    from plexos2gtopt.parsers import extract_reservoir_production_factors

    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text(_HEAD_EFFECT_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path, n_days=1)
    db = load_xml(bundle.xml_path)
    rpfs = extract_reservoir_production_factors(db, bundle)

    assert len(rpfs) == 1
    r = rpfs[0]
    assert r.turbine_name == "HYDRO"
    assert r.reservoir_name == "RES"
    assert r.mean_production_factor == pytest.approx(1.5)  # (1.0 + 2.0) / 2
    assert [s.volume for s in r.segments] == [
        pytest.approx(10.0),
        pytest.approx(50.0),
    ]
    # PF at the breakpoints, with the local concave slope toward the next.
    assert r.segments[0].constant == pytest.approx(1.0)
    assert r.segments[0].slope == pytest.approx((2.0 - 1.0) / (50.0 - 10.0))
    assert r.segments[1].constant == pytest.approx(2.0)
    assert r.segments[1].slope == pytest.approx(0.0)  # flat past the last point


def test_no_reservoir_production_factor_without_head_data(tmp_path: Path) -> None:
    """(b) No-op when the bundle ships no head-storage curve properties — the
    case for every CEN PCP bundle (turbines keep their scalar PF)."""
    from plexos2gtopt.parsers import extract_reservoir_production_factors

    # _TURBINE_XML has a Head Storage membership but no head-storage-point
    # properties, exactly like the CEN PCP schema.
    (tmp_path / "DBSEN_PRGDIARIO.xml").write_text(_TURBINE_XML)
    bundle = PlexosBundle(root=tmp_path, source=tmp_path, n_days=1)
    db = load_xml(bundle.xml_path)
    assert not extract_reservoir_production_factors(db, bundle)
