"""Unit + integration tests for piecewise gcost, noload_cost, and FlowRight.fcost."""

from __future__ import annotations

import json
import lzma
import shutil
from pathlib import Path

from plexos2gtopt.entities import (
    BundleSpec,
    FlowRightSpec,
    FuelSpec,
    GeneratorSpec,
    PlexosCase,
)
from plexos2gtopt.gtopt_writer import (
    VIRTUAL_FUEL_NAME,
    build_flow_right_array,
    build_generator_array,
    build_planning,
)
from plexos2gtopt.plexos2gtopt import convert_plexos_bundle


_FIXTURES = Path(__file__).resolve().parent / "data" / "plexos_coad"


def test_piecewise_emits_segments_and_fuel_ref() -> None:
    """build_generator_array emits pmax/heat_rate_segments + a fuel ref."""
    gens = (
        GeneratorSpec(
            object_id=1,
            name="g1",
            bus_name="b1",
            pmin=0.0,
            pmax=100.0,
            pmax_segments=(50.0, 100.0),
            heat_rate_segments=(40.5, 41.5),
            fuel_price_override=2.0,
            fuel_names=(),
        ),
    )
    out = build_generator_array(gens, fuels=())
    g = out[0]
    assert g["fuel"] == VIRTUAL_FUEL_NAME
    assert g["pmax_segments"] == [50.0, 100.0]
    # Pre-multiplied by fuel_price_override=2.0 → 81, 83.
    assert g["heat_rate_segments"] == [81.0, 83.0]
    # gcost is just vom + transport here (zero).
    assert g["gcost"] == 0.0


def test_piecewise_with_real_fuel_does_not_premultiply() -> None:
    """When the generator has a Fuel membership, slopes are NOT pre-multiplied."""
    gens = (
        GeneratorSpec(
            object_id=1,
            name="g1",
            bus_name="b1",
            pmin=0.0,
            pmax=100.0,
            pmax_segments=(50.0, 100.0),
            heat_rate_segments=(40.5, 41.5),
            fuel_names=("diesel",),
        ),
    )
    fuels = (FuelSpec(object_id=10, name="diesel", price=2.0),)
    out = build_generator_array(gens, fuels)
    # Fuel ref points at the real fuel; gtopt multiplies internally.
    assert out[0]["fuel"] == "diesel"
    # Slopes emit as-is (gtopt does the × price inside the LP).
    assert out[0]["heat_rate_segments"] == [40.5, 41.5]


def test_scalar_path_unchanged_when_no_segments() -> None:
    """Scalar generator with a known Fuel + heat_rate now emits the
    explicit Fuel FK + heat_rate (un-baked).

    PR #490 split the scalar path into two: when a Fuel FK + heat_rate
    are both known, the writer emits ``fuel`` + ``heat_rate`` + the
    NON-FUEL adder as ``gcost`` so gtopt's ``FuelLP.max_offtake``
    cap row can find this generator at LP-build.  gtopt then
    reconstructs the per-MWh cost as
    ``fuel.price × heat_rate + gcost`` (= 0.5 × 10 + 3 = 8 here).
    """
    gens = (
        GeneratorSpec(
            object_id=1,
            name="g1",
            bus_name="b1",
            pmax=100.0,
            heat_rate=0.5,
            vom_charge=3.0,
            fuel_names=("diesel",),
        ),
    )
    fuels = (FuelSpec(object_id=10, name="diesel", price=10.0),)
    out = build_generator_array(gens, fuels)
    # Explicit Fuel FK + heat_rate; gcost is the non-fuel adder.
    assert out[0]["fuel"] == "diesel"
    assert out[0]["heat_rate"] == 0.5
    assert out[0]["gcost"] == 3.0
    # Piecewise is still not present in the scalar path.
    assert "pmax_segments" not in out[0]
    assert "heat_rate_segments" not in out[0]


def test_legacy_baked_path_preserves_heat_rate_metadata() -> None:
    """Thermals with a real heat_rate but no Fuel FK (PLEXOS CCGT
    mode-variants like ``ATA-TG1A_GNL_X`` or CSV-only entries like
    ``UJINA_U5_DIE``) bake ``HR × price`` into ``gcost`` and *also*
    publish the raw ``heat_rate`` as informational metadata so the
    PLEXOS HR signal survives the conversion.  The LP behaviour is
    unchanged: gtopt's ``GeneratorLP`` ignores ``heat_rate`` when no
    ``fuel`` is bound (``source/generator_lp.cpp:151-154``).
    """
    gens = (
        GeneratorSpec(
            object_id=1,
            name="ATA-TG1A_GNL_X",
            bus_name="b1",
            pmax=100.0,
            heat_rate=0.21,
            vom_charge=2.0,
            fuel_transport=0.5,
            fuel_price_override=300.0,
            fuel_names=(),
        ),
    )
    out = build_generator_array(gens, fuels=())
    entry = out[0]
    assert entry["heat_rate"] == 0.21
    assert "fuel" not in entry
    # gcost = HR × price + vom + transport = 0.21·300 + 2 + 0.5 = 65.5
    assert entry["gcost"] == 65.5


