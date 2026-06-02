"""Unit tests for the Sienna 5-bus CSV / YAML parsers."""

from __future__ import annotations

from sienna_to_gtopt._bundle import extract_bundle
from sienna_to_gtopt._reader import (
    SiennaBranch,
    SiennaBus,
    SiennaGen,
    load_case,
    parse_branches,
    parse_buses,
    parse_generators,
    parse_hydro_upstream,
    parse_user_descriptors,
)


def test_parse_buses_yields_five_buses():
    case_dir = extract_bundle("5bus")
    buses = parse_buses(case_dir / "bus.csv")
    assert len(buses) == 5
    ids = sorted(b.bus_id for b in buses)
    assert ids == [1, 2, 3, 4, 10]
    # Bus 4 is the REF; carries a non-zero MW Load.
    bus4 = next(b for b in buses if b.bus_id == 4)
    assert bus4.bus_type == "REF"
    assert bus4.mw_load > 0.0


def test_parse_branches_yields_seven_branches():
    case_dir = extract_bundle("5bus")
    branches = parse_branches(case_dir / "branch.csv")
    assert len(branches) == 7
    uids = [b.uid for b in branches]
    # Spot-check: the CSV's first row is "branch4" (bus 2 → bus 3).
    assert uids[0] == "branch4"
    first = branches[0]
    assert first.from_bus == 2
    assert first.to_bus == 3
    assert first.rate == 80.0
    # All branches have non-zero series reactance (no DC links in the
    # source bundle — that's something the hvdc builder synthesises).
    for branch in branches:
        assert branch.x > 0.0


def test_parse_generators_yields_thermal_plus_hydro():
    case_dir = extract_bundle("5bus")
    gens = parse_generators(case_dir / "gen.csv")
    assert len(gens) == 8
    fuels = sorted({g.fuel for g in gens})
    assert "HYDRO" in fuels
    assert "NATURAL_GAS" in fuels
    hydro = [g for g in gens if g.fuel == "HYDRO"]
    assert len(hydro) == 3
    # Verify one specific thermal entry: Solitude (NG CT, 35 MW).
    sol = next(g for g in gens if g.name == "Solitude")
    assert sol.unit_type == "CT"
    assert sol.fuel == "NATURAL_GAS"
    assert sol.pmax_mw == 35.0
    assert sol.var_cost == 30.0


def test_parse_hydro_upstream_chain():
    case_dir = extract_bundle("5bus")
    chain = parse_hydro_upstream(case_dir / "Hydro_Upstream_Input.csv")
    assert chain == {
        "HydroUnit1": [],
        "HydroUnit2": ["HydroUnit1"],
        "HydroUnit3": ["HydroUnit2"],
    }


def test_parse_user_descriptors_has_branch_mapping():
    case_dir = extract_bundle("5bus")
    sections = parse_user_descriptors(case_dir / "user_descriptors.yaml")
    assert "branch" in sections
    assert sections["branch"].get("UID") == "name"
    assert sections["branch"].get("From Bus") == "connection_points_from"
    assert sections["branch"].get("R") == "r"


def test_load_case_aggregates_all_csvs():
    case_dir = extract_bundle("5bus")
    case = load_case(case_dir)
    assert len(case.buses) == 5
    assert len(case.branches) == 7
    assert len(case.generators) == 8
    assert len(case.hydro_upstream) == 3


def test_dataclass_field_types():
    """Sanity-check dataclass field round-trips."""

    case_dir = extract_bundle("5bus")
    case = load_case(case_dir)
    assert isinstance(case.buses[0], SiennaBus)
    assert isinstance(case.branches[0], SiennaBranch)
    assert isinstance(case.generators[0], SiennaGen)
