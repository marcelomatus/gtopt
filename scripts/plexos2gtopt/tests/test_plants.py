"""Unit tests for the native :class:`PlantSpec` extraction and writer.

The PLEXOS converter used to ship a soft ``PlantCap_<stem>``
:class:`UserConstraintSpec` per multi-fuel-band Generator family — the
LP enforced ``Σ generation ≤ pmax`` via a per-block UC with a 10 000
$/MWh slack column.  The native :class:`Plant` primitive replaces that
with a single hard :class:`PlantSpec` whose ``PlantLP`` emits one
``plant_cap_<name>`` row per (stage, block).

These tests pin:

  * ``extract_plants`` groups generators by name-stem (fuel-band suffix
    stripped) and only emits a PlantSpec when ≥ 2 active variants
    share a stem.
  * ``build_plant_array`` round-trips the PlantSpec into the gtopt
    JSON shape consumed by ``include/gtopt/json/json_plant.hpp``.
"""

from __future__ import annotations

from plexos2gtopt.entities import (
    BundleSpec,
    GeneratorSpec,
    PlantSpec,
    PlexosCase,
)
from plexos2gtopt.gtopt_writer import build_plant_array
from plexos2gtopt.parsers import extract_plants


def _gen(name: str, pmax: float = 100.0) -> GeneratorSpec:
    """Helper — a generator on a dummy bus with the given pmax."""
    return GeneratorSpec(
        object_id=0,
        name=name,
        bus_name="b_a",
        pmax=pmax,
        fuel_names=(),
    )


def _empty_case(gens: tuple[GeneratorSpec, ...]) -> PlexosCase:
    """A minimal :class:`PlexosCase` with just the generator set."""
    return PlexosCase(
        bundle=BundleSpec(bundle_name="test", n_days=1),
        generators=gens,
    )


def test_extract_plants_emits_one_spec_per_multi_variant_family() -> None:
    """Two fuel-band variants of one plant → one PlantSpec with both names.

    ``NUEVA_RENCA-TG+TV_GN_A`` + ``NUEVA_RENCA-TG+TV_GN_B`` share the
    stem ``NUEVA_RENCA-TG+TV`` after the ``_GN_<band>`` suffix is
    stripped; the converter emits one :class:`PlantSpec` covering both.
    """
    case = _empty_case(
        (
            _gen("NUEVA_RENCA-TG+TV_GN_A", pmax=391.0),
            _gen("NUEVA_RENCA-TG+TV_GN_B", pmax=391.0),
            # Different stem — must NOT be merged in.
            _gen("OTHER_PLANT", pmax=50.0),
        )
    )
    plants = extract_plants(case)
    assert len(plants) == 1
    plant = plants[0]
    assert plant.name == "NUEVA_RENCA-TG+TV"
    assert set(plant.generator_names) == {
        "NUEVA_RENCA-TG+TV_GN_A",
        "NUEVA_RENCA-TG+TV_GN_B",
    }
    assert plant.pmax == 391.0
    # uniq_mutex stays off by default — the Σ-cap subsumes the mutex
    # when every variant runs near pmax, and PLEXOS does not always
    # ship a per-plant config-binary.
    assert plant.uniq_mutex is False
    assert plant.n_units is None


def test_extract_plants_skips_single_variant_family() -> None:
    """One-variant families: nothing to cap → no PlantSpec emitted."""
    case = _empty_case(
        (
            _gen("ALONE_PLANT_GN_A", pmax=100.0),
            # A different stem entirely — also a singleton.
            _gen("AGAIN_ALONE_DIE", pmax=50.0),
        )
    )
    plants = extract_plants(case)
    assert not plants


def test_extract_plants_skips_inactive_variants() -> None:
    """A family with one active + one zero-pmax variant has only one
    active variant left, and is therefore skipped."""
    case = _empty_case(
        (
            _gen("MIX_GN_A", pmax=100.0),
            _gen("MIX_GN_B", pmax=0.0),  # inactive
        )
    )
    plants = extract_plants(case)
    assert not plants


def test_extract_plants_emits_three_variant_plant() -> None:
    """A 3-variant family → one PlantSpec carrying all 3 names."""
    case = _empty_case(
        (
            _gen("CC_GNL_A", pmax=200.0),
            _gen("CC_GNL_B", pmax=180.0),
            _gen("CC_GNL_C", pmax=190.0),
        )
    )
    plants = extract_plants(case)
    assert len(plants) == 1
    plant = plants[0]
    assert plant.name == "CC"
    assert len(plant.generator_names) == 3
    # pmax = max single-config envelope.
    assert plant.pmax == 200.0


def test_extract_plants_picks_max_peak_pmax() -> None:
    """``pmax`` = max(peak_pmax across active variants)."""
    case = _empty_case(
        (
            _gen("PLNT_GN_A", pmax=120.0),
            _gen("PLNT_GN_B", pmax=300.0),
            _gen("PLNT_GN_C", pmax=200.0),
        )
    )
    plants = extract_plants(case)
    assert len(plants) == 1
    assert plants[0].pmax == 300.0


def test_build_plant_array_round_trips_spec_into_gtopt_json() -> None:
    """``build_plant_array`` maps :class:`PlantSpec` 1-to-1 onto the
    JSON dict shape that :file:`json_plant.hpp` consumes."""
    plants = (
        PlantSpec(
            name="ATA_CC_1",
            generator_names=(
                "ATA_CC_1_ConfTGA",
                "ATA_CC_1_ConfTGB",
                "ATA_CC_1_ConfTV",
            ),
            pmax=391.0,
            n_units=1,
            uniq_mutex=True,
        ),
    )
    out = build_plant_array(plants)
    assert len(out) == 1
    entry = out[0]
    assert entry["uid"] == 1
    assert entry["name"] == "ATA_CC_1"
    assert entry["generator_names"] == [
        "ATA_CC_1_ConfTGA",
        "ATA_CC_1_ConfTGB",
        "ATA_CC_1_ConfTV",
    ]
    assert entry["pmax"] == 391.0
    assert entry["n_units"] == 1
    assert entry["uniq_mutex"] is True


def test_build_plant_array_omits_default_optional_fields() -> None:
    """When the spec leaves ``n_units`` / ``uniq_mutex`` /
    ``commit_coeffs`` at their no-op defaults, the writer must drop
    them from the JSON dict (cleaner artifact for diffs / inspection).
    """
    plants = (
        PlantSpec(
            name="MIN_PLANT",
            generator_names=("G1", "G2"),
            pmax=50.0,
        ),
    )
    out = build_plant_array(plants)
    assert len(out) == 1
    entry = out[0]
    assert set(entry) == {"uid", "name", "generator_names", "pmax"}


def test_build_plant_array_emits_sequential_uids() -> None:
    """Each plant gets a unique ``uid`` starting at 1, preserving
    insertion order — the same convention as every other ``build_*``
    writer in this module."""
    plants = (
        PlantSpec(name="P1", generator_names=("a", "b"), pmax=1.0),
        PlantSpec(name="P2", generator_names=("c", "d"), pmax=2.0),
        PlantSpec(name="P3", generator_names=("e", "f"), pmax=3.0),
    )
    out = build_plant_array(plants)
    assert [e["uid"] for e in out] == [1, 2, 3]
    assert [e["name"] for e in out] == ["P1", "P2", "P3"]
