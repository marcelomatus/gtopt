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
    assert writer.to_json_array() == []


def test_pasada_with_aflce():
    """Pasada pura central with aflce data produces Afluent@afluent profile."""
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
    assert result[0]["generator"] == 1
    assert result[0]["profile"] == "Afluent@afluent"


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
    assert writer.to_json_array() == []


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
    assert writer.to_json_array() == []


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
    assert writer.to_json_array() == []
