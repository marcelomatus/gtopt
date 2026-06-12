"""Round-trip tests for ``sienna_to_gtopt._bundle.extract_bundle``."""

from __future__ import annotations

import pytest

from sienna_to_gtopt._bundle import BUNDLES, available_bundles, extract_bundle


EXPECTED_5BUS_FILES = {
    "branch.csv",
    "bus.csv",
    "gen.csv",
    "generator_mapping.yaml",
    "modifier.jl",
    "reserves.csv",
    "storage.csv",
    "Hydro_Upstream_Input.csv",
    "user_descriptors.yaml",
    "user_descriptors_var_cost.yaml",
    "timeseries_pointers_da.json",
    "timeseries_pointers_rt.json",
}


def test_bundles_registered():
    assert "5bus" in BUNDLES
    assert "14bus" in BUNDLES


def test_available_bundles_lists_only_present_files():
    # The 5-bus bundle is shipped with the repo; the 14-bus one
    # may or may not be (it's a smaller fixture).  Either way the
    # function must return a sorted list of strings, with at least
    # the 5-bus archive present.
    avail = available_bundles()
    assert isinstance(avail, list)
    assert avail == sorted(avail)
    assert "5bus" in avail


def test_extract_bundle_5bus_layout():
    case_dir = extract_bundle("5bus")
    assert case_dir.exists()
    assert case_dir.is_dir()
    files = {p.name for p in case_dir.iterdir()}
    # Everything in EXPECTED_5BUS_FILES (plus possibly extras) must be
    # present so downstream parsers don't trip on a stripped bundle.
    missing = EXPECTED_5BUS_FILES - files
    assert not missing, f"Missing from bundle: {sorted(missing)}"


def test_extract_bundle_cached():
    """Second call returns the same dir (lru_cache contract)."""

    first = extract_bundle("5bus")
    second = extract_bundle("5bus")
    assert first == second


def test_extract_bundle_unknown_name():
    with pytest.raises(KeyError):
        extract_bundle("does_not_exist")
