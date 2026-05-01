"""Smoke + scope tests for the upstream PSR sample cases.

`case1`, `case2`, `case3` come from `psrenergy/PSRClassesInterface.jl`
under MPL-2.0.  They are larger and more diverse than `case0` and
exercise PSR collections that v0 deliberately does not handle yet
(``PSRBus`` topology, multi-system, ``PSRBattery``, ``PSRGenerator``,
``PSRSerie`` transmission lines).  These tests therefore verify two
things only:

1. The loader can ingest the JSON without raising — proving the
   schema-agnostic design holds against real-world cases.
2. The converter recognises that the case is out of v0 scope and
   either rejects with a clear ``ValueError`` (multi-system) or runs
   without error when v0 happens to be sufficient.

Once v1+ implementations land, these tests will be lifted to cover
end-to-end conversion.
"""

from __future__ import annotations

from pathlib import Path

import pytest

from sddp2gtopt.parsers import (
    parse_demands,
    parse_hydro_plants,
    parse_study,
    parse_systems,
    parse_thermal_plants,
)
from sddp2gtopt.psrclasses_loader import PsrClassesLoader
from sddp2gtopt.sddp2gtopt import convert_sddp_case, validate_sddp_case


# --- case1 (IEEE-123) -------------------------------------------------


def test_case1_loader_smoke(psri_case1_dir: Path) -> None:
    """The IEEE-123 case parses without raising."""
    loader = PsrClassesLoader.from_case_dir(psri_case1_dir)
    # Sanity: collection cardinalities documented in the upstream README.
    assert loader.count("PSRBus") == 129
    assert loader.count("PSRSerie") == 130  # transmission lines
    assert loader.count("PSRTransformer") == 4
    assert loader.count("PSRBattery") == 3
    assert loader.count("PSRGenerator") == 9
    assert loader.count("PSRSystem") == 1


def test_case1_validate_passes(psri_case1_dir: Path) -> None:
    """v0's ``--validate`` only checks Study + System presence."""
    assert validate_sddp_case({"input_dir": psri_case1_dir}) is True


def test_case1_thermal_only_subset_parses(psri_case1_dir: Path) -> None:
    """Even though we cannot convert IEEE-123 yet, our parsers must
    not blow up on its data — they should ignore unknown collections
    and surface what they understand.
    """
    loader = PsrClassesLoader.from_case_dir(psri_case1_dir)
    study = parse_study(loader)
    assert study.num_systems == 1
    systems = parse_systems(loader)
    assert len(systems) == 1
    # case1 puts thermals in PSRThermalPlant *and* the renewable mix
    # in PSRGenerator (which v0 doesn't yet read), so the thermal
    # parser sees only the PSRThermalPlant subset.
    thermals = parse_thermal_plants(loader)
    assert len(thermals) == 5


def test_case1_demand_parser_handles_85_loads(psri_case1_dir: Path) -> None:
    """case1 has both ``PSRDemand`` (1 entity) and 85 ``PSRLoad``
    rows — the latter is per-bus disaggregation.  v0 reads only
    ``PSRDemand`` and its segment.
    """
    loader = PsrClassesLoader.from_case_dir(psri_case1_dir)
    demands = parse_demands(loader)
    assert len(demands) == 1
    assert demands[0].name == "LOAD"


# --- case2 / case3 (multi-system hydrothermal) -----------------------


@pytest.mark.parametrize("fixture_name", ["psri_case2_dir", "psri_case3_dir"])
def test_multi_system_cases_validate_but_reject_convert(
    fixture_name: str, request: pytest.FixtureRequest, tmp_path: Path
) -> None:
    """Multi-system cases pass ``--validate`` (Study + System present)
    but the writer rejects them so we don't silently flatten across
    interconnections.
    """
    case_dir: Path = request.getfixturevalue(fixture_name)
    assert validate_sddp_case({"input_dir": case_dir}) is True
    with pytest.raises(ValueError, match="single-system"):
        convert_sddp_case({"input_dir": case_dir, "output_dir": tmp_path / "x"})


def test_case2_collections_present(psri_case2_dir: Path) -> None:
    """Confirm the v1+ feature surface we need to support."""
    loader = PsrClassesLoader.from_case_dir(psri_case2_dir)
    assert loader.count("PSRSystem") == 2
    assert loader.count("PSRThermalPlant") == 3
    assert loader.count("PSRHydroPlant") == 2
    assert loader.count("PSRGaugingStation") == 2
    # Constraints + interconnection + reservoir-set features for v1+
    assert loader.count("PSRInterconnection") >= 1
    assert loader.count("PSRGenerationConstraintData") >= 1
    assert loader.count("PSRReserveGenerationConstraintData") >= 1
    assert loader.count("PSRReservoirSet") >= 1


def test_case2_systems_have_distinct_names(psri_case2_dir: Path) -> None:
    systems = parse_systems(PsrClassesLoader.from_case_dir(psri_case2_dir))
    assert {s.name for s in systems} == {"System 1", "System 2"}


def test_case2_hydro_plants_named(psri_case2_dir: Path) -> None:
    hydros = parse_hydro_plants(PsrClassesLoader.from_case_dir(psri_case2_dir))
    assert {h.name for h in hydros} == {"Hydro 1", "Hydro 2"}
