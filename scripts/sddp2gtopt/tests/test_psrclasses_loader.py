"""Tests for :mod:`sddp2gtopt.psrclasses_loader`."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from sddp2gtopt.psrclasses_loader import (
    PSRCLASSES_FILENAME,
    PsrClassesLoader,
    load_psrclasses,
)


def test_filename_constant() -> None:
    assert PSRCLASSES_FILENAME == "psrclasses.json"


def test_load_case0_collections(case0_psrclasses: Path) -> None:
    """case0 has a known-fixed set of collections; assert presence."""
    loader = PsrClassesLoader.from_file(case0_psrclasses)
    expected = {
        "PSRStudy",
        "PSRSystem",
        "PSRDemand",
        "PSRDemandSegment",
        "PSRFuel",
        "PSRThermalPlant",
        "PSRHydroPlant",
        "PSRGaugingStation",
    }
    present = set(loader.collections())
    missing = expected - present
    assert not missing, f"missing collections: {missing}"


def test_count_known_cardinalities(case0_psrclasses: Path) -> None:
    """Sanity: case0 has 1 study, 1 system, 3 thermal, 1 hydro, 2 fuels."""
    loader = PsrClassesLoader.from_file(case0_psrclasses)
    assert loader.count("PSRStudy") == 1
    assert loader.count("PSRSystem") == 1
    assert loader.count("PSRThermalPlant") == 3
    assert loader.count("PSRHydroPlant") == 1
    assert loader.count("PSRFuel") == 2
    assert loader.count("PSRGaugingStation") == 2
    assert loader.count("DoesNotExist") == 0


def test_first_returns_none_for_missing(case0_psrclasses: Path) -> None:
    loader = PsrClassesLoader.from_file(case0_psrclasses)
    assert loader.first("PSRStudy") is not None
    assert loader.first("MissingCollection") is None


def test_entities_returns_copy(case0_psrclasses: Path) -> None:
    """``entities`` should return a fresh list so callers can mutate it."""
    loader = PsrClassesLoader.from_file(case0_psrclasses)
    a = loader.entities("PSRThermalPlant")
    a.clear()
    b = loader.entities("PSRThermalPlant")
    assert len(b) == 3, "internal data was mutated through entities()"


def test_iter_entities_total(case0_psrclasses: Path) -> None:
    loader = PsrClassesLoader.from_file(case0_psrclasses)
    total = sum(1 for _ in loader.iter_entities())
    expected = sum(loader.count(c) for c in loader.collections())
    assert total == expected


def test_reference_id_resolution(case0_psrclasses: Path) -> None:
    """Each entity's reference_id resolves back to itself."""
    loader = PsrClassesLoader.from_file(case0_psrclasses)
    sample = loader.first("PSRThermalPlant")
    assert sample is not None
    ref = sample["reference_id"]
    assert loader.by_reference_id(ref) is sample
    assert loader.collection_of(ref) == "PSRThermalPlant"
    assert loader.by_reference_id(-1) is None
    assert loader.collection_of(-1) is None


def test_thermal_plant_links_to_system(case0_psrclasses: Path) -> None:
    """Each thermal plant's ``system`` field should resolve to a PSRSystem."""
    loader = PsrClassesLoader.from_file(case0_psrclasses)
    system = loader.first("PSRSystem")
    assert system is not None
    sys_ref = system["reference_id"]
    for plant in loader.entities("PSRThermalPlant"):
        assert plant.get("system") == sys_ref


def test_load_case_dir_helper(case0_dir: Path) -> None:
    loader = load_psrclasses(case0_dir)
    assert loader.count("PSRStudy") == 1


def test_missing_file_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        PsrClassesLoader.from_file(tmp_path / "missing.json")
    with pytest.raises(FileNotFoundError):
        PsrClassesLoader.from_case_dir(tmp_path)


def test_malformed_top_level_raises(tmp_path: Path) -> None:
    bad = tmp_path / "psrclasses.json"
    bad.write_text("[1, 2, 3]", encoding="utf-8")
    with pytest.raises(ValueError, match="expected top-level object"):
        PsrClassesLoader.from_file(bad)


def test_malformed_collection_raises(tmp_path: Path) -> None:
    bad = tmp_path / "psrclasses.json"
    bad.write_text(json.dumps({"PSRStudy": {"not": "a list"}}), encoding="utf-8")
    with pytest.raises(ValueError, match="must be a list"):
        PsrClassesLoader.from_file(bad)
