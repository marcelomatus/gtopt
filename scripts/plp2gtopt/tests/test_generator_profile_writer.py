#!/usr/bin/env python3
# -*- coding: utf-8 -*-

"""Unit tests for GeneratorProfileWriter class."""

import typing
from typing import Any, Dict, List
import pytest
from ..generator_profile_writer import GeneratorProfileWriter


# ---------------------------------------------------------------------------
# Mock parsers
# ---------------------------------------------------------------------------


class MockCentralParser:
    """Minimal CentralParser stub."""

    def __init__(self, centrals: List[Dict[str, Any]]):
        self._data = centrals
        self._by_name = {c["name"]: c for c in centrals}
        self.centrals_of_type: Dict[str, List[Dict[str, Any]]] = {}
        for c in centrals:
            ctype = c.get("type", "unknown")
            self.centrals_of_type.setdefault(ctype, []).append(c)

    @property
    def items(self):
        return self._data

    @property
    def num_centrals(self):
        return len(self._data)

    def get_all(self):
        return self._data

    def get_item_by_name(self, name: str):
        return self._by_name.get(name)


class MockBusParser:
    """Minimal BusParser stub."""

    def __init__(self, buses: List[Dict[str, Any]]):
        self._buses = {b["number"]: b for b in buses}

    def get_bus_by_number(self, number: int):
        return self._buses.get(number)


class MockAflceParser:
    """Minimal AflceParser stub."""

    def __init__(self, flows: List[Dict[str, Any]]):
        self._flows = {f["name"]: f for f in flows}

    def get_item_by_name(self, name: str):
        return self._flows.get(name)


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


def test_empty_writer():
    """GeneratorProfileWriter with no parsers returns empty array."""
    writer = GeneratorProfileWriter()
    assert not writer.to_json_array()


def test_pasada_with_aflce():
    """Pasada pura central with aflce data produces Flow@discharge profile."""
    centrals = [
        {
            "number": 1,
            "name": "HydroGen",
            "type": "pasada",
            "bus": 1,
            "pmax": 200.0,
            "afluent": 50.0,
        },
    ]
    buses = [{"number": 1, "name": "Bus1"}]
    flows = [{"name": "HydroGen", "flow": [[50.0], [80.0]]}]

    writer = GeneratorProfileWriter(
        central_parser=typing.cast(typing.Any, MockCentralParser(centrals)),
        bus_parser=typing.cast(typing.Any, MockBusParser(buses)),
        aflce_parser=typing.cast(typing.Any, MockAflceParser(flows)),
    )
    result = writer.to_json_array()

    assert len(result) == 1
    assert result[0]["uid"] == 1
    assert result[0]["name"] == "HydroGen"
    assert result[0]["generator"] == "HydroGen"
    assert result[0]["profile"] == "Flow@discharge"


def test_pasada_without_aflce_uses_afluent():
    """Pasada without aflce parser uses inline afluent value as profile."""
    centrals = [
        {
            "number": 1,
            "name": "HydroGen",
            "type": "pasada",
            "bus": 1,
            "pmax": 200.0,
            "afluent": 50.0,
        },
    ]
    buses = [{"number": 1, "name": "Bus1"}]

    writer = GeneratorProfileWriter(
        central_parser=typing.cast(typing.Any, MockCentralParser(centrals)),
        bus_parser=typing.cast(typing.Any, MockBusParser(buses)),
    )
    result = writer.to_json_array()

    assert len(result) == 1
    assert result[0]["profile"] == pytest.approx(50.0)


def test_pasada_zero_afluent_skipped():
    """Pasada with zero afluent and no aflce data is skipped."""
    centrals = [
        {
            "number": 1,
            "name": "NoFlow",
            "type": "pasada",
            "bus": 1,
            "pmax": 100.0,
            "afluent": 0.0,
        },
    ]
    buses = [{"number": 1, "name": "Bus1"}]

    writer = GeneratorProfileWriter(
        central_parser=typing.cast(typing.Any, MockCentralParser(centrals)),
        bus_parser=typing.cast(typing.Any, MockBusParser(buses)),
    )
    assert not writer.to_json_array()


def test_invalid_bus_skipped():
    """Central with bus not in bus_parser is skipped."""
    centrals = [
        {
            "number": 1,
            "name": "HydroGen",
            "type": "pasada",
            "bus": 99,
            "pmax": 200.0,
            "afluent": 50.0,
        },
    ]
    buses = [{"number": 1, "name": "Bus1"}]

    writer = GeneratorProfileWriter(
        central_parser=typing.cast(typing.Any, MockCentralParser(centrals)),
        bus_parser=typing.cast(typing.Any, MockBusParser(buses)),
    )
    assert not writer.to_json_array()


def test_profile_parquet_filename(tmp_path):
    """Profile mode writes profile.parquet (not discharge.parquet)."""
    import numpy as np

    centrals = [
        {
            "number": 1,
            "name": "Solar1",
            "type": "pasada",
            "bus": 1,
            "pmax": 100.0,
            "afluent": 0.5,
        },
    ]

    # Create a minimal aflce parser with flow data
    class FullAflceParser:
        def __init__(self):
            self._flows = [
                {
                    "name": "Solar1",
                    "block": np.array([1, 2], dtype=np.int32),
                    "flow": np.array([[0.5], [0.8]], dtype=np.float64),
                    "num_hydrologies": 1,
                }
            ]

        @property
        def flows(self):
            return self._flows

        def get_all(self):
            return self._flows

        def get_item_by_name(self, name):
            for f in self._flows:
                if f["name"] == name:
                    return f
            return None

    scenarios = [{"uid": 1, "hydrology": 0}]
    writer = GeneratorProfileWriter(
        central_parser=typing.cast(typing.Any, MockCentralParser(centrals)),
        bus_parser=typing.cast(
            typing.Any, MockBusParser([{"number": 1, "name": "B1"}])
        ),
        aflce_parser=typing.cast(typing.Any, FullAflceParser()),
        block_parser=None,
        scenarios=scenarios,
        options={
            "output_dir": tmp_path,
            "compression": "gzip",
            "_pasada_profile_names": {"Solar1"},
        },
    )
    writer.to_json_array()

    profile_dir = tmp_path / "GeneratorProfile"
    assert (profile_dir / "profile.parquet").exists(), (
        "Should write profile.parquet, not discharge.parquet"
    )
    assert not (profile_dir / "discharge.parquet").exists(), (
        "Should NOT write discharge.parquet in GeneratorProfile/"
    )


def test_zero_bus_skipped():
    """Central with bus 0 is skipped."""
    centrals = [
        {
            "number": 1,
            "name": "HydroGen",
            "type": "pasada",
            "bus": 0,
            "pmax": 200.0,
            "afluent": 50.0,
        },
    ]

    writer = GeneratorProfileWriter(
        central_parser=typing.cast(typing.Any, MockCentralParser(centrals)),
    )
    assert not writer.to_json_array()