def test_legacy_baked_path_skips_heat_rate_for_renewables() -> None:
    """Renewables (heat_rate=0, no fuel) must not emit a redundant
    ``heat_rate=0`` field — only thermals with a real HR signal get
    the metadata.
    """
    gens = (
        GeneratorSpec(
            object_id=1,
            name="solar_fv",
            bus_name="b1",
            pmax=50.0,
            heat_rate=0.0,
            vom_charge=0.0,
            fuel_names=(),
        ),
    )
    out = build_generator_array(gens, fuels=())
    assert "heat_rate" not in out[0]
    assert out[0]["gcost"] == 0.0


def test_build_planning_synthesises_virtual_fuel() -> None:
    """When any gen needs piecewise without a real fuel, planning ships the virtual one."""
    case = PlexosCase(
        bundle=BundleSpec(),
        generators=(
            GeneratorSpec(
                object_id=1,
                name="g1",
                bus_name="b1",
                pmax=100.0,
                pmax_segments=(50.0, 100.0),
                heat_rate_segments=(40.5, 41.5),
                fuel_price_override=2.0,
            ),
        ),
    )
    planning = build_planning(case, name="pw")
    fuels = planning["system"].get("fuel_array", [])
    names = {f["name"] for f in fuels}
    assert VIRTUAL_FUEL_NAME in names
    # Unit price → gtopt's fuel × heat_rate_segments returns whatever
    # the converter pre-multiplied.
    virtual = next(f for f in fuels if f["name"] == VIRTUAL_FUEL_NAME)
    assert virtual["price"] == 1.0


def test_build_planning_skips_virtual_fuel_when_no_piecewise() -> None:
    """No piecewise generators → no virtual-fuel injection."""
    case = PlexosCase(
        bundle=BundleSpec(),
        generators=(
            GeneratorSpec(
                object_id=1, name="g1", bus_name="b1", pmax=100.0, heat_rate=0.5
            ),
        ),
    )
    planning = build_planning(case, name="scalar")
    fuels = planning["system"].get("fuel_array", [])
    assert all(f["name"] != VIRTUAL_FUEL_NAME for f in fuels)


def test_flow_right_fcost_emits_tb_matrix() -> None:
    """FlowRight.fcost broadcasts the scalar across the 24-block horizon."""
    fr = (
        FlowRightSpec(
            name="irr", junction_name="J", fmax=50.0, fcost=10.0, purpose="irrigation"
        ),
    )
    out = build_flow_right_array(fr)
    assert "fcost" in out[0]
    # Shape: 1 stage × 24 blocks, every entry = 10.0.
    assert len(out[0]["fcost"]) == 1
    assert len(out[0]["fcost"][0]) == 24
    assert out[0]["fcost"][0][7] == 10.0


def test_flow_right_fcost_zero_is_omitted() -> None:
    """fcost = 0 → no fcost key (hard bound)."""
    fr = (FlowRightSpec(name="hard", junction_name="J", fmax=50.0, fcost=0.0),)
    out = build_flow_right_array(fr)
    assert "fcost" not in out[0]


def test_118_bus_piecewise_end_to_end(tmp_path: Path) -> None:
    """Converting 118-Bus.xml produces piecewise segments + virtual fuel."""
    bundle_dir = tmp_path / "bundle"
    bundle_dir.mkdir()
    src = _FIXTURES / "118-Bus.xml.xz"
    if not src.is_file():
        import pytest

        pytest.skip(f"fixture not vendored: {src}")
    with lzma.open(src, "rb") as fin, (bundle_dir / "DBSEN_PRGDIARIO.xml").open(
        "wb"
    ) as fout:
        shutil.copyfileobj(fin, fout)
    out_dir = tmp_path / "out"
    out_file = out_dir / "118.json"
    convert_plexos_bundle(
        {
            "input_bundle": bundle_dir,
            "output_dir": out_dir,
            "output_file": out_file,
            # Synthetic IEEE 118-bus has no CEN PCP solution .accdb;
            # use uniform-hourly explicitly.
            "horizon_mode": "hourly",
            "horizon_days": 1,
        }
    )
    planning = json.loads(out_file.read_text())
    gens = planning["system"]["generator_array"]
    fuels = planning["system"].get("fuel_array", [])
    # All 54 gens get piecewise + virtual-fuel reference.
    assert all("heat_rate_segments" in g for g in gens)
    assert all(g.get("fuel") == VIRTUAL_FUEL_NAME for g in gens)
    # Each segment array has exactly 2 entries (midpoint + pmax).
    for g in gens:
        assert len(g["pmax_segments"]) == 2
        assert len(g["heat_rate_segments"]) == 2
    # Virtual fuel sits in the fuel_array with price 1.
    virtual = next((f for f in fuels if f["name"] == VIRTUAL_FUEL_NAME), None)
    assert virtual is not None and virtual["price"] == 1.0
